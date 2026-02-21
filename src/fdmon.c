/*
 * fdmon.c – Core fd-monitor: ring buffer, PID-group tracking, backend dispatch.
 *
 * This file contains:
 *   • The lock-free(ish) ring buffer for events.
 *   • PID-group tracking (root pid + descendant enumeration via /proc).
 *   • The fanotify backend (always available on Linux ≥ 5.1).
 *   • Backend auto-selection and the public fdmon_* API.
 *
 * The eBPF backend lives in fdmon_ebpf.c to keep things modular.
 */

#include "fdmon_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/fanotify.h>
#include <sys/stat.h>

/* ── forward declarations for eBPF backend (fdmon_ebpf.c) ───── */

/* Returns 0 if eBPF backend initialised successfully, -1 otherwise. */
int  fdmon_ebpf_init(struct fdmon_ctx *ctx);
void fdmon_ebpf_destroy(struct fdmon_ctx *ctx);

/* pid_set_t, ring_t, struct fdmon_ctx, and submit_event()
 * are now defined/declared in fdmon_internal.h */

/* ── PID set operations ──────────────────────────────────────── */

static void pid_set_init(pid_set_t *s)
{
    s->pids     = NULL;
    s->count    = 0;
    s->capacity = 0;
}

static void pid_set_free(pid_set_t *s)
{
    free(s->pids);
    s->pids     = NULL;
    s->count    = 0;
    s->capacity = 0;
}

/* Binary search: returns index where pid is or should be inserted. */
static size_t pid_set_lower(const pid_set_t *s, pid_t pid)
{
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->pids[mid] < pid) lo = mid + 1;
        else                    hi = mid;
    }
    return lo;
}

static int pid_set_contains(const pid_set_t *s, pid_t pid)
{
    size_t i = pid_set_lower(s, pid);
    return i < s->count && s->pids[i] == pid;
}

static int pid_set_add(pid_set_t *s, pid_t pid)
{
    size_t i = pid_set_lower(s, pid);
    if (i < s->count && s->pids[i] == pid)
        return 0;  /* already present */

    if (s->count >= s->capacity) {
        size_t newcap = s->capacity ? s->capacity * 2 : 128;
        pid_t *tmp = realloc(s->pids, newcap * sizeof(pid_t));
        if (!tmp) return -1;
        s->pids     = tmp;
        s->capacity = newcap;
    }

    /* Shift right to make room. */
    memmove(&s->pids[i + 1], &s->pids[i], (s->count - i) * sizeof(pid_t));
    s->pids[i] = pid;
    s->count++;
    return 1;  /* newly added */
}

static int pid_set_remove(pid_set_t *s, pid_t pid)
{
    size_t i = pid_set_lower(s, pid);
    if (i >= s->count || s->pids[i] != pid)
        return 0;

    memmove(&s->pids[i], &s->pids[i + 1], (s->count - i - 1) * sizeof(pid_t));
    s->count--;
    return 1;
}

/* ── ring buffer ─────────────────────────────────────────────── */

static void ring_init(ring_t *r)
{
    memset(r->slots, 0, sizeof(r->slots));
    r->head    = 0;
    r->tail    = 0;
    r->dropped = 0;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
}

static void ring_destroy(ring_t *r)
{
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
}

/* Push an event (caller holds lock or is sole writer). */
static int ring_push(ring_t *r, const fdmon_event_t *ev)
{
    pthread_mutex_lock(&r->lock);
    size_t next = (r->head + 1) & RING_MASK;
    if (next == r->tail) {
        /* Full – drop oldest. */
        r->tail = (r->tail + 1) & RING_MASK;
        r->dropped++;
    }
    r->slots[r->head & RING_MASK] = *ev;
    r->head = next;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
    return 0;
}

/* Pop an event.  Returns 1 if got one, 0 if empty. */
static int ring_pop(ring_t *r, fdmon_event_t *ev)
{
    pthread_mutex_lock(&r->lock);
    if (r->head == r->tail) {
        pthread_mutex_unlock(&r->lock);
        return 0;
    }
    *ev = r->slots[r->tail & RING_MASK];
    r->tail = (r->tail + 1) & RING_MASK;
    pthread_mutex_unlock(&r->lock);
    return 1;
}

/* Blocking pop with timeout.  Returns 1/0/-1. */
static int ring_pop_wait(ring_t *r, fdmon_event_t *ev, int timeout_ms)
{
    pthread_mutex_lock(&r->lock);

    while (r->head == r->tail) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&r->lock);
            return 0;
        }
        if (timeout_ms < 0) {
            pthread_cond_wait(&r->cond, &r->lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            int rc = pthread_cond_timedwait(&r->cond, &r->lock, &ts);
            if (rc == ETIMEDOUT && r->head == r->tail) {
                pthread_mutex_unlock(&r->lock);
                return 0;
            }
        }
    }

    *ev = r->slots[r->tail & RING_MASK];
    r->tail = (r->tail + 1) & RING_MASK;
    pthread_mutex_unlock(&r->lock);
    return 1;
}

static size_t ring_count(const ring_t *r)
{
    return (r->head - r->tail) & RING_MASK;
}

/* struct fdmon_ctx is defined in fdmon_internal.h */

/* ── descendant enumeration via /proc ────────────────────────── */

/*
 * Walk /proc to find all descendants of `root_pid` and add them to
 * the watched set.  This is O(all processes) but runs infrequently.
 */
static void enumerate_descendants(pid_set_t *watched, pid_t root_pid)
{
    /*
     * Strategy: read all (pid, ppid) pairs from /proc, then do a
     * transitive-closure walk starting from root_pid.
     *
     * We store (pid, ppid) in a flat array, build a parent→child
     * mapping with a simple scan, then BFS from root_pid.
     */

    struct pp { pid_t pid, ppid; };
    struct pp *procs = NULL;
    size_t nprocs = 0, cap = 512;

    procs = malloc(cap * sizeof(struct pp));
    if (!procs) return;

    DIR *dp = opendir("/proc");
    if (!dp) { free(procs); return; }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_type != DT_DIR) continue;
        /* Quick numeric check */
        const char *s = de->d_name;
        pid_t pid = 0;
        while (*s >= '0' && *s <= '9') { pid = pid * 10 + (*s - '0'); s++; }
        if (*s != '\0' || pid == 0) continue;

        /* Read ppid from /proc/<pid>/stat */
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char buf[512];
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
        fclose(f);

        const char *cp = strrchr(buf, ')');
        if (!cp) continue;
        cp++;

        pid_t ppid = 0;
        sscanf(cp, " %*c %d", &ppid);

        if (nprocs >= cap) {
            cap *= 2;
            struct pp *tmp = realloc(procs, cap * sizeof(struct pp));
            if (!tmp) break;
            procs = tmp;
        }
        procs[nprocs].pid  = pid;
        procs[nprocs].ppid = ppid;
        nprocs++;
    }
    closedir(dp);

    /* BFS from root_pid using a simple queue. */
    pid_t *queue = malloc(nprocs * sizeof(pid_t));
    if (!queue) { free(procs); return; }

    size_t qhead = 0, qtail = 0;
    queue[qtail++] = root_pid;
    pid_set_add(watched, root_pid);

    while (qhead < qtail) {
        pid_t parent = queue[qhead++];
        for (size_t i = 0; i < nprocs; i++) {
            if (procs[i].ppid == parent && !pid_set_contains(watched, procs[i].pid)) {
                pid_set_add(watched, procs[i].pid);
                if (qtail < nprocs)
                    queue[qtail++] = procs[i].pid;
            }
        }
    }

    free(queue);
    free(procs);
}

/* ── helpers ─────────────────────────────────────────────────── */

static uint64_t now_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Resolve /proc/<pid>/fd/<fd> to a path (best effort). */
static void resolve_fd_path(pid_t pid, int fd, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/fd/%d", pid, fd);
    ssize_t n = readlink(link, buf, bufsz - 1);
    if (n > 0) buf[n] = '\0';
}

/* Check if a PID is in the watched set (must hold group_lock). */
static int is_watched(fdmon_ctx_t *ctx, pid_t pid)
{
    return pid_set_contains(&ctx->watched, pid);
}

/* Submit an event if the PID is in the watched set.
 * Non-static: also called from fdmon_ebpf.c. */
void submit_event(fdmon_ctx_t *ctx, fdmon_event_type_t type,
                  pid_t pid, pid_t tgid, int fd, const char *path)
{
    __atomic_add_fetch(&ctx->stat_captured, 1, __ATOMIC_RELAXED);

    pthread_mutex_lock(&ctx->group_lock);
    int dominated = is_watched(ctx, tgid) || is_watched(ctx, pid);
    pthread_mutex_unlock(&ctx->group_lock);

    if (!dominated)
        return;

    fdmon_event_t ev;
    ev.type      = type;
    ev.pid       = pid;
    ev.tgid      = tgid;
    ev.fd        = fd;
    ev.timestamp = now_monotonic_ns();
    ev.path[0]   = '\0';

    if (path && path[0])
        snprintf(ev.path, sizeof(ev.path), "%s", path);
    else if (type == FDMON_OPEN && fd >= 0)
        resolve_fd_path(pid, fd, ev.path, sizeof(ev.path));

    ring_push(&ctx->ring, &ev);
    __atomic_add_fetch(&ctx->stat_delivered, 1, __ATOMIC_RELAXED);
}

/* ═══════════════════════════════════════════════════════════════
 *  FANOTIFY BACKEND
 * ═══════════════════════════════════════════════════════════════ */

/*
 * fanotify limitations vs eBPF:
 *   - Only sees filesystem opens (not sockets, pipes, eventfds).
 *   - FAN_OPEN/FAN_CLOSE_WRITE/FAN_CLOSE_NOWRITE are informational
 *     (don't need FAN_CLASS_PRE_CONTENT).
 *   - The event metadata gives us pid + fd, but the fd points to the
 *     opened file in *our* process's fd table (via the fanotify fd),
 *     so we readlink /proc/self/fd/<metadata.fd> to get the path.
 *   - No direct fd-number from the target process; we get the event
 *     *after* the syscall returns.
 *
 * We mark the filesystem ("/") so we see opens system-wide, then
 * filter by our watched PID set.
 */

static void *fanotify_reader_thread(void *arg)
{
    fdmon_ctx_t *ctx = arg;
    char buf[8192] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));

    while (ctx->fan_running) {
        struct pollfd pfd = { .fd = ctx->fan_fd, .events = POLLIN };
        int rc = poll(&pfd, 1, 200);  /* 200 ms timeout for shutdown check */
        if (rc <= 0) continue;

        ssize_t len = read(ctx->fan_fd, buf, sizeof(buf));
        if (len <= 0) continue;

        struct fanotify_event_metadata *meta = (void *)buf;
        while (FAN_EVENT_OK(meta, len)) {
            if (meta->vers != FANOTIFY_METADATA_VERSION)
                break;

            if (meta->fd >= 0) {
                /* Determine event type */
                fdmon_event_type_t type;
                if (meta->mask & FAN_OPEN)
                    type = FDMON_OPEN;
                else if (meta->mask & (FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE))
                    type = FDMON_CLOSE;
                else {
                    close(meta->fd);
                    meta = FAN_EVENT_NEXT(meta, len);
                    continue;
                }

                /* Resolve path from fanotify's fd in *our* process */
                char path[1024] = "";
                char fdlink[64];
                snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", meta->fd);
                ssize_t plen = readlink(fdlink, path, sizeof(path) - 1);
                if (plen > 0) path[plen] = '\0';

                /* Submit; PID filtering happens inside. For fanotify
                 * we don't get a separate tgid, use pid for both. */
                submit_event(ctx, type, meta->pid, meta->pid,
                             -1 /* fd# in target unknown */, path);

                close(meta->fd);
            }

            meta = FAN_EVENT_NEXT(meta, len);
        }
    }

    return NULL;
}

static int fanotify_backend_init(fdmon_ctx_t *ctx)
{
    /* FAN_CLASS_NOTIF = informational, no permission decisions.
     * FAN_UNLIMITED_QUEUE avoids kernel-side drops on busy systems. */
    int fan_fd = fanotify_init(
        FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK | FAN_UNLIMITED_QUEUE,
        O_RDONLY | O_CLOEXEC | O_LARGEFILE);

    if (fan_fd < 0)
        return -1;

    /* Mark the entire root filesystem. */
    if (fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
                      FAN_OPEN | FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE,
                      AT_FDCWD, "/") < 0) {
        close(fan_fd);
        return -1;
    }

    ctx->fan_fd      = fan_fd;
    ctx->fan_running = 1;

    if (pthread_create(&ctx->fan_thread, NULL,
                       fanotify_reader_thread, ctx) != 0) {
        close(fan_fd);
        ctx->fan_fd = -1;
        return -1;
    }

    ctx->backend = FDMON_BACKEND_FANOTIFY;
    return 0;
}

static void fanotify_backend_destroy(fdmon_ctx_t *ctx)
{
    if (ctx->fan_fd < 0) return;

    ctx->fan_running = 0;
    pthread_join(ctx->fan_thread, NULL);
    close(ctx->fan_fd);
    ctx->fan_fd = -1;
}

/* ═══════════════════════════════════════════════════════════════
 *  PUBLIC API
 * ═══════════════════════════════════════════════════════════════ */

const char *fdmon_backend_name(fdmon_backend_t backend)
{
    switch (backend) {
    case FDMON_BACKEND_FANOTIFY: return "fanotify";
    case FDMON_BACKEND_EBPF:     return "eBPF";
    default:                     return "none";
    }
}

fdmon_ctx_t *fdmon_create(fdmon_backend_t backend)
{
    fdmon_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->fan_fd    = -1;
    ctx->ebpf_state = NULL;
    pthread_mutex_init(&ctx->group_lock, NULL);
    pid_set_init(&ctx->watched);
    ring_init(&ctx->ring);

    int ok = -1;

    if (backend == FDMON_BACKEND_AUTO || backend == FDMON_BACKEND_EBPF) {
        ok = fdmon_ebpf_init(ctx);
        if (ok == 0)
            ctx->backend = FDMON_BACKEND_EBPF;
    }

    if (ok != 0 && (backend == FDMON_BACKEND_AUTO ||
                    backend == FDMON_BACKEND_FANOTIFY)) {
        ok = fanotify_backend_init(ctx);
    }

    if (ok != 0) {
        pthread_mutex_destroy(&ctx->group_lock);
        pid_set_free(&ctx->watched);
        ring_destroy(&ctx->ring);
        free(ctx);
        errno = ENOTSUP;
        return NULL;
    }

    return ctx;
}

void fdmon_destroy(fdmon_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->backend == FDMON_BACKEND_EBPF)
        fdmon_ebpf_destroy(ctx);
    else if (ctx->backend == FDMON_BACKEND_FANOTIFY)
        fanotify_backend_destroy(ctx);

    pthread_mutex_destroy(&ctx->group_lock);
    pid_set_free(&ctx->watched);
    ring_destroy(&ctx->ring);
    free(ctx);
}

fdmon_backend_t fdmon_active_backend(const fdmon_ctx_t *ctx)
{
    return ctx ? ctx->backend : FDMON_BACKEND_AUTO;
}

int fdmon_watch_pid(fdmon_ctx_t *ctx, pid_t pid)
{
    if (!ctx || pid <= 0) { errno = EINVAL; return -1; }

    pthread_mutex_lock(&ctx->group_lock);

    if (ctx->root_count >= MAX_ROOTS) {
        pthread_mutex_unlock(&ctx->group_lock);
        errno = ENOMEM;
        return -1;
    }

    /* Check for duplicate root. */
    for (size_t i = 0; i < ctx->root_count; i++) {
        if (ctx->roots[i] == pid) {
            pthread_mutex_unlock(&ctx->group_lock);
            return 0;
        }
    }

    ctx->roots[ctx->root_count++] = pid;

    /* Enumerate all current descendants and add to watched set. */
    enumerate_descendants(&ctx->watched, pid);

    pthread_mutex_unlock(&ctx->group_lock);
    return 0;
}

int fdmon_unwatch_pid(fdmon_ctx_t *ctx, pid_t pid)
{
    if (!ctx || pid <= 0) { errno = EINVAL; return -1; }

    pthread_mutex_lock(&ctx->group_lock);

    /* Find and remove from roots. */
    int found = 0;
    for (size_t i = 0; i < ctx->root_count; i++) {
        if (ctx->roots[i] == pid) {
            ctx->roots[i] = ctx->roots[--ctx->root_count];
            found = 1;
            break;
        }
    }

    if (!found) {
        pthread_mutex_unlock(&ctx->group_lock);
        errno = ESRCH;
        return -1;
    }

    /* Rebuild the watched set from remaining roots. */
    pid_set_free(&ctx->watched);
    pid_set_init(&ctx->watched);
    for (size_t i = 0; i < ctx->root_count; i++)
        enumerate_descendants(&ctx->watched, ctx->roots[i]);

    pthread_mutex_unlock(&ctx->group_lock);
    return 0;
}

void fdmon_refresh_descendants(fdmon_ctx_t *ctx)
{
    if (!ctx) return;

    pthread_mutex_lock(&ctx->group_lock);

    /* Rebuild from scratch for simplicity and correctness. */
    pid_set_free(&ctx->watched);
    pid_set_init(&ctx->watched);
    for (size_t i = 0; i < ctx->root_count; i++)
        enumerate_descendants(&ctx->watched, ctx->roots[i]);

    pthread_mutex_unlock(&ctx->group_lock);
}

int fdmon_poll(fdmon_ctx_t *ctx, fdmon_event_t *ev, int timeout_ms)
{
    if (!ctx || !ev) { errno = EINVAL; return -1; }
    return ring_pop_wait(&ctx->ring, ev, timeout_ms);
}

size_t fdmon_drain(fdmon_ctx_t *ctx, fdmon_event_t *buf, size_t max_events)
{
    if (!ctx || !buf) return 0;
    size_t n = 0;
    while (n < max_events && ring_pop(&ctx->ring, &buf[n]))
        n++;
    return n;
}

size_t fdmon_pending(const fdmon_ctx_t *ctx)
{
    return ctx ? ring_count(&ctx->ring) : 0;
}

void fdmon_get_stats(const fdmon_ctx_t *ctx, fdmon_stats_t *stats)
{
    if (!ctx || !stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->events_captured  = __atomic_load_n(&ctx->stat_captured,  __ATOMIC_RELAXED);
    stats->events_delivered = __atomic_load_n(&ctx->stat_delivered, __ATOMIC_RELAXED);
    stats->events_dropped   = ctx->ring.dropped;
    stats->watched_pids     = ctx->root_count;
    stats->tracked_descendants = ctx->watched.count;
}
