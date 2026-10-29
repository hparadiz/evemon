#include "query.h"
#include "../plugins/broker_data.h"
#include "query_events.h"
#include "query_gpu.h"
#include "query_about.h"
#ifdef HAVE_NVTOP
#include "../plugins/gpu_nvtop_adapter.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CLIENTS 16
/* One bounded inspector queue, separate from both sampling and socket I/O. */
typedef struct {
    int state; /* 0 free, 1 queued, 2 running, 3 complete */
    uint64_t ticket;
    pid_t pid;
    unsigned needs;
    uint16_t type;
    ipc_buf_t result;
    int error;
} inspect_job_t;
struct query_server {
    monitor_state_t *state;
    query_events_t *events;
    int listener, wake[2], lock_fd;
    pthread_t thread, inspector;
    pthread_mutex_t jobs_lock;
    pthread_cond_t jobs_changed;
    inspect_job_t jobs[CLIENTS];
    atomic_uint clients;
    int stopping, inspector_started;
    uint64_t instance;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    dev_t device;
    ino_t inode;
};
typedef struct {
    int fd, hello, job;
    uint64_t ticket;
    unsigned char request[IPC_HEADER_SIZE + IPC_REQUEST_LIMIT], header[IPC_HEADER_SIZE];
    size_t used, expected, sent;
    ipc_header_t incoming;
    ipc_buf_t response;
    int writing;
    uint32_t period, subscription, event_subscription;
    int events;
    uint64_t event_cursor;
    int64_t next_event;
    int64_t next_sample, deadline;
} client_t;
static void client_close(query_server_t *s, client_t *c)
{
    if (c->fd >= 0) {
        query_controls_drop(s->events, c->ticket);
        close(c->fd);
        atomic_fetch_sub_explicit(&s->clients, 1, memory_order_relaxed);
    }
    ipc_free(&c->response);
    memset(c, 0, sizeof(*c)); c->fd = -1; c->job = -1;
}
static void reply(client_t *c, uint16_t type, uint32_t id, ipc_buf_t *body)
{
    ipc_free(&c->response); c->response = *body; memset(body, 0, sizeof(*body));
    ipc_header_write(c->header, type, id, (uint32_t)c->response.len);
    c->sent = 0; c->writing = 1; c->used = 0; c->expected = IPC_HEADER_SIZE;
    c->deadline = ipc_now_ms() + IPC_TIMEOUT_MS;
}
static void reply_error(client_t *c, uint32_t id, int code)
{
    static const char *messages[] = {"", "invalid request", "snapshot not ready", "process not found",
                                     "unsupported operation", "evemon error", "inspector queue full"};
    ipc_buf_t body = {0};
    ipc_put_u32(&body, code); ipc_put_string(&body, messages[code > 0 && code < 7 ? code : IPC_INTERNAL]);
    reply(c, IPC_ERROR, id, &body);
}
static void *inspect_worker(void *arg)
{
    query_server_t *s = arg;
    pthread_setname_np(pthread_self(), "ev-inspect");
#ifdef HAVE_NVTOP
    evemon_gpu_backend_t *gpu = NULL;
    evemon_gpu_snapshot_t gpu_cache = {0};
    int64_t gpu_time = 0;
#endif
    unsigned next_job = 0;
    pthread_mutex_lock(&s->jobs_lock);
    for (;;) {
        int index = -1;
        for (unsigned i = 0; i < CLIENTS; ++i) { unsigned slot = (next_job + i) % CLIENTS;
            if (s->jobs[slot].state == 1) { index = slot; next_job = (slot + 1) % CLIENTS; break; } }
        if (s->stopping) break;
        if (index < 0) { pthread_cond_wait(&s->jobs_changed, &s->jobs_lock); continue; }
        inspect_job_t *j = &s->jobs[index]; j->state = 2;
        pthread_mutex_unlock(&s->jobs_lock);
        ipc_buf_t body = {0};
        int error;
#ifdef HAVE_NVTOP
        if (j->type == IPC_GPU || j->type == IPC_ABOUT) {
            if (!gpu) { gpu = evemon_gpu_backend_new(); if (gpu) evemon_gpu_backend_init(gpu); }
            int64_t now = ipc_now_ms();
            if (gpu && (!gpu_time || now - gpu_time >= 250)) {
                evemon_gpu_snapshot_clear(&gpu_cache);
                if (evemon_gpu_backend_snapshot(gpu, &gpu_cache, (uint64_t)now * 1000000)) gpu_time = now;
            }
        }
#endif
        if (j->type == IPC_ABOUT) {
            query_about_encode(&body,
#ifdef HAVE_NVTOP
                               &gpu_cache
#else
                               NULL
#endif
            );
            error = body.error ? IPC_INTERNAL : 0;
        } else if (j->type == IPC_GPU) {
#ifdef HAVE_NVTOP
            if (gpu) { query_gpu_encode(&gpu_cache, &body); error = body.error ? IPC_INTERNAL : 0; }
            else error = IPC_INTERNAL;
#else
            error = IPC_UNSUPPORTED;
#endif
        } else error = query_inspect_encode(j->pid, j->needs, s->state->fdmon, &body);
        pthread_mutex_lock(&s->jobs_lock);
        j->error = error; j->result = body; j->state = 3;
    }
    pthread_mutex_unlock(&s->jobs_lock);
#ifdef HAVE_NVTOP
    evemon_gpu_snapshot_clear(&gpu_cache); evemon_gpu_backend_free(gpu);
#endif
    return NULL;
}
static void request_dispatch(query_server_t *s, client_t *c)
{
    ipc_header_t h = c->incoming;
    ipc_buf_t in = {.data = c->request + IPC_HEADER_SIZE, .len = h.size}, out = {0};
    if (!c->hello && h.type != IPC_HELLO) { reply_error(c, h.id, IPC_BAD_REQUEST); return; }
    int error = 0;
    switch (h.type) {
    case IPC_HELLO:
        if (h.size || c->hello) { error = IPC_BAD_REQUEST; break; }
        c->hello = 1;
        ipc_put_u32(&out, IPC_CAP_SNAPSHOT | IPC_CAP_INSPECT | IPC_CAP_MPRIS | IPC_CAP_EVENTS | IPC_CAP_SIGNAL | IPC_CAP_ABOUT
#ifdef HAVE_PIPEWIRE
                    | IPC_CAP_PIPEWIRE
#endif
#ifdef HAVE_NVTOP
                    | IPC_CAP_GPU
#endif
        );
        ipc_put_u32(&out, IPC_LIMIT); ipc_put_u64(&out, s->instance);
        ipc_put_u32(&out, s->state->fdmon ? fdmon_active_backend(s->state->fdmon) : 0);
        ipc_put_string(&out, EVEMON_VERSION);
        break;
    case IPC_HOST:
        if (h.size) error = IPC_BAD_REQUEST;
        else query_host_encode(&out);
        break;
    case IPC_SIGNAL:
        query_signal_dispatch(&in, &out, s->instance);
        break;
    case IPC_STATUS:
    case IPC_SNAPSHOT:
        if (h.size) error = IPC_BAD_REQUEST;
        else error = query_snapshot_encode(s->state, h.type, 0, &out);
        break;
    case IPC_PROCESS: {
        uint32_t pid = ipc_get_u32(&in);
        if (!ipc_done(&in) || !pid || pid > INT_MAX) error = IPC_BAD_REQUEST;
        else error = query_snapshot_encode(s->state, h.type, pid, &out);
        break;
    }
    case IPC_SUBSCRIBE: {
        uint32_t period = ipc_get_u32(&in);
        if (!ipc_done(&in) || period > 3600000) { error = IPC_BAD_REQUEST; break; }
        c->period = period; c->subscription = h.id; c->next_sample = ipc_now_ms();
        ipc_put_u32(&out, period); break;
    }
    case IPC_CONTROL:
        error = query_controls_replace(s->events, c->ticket, &in);
        break;
    case IPC_EVENTS: {
        uint32_t enabled = ipc_get_u32(&in);
        if (!ipc_done(&in) || enabled > 1) { error = IPC_BAD_REQUEST; break; }
        c->events = enabled; c->event_subscription = h.id;
        c->event_cursor = query_events_latest(s->events); c->next_event = ipc_now_ms();
        ipc_put_u32(&out, enabled); break;
    }
    case IPC_GPU:
    case IPC_ABOUT:
    case IPC_INSPECT: {
        uint32_t pid = h.type == IPC_INSPECT ? ipc_get_u32(&in) : 1;
        uint32_t needs = h.type == IPC_INSPECT ? ipc_get_u32(&in) : 0;
        if (!ipc_done(&in) || !pid || pid > INT_MAX || needs & ~2047u) { error = IPC_BAD_REQUEST; break; }
        pthread_mutex_lock(&s->jobs_lock);
        int index = -1;
        for (int i = 0; i < CLIENTS; ++i) if (!s->jobs[i].state) { index = i; break; }
        if (index >= 0) {
            s->jobs[index] = (inspect_job_t){.state = 1, .ticket = c->ticket, .pid = pid, .needs = needs, .type = h.type};
            c->job = index; c->deadline = ipc_now_ms() + IPC_TIMEOUT_MS;
            pthread_cond_signal(&s->jobs_changed);
        }
        pthread_mutex_unlock(&s->jobs_lock);
        if (index >= 0) return;
        error = IPC_BUSY; break;
    }
    default: error = IPC_UNSUPPORTED; break;
    }
    if (error || out.error) { ipc_free(&out); reply_error(c, h.id, error ? error : IPC_INTERNAL); }
    else reply(c, h.type | IPC_REPLY, h.id, &out);
}
static void read_request(query_server_t *s, client_t *c)
{
    if (!c->used) c->deadline = ipc_now_ms() + IPC_TIMEOUT_MS;
    ssize_t n = recv(c->fd, c->request + c->used, c->expected - c->used, 0);
    if (n <= 0) {
        if (!n || (errno != EAGAIN && errno != EINTR)) client_close(s, c);
        return;
    }
    c->used += n;
    if (c->used == IPC_HEADER_SIZE && c->expected == IPC_HEADER_SIZE) {
        if (ipc_header_read(c->request, &c->incoming) || c->incoming.size > IPC_REQUEST_LIMIT) { client_close(s, c); return; }
        c->expected += c->incoming.size;
    }
    if (c->used == c->expected) request_dispatch(s, c);
}
static void send_reply(query_server_t *s, client_t *c)
{
    const void *data = c->sent < IPC_HEADER_SIZE ? c->header + c->sent : c->response.data + c->sent - IPC_HEADER_SIZE;
    size_t size = c->sent < IPC_HEADER_SIZE ? IPC_HEADER_SIZE - c->sent : c->response.len + IPC_HEADER_SIZE - c->sent;
    ssize_t n = send(c->fd, data, size, MSG_NOSIGNAL);
    if (n < 0 && errno != EAGAIN && errno != EINTR) { client_close(s, c); return; }
    if (n > 0) c->sent += n;
    if (c->sent == IPC_HEADER_SIZE + c->response.len) {
        ipc_free(&c->response); c->writing = 0; c->deadline = c->hello ? 0 : ipc_now_ms() + IPC_TIMEOUT_MS;
    }
}
static void *query_worker(void *arg)
{
    query_server_t *s = arg;
    pthread_setname_np(pthread_self(), "ev-query");
    client_t clients[CLIENTS] = {0};
    uint64_t ticket = 0;
    for (int i = 0; i < CLIENTS; ++i) { clients[i].fd = -1; clients[i].job = -1; }
    for (;;) {
        int64_t now = ipc_now_ms();
        /* Reap completed work by connection identity; descriptor reuse cannot steal replies. */
        pthread_mutex_lock(&s->jobs_lock);
        for (int i = 0; i < CLIENTS; ++i) if (s->jobs[i].state == 3) {
            inspect_job_t *job = &s->jobs[i];
            for (int j = 0; j < CLIENTS; ++j) {
                client_t *c = &clients[j];
                if (c->fd >= 0 && c->job == i && c->ticket == job->ticket) {
                    if (job->error) reply_error(c, c->incoming.id, job->error);
                    else reply(c, c->incoming.type | IPC_REPLY, c->incoming.id, &job->result);
                    c->job = -1; break;
                }
            }
            ipc_free(&job->result); job->state = 0;
        }
        pthread_mutex_unlock(&s->jobs_lock);
        int timeout = 25;
        struct pollfd fds[CLIENTS + 2] = {{s->wake[0], POLLIN, 0}, {s->listener, POLLIN, 0}};
        for (int i = 0; i < CLIENTS; ++i) {
            client_t *c = &clients[i];
            struct pollfd input = {c->fd, POLLIN, 0};
            int has_input = c->fd >= 0 && poll(&input, 1, 0) > 0 && (input.revents & POLLIN);
            if (!has_input && c->fd >= 0 && c->period && !c->writing && c->job < 0 && !c->used) {
                if (now >= c->next_sample) {
                    ipc_buf_t body = {0}; int error = query_snapshot_encode(s->state, IPC_SNAPSHOT, 0, &body);
                    if (error) { ipc_free(&body); reply_error(c, c->subscription, error); }
                    else reply(c, IPC_SNAPSHOT | IPC_REPLY, c->subscription, &body);
                    c->next_sample = now + c->period;
                } else if (c->next_sample - now < timeout) timeout = (int)(c->next_sample - now);
            }
            if (!has_input && c->fd >= 0 && c->events && !c->writing && c->job < 0 && !c->used && now >= c->next_event) {
                ipc_buf_t body = {0}; query_events_encode(s->events, &c->event_cursor, &body);
                reply(c, IPC_EVENTS | IPC_REPLY, c->event_subscription, &body);
                c->next_event = now + 50;
            }
            fds[i + 2] = (struct pollfd){c->fd, c->writing ? POLLOUT : c->job < 0 ? POLLIN : 0, 0};
        }
        int result = poll(fds, CLIENTS + 2, timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || fds[0].revents) break;
        now = ipc_now_ms();
        for (int i = 0; i < CLIENTS; ++i) {
            client_t *c = &clients[i]; short events = fds[i + 2].revents;
            if (c->fd < 0) continue;
            if ((c->deadline && now >= c->deadline) || (events & (POLLERR | POLLNVAL))) { client_close(s, c); continue; }
            if (events & POLLIN) read_request(s, c);
            if (c->fd >= 0 && (events & POLLOUT)) send_reply(s, c);
            if (c->fd >= 0 && (events & POLLHUP) && !(events & POLLIN)) client_close(s, c);
        }
        if (fds[1].revents & POLLIN) for (int count = 0; count < CLIENTS; ++count) {
            int fd = accept4(s->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            struct ucred cred; socklen_t length = sizeof(cred); int slot = -1;
            for (int i = 0; i < CLIENTS; ++i) if (clients[i].fd < 0) { slot = i; break; }
            /* Filesystem access (including inherited ACLs) authorizes clients
             * of root Evemon. Abstract sockets have no filesystem gate. */
            int filesystem_root = geteuid() == 0 && s->path[0] != '@';
            if (slot < 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) ||
                (!filesystem_root && cred.uid != getuid())) { close(fd); continue; }
            clients[slot] = (client_t){.fd = fd, .job = -1, .ticket = ++ticket,
                .expected = IPC_HEADER_SIZE, .deadline = now + IPC_TIMEOUT_MS};
            atomic_fetch_add_explicit(&s->clients, 1, memory_order_relaxed);
        }
    }
    for (int i = 0; i < CLIENTS; ++i) client_close(s, &clients[i]);
    return NULL;
}
int query_default_path(char *path, size_t capacity)
{
    char directory[256];
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    int system = geteuid() == 0;
    if (system)
        snprintf(directory, sizeof(directory), "%s", IPC_SYSTEM_DIRECTORY);
    else if (runtime && stat(runtime, &st) == 0 && S_ISDIR(st.st_mode) &&
        st.st_uid == getuid() && !(st.st_mode & 0077))
        snprintf(directory, sizeof(directory), "%s/evemon", runtime);
    else
        snprintf(directory, sizeof(directory), "/tmp/evemon-%lu", (unsigned long)getuid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    if (lstat(directory, &st) < 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & (system ? 0022 : 0077))) { errno = EACCES; return -1; }
    /* Preserve administrators' directory/default ACLs across every launch. */
    int n = snprintf(path, capacity, "%s/%s", directory, system ? "evemon.sock" : "query.sock");
    if (n < 0 || (size_t)n >= capacity) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static void server_free(query_server_t *s)
{
    if (s->inspector_started) {
        pthread_mutex_lock(&s->jobs_lock); s->stopping = 1;
        pthread_cond_signal(&s->jobs_changed); pthread_mutex_unlock(&s->jobs_lock);
        pthread_join(s->inspector, NULL);
    }
    for (int i = 0; i < CLIENTS; ++i) ipc_free(&s->jobs[i].result);
    pthread_mutex_destroy(&s->jobs_lock); pthread_cond_destroy(&s->jobs_changed);
    if (s->listener >= 0) close(s->listener);
    if (s->wake[0] >= 0) close(s->wake[0]);
    if (s->wake[1] >= 0) close(s->wake[1]);
    struct stat st;
    if (s->inode && lstat(s->path, &st) == 0 && st.st_dev == s->device && st.st_ino == s->inode)
        unlink(s->path);
    if (s->lock_fd >= 0) close(s->lock_fd);
    query_events_stop(s->events);
    free(s);
}
static int inherit_socket_acl(const char *path)
{
    /* Linux's Unix bind masks the creation mode before the filesystem applies
     * its default ACL. Restore that ACL before listen, while the new socket is
     * still inaccessible to clients. Policy stays entirely on the directory. */
    char *directory = g_path_get_dirname(path);
    ssize_t size = getxattr(directory, "system.posix_acl_default", NULL, 0);
    if (size < 0 && (errno == ENODATA || errno == ENOTSUP)) { g_free(directory); return 0; }
    if (size <= 0) { g_free(directory); return size < 0 ? -1 : 0; }
    void *acl = malloc((size_t)size);
    if (!acl) { g_free(directory); return -1; }
    ssize_t read = getxattr(directory, "system.posix_acl_default", acl, (size_t)size);
    int rc = read < 0 ? -1 : lsetxattr(path, "system.posix_acl_access", acl, (size_t)read, 0);
    int saved = errno;
    free(acl); g_free(directory); errno = saved;
    return rc;
}
query_server_t *query_server_start(monitor_state_t *state, const char *path)
{
    if (geteuid() == 0 && !strcmp(path, IPC_SYSTEM_SOCKET)) {
        char checked[sizeof(((struct sockaddr_un *)0)->sun_path)];
        if (query_default_path(checked, sizeof(checked))) return NULL;
    }
    query_server_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->listener = s->wake[0] = s->wake[1] = s->lock_fd = -1;
    atomic_init(&s->clients, 0);
    s->state = state;
    pthread_mutex_init(&s->jobs_lock, NULL); pthread_cond_init(&s->jobs_changed, NULL);
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (!state->evemon_instance)
        state->evemon_instance = (uint64_t)now.tv_sec * 1000000000u +
                                 (uint64_t)now.tv_nsec;
    s->instance = state->evemon_instance;
    s->events = query_events_start(state->fdmon);
    if (!s->events) goto fail;
    if (strlen(path) >= sizeof(s->path)) { errno = ENAMETOOLONG; goto fail; }
    strcpy(s->path, path);
    struct sockaddr_un address; socklen_t address_size;
    if (ipc_address(path, &address, &address_size)) goto fail;
    const int abstract = path[0] == '@';
    struct stat st;
    if (!abstract) {
        char lock_path[sizeof(s->path) + 6];
        snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
        s->lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (s->lock_fd < 0) goto fail;
        if (fstat(s->lock_fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
            errno = EACCES; goto fail;
        }
        if (flock(s->lock_fd, LOCK_EX | LOCK_NB)) goto fail;
        if (lstat(path, &st) == 0) {
            if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) { errno = EACCES; goto fail; }
            int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (probe < 0) goto fail;
            int connected = connect(probe, (struct sockaddr *)&address, address_size);
            int error = errno;
            close(probe);
            if (connected == 0 || error != ECONNREFUSED) { errno = EADDRINUSE; goto fail; }
            if (unlink(path)) goto fail;
        } else if (errno != ENOENT) goto fail;
    }
    s->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s->listener < 0) goto fail;
    /* Bind privately; apply any directory ACL before accepting connections. */
    mode_t mask = umask(0177); /* Called before application worker threads start. */
    int bound = bind(s->listener, (struct sockaddr *)&address, address_size);
    int error = errno;
    umask(mask);
    if (bound) { errno = error; goto fail; }
    if (!abstract) {
        if (lstat(path, &st)) goto fail;
        s->device = st.st_dev; s->inode = st.st_ino;
        if (inherit_socket_acl(path)) goto fail;
    }
    if (listen(s->listener, CLIENTS) || pipe2(s->wake, O_CLOEXEC | O_NONBLOCK)) goto fail;
    int rc = pthread_create(&s->inspector, NULL, inspect_worker, s);
    if (rc) { errno = rc; goto fail; }
    s->inspector_started = 1;
    rc = pthread_create(&s->thread, NULL, query_worker, s);
    if (rc) { errno = rc; goto fail; }
    return s;
fail:
    {
        int saved = errno;
        server_free(s);
        errno = saved;
        return NULL;
    }
}
void query_server_stop(query_server_t *s)
{
    if (!s) return;
    char wake = 1;
    ssize_t written = write(s->wake[1], &wake, 1);
    (void)written;
    pthread_join(s->thread, NULL);
    server_free(s);
}
unsigned query_server_client_count(const query_server_t *s)
{
    return s ? atomic_load_explicit(&s->clients, memory_order_relaxed) : 0;
}
