/*
 * fdmon.h – File-descriptor event monitoring abstraction.
 *
 * Provides a unified interface for tracking file-descriptor lifecycle
 * events (open, close) across a process group (a root PID and all its
 * descendants).  Two backends are supported:
 *
 *   • fanotify  – available on any kernel ≥ 5.1 (FAN_OPEN_PERM not
 *                 required; we use informational FAN_OPEN / FAN_CLOSE).
 *                 Filesystem-scoped: sees opens on mounted filesystems.
 *                 Cannot see sockets, pipes, or anonymous fds.
 *
 *   • eBPF      – requires CAP_BPF (or root) and libbpf at runtime.
 *                 Attaches tracepoints on sys_enter_openat / sys_exit_openat
 *                 and sys_enter_close.  Sees *all* fd operations
 *                 including sockets, pipes, eventfds, etc.
 *
 * The library auto-selects the best available backend at init time
 * (eBPF preferred, fanotify fallback) unless the caller forces one.
 *
 * Thread safety: one fdmon_ctx per monitoring session.  The context is
 * internally locked; events can be produced by the kernel callback
 * thread and consumed from any other thread.
 *
 * Typical usage:
 *
 *     fdmon_ctx_t *ctx = fdmon_create(NULL);   // auto-select backend
 *     fdmon_watch_pid(ctx, some_pid);          // watch a process group
 *     // ...
 *     fdmon_event_t ev;
 *     while (fdmon_poll(ctx, &ev, 0))          // non-blocking drain
 *         process(ev);
 *     fdmon_destroy(ctx);
 */

#ifndef ALLMON_FDMON_H
#define ALLMON_FDMON_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── event types ─────────────────────────────────────────────── */

typedef enum {
    FDMON_OPEN  = 1,    /* a file descriptor was opened   */
    FDMON_CLOSE = 2,    /* a file descriptor was closed    */
} fdmon_event_type_t;

/* Describes a single fd lifecycle event. */
typedef struct {
    fdmon_event_type_t type;
    pid_t              pid;         /* process that performed the op  */
    pid_t              tgid;        /* thread-group leader (main PID) */
    int                fd;          /* file descriptor number         */
    uint64_t           timestamp;   /* CLOCK_MONOTONIC nanoseconds    */
    char               path[1024];  /* resolved path (best effort,
                                       may be empty for sockets etc.) */
} fdmon_event_t;

/* ── backend selection ───────────────────────────────────────── */

typedef enum {
    FDMON_BACKEND_AUTO     = 0,   /* try eBPF first, fall back to fanotify */
    FDMON_BACKEND_FANOTIFY = 1,
    FDMON_BACKEND_EBPF     = 2,
} fdmon_backend_t;

/* Human-readable backend name (for status display). */
const char *fdmon_backend_name(fdmon_backend_t backend);

/* ── context (opaque) ────────────────────────────────────────── */

typedef struct fdmon_ctx fdmon_ctx_t;

/*
 * Create a monitoring context.
 *
 *   backend  – FDMON_BACKEND_AUTO to let the library decide, or force
 *              a specific backend.
 *
 * Returns NULL on failure (e.g. no backend available, or insufficient
 * permissions).  errno is set.
 */
fdmon_ctx_t *fdmon_create(fdmon_backend_t backend);

/* Shut down and free all resources. Safe to call with NULL. */
void fdmon_destroy(fdmon_ctx_t *ctx);

/* Which backend is actually in use? */
fdmon_backend_t fdmon_active_backend(const fdmon_ctx_t *ctx);

/* ── process-group watching ──────────────────────────────────── */

/*
 * Start watching a process group rooted at `pid`.
 *
 * "Process group" here means `pid` and all of its current and future
 * descendants (children, grandchildren, …).  The library walks
 * /proc/<pid>/task/… children on each snapshot to discover new
 * descendants automatically.
 *
 * Multiple root PIDs can be watched simultaneously.
 *
 * Returns 0 on success, -1 on error.
 */
int fdmon_watch_pid(fdmon_ctx_t *ctx, pid_t pid);

/*
 * Stop watching a previously-added root PID (and its descendants).
 * Pending events for that group remain in the ring until consumed.
 * Returns 0 on success, -1 if pid was not being watched.
 */
int fdmon_unwatch_pid(fdmon_ctx_t *ctx, pid_t pid);

/*
 * Refresh the descendant set for all watched root PIDs.
 *
 * The eBPF backend can auto-track fork/clone, so this is mainly
 * useful for the fanotify backend.  Call periodically (e.g. once per
 * second from your existing monitor loop).
 */
void fdmon_refresh_descendants(fdmon_ctx_t *ctx);

/* ── event consumption ───────────────────────────────────────── */

/*
 * Retrieve the next event from the internal ring buffer.
 *
 *   timeout_ms  –  0 = non-blocking (return immediately)
 *                 -1 = block until an event is available
 *                 >0 = block for at most timeout_ms milliseconds
 *
 * Returns 1 if an event was written to *ev, 0 if no event (timeout
 * or empty ring), -1 on error.
 */
int fdmon_poll(fdmon_ctx_t *ctx, fdmon_event_t *ev, int timeout_ms);

/*
 * Drain all pending events into a caller-supplied array.
 * Returns the number of events written (0 … max_events).
 */
size_t fdmon_drain(fdmon_ctx_t *ctx, fdmon_event_t *buf, size_t max_events);

/*
 * Return the number of pending events without consuming them.
 */
size_t fdmon_pending(const fdmon_ctx_t *ctx);

/* ── statistics ──────────────────────────────────────────────── */

typedef struct {
    uint64_t events_captured;    /* total events seen by backend     */
    uint64_t events_delivered;   /* events that passed PID filter    */
    uint64_t events_dropped;     /* ring-buffer overflows            */
    uint64_t watched_pids;       /* root PIDs currently watched      */
    uint64_t tracked_descendants;/* total descendant PIDs tracked    */
} fdmon_stats_t;

void fdmon_get_stats(const fdmon_ctx_t *ctx, fdmon_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* ALLMON_FDMON_H */
