/*
 * monitor.c – background thread that reads /proc to build a process snapshot.
 *
 * The monitor thread loops every POLL_INTERVAL_MS, scans /proc for numeric
 * entries (PIDs), reads each process's comm and cmdline, and publishes a
 * new snapshot under the shared lock so the UI thread can consume it.
 */

#include "proc.h"
#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>

#define POLL_INTERVAL_MS 2000

/* ── helpers ─────────────────────────────────────────────────── */

/* Read the first line of /proc/<pid>/comm into buf (strips newline). */
static int read_comm(pid_t pid, char *buf, size_t bufsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    if (!fgets(buf, (int)bufsz, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* strip trailing newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';

    return 0;
}

/* Read /proc/<pid>/cmdline (NUL-delimited) and join with spaces. */
static int read_cmdline(pid_t pid, char *buf, size_t bufsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);

    if (n == 0) {
        buf[0] = '\0';
        return 0;   /* kernel thread – empty cmdline is fine */
    }

    /* Replace internal NULs with spaces */
    for (size_t i = 0; i < n - 1; i++) {
        if (buf[i] == '\0')
            buf[i] = ' ';
    }
    buf[n] = '\0';

    return 0;
}

/* Read /proc/<pid>/status and extract PPid. */
static pid_t read_ppid(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[256];
    pid_t ppid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PPid:", 5) == 0) {
            ppid = (pid_t)atoi(line + 5);
            break;
        }
    }
    fclose(f);
    return ppid;
}

/* Read the owning user of /proc/<pid> via Uid from status + getpwuid. */
static int read_user(pid_t pid, char *buf, size_t bufsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    uid_t uid = (uid_t)-1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            uid = (uid_t)atoi(line + 4);
            break;
        }
    }
    fclose(f);

    if (uid == (uid_t)-1) {
        snprintf(buf, bufsz, "?");
        return -1;
    }

    struct passwd *pw = getpwuid(uid);
    if (pw)
        snprintf(buf, bufsz, "%s", pw->pw_name);
    else
        snprintf(buf, bufsz, "%u", (unsigned)uid);

    return 0;
}

/* Read the current working directory of /proc/<pid>/cwd via readlink. */
static int read_cwd(pid_t pid, char *buf, size_t bufsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cwd", pid);

    ssize_t n = readlink(path, buf, bufsz - 1);
    if (n < 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

/* Read VmRSS (resident set size in kB) from /proc/<pid>/status. */
static long read_rss(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    long rss = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            rss = atol(line + 6);
            break;
        }
    }
    fclose(f);
    return rss;
}

/*
 * Read utime + stime from /proc/<pid>/stat (fields 14 & 15, 1-based).
 * Returns cumulative CPU ticks in USER_HZ, or 0 on failure.
 */
static unsigned long long read_cpu_ticks(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char buf[1024];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    /* The comm field (field 2) is wrapped in parentheses and may contain
     * spaces.  Skip past the closing ')' to reach field 3 onwards.      */
    const char *p = strrchr(buf, ')');
    if (!p) return 0;
    p++;  /* skip ')' */

    /* Fields after comm: state(3), ppid(4), pgrp(5), session(6),
     * tty_nr(7), tpgid(8), flags(9), minflt(10), cminflt(11),
     * majflt(12), cmajflt(13), utime(14), stime(15).               */
    unsigned long utime = 0, stime = 0;
    int n = sscanf(p, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                   &utime, &stime);
    if (n != 2)
        return 0;

    return (unsigned long long)utime + (unsigned long long)stime;
}

/* Returns non-zero when every character in s is a digit (PID directory). */
static int is_pid_dir(const char *s)
{
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s))
            return 0;
    }
    return 1;
}

/* ── previous CPU tick tracking for delta computation ────────── */

#define CPU_HT_SIZE 8192

typedef struct {
    pid_t              pid;
    unsigned long long ticks;
    int                used;
} cpu_prev_entry_t;

static cpu_prev_entry_t g_prev_ticks[CPU_HT_SIZE];
static int              g_prev_valid = 0;  /* has a previous sample? */

static unsigned long long cpu_ht_get(const cpu_prev_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % CPU_HT_SIZE;
    for (int k = 0; k < CPU_HT_SIZE; k++) {
        if (!ht[h].used) return 0;
        if (ht[h].pid == pid) return ht[h].ticks;
        h = (h + 1) % CPU_HT_SIZE;
    }
    return 0;
}

static void cpu_ht_set(cpu_prev_entry_t *ht, pid_t pid, unsigned long long ticks)
{
    unsigned h = (unsigned)pid % CPU_HT_SIZE;
    while (ht[h].used && ht[h].pid != pid)
        h = (h + 1) % CPU_HT_SIZE;
    ht[h].pid   = pid;
    ht[h].ticks = ticks;
    ht[h].used  = 1;
}

/* ── snapshot builder ────────────────────────────────────────── */

static proc_snapshot_t build_snapshot(void)
{
    proc_snapshot_t snap = { .entries = NULL, .count = 0 };

    DIR *dp = opendir("/proc");
    if (!dp)
        return snap;

    size_t capacity = 512;
    snap.entries = malloc(capacity * sizeof(proc_entry_t));
    if (!snap.entries) {
        closedir(dp);
        return snap;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_type != DT_DIR || !is_pid_dir(de->d_name))
            continue;

        pid_t pid = (pid_t)atoi(de->d_name);

        /* grow buffer if needed */
        if (snap.count >= capacity) {
            capacity *= 2;
            proc_entry_t *tmp = realloc(snap.entries,
                                        capacity * sizeof(proc_entry_t));
            if (!tmp)
                break;
            snap.entries = tmp;
        }

        proc_entry_t *e = &snap.entries[snap.count];
        e->pid  = pid;
        e->ppid = read_ppid(pid);

        if (read_comm(pid, e->name, sizeof(e->name)) != 0)
            continue;   /* process vanished, skip */

        if (read_cmdline(pid, e->cmdline, sizeof(e->cmdline)) != 0)
            snprintf(e->cmdline, sizeof(e->cmdline), "[%s]", e->name);

        /* If cmdline was empty (kernel thread), show [name] */
        if (e->cmdline[0] == '\0')
            snprintf(e->cmdline, sizeof(e->cmdline), "[%s]", e->name);

        /* user, cwd, memory */
        if (read_user(pid, e->user, sizeof(e->user)) != 0)
            snprintf(e->user, sizeof(e->user), "?");

        if (read_cwd(pid, e->cwd, sizeof(e->cwd)) != 0)
            e->cwd[0] = '\0';

        e->mem_rss_kb = read_rss(pid);

        /* CPU ticks and percentage */
        e->cpu_ticks = read_cpu_ticks(pid);
        e->cpu_percent = 0.0;

        snap.count++;
    }

    closedir(dp);
    return snap;
}

/* ── public API ──────────────────────────────────────────────── */

int monitor_state_init(monitor_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->running = 1;

    if (pthread_mutex_init(&state->lock, NULL) != 0)
        return -1;
    if (pthread_cond_init(&state->updated, NULL) != 0) {
        pthread_mutex_destroy(&state->lock);
        return -1;
    }
    return 0;
}

void monitor_state_destroy(monitor_state_t *state)
{
    pthread_mutex_lock(&state->lock);
    free(state->snapshot.entries);
    state->snapshot.entries = NULL;
    state->snapshot.count   = 0;
    pthread_mutex_unlock(&state->lock);

    pthread_cond_destroy(&state->updated);
    pthread_mutex_destroy(&state->lock);
}

void *monitor_thread(void *arg)
{
    monitor_state_t *state = (monitor_state_t *)arg;

    while (1) {
        /* Check if we should keep running */
        pthread_mutex_lock(&state->lock);
        int running = state->running;
        pthread_mutex_unlock(&state->lock);

        if (!running)
            break;

        /* Build a fresh snapshot outside the lock */
        PROFILE_BEGIN(snapshot_build);
        proc_snapshot_t snap = build_snapshot();
        PROFILE_END(snapshot_build);

        /* Compute CPU% from delta between previous and current ticks.
         * CPU% = (delta_ticks / (elapsed_seconds * CLK_TCK * num_cpus)) * 100 */
        static struct timespec prev_ts = { 0, 0 };
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);

        long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        long clk_tck  = sysconf(_SC_CLK_TCK);
        if (num_cpus < 1) num_cpus = 1;
        if (clk_tck < 1) clk_tck = 100;

        if (g_prev_valid && prev_ts.tv_sec != 0) {
            double elapsed = (double)(now_ts.tv_sec - prev_ts.tv_sec)
                           + (double)(now_ts.tv_nsec - prev_ts.tv_nsec) / 1e9;
            if (elapsed > 0.01) {
                double total_ticks = elapsed * (double)clk_tck * (double)num_cpus;
                for (size_t i = 0; i < snap.count; i++) {
                    unsigned long long prev = cpu_ht_get(g_prev_ticks,
                                                         snap.entries[i].pid);
                    if (prev > 0 && snap.entries[i].cpu_ticks >= prev) {
                        double delta = (double)(snap.entries[i].cpu_ticks - prev);
                        snap.entries[i].cpu_percent = (delta / total_ticks) * 100.0;
                    }
                }
            }
        }

        /* Store current ticks as the "previous" for next iteration */
        memset(g_prev_ticks, 0, sizeof(g_prev_ticks));
        for (size_t i = 0; i < snap.count; i++)
            cpu_ht_set(g_prev_ticks, snap.entries[i].pid,
                       snap.entries[i].cpu_ticks);
        g_prev_valid = 1;
        prev_ts = now_ts;

        /* Swap it in */
        pthread_mutex_lock(&state->lock);
        free(state->snapshot.entries);
        state->snapshot = snap;
        pthread_cond_signal(&state->updated);
        pthread_mutex_unlock(&state->lock);

        /* Sleep for the poll interval */
        struct timespec ts = {
            .tv_sec  = POLL_INTERVAL_MS / 1000,
            .tv_nsec = (POLL_INTERVAL_MS % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }

    return NULL;
}
