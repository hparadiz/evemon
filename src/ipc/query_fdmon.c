#include "query_events.h"
#include "../platform/fdmon_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>

#define WATCHES 128
typedef struct { uint32_t kind, pid, value; } watch_t;
typedef struct {
    monitor_state_t *state;
    char path[108];
    pthread_mutex_t lock;
    pthread_t thread;
    int wake[2];
    unsigned count;
    uint64_t revision;
    watch_t watches[WATCHES];
    atomic_int active;
} remote_t;
static int send_controls(remote_t *r, int fd, uint64_t *sent_revision)
{
    ipc_buf_t body = {0};
    pthread_mutex_lock(&r->lock);
    uint64_t revision = r->revision;
    if (*sent_revision != revision) {
        ipc_put_u32(&body, r->count);
        for (unsigned i = 0; i < r->count; ++i) {
            ipc_put_u32(&body, r->watches[i].kind); ipc_put_u32(&body, r->watches[i].pid);
            ipc_put_u32(&body, r->watches[i].value);
        }
    }
    pthread_mutex_unlock(&r->lock);
    int result = body.len ? ipc_send(fd, IPC_CONTROL, 3, &body) : 0;
    ipc_free(&body);
    if (!result) *sent_revision = revision;
    return result;
}
static void *remote_worker(void *opaque)
{
    remote_t *r = opaque;
    pthread_setname_np(pthread_self(), "ev-events");
    for (;;) {
        struct pollfd stop = {r->wake[0], POLLIN, 0};
        if (poll(&stop, 1, 0) > 0) break;
        if (!atomic_load(&r->active)) { poll(&stop, 1, 20); continue; }
        int fd = ipc_connect(r->path);
        if (fd >= 0 && !ipc_hello(fd, NULL, NULL)) {
            ipc_buf_t body = {0}; ipc_header_t h;
            ipc_put_u32(&body, 1);
            int error = ipc_send(fd, IPC_EVENTS, 2, &body);
            ipc_free(&body);
            if (!error) error = ipc_receive(fd, &h, &body, r->wake[0]);
            if (!error && (h.type != (IPC_EVENTS | IPC_REPLY) || h.id != 2 || ipc_get_u32(&body) != 1 || !ipc_done(&body))) error = -1;
            ipc_free(&body);
            uint64_t revision = UINT64_MAX;
            while (!error) {
                if (send_controls(r, fd, &revision)) break;
                struct pollfd p[2] = {{r->wake[0], POLLIN, 0}, {fd, POLLIN, 0}};
                int n = poll(p, 2, 50);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 || p[0].revents || (p[1].revents & (POLLERR | POLLHUP | POLLNVAL))) break;
                if (!p[1].revents) continue;
                error = ipc_receive(fd, &h, &body, r->wake[0]);
                if (!error) {
                    if (h.type == (IPC_EVENTS | IPC_REPLY) && h.id == 2) error = query_events_dispatch(&body);
                    else if (h.type == IPC_ERROR && h.id == 3) {
                        fprintf(stderr, "evemon: evemon rejected tracing registrations (error %u)\n", ipc_get_u32(&body));
                    } else if (h.type != (IPC_CONTROL | IPC_REPLY) || h.id != 3 || body.len) error = -1;
                }
                ipc_free(&body);
            }
        }
        if (fd >= 0) close(fd);
        if (poll(&stop, 1, 500) > 0) break;
    }
    return NULL;
}
fdmon_ctx_t *query_fdmon_prepare(monitor_state_t *state, const char *path)
{
    if (strlen(path) >= 108) { errno = ENAMETOOLONG; return NULL; }
    fdmon_ctx_t *ctx = fdmon_create(FDMON_BACKEND_REMOTE);
    remote_t *r = calloc(1, sizeof(*r));
    if (!ctx || !r) { fdmon_destroy(ctx); free(r); return NULL; }
    r->state = state; strcpy(r->path, path);
    pthread_mutex_init(&r->lock, NULL);
    if (pipe2(r->wake, O_CLOEXEC | O_NONBLOCK)) { pthread_mutex_destroy(&r->lock); free(r); fdmon_destroy(ctx); return NULL; }
    ctx->remote = r;
    int rc = pthread_create(&r->thread, NULL, remote_worker, r);
    if (rc) {
        ctx->remote = NULL; close(r->wake[0]); close(r->wake[1]);
        pthread_mutex_destroy(&r->lock); free(r); fdmon_destroy(ctx); errno = rc; return NULL;
    }
    return ctx;
}
void query_fdmon_activate(fdmon_ctx_t *ctx)
{
    remote_t *r = ctx->remote; atomic_store(&r->active, 1);
}
fdmon_ctx_t *query_fdmon_create(monitor_state_t *state, const char *path)
{
    fdmon_ctx_t *ctx = query_fdmon_prepare(state, path);
    if (ctx) query_fdmon_activate(ctx);
    return ctx;
}
void query_fdmon_destroy(fdmon_ctx_t *ctx)
{
    remote_t *r = ctx->remote;
    char wake = 1; ssize_t n = write(r->wake[1], &wake, 1); (void)n;
    pthread_join(r->thread, NULL);
    close(r->wake[0]); close(r->wake[1]); pthread_mutex_destroy(&r->lock);
    free(r); ctx->remote = NULL;
}
int query_fdmon_backend(const fdmon_ctx_t *ctx)
{
    remote_t *r = ctx->remote;
    pthread_mutex_lock(&r->state->lock);
    int backend = r->state->connected ? r->state->system.fdmon_backend : 0;
    pthread_mutex_unlock(&r->state->lock); return backend;
}
int query_fdmon_net(const fdmon_ctx_t *ctx, pid_t pid, uint64_t *sent, uint64_t *received)
{
    remote_t *r = ctx->remote; int rc = -1;
    *sent = *received = 0;
    pthread_mutex_lock(&r->state->lock);
    for (uint32_t i = 0; i < r->state->snapshot.len; ++i) {
        const proc_entry_t *p = &r->state->snapshot.entries[i];
        if (p->pid == pid) { *sent = p->net_send_bytes; *received = p->net_recv_bytes; rc = 0; break; }
    }
    pthread_mutex_unlock(&r->state->lock); return rc;
}
int query_fdmon_control(fdmon_ctx_t *ctx, unsigned kind, pid_t pid, int value, int remove)
{
    if (!remove && query_fdmon_backend(ctx) != FDMON_BACKEND_EBPF) { errno = ENOTSUP; return -1; }
    remote_t *r = ctx->remote;
    pthread_mutex_lock(&r->lock);
    unsigned slot;
    for (slot = 0; slot < r->count; ++slot) {
        watch_t w = r->watches[slot];
        if (w.kind == kind && w.pid == (uint32_t)pid && (kind != 1 || w.value == (uint32_t)value)) break;
    }
    if (remove) {
        if (slot < r->count) { r->watches[slot] = r->watches[--r->count]; ++r->revision; }
    } else {
        if (slot == WATCHES) { pthread_mutex_unlock(&r->lock); errno = ENOSPC; return -1; }
        if (slot == r->count) ++r->count;
        r->watches[slot] = (watch_t){kind, pid, value}; ++r->revision;
    }
    pthread_mutex_unlock(&r->lock); return 0;
}
