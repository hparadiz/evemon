#include "query.h"
#include "query_gpu.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static char remote_path[108];
static uint32_t remote_capabilities;
static monitor_state_t *remote_state;
int query_cached_process(pid_t pid, proc_entry_t *entry, char *name, size_t size)
{
    if (name && size) name[0] = 0;
    if (!remote_state) return 0;
    int found = 0;
    pthread_mutex_lock(&remote_state->lock);
    for (uint32_t i = 0; i < remote_state->snapshot.len; ++i) {
        const proc_entry_t *p = &remote_state->snapshot.entries[i];
        if (p->pid != pid) continue;
        if (entry) *entry = *p;
        if (name && size) snprintf(name, size, "%s", PROC_NAME(&remote_state->snapshot, p));
        found = 1; break;
    }
    pthread_mutex_unlock(&remote_state->lock);
    return found;
}
uint32_t query_remote_capabilities(void) { return remote_capabilities; }
void query_set_remote(const char *path)
{
    snprintf(remote_path, sizeof(remote_path), "%s", path ? path : "");
}
const char *query_remote_path(void) { return remote_path[0] ? remote_path : NULL; }
int query_exchange(const char *path, uint16_t type, const ipc_buf_t *request, ipc_buf_t *response)
{
    int fd = ipc_connect(path);
    if (fd < 0) return -1;
    ipc_header_t h;
    int result = ipc_hello(fd, NULL, NULL);
    if (!result) result = ipc_send(fd, type, 2, request);
    if (!result) result = ipc_receive(fd, &h, response, -1);
    if (!result && (h.type != (type | IPC_REPLY) || h.id != 2)) {
        if (h.type == IPC_ERROR) {
            uint32_t code = ipc_get_u32(response);
            errno = code == IPC_NOT_FOUND ? ESRCH : code == IPC_NOT_READY ? EAGAIN : code == IPC_BUSY ? EBUSY : EPROTO;
        } else errno = EPROTO;
        result = -1;
    }
    int saved = errno; close(fd); errno = saved; return result;
}
int query_client(const char *path, const char *request, int binary)
{
    uint16_t type = !strcmp(request, "status") ? IPC_STATUS : !strcmp(request, "snapshot") ? IPC_SNAPSHOT : !strcmp(request, "gpu") ? IPC_GPU : IPC_PROCESS;
    ipc_buf_t body = {0}, result = {0};
    if (type == IPC_PROCESS) ipc_put_u32(&body, (uint32_t)strtoul(request + 8, NULL, 10));
    int rc = query_exchange(path, type, &body, &result);
    ipc_free(&body);
    if (rc) { ipc_free(&result); return -1; }
    if (binary) {
        unsigned char header[IPC_HEADER_SIZE]; ipc_header_write(header, type | IPC_REPLY, 2, result.len);
        rc = fwrite(header, 1, sizeof(header), stdout) == sizeof(header) &&
             fwrite(result.data, 1, result.len, stdout) == result.len ? 0 : -1;
    } else if (type == IPC_GPU) {
        evemon_gpu_snapshot_t gpu = {0};
        rc = query_gpu_decode(&result, &gpu);
        if (!rc) {
            printf("devices=%u processes=%u sample_monotonic_ns=%" PRIu64 "\n", gpu.device_count, gpu.process_count, gpu.monotonic_ns);
            for (uint32_t i = 0; i < gpu.device_count; ++i)
                printf("%s\tGPU=%u%%\tVRAM=%" PRIu64 "/%" PRIu64 "\n", gpu.devices[i].name,
                       gpu.devices[i].utilization_percent, gpu.devices[i].memory_used_bytes, gpu.devices[i].memory_total_bytes);
        }
        free(gpu.devices); free(gpu.processes);
    } else {
        monitor_state_t state;
        if (monitor_state_init(&state)) { ipc_free(&result); return -1; }
        /* Count is part of the common status header, independent of row selection. */
        ipc_buf_t cursor = result; cursor.pos = 24;
        uint32_t count = ipc_get_u32(&cursor);
        rc = query_snapshot_decode(&result, &state, type == IPC_STATUS);
        if (!rc) {
            double sampled = state.sampled_at.tv_sec * 1000.0 + state.sampled_at.tv_nsec / 1e6;
            printf("ready=%d generation=%" PRIu64 " processes=%u interval_ms=%u sample_age_ms=%.1f\n",
                   state.snapshot_complete, state.generation, count, state.interval_ms,
                   state.generation ? ipc_now_ms() - sampled : 0.0);
            if (type == IPC_STATUS) {
                printf("cpus=%u uptime_seconds=%" PRIu64 " memory_total_bytes=%" PRIu64
                       " memory_available_bytes=%" PRIu64 " load=%.2f %.2f %.2f\n",
                       state.system.cpus, state.system.uptime_seconds, state.system.memory_total,
                       state.system.memory_available, state.system.load[0], state.system.load[1], state.system.load[2]);
            } else {
                puts("PID\tPPID\tCPU%\tRSS_KiB\tUSER\tNAME");
                for (uint32_t i = 0; i < state.snapshot.len; ++i) {
                    const proc_entry_t *p = &state.snapshot.entries[i];
                    printf("%d\t%d\t%.2f\t%" PRIu64 "\t%s\t%s\n", p->pid, p->ppid,
                           p->cpu_percent, p->mem_rss_kb, PROC_USER(&state.snapshot, p), PROC_NAME(&state.snapshot, p));
                }
            }
        }
        monitor_state_destroy(&state);
    }
    ipc_free(&result); return rc;
}

struct query_connection {
    monitor_state_t *state;
    char path[108];
    int fd, wake[2];
    pthread_t thread;
    uint32_t capabilities;
    query_host_t host;
    int active;
};
static void connected(query_connection_t *c, int value)
{
    pthread_mutex_lock(&c->state->lock);
    c->state->connected = value;
    pthread_mutex_unlock(&c->state->lock);
}
static int subscribe(query_connection_t *c)
{
    ipc_buf_t b = {0}; ipc_header_t h;
    ipc_put_u32(&b, 250);
    int result = ipc_send(c->fd, IPC_SUBSCRIBE, 2, &b);
    if (!result) result = ipc_receive(c->fd, &h, &b, c->wake[0]);
    if (!result) {
        unsigned interval = ipc_get_u32(&b);
        if (h.type != (IPC_SUBSCRIBE | IPC_REPLY) || h.id != 2 || interval != 250 || !ipc_done(&b)) {
            errno = EPROTO; result = -1;
        }
    }
    ipc_free(&b); return result;
}
static void *connection_worker(void *arg)
{
    query_connection_t *c = arg;
    pthread_setname_np(pthread_self(), "ev-client");
    int announced = 0;
    for (;;) {
        if (c->fd >= 0 && !subscribe(c)) {
            connected(c, 1);
            if (announced) { fputs("evemon: evemon reconnected\n", stderr); announced = 0; }
            for (;;) {
                ipc_buf_t b = {0}; ipc_header_t h;
                int rc = ipc_receive(c->fd, &h, &b, c->wake[0]);
                if (!rc && (h.type != (IPC_SNAPSHOT | IPC_REPLY) || h.id != 2)) { errno = EPROTO; rc = -1; }
                if (!rc) rc = query_snapshot_decode(&b, c->state, 0);
                ipc_free(&b);
                if (rc) break;
            }
        }
        if (c->fd >= 0) close(c->fd);
        c->fd = -1; connected(c, 0);
        struct pollfd wake = {c->wake[0], POLLIN, 0};
        if (poll(&wake, 1, 0) > 0) break;
        if (!announced) { fputs("evemon: evemon disconnected; reconnecting\n", stderr); announced = 1; }
        if (poll(&wake, 1, 500) > 0) break;
        c->fd = ipc_connect(c->path);
        if (c->fd >= 0 && ipc_hello(c->fd, NULL, NULL)) { close(c->fd); c->fd = -1; }
    }
    return NULL;
}
query_connection_t *query_connection_prepare(monitor_state_t *state, const char *path)
{
    if (strlen(path) >= sizeof(remote_path)) { errno = ENAMETOOLONG; return NULL; }
    query_connection_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->state = state; strcpy(c->path, path);
    c->fd = ipc_connect(path); c->wake[0] = c->wake[1] = -1;
    if (c->fd < 0 || ipc_hello(c->fd, &c->capabilities, &state->system.fdmon_backend) || pipe2(c->wake, O_CLOEXEC | O_NONBLOCK)) goto fail;
    if (query_host_read(path, &c->host)) goto fail;
    state->remote = 1; state->connected = 1;
    int rc = pthread_create(&c->thread, NULL, connection_worker, c);
    if (rc) { errno = rc; goto fail; }
    return c;
fail: {
    int error = errno;
    if (c->fd >= 0) close(c->fd);
    if (c->wake[0] >= 0) close(c->wake[0]);
    if (c->wake[1] >= 0) close(c->wake[1]);
    free(c); errno = error; return NULL;
    }
}
void query_connection_activate(query_connection_t *c)
{
    query_set_remote(c->path);
    remote_state = c->state;
    remote_capabilities = c->capabilities;
    query_host_activate(&c->host);
    c->active = 1;
}
query_connection_t *query_connection_start(monitor_state_t *state, const char *path)
{
    query_connection_t *c = query_connection_prepare(state, path);
    if (c) query_connection_activate(c);
    return c;
}
void query_connection_stop(query_connection_t *c)
{
    if (!c) return;
    char wake = 1; ssize_t n = write(c->wake[1], &wake, 1); (void)n;
    pthread_join(c->thread, NULL);
    close(c->wake[0]); close(c->wake[1]);
    if (c->active) {
        query_set_remote(NULL);
        remote_state = NULL;
        remote_capabilities = 0;
    }
    free(c);
}
