/*
 * fdmon_internal.h – Shared internals between fdmon.c and fdmon_ebpf.c.
 *
 * NOT part of the public API.  Exposes the fdmon_ctx layout and
 * submit_event so the eBPF backend can push events into the ring.
 */

#ifndef ALLMON_FDMON_INTERNAL_H
#define ALLMON_FDMON_INTERNAL_H

#include "fdmon.h"
#include <pthread.h>
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
    volatile int      fan_running;

    /* eBPF backend (opaque to fanotify side) */
    void             *ebpf_state;
};

/* Push a filtered event into the ring.  Defined in fdmon.c. */
void submit_event(fdmon_ctx_t *ctx, fdmon_event_type_t type,
                  pid_t pid, pid_t tgid, int fd, const char *path);

#endif /* ALLMON_FDMON_INTERNAL_H */
