#include "query.h"
#include <errno.h>
#include <limits.h>
#include <float.h>
#include <math.h>

static void put_system(ipc_buf_t *b, const monitor_system_t *s)
{
    ipc_put_u64(b, s->uptime_seconds); ipc_put_u64(b, s->memory_total);
    ipc_put_u64(b, s->memory_free); ipc_put_u64(b, s->memory_available);
    for (int i = 0; i < 3; ++i) ipc_put_f64(b, s->load[i]);
    ipc_put_u32(b, s->cpus); ipc_put_u32(b, s->users); ipc_put_u32(b, s->fdmon_backend);
}
static void get_system(ipc_buf_t *b, monitor_system_t *s)
{
    s->uptime_seconds = ipc_get_u64(b); s->memory_total = ipc_get_u64(b);
    s->memory_free = ipc_get_u64(b); s->memory_available = ipc_get_u64(b);
    for (int i = 0; i < 3; ++i) s->load[i] = ipc_get_f64(b);
    s->cpus = ipc_get_u32(b); s->users = ipc_get_u32(b); s->fdmon_backend = ipc_get_u32(b);
}
static void put_steam(ipc_buf_t *b, const steam_info_t *s)
{
    ipc_put_u32(b, s != NULL);
    if (!s) return;
    ipc_put_u32(b, s->is_steam); ipc_put_u32(b, s->is_wine);
    ipc_put_string(b, s->app_id); ipc_put_string(b, s->game_name);
    ipc_put_string(b, s->proton_version); ipc_put_string(b, s->proton_dist);
    ipc_put_string(b, s->compat_data); ipc_put_string(b, s->game_dir);
    ipc_put_string(b, s->runtime_layer); ipc_put_string(b, s->display_label);
}
static steam_info_t *get_steam(ipc_buf_t *b)
{
    uint32_t present = ipc_get_u32(b);
    if (present > 1) b->error = EPROTO;
    if (!present || b->error) return NULL;
    steam_info_t *s = calloc(1, sizeof(*s));
    if (!s) { b->error = ENOMEM; return NULL; }
    s->is_steam = ipc_get_u32(b); s->is_wine = ipc_get_u32(b);
#define STR(field) ipc_get_fixed(b, s->field, sizeof(s->field))
    STR(app_id); STR(game_name); STR(proton_version); STR(proton_dist);
    STR(compat_data); STR(game_dir); STR(runtime_layer); STR(display_label);
#undef STR
    return s;
}
static void put_row(ipc_buf_t *b, const proc_snapshot_t *snap, const proc_entry_t *p)
{
    ipc_put_u32(b, p->pid); ipc_put_u32(b, p->ppid);
    ipc_put_u64(b, p->mem_rss_kb); ipc_put_u64(b, p->cpu_ticks); ipc_put_u64(b, p->start_time);
    ipc_put_u64(b, p->start_ticks); ipc_put_u64(b, p->evemon_instance);
    ipc_put_u64(b, p->io_read_bytes); ipc_put_u64(b, p->io_write_bytes);
    ipc_put_u64(b, p->net_send_bytes); ipc_put_u64(b, p->net_recv_bytes);
    ipc_put_f64(b, p->cpu_percent); ipc_put_f64(b, p->io_read_rate); ipc_put_f64(b, p->io_write_rate);
    ipc_put_u32(b, p->io_history_len); ipc_put_u32(b, p->flags);
    for (int i = 0; i < IO_HISTORY_LEN; ++i) ipc_put_f64(b, p->io_history[i]);
    ipc_put_string(b, PROC_NAME(snap, p)); ipc_put_string(b, PROC_CMDLINE(snap, p));
    ipc_put_string(b, PROC_USER(snap, p)); ipc_put_string(b, PROC_CWD(snap, p));
    ipc_put_string(b, PROC_CONTAINER(snap, p)); ipc_put_string(b, PROC_SERVICE(snap, p));
    put_steam(b, p->extra_idx < snap->extras_len ? snap->extras[p->extra_idx].steam : NULL);
}
static float get_float(ipc_buf_t *b)
{
    double value = ipc_get_f64(b);
    if (fabs(value) > FLT_MAX) { b->error = EPROTO; return 0; }
    return (float)value;
}
static void get_row(ipc_buf_t *b, proc_snapshot_t *snap, proc_entry_t *p)
{
    uint32_t pid = ipc_get_u32(b), ppid = ipc_get_u32(b);
    if (!pid || pid > INT_MAX || ppid > INT_MAX) b->error = EPROTO;
    p->pid = pid; p->ppid = ppid;
    p->mem_rss_kb = ipc_get_u64(b); p->cpu_ticks = ipc_get_u64(b); p->start_time = ipc_get_u64(b);
    p->start_ticks = ipc_get_u64(b); p->evemon_instance = ipc_get_u64(b);
    p->io_read_bytes = ipc_get_u64(b); p->io_write_bytes = ipc_get_u64(b);
    p->net_send_bytes = ipc_get_u64(b); p->net_recv_bytes = ipc_get_u64(b);
    p->cpu_percent = get_float(b); p->io_read_rate = get_float(b); p->io_write_rate = get_float(b);
    uint32_t history = ipc_get_u32(b), flags = ipc_get_u32(b);
    if (history > IO_HISTORY_LEN || flags > UINT16_MAX) b->error = EPROTO;
    p->io_history_len = history; p->flags = flags;
    for (int i = 0; i < IO_HISTORY_LEN; ++i) p->io_history[i] = get_float(b);
#define STR(field) do { \
    char *s = ipc_get_string(b); \
    if (s) { size_t n = strlen(s); \
        p->field##_len = n > UINT16_MAX ? UINT16_MAX : n; p->field##_off = strtab_push(&snap->strtab, s); \
        if (n && !p->field##_off) b->error = ENOMEM; \
        free(s); } \
} while (0)
    STR(name); STR(cmd); STR(user); STR(cwd); STR(container); STR(service);
#undef STR
    p->extra_idx = UINT32_MAX;
    steam_info_t *steam = get_steam(b);
    if (steam) {
        p->extra_idx = snap->extras_len;
        snap->extras[snap->extras_len++].steam = steam;
    }
}
/* Deep-copy under the producer lock; encode outside it, including Steam extras. */
static int copy_snapshot(const proc_snapshot_t *source, proc_snapshot_t *copy, pid_t pid)
{
    if (!source->len) return 0;
    copy->entries = calloc(pid ? 1 : source->len, sizeof(*copy->entries));
    copy->extras = calloc(pid ? 1 : source->len, sizeof(*copy->extras));
    copy->strtab.buf = malloc(source->strtab.len);
    if (!copy->entries || !copy->extras || !copy->strtab.buf) return -1;
    memcpy(copy->strtab.buf, source->strtab.buf, source->strtab.len);
    copy->strtab.len = source->strtab.len;
    for (uint32_t i = 0; i < source->len; ++i) {
        const proc_entry_t *entry = &source->entries[i];
        if (pid && entry->pid != pid) continue;
        proc_entry_t *target = &copy->entries[copy->len++]; *target = *entry;
        target->extra_idx = UINT32_MAX;
        if (entry->extra_idx < source->extras_len && source->extras[entry->extra_idx].steam) {
            steam_info_t *s = malloc(sizeof(*s));
            if (!s) return -1;
            *s = *source->extras[entry->extra_idx].steam;
            target->extra_idx = copy->extras_len;
            copy->extras[copy->extras_len++].steam = s;
        }
        if (pid) break;
    }
    return 0;
}
int query_snapshot_encode(monitor_state_t *state, uint16_t type, pid_t pid, ipc_buf_t *out)
{
    proc_snapshot_t copy = {0};
    pthread_mutex_lock(&state->lock);
    uint64_t generation = state->generation;
    struct timespec sampled = state->sampled_at;
    unsigned interval = state->interval_ms;
    int ready = state->snapshot_complete;
    uint32_t count = state->snapshot.len;
    monitor_system_t system = state->system;
    uint64_t instance = state->evemon_instance;
    int rc = type == IPC_STATUS ? 0 : copy_snapshot(&state->snapshot, &copy, pid);
    pthread_mutex_unlock(&state->lock);
    if (rc) { proc_snapshot_free(&copy); return IPC_INTERNAL; }
    if (pid && !copy.len) { proc_snapshot_free(&copy); return ready ? IPC_NOT_FOUND : IPC_NOT_READY; }
    ipc_put_u64(out, generation);
    ipc_put_u64(out, (uint64_t)sampled.tv_sec * 1000000000 + sampled.tv_nsec);
    ipc_put_u32(out, interval); ipc_put_u32(out, ready); ipc_put_u32(out, count);
    put_system(out, &system);
    struct timespec sent; clock_gettime(CLOCK_MONOTONIC, &sent);
    ipc_put_u64(out, (uint64_t)sent.tv_sec * 1000000000 + sent.tv_nsec);
    if (type != IPC_STATUS) {
        ipc_put_u32(out, copy.len);
        for (uint32_t i = 0; i < copy.len && !out->error; ++i) {
            copy.entries[i].evemon_instance = instance;
            put_row(out, &copy, &copy.entries[i]);
        }
    }
    proc_snapshot_free(&copy);
    return out->error ? IPC_INTERNAL : 0;
}
int query_snapshot_decode(ipc_buf_t *b, monitor_state_t *state, int status_only)
{
    uint64_t generation = ipc_get_u64(b), ns = ipc_get_u64(b);
    unsigned interval = ipc_get_u32(b), ready = ipc_get_u32(b);
    uint32_t total = ipc_get_u32(b);
    monitor_system_t system = {0}; get_system(b, &system);
    uint64_t sent = ipc_get_u64(b);
    if (sent < ns) b->error = EPROTO;
    proc_snapshot_t next = {0};
    if (!interval || interval > 3600000 || ready > 1) b->error = EPROTO;
    if (!status_only) {
        uint32_t count = ipc_get_u32(b);
        /* Fixed fields, empty strings and absent Steam metadata take 300 bytes. */
        if (count > total || count > (b->len - b->pos) / 300) b->error = EPROTO;
        if (!b->error) {
            next.entries = calloc(count ? count : 1, sizeof(*next.entries));
            next.extras = calloc(count ? count : 1, sizeof(*next.extras));
            if (!next.entries || !next.extras || strtab_init(&next.strtab, 4096)) b->error = ENOMEM;
            next.cap = next.extras_cap = count;
            while (next.len < count && !b->error) get_row(b, &next, &next.entries[next.len++]);
        }
    }
    if (!ipc_done(b)) { proc_snapshot_free(&next); errno = b->error ? b->error : EPROTO; return -1; }
    pthread_mutex_lock(&state->lock);
    if (!status_only) { proc_snapshot_free(&state->snapshot); state->snapshot = next; }
    /* Monotonic clocks on different machines have unrelated origins. */
    struct timespec received; clock_gettime(CLOCK_MONOTONIC, &received);
    uint64_t local_ns = (uint64_t)received.tv_sec * 1000000000 + received.tv_nsec;
    uint64_t age = sent - ns;
    uint64_t local_sample = ns && age <= local_ns ? local_ns - age : 0;
    state->generation = generation;
    state->sampled_at = (struct timespec){local_sample / 1000000000, local_sample % 1000000000};
    state->interval_ms = interval; state->snapshot_complete = ready; state->system = system;
    state->connected = 1;
    pthread_cond_broadcast(&state->updated);
    pthread_mutex_unlock(&state->lock);
    return 0;
}
