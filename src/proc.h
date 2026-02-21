#ifndef ALLMON_PROC_H
#define ALLMON_PROC_H

#include <sys/types.h>
#include <pthread.h>

/* Maximum lengths for process info strings */
#define PROC_NAME_MAX  256
#define PROC_CMD_MAX   4096
#define PROC_USER_MAX  64
#define PROC_CWD_MAX   1024
#define PROC_CTR_MAX   64
#define PROC_LIST_MAX  2048

/* A single process entry */
typedef struct {
    pid_t    pid;
    pid_t    ppid;
    char     name[PROC_NAME_MAX];
    char     cmdline[PROC_CMD_MAX];
    char     user[PROC_USER_MAX];
    char     cwd[PROC_CWD_MAX];
    char     container[PROC_CTR_MAX];   /* container runtime or empty   */
    long     mem_rss_kb;            /* resident set size in KiB */
    unsigned long long cpu_ticks;   /* utime + stime (USER_HZ ticks)  */
    double   cpu_percent;           /* CPU% since last snapshot        */
    unsigned long long start_time;  /* process start time (epoch secs) */
} proc_entry_t;

/* Snapshot of all processes at a point in time */
typedef struct {
    proc_entry_t *entries;
    size_t        count;
} proc_snapshot_t;

/* Thread-safe shared state between the monitor and the UI */
typedef struct {
    proc_snapshot_t  snapshot;   /* latest process snapshot         */
    pthread_mutex_t  lock;       /* protects snapshot               */
    pthread_cond_t   updated;    /* signalled when snapshot changes */
    int              running;    /* 0 = shutdown requested          */
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

#endif /* ALLMON_PROC_H */
