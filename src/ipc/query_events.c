#include "query_events.h"
#include "../platform/fdmon_internal.h"
#include "../plugins/event_bus.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>

#define EVENT_RING 256
#define OWNERS 16
#define WATCHES 128
#define UNION_MAX (OWNERS * WATCHES)
typedef struct { uint32_t kind, pid, value; } watch_t;
typedef struct { uint64_t owner; unsigned count; watch_t watches[WATCHES]; } owner_t;
typedef struct {
    uint64_t sequence;
    unsigned type;
    union { evemon_fd_write_payload_t write; evemon_exec_payload_t exec; } data;
} record_t;
struct query_events {
    fdmon_ctx_t *fdmon;
    pthread_mutex_t lock;
    uint64_t sequence;
    record_t ring[EVENT_RING];
    int write_subscription, exec_subscription;
    owner_t owners[OWNERS];
};
static void record_event(const evemon_event_t *event, void *opaque)
{
    query_events_t *e = opaque;
    if (!event->payload) return;
    pthread_mutex_lock(&e->lock);
    record_t *record = &e->ring[e->sequence % EVENT_RING];
    record->sequence = ++e->sequence; record->type = event->type;
    if (event->type == EVEMON_EVENT_FD_WRITE) {
        record->data.write = *(const evemon_fd_write_payload_t *)event->payload;
        if (record->data.write.len > sizeof(record->data.write.data)) record->data.write.len = sizeof(record->data.write.data);
    } else record->data.exec = *(const evemon_exec_payload_t *)event->payload;
    pthread_mutex_unlock(&e->lock);
}
query_events_t *query_events_start(fdmon_ctx_t *fdmon)
{
    query_events_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->fdmon = fdmon; pthread_mutex_init(&e->lock, NULL);
    e->write_subscription = evemon_event_bus_subscribe(EVEMON_EVENT_FD_WRITE, record_event, e);
    e->exec_subscription = evemon_event_bus_subscribe(EVEMON_EVENT_CHILD_EXEC, record_event, e);
    return e;
}
void query_events_stop(query_events_t *e)
{
    if (!e) return;
    evemon_event_bus_unsubscribe(e->write_subscription); evemon_event_bus_unsubscribe(e->exec_subscription);
    pthread_mutex_destroy(&e->lock); free(e);
}
static int same_watch(watch_t a, watch_t b)
{
    return a.kind == b.kind && a.pid == b.pid && (a.kind != 1 || a.value == b.value);
}
static unsigned collect_union(query_events_t *e, watch_t *out)
{
    unsigned count = 0;
    for (int i = 0; i < OWNERS; ++i) for (unsigned j = 0; j < e->owners[i].count; ++j) {
        watch_t watch = e->owners[i].watches[j]; unsigned k;
        for (k = 0; k < count; ++k) if (same_watch(out[k], watch)) break;
        if (k == count) out[count++] = watch;
        else if (watch.kind == 2) out[k].value |= watch.value;
    }
    return count;
}
static int contains(const watch_t *set, unsigned count, watch_t watch)
{
    for (unsigned i = 0; i < count; ++i) if (same_watch(set[i], watch)) return 1;
    return 0;
}
static int apply_union(fdmon_ctx_t *fdmon, const watch_t *old, unsigned old_count,
                       const watch_t *next, unsigned next_count)
{
    if (next_count && fdmon_write_enable(fdmon)) return -1;
    for (unsigned i = 0; i < old_count; ++i) if (!contains(next, next_count, old[i])) {
        watch_t w = old[i];
        if (w.kind == 1) fdmon_remove_pid_fd(fdmon, w.pid, w.value);
        else if (w.kind == 2) fdmon_unwatch_parent_fds(fdmon, w.pid);
        else fdmon_orphan_stdout_disable(fdmon);
    }
    /* Reapply all remaining roots: the existing eBPF unwatch flushes inherited entries. */
    for (unsigned i = 0; i < next_count; ++i) {
        watch_t w = next[i]; int rc;
        if (w.kind == 1) rc = fdmon_add_pid_fd(fdmon, w.pid, w.value);
        else if (w.kind == 2) rc = fdmon_watch_parent_fds(fdmon, w.pid, w.value);
        else rc = fdmon_orphan_stdout_enable(fdmon);
        if (rc) return -1;
    }
    return 0;
}
int query_controls_replace(query_events_t *e, uint64_t owner, ipc_buf_t *request)
{
    owner_t replacement = {.owner = owner};
    replacement.count = ipc_get_u32(request);
    if (replacement.count > WATCHES) return IPC_BAD_REQUEST;
    for (unsigned i = 0; i < replacement.count; ++i) {
        watch_t *w = &replacement.watches[i];
        w->kind = ipc_get_u32(request); w->pid = ipc_get_u32(request); w->value = ipc_get_u32(request);
        if (w->kind < 1 || w->kind > 3 || w->pid > INT_MAX ||
            (w->kind != 3 && !w->pid) || (w->kind == 1 && w->value > INT_MAX) ||
            (w->kind == 2 && (!w->value || w->value > 3)) ||
            (w->kind == 3 && (w->pid || w->value))) return IPC_BAD_REQUEST;
    }
    if (!ipc_done(request)) return IPC_BAD_REQUEST;
    if (replacement.count && (!e->fdmon || fdmon_active_backend(e->fdmon) != FDMON_BACKEND_EBPF)) return IPC_UNSUPPORTED;
    int slot = -1;
    for (int i = 0; i < OWNERS; ++i) if (e->owners[i].owner == owner) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < OWNERS; ++i) if (!e->owners[i].owner) { slot = i; break; }
    if (slot < 0) return IPC_BUSY;
    watch_t before[UNION_MAX], after[UNION_MAX];
    unsigned before_count = collect_union(e, before);
    owner_t previous = e->owners[slot]; e->owners[slot] = replacement;
    unsigned after_count = collect_union(e, after);
    if (apply_union(e->fdmon, before, before_count, after, after_count)) {
        e->owners[slot] = previous;
        (void)apply_union(e->fdmon, after, after_count, before, before_count);
        return IPC_INTERNAL;
    }
    if (!replacement.count) memset(&e->owners[slot], 0, sizeof(e->owners[slot]));
    return 0;
}
void query_controls_drop(query_events_t *e, uint64_t owner)
{
    int found = 0;
    for (int i = 0; i < OWNERS; ++i) if (e->owners[i].owner == owner) { found = 1; break; }
    if (!found) return;
    ipc_buf_t empty = {0}; ipc_put_u32(&empty, 0);
    (void)query_controls_replace(e, owner, &empty); ipc_free(&empty);
}
uint64_t query_events_latest(query_events_t *e)
{
    pthread_mutex_lock(&e->lock); uint64_t sequence = e->sequence; pthread_mutex_unlock(&e->lock); return sequence;
}
void query_events_encode(query_events_t *e, uint64_t *cursor, ipc_buf_t *out)
{
    pthread_mutex_lock(&e->lock);
    uint64_t newest = e->sequence, oldest = newest > EVENT_RING ? newest - EVENT_RING : 0;
    uint64_t dropped = *cursor < oldest ? oldest - *cursor : 0;
    if (*cursor < oldest) *cursor = oldest;
    if (*cursor > newest) *cursor = newest;
    ipc_put_u64(out, dropped); ipc_put_u32(out, newest - *cursor);
    for (uint64_t i = *cursor; i < newest; ++i) {
        const record_t *r = &e->ring[i % EVENT_RING];
        if (r->type == EVEMON_EVENT_FD_WRITE) {
            const evemon_fd_write_payload_t *w = &r->data.write;
            ipc_put_u32(out, 1); ipc_put_u32(out, w->pid); ipc_put_u32(out, w->tgid);
            ipc_put_u32(out, w->fd); ipc_put_u32(out, w->truncated); ipc_put_u32(out, w->len);
            ipc_put_bytes(out, w->data, w->len);
        } else {
            ipc_put_u32(out, 2); ipc_put_u32(out, r->data.exec.pid); ipc_put_u32(out, r->data.exec.ppid);
            ipc_put_string(out, r->data.exec.path);
        }
    }
    *cursor = newest;
    pthread_mutex_unlock(&e->lock);
}
int query_events_dispatch(ipc_buf_t *in)
{
    uint64_t dropped = ipc_get_u64(in); uint32_t count = ipc_get_u32(in);
    if (count > EVENT_RING) { errno = EPROTO; return -1; }
    /* Decode the complete frame before publishing any part of it. */
    record_t *records = calloc(count ? count : 1, sizeof(*records));
    if (!records) return -1;
    for (uint32_t i = 0; i < count && !in->error; ++i) {
        record_t *r = &records[i]; r->type = ipc_get_u32(in);
        if (r->type == 1) {
            evemon_fd_write_payload_t *w = &r->data.write;
            w->pid = ipc_get_u32(in); w->tgid = ipc_get_u32(in); w->fd = ipc_get_u32(in);
            w->truncated = ipc_get_u32(in); w->len = ipc_get_u32(in);
            if (w->len > sizeof(w->data) || w->len > in->len - in->pos) in->error = EPROTO;
            else { memcpy(w->data, in->data + in->pos, w->len); in->pos += w->len; }
        } else if (r->type == 2) {
            r->data.exec.pid = ipc_get_u32(in); r->data.exec.ppid = ipc_get_u32(in);
            ipc_get_fixed(in, r->data.exec.path, sizeof(r->data.exec.path));
        } else in->error = EPROTO;
    }
    int valid = ipc_done(in);
    if (valid) {
        if (dropped) fprintf(stderr, "evemon: evemon event stream lost %llu events\n", (unsigned long long)dropped);
        for (uint32_t i = 0; i < count; ++i) {
            record_t *r = &records[i];
            evemon_event_t event = {0};
            if (r->type == 1) { event.type = EVEMON_EVENT_FD_WRITE; event.payload = &r->data.write; }
            else { event.type = EVEMON_EVENT_CHILD_EXEC; event.payload = &r->data.exec; }
            evemon_event_bus_publish(&event);
        }
    }
    free(records);
    if (!valid) { errno = EPROTO; return -1; }
    return 0;
}
