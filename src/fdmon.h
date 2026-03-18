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

#ifndef evemon_FDMON_H
#define evemon_FDMON_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

/* ── event types ─────────────────────────────────────────────── */

typedef enum {
    FDMON_OPEN     = 1,    /* a file descriptor was opened     */
    FDMON_CLOSE    = 2,    /* a file descriptor was closed      */
    FDMON_NET_SEND = 3,    /* TCP data sent (bytes in .net_bytes) */
    FDMON_NET_RECV = 4,    /* TCP data received                  */
} fdmon_event_type_t;

/* Describes a single fd lifecycle event. */
typedef struct {
    fdmon_event_type_t type;
    pid_t              pid;         /* process that performed the op  */
    pid_t              tgid;        /* thread-group leader (main PID) */
    int                fd;          /* file descriptor number         */
    uint64_t           timestamp;   /* CLOCK_MONOTONIC nanoseconds    */
    uint32_t           net_bytes;   /* bytes sent/received (NET_SEND/RECV) */
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

/* ── per-PID network I/O throughput ──────────────────────────── */

/*
 * Retrieve accumulated network send/recv bytes for a process since
 * the last call to fdmon_net_io_snapshot().  The library tracks
 * per-PID byte counters from eBPF tcp_sendmsg / tcp_recvmsg probes.
 *
 * Call fdmon_net_io_snapshot() periodically (e.g. once per second)
 * to latch the current counters.  Then fdmon_net_io_get() returns
 * the delta (bytes/interval) for any PID.
 *
 * Returns 0 on success, -1 if PID has no recorded traffic.
 */
int fdmon_net_io_get(const fdmon_ctx_t *ctx, pid_t tgid,
                     uint64_t *send_bytes, uint64_t *recv_bytes);

/*
 * Latch the current per-PID network byte counters and compute deltas.
 * Must be called periodically from the UI refresh loop.
 */
void fdmon_net_io_snapshot(fdmon_ctx_t *ctx);

/* ── per-socket (connection) network I/O ─────────────────────── */

/*
 * Query result for per-socket throughput.
 * Matches a network connection by its 4-tuple (local/remote addr:port).
 */
typedef struct {
    uint32_t laddr;      /* local  IPv4 (network order)   */
    uint32_t raddr;      /* remote IPv4 (network order)   */
    uint16_t lport;      /* local  port (host order)      */
    uint16_t rport;      /* remote port (network order)   */
    uint64_t inode;      /* socket inode — primary key    */
    uint64_t delta_send; /* bytes sent since last snapshot */
    uint64_t delta_recv; /* bytes recv since last snapshot */
} fdmon_sock_io_t;

/*
 * Retrieve per-socket throughput data for a given PID.
 * Caller provides an output buffer and its capacity.  The function
 * fills up to *count entries and sets *count to the number returned.
 * Returns 0 on success.
 */
int fdmon_sock_io_list(const fdmon_ctx_t *ctx, pid_t tgid,
                       fdmon_sock_io_t *out, size_t *count);

/* ── orphan-stdout capture ───────────────────────────────────── */

/*
 * Enable "orphan-stdout" mode: automatically capture writes to fd 1
 * and fd 2 from any newly exec'd process whose stdout/stderr is NOT a
 * real terminal.  This catches output from:
 *
 *   • cron jobs  (stdout/stderr → pipe to nobody, or /dev/null)
 *   • systemd/init services  (stdout redirected to the journal pipe)
 *   • scripts run via `nohup`, `at`, SSH without a tty, etc.
 *   • anything launched with its output going to a log file
 *
 * How it works:
 *   The eBPF sched_process_exec tracepoint fires on every successful
 *   execve().  When that event arrives in userspace, evemon reads
 *   /proc/<pid>/fd/1 and /proc/<pid>/fd/2 via readlink().  If the
 *   target is a pipe, a regular file, or /dev/null (i.e. not a PTY or
 *   other /dev/tty* device), the (pid, fd) pair is inserted into the
 *   monitored_pid_fds BPF map.  From then on, all write()/writev()/
 *   pwrite64() calls to those fds are captured and published on the
 *   event bus as EVEMON_EVENT_FD_WRITE events.
 *
 * Requires:
 *   • eBPF backend must be active.
 *   • The sched_process_exec tracepoint must be available on the kernel.
 *   • CAP_BPF (or root).
 *
 * Returns 0 on success, -1 if unavailable (check errno).
 * Implicitly calls fdmon_write_enable() so write tracepoints are live.
 */
int  fdmon_orphan_stdout_enable(fdmon_ctx_t *ctx);

/*
 * Disable orphan-stdout mode.  No new PIDs will be auto-registered.
 * Already-registered (pid, fd) pairs in the BPF map remain until the
 * process exits or fdmon_remove_pid_fd() is called explicitly.
 */
void fdmon_orphan_stdout_disable(fdmon_ctx_t *ctx);

#endif /* evemon_FDMON_H */
