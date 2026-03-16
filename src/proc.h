#ifndef evemon_PROC_H
#define evemon_PROC_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "steam.h"
#include "fdmon.h"

#include "log.h"

/* Maximum comm name length (used in a few places that still need it) */
#define PROC_NAME_MAX  256
#define PROC_LIST_MAX  2048

/* Number of I/O rate history samples kept per process (sparkline depth) */
#define IO_HISTORY_LEN 20

/* ── Variable-length string table ────────────────────────────── */

typedef uint32_t str_off_t;  /* 0 = empty / none (reserved NUL at slot 0) */
typedef uint16_t str_len_t;

/*
 * proc_strtab_t – a single flat buffer holding all per-snapshot strings.
 * Offset 0 is reserved as the empty-string sentinel (a single NUL byte
 * placed there during init).  Every other string is NUL-terminated and
 * can be addressed by its offset.
 */
typedef struct {
    char     *buf;
    uint32_t  len;   /* bytes used (including the sentinel NUL at [0]) */
    uint32_t  cap;   /* bytes allocated */
} proc_strtab_t;

/*
 * Rare/expensive metadata kept out of the hot row.
 * Heap-allocated; NULL extras_idx means no extras for this entry.
 */
typedef struct {
    steam_info_t *steam;   /* NULL if not a Steam/Wine process */
} proc_extra_t;

/* ── Compact process row ─────────────────────────────────────── */

typedef struct {
    pid_t     pid;
    pid_t     ppid;

    str_off_t name_off;       str_len_t name_len;
    str_off_t cmd_off;        str_len_t cmd_len;
    str_off_t user_off;       str_len_t user_len;
    str_off_t cwd_off;        str_len_t cwd_len;
    str_off_t container_off;  str_len_t container_len;
    str_off_t service_off;    str_len_t service_len;

    uint64_t  mem_rss_kb;
    uint64_t  cpu_ticks;
    uint64_t  start_time;
    uint64_t  io_read_bytes;
    uint64_t  io_write_bytes;

    float     cpu_percent;
    float     io_read_rate;
    float     io_write_rate;

    uint8_t   io_history_len;
    uint8_t   io_history_head;   /* unused currently; reserved for ring-head */
    uint16_t  flags;             /* reserved */

    float     io_history[IO_HISTORY_LEN];

    uint32_t  extra_idx;   /* index into proc_snapshot_t::extras[], UINT32_MAX = none */
} proc_entry_t;

/* ── String-table accessors (two-argument) ───────────────────── */

#define PROC_STR(tab, off)       ((off) ? ((tab)->buf + (off)) : "")
#define PROC_NAME(s, e)          PROC_STR(&(s)->strtab, (e)->name_off)
#define PROC_CMDLINE(s, e)       PROC_STR(&(s)->strtab, (e)->cmd_off)
#define PROC_USER(s, e)          PROC_STR(&(s)->strtab, (e)->user_off)
#define PROC_CWD(s, e)           PROC_STR(&(s)->strtab, (e)->cwd_off)
#define PROC_CONTAINER(s, e)     PROC_STR(&(s)->strtab, (e)->container_off)
#define PROC_SERVICE(s, e)       PROC_STR(&(s)->strtab, (e)->service_off)

/* ── Snapshot ────────────────────────────────────────────────── */

typedef struct {
    proc_entry_t   *entries;
    uint32_t        len;
    uint32_t        cap;

    proc_strtab_t   strtab;

    proc_extra_t   *extras;
    uint32_t        extras_len;
    uint32_t        extras_cap;
} proc_snapshot_t;

/* ── strtab helper ───────────────────────────────────────────── */

/*
 * Initialise a strtab: allocate buf, write sentinel NUL at offset 0.
 * cap: initial buffer capacity in bytes (grows by doubling as needed).
 */
static inline int strtab_init(proc_strtab_t *t, uint32_t cap)
{
    t->buf = (char *)malloc(cap);
    if (!t->buf) return -1;
    t->buf[0] = '\0';
    t->len = 1;    /* offset 0 = empty sentinel */
    t->cap = cap;
    return 0;
}

static inline void strtab_free(proc_strtab_t *t)
{
    free(t->buf);
    t->buf = NULL;
    t->len = 0;
    t->cap = 0;
}

/*
 * Append string s (NUL-terminated) to the strtab.
 * Returns the offset of the stored string, or 0 on OOM/empty.
 * Offset 0 is the empty sentinel, so callers can use 0 as "none".
 * Empty strings (s == NULL or s[0] == '\0') return 0.
 */
static inline str_off_t strtab_push(proc_strtab_t *t, const char *s)
{
    if (!s || s[0] == '\0') return 0;

    uint32_t slen = (uint32_t)strlen(s);
    uint32_t need = t->len + slen + 1;

    if (need > t->cap) {
        uint32_t newcap = t->cap * 2;
        if (newcap < need) newcap = need + 1024;
        char *nb = (char *)realloc(t->buf, newcap);
        if (!nb) return 0;
        t->buf = nb;
        t->cap = newcap;
    }

    str_off_t off = t->len;
    memcpy(t->buf + off, s, slen + 1);
    t->len += slen + 1;
    return off;
}

/*
 * Reset the strtab to its initial state (len=1, preserving capacity).
 * Used each tick by the store to repopulate from a fresh snapshot.
 */
static inline void strtab_reset(proc_strtab_t *t)
{
    if (t->buf) {
        t->buf[0] = '\0';
        t->len = 1;
    }
}

/* Free a snapshot and all heap-allocated data */
void proc_snapshot_free(proc_snapshot_t *snap);

/* Thread-safe shared state between the monitor and the UI */
typedef struct {
    proc_snapshot_t  snapshot;   /* latest process snapshot         */
    pthread_mutex_t  lock;       /* protects snapshot               */
    pthread_cond_t   updated;    /* signalled when snapshot changes */
    int              running;    /* 0 = shutdown requested          */
    fdmon_ctx_t     *fdmon;      /* eBPF fd/network monitor (may be NULL) */
    pid_t            preselect_pid; /* PID to fast-path on first scan (0 = none) */
} monitor_state_t;

/* ── Monitor (backend) API ──────────────────────────────────── */

/* Initialise shared state; must be called before any threads start */
int  monitor_state_init(monitor_state_t *state);

/* Free shared state resources */
void monitor_state_destroy(monitor_state_t *state);

/* Background thread entry point: polls /proc and updates state */
void *monitor_thread(void *arg);   /* arg = monitor_state_t* */

/* ── UI (frontend) API ──────────────────────────────────────── */

/* Placeholder thread entry point for the future GTK3 UI */
void *ui_thread(void *arg);        /* arg = monitor_state_t* */

#endif /* evemon_PROC_H */
