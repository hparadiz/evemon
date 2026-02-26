#ifndef evemon_PROC_H
#define evemon_PROC_H

#include <sys/types.h>
#include <pthread.h>

#include "steam.h"
#include "fdmon.h"

/* Global debug flag, set by --debug on the command line */
extern int evemon_debug;

/* Maximum lengths for process info strings */
#define PROC_NAME_MAX  256
#define PROC_CMD_MAX   4096
#define PROC_USER_MAX  64
#define PROC_CWD_MAX   1024
#define PROC_CTR_MAX   64
#define PROC_SVC_MAX   128
#define PROC_LIST_MAX  2048

/* Number of I/O rate history samples kept per process (sparkline depth) */
#define IO_HISTORY_LEN 20

/* A single process entry */
typedef struct {
    pid_t    pid;
    pid_t    ppid;
    char     name[PROC_NAME_MAX];
    char     cmdline[PROC_CMD_MAX];
    char     user[PROC_USER_MAX];
    char     cwd[PROC_CWD_MAX];
    char     container[PROC_CTR_MAX];   /* container runtime or empty   */
    char     service[PROC_SVC_MAX];     /* systemd unit or openrc svc   */
    long     mem_rss_kb;            /* resident set size in KiB */
    unsigned long long cpu_ticks;   /* utime + stime (USER_HZ ticks)  */
    double   cpu_percent;           /* CPU% since last snapshot        */
    unsigned long long start_time;  /* process start time (epoch secs) */
    unsigned long long io_read_bytes;  /* cumulative read_bytes from /proc/<pid>/io  */
    unsigned long long io_write_bytes; /* cumulative write_bytes from /proc/<pid>/io */
    double   io_read_rate;          /* disk read  bytes/sec since last snapshot    */
    double   io_write_rate;         /* disk write bytes/sec since last snapshot    */
    float    io_history[IO_HISTORY_LEN]; /* ring buffer of combined I/O rate samples */
    int      io_history_len;        /* number of valid samples in io_history       */
    steam_info_t *steam;            /* Steam/Proton metadata (heap, NULL if not Steam) */
} proc_entry_t;

/* Snapshot of all processes at a point in time */
typedef struct {
    proc_entry_t *entries;
    size_t        count;
} proc_snapshot_t;

/* Free a snapshot and all heap-allocated per-entry data (e.g. steam info) */
void proc_snapshot_free(proc_snapshot_t *snap);

/* Thread-safe shared state between the monitor and the UI */
typedef struct {
    proc_snapshot_t  snapshot;   /* latest process snapshot         */
    pthread_mutex_t  lock;       /* protects snapshot               */
    pthread_cond_t   updated;    /* signalled when snapshot changes */
    int              running;    /* 0 = shutdown requested          */
    fdmon_ctx_t     *fdmon;      /* eBPF fd/network monitor (may be NULL) */
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
