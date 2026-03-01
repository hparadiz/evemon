/*
 * fdmon_internal.h – Shared internals between fdmon.c and fdmon_ebpf.c.
 *
 * NOT part of the public API.  Exposes the fdmon_ctx layout and
 * submit_event so the eBPF backend can push events into the ring.
 */

#ifndef evemon_FDMON_INTERNAL_H
#define evemon_FDMON_INTERNAL_H

#include "fdmon.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/* ── PID set ─────────────────────────────────────────────────── */

typedef struct {
    pid_t  *pids;
    size_t  count;
    size_t  capacity;
} pid_set_t;

/* ── ring buffer ─────────────────────────────────────────────── */

#define RING_CAPACITY  4096
#define RING_MASK      (RING_CAPACITY - 1)

typedef struct {
    fdmon_event_t   slots[RING_CAPACITY];
    volatile size_t head;
    volatile size_t tail;
    size_t          dropped;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} ring_t;

/* ── max limits ──────────────────────────────────────────────── */

#define MAX_ROOTS       64
#define MAX_DESCENDANTS 8192

/* ── per-PID network I/O accumulator ─────────────────────────── */

#define NET_IO_HT_SIZE  4096

typedef struct {
    pid_t    tgid;
    uint64_t send_bytes;     /* cumulative bytes sent              */
    uint64_t recv_bytes;     /* cumulative bytes received          */
    uint64_t prev_send;      /* send_bytes at last snapshot        */
    uint64_t prev_recv;      /* recv_bytes at last snapshot        */
    uint64_t delta_send;     /* bytes sent since last snapshot     */
    uint64_t delta_recv;     /* bytes received since last snapshot */
    int      used;
} net_io_entry_t;

/* ── per-socket (4-tuple) network I/O accumulator ────────────── */

#define SOCK_IO_HT_SIZE  8192

typedef struct {
    pid_t    tgid;
    uint32_t laddr;          /* local  IPv4 (network order)        */
    uint32_t raddr;          /* remote IPv4 (network order)        */
    uint16_t lport;          /* local  port (host order)           */
    uint16_t rport;          /* remote port (network order)        */
    uint64_t send_bytes;
    uint64_t recv_bytes;
    uint64_t prev_send;
    uint64_t prev_recv;
    uint64_t delta_send;
    uint64_t delta_recv;
    int      used;
} sock_io_entry_t;

/* ── context layout (full definition) ────────────────────────── */

struct fdmon_ctx {
    fdmon_backend_t   backend;

    /* PID-group tracking */
    pthread_mutex_t   group_lock;
    pid_t             roots[MAX_ROOTS];
    size_t            root_count;
    pid_set_t         watched;

    /* Event ring buffer */
    ring_t            ring;

    /* Statistics */
    uint64_t          stat_captured;
    uint64_t          stat_delivered;

    /* fanotify backend */
    int               fan_fd;
    pthread_t         fan_thread;
    atomic_int        fan_running;

    /* eBPF backend (opaque to fanotify side) */
    void             *ebpf_state;

    /* Per-PID network I/O counters (populated by eBPF backend) */
    pthread_mutex_t   net_io_lock;
    net_io_entry_t    net_io[NET_IO_HT_SIZE];

    /* Per-socket (4-tuple) network I/O counters */
    sock_io_entry_t   sock_io[SOCK_IO_HT_SIZE];
};

/* Push a filtered event into the ring.  Defined in fdmon.c. */
void submit_event(fdmon_ctx_t *ctx, fdmon_event_type_t type,
                  pid_t pid, pid_t tgid, int fd, const char *path);

/* Accumulate network I/O bytes for a PID.  Defined in fdmon.c.
 * Called from the eBPF perf buffer callback. */
void submit_net_event(fdmon_ctx_t *ctx, pid_t tgid, uint32_t bytes,
                      int is_send);

/* Accumulate per-socket (4-tuple) I/O bytes.  Defined in fdmon.c.
 * Called from the eBPF perf buffer callback with connection details. */
void submit_sock_event(fdmon_ctx_t *ctx, pid_t tgid,
                       uint32_t laddr, uint16_t lport,
                       uint32_t raddr, uint16_t rport,
                       uint32_t bytes, int is_send);

/* Dynamic write monitoring helpers (eBPF backend) */
int  fdmon_write_enable(struct fdmon_ctx *ctx);
void fdmon_write_disable(struct fdmon_ctx *ctx);
int  fdmon_add_pid_fd(struct fdmon_ctx *ctx, pid_t pid, int fd);
int  fdmon_remove_pid_fd(struct fdmon_ctx *ctx, pid_t pid, int fd);

/*
 * Orphan-stdout mode: when enabled, the eBPF backend automatically
 * registers fd 1 and fd 2 of every newly exec'd process whose stdout
 * or stderr is NOT a real TTY (e.g. pipes to nowhere, /dev/null, log
 * files).  This captures "lost" output from cron jobs, services, and
 * any other processes whose output is silently discarded.
 *
 * Enabling this also implicitly calls fdmon_write_enable() to ensure
 * the write tracepoints are attached.
 *
 * Returns 0 on success, -1 if the eBPF backend is unavailable.
 */
int  fdmon_orphan_stdout_enable(struct fdmon_ctx *ctx);
void fdmon_orphan_stdout_disable(struct fdmon_ctx *ctx);

/*
 * Watched-parent table: register a parent PID so any child that calls
 * execve() has its fd 1/2 inserted into the BPF map immediately in the
 * reader thread — before the child can write.
 * fd_mask: bit0 = fd1, bit1 = fd2.
 */
int  fdmon_watch_parent_fds(struct fdmon_ctx *ctx, pid_t pid, int fd_mask);
void fdmon_unwatch_parent_fds(struct fdmon_ctx *ctx, pid_t pid);

#endif /* evemon_FDMON_INTERNAL_H */
