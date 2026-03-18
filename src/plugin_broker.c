/*
 * plugin_broker.c – Single-gather data broker for plugin instances.
 *
 * For each unique tracked PID, reads /proc data ONCE according to the
 * combined_needs bitmask, then dispatches results to all instances via
 * a frontend-agnostic completion callback.
 *
 * Data flow:
 *   1. broker_start() is called (from any thread) to kick a cycle
 *   2. Collects unique PIDs + combined data_needs from the registry
 *   3. Spawns a pthread worker that gathers data for each PID
 *   4. Worker posts completion via broker_on_complete_cb, which the
 *      active frontend wires to its own main-loop dispatch mechanism
 *      (g_idle_add for GTK, QMetaObject::invokeMethod for Qt, etc.)
 */

#include "plugin_broker.h"
#include "mpris.h"

/* from main.c */
#include "log.h"
#ifdef HAVE_PIPEWIRE
#include "ui/pipewire_graph.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/tcp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/inet_diag.h>

#ifndef SOCK_DIAG_BY_FAMILY
#  define SOCK_DIAG_BY_FAMILY 20
#endif

#ifndef __NR_pidfd_open
#  define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_getfd
#  define __NR_pidfd_getfd 438
#endif

static int sys_pidfd_open(pid_t pid, unsigned int flags)
{
    return (int)syscall(__NR_pidfd_open, pid, flags);
}

static int sys_pidfd_getfd(int pidfd, int targetfd, unsigned int flags)
{
    return (int)syscall(__NR_pidfd_getfd, pidfd, targetfd, flags);
}

/* Defined in ui/devices.c — resolves /dev/ paths to hardware names */
extern void label_device(const char *path, char *desc, size_t descsz);

/* ── Per-PID gathered data ───────────────────────────────────── */

typedef struct {
    pid_t                pid;
    evemon_proc_data_t   data;

    /* Owned storage (freed after dispatch) */
    evemon_fd_t         *fd_buf;
    size_t               fd_cap;

    evemon_env_t        *env_buf;
    char                *env_raw;       /* raw /proc/<pid>/environ */
    size_t               env_cap;

    evemon_mmap_t       *mmap_buf;
    char                *mmap_raw;      /* raw /proc/<pid>/maps    */
    size_t               mmap_cap;

    evemon_lib_t        *lib_buf;
    size_t               lib_cap;

    evemon_socket_t     *sock_buf;
    size_t               sock_cap;

    evemon_cgroup_t      cgroup_data;

    pid_t               *desc_pids;
    size_t               desc_count;
    size_t               desc_cap;

    evemon_thread_t     *thread_buf;
    size_t               thread_cap;

    char                *raw_status;
    char                *raw_maps;
    char                *raw_cgroup;

    /* PipeWire graph (if evemon_NEED_PIPEWIRE) */
    evemon_pw_node_t    *pw_nodes;
    size_t               pw_node_cap;
    evemon_pw_link_t    *pw_links;
    size_t               pw_link_cap;
    evemon_pw_port_t    *pw_ports;
    size_t               pw_port_cap;

    /* MPRIS media metadata */
    evemon_mpris_player_t *mpris_players;
    size_t                 mpris_player_count;
} broker_pid_data_t;

static void broker_pid_data_free(broker_pid_data_t *d)
{
    free(d->fd_buf);
    free(d->env_buf);
    free(d->env_raw);
    free(d->mmap_buf);
    free(d->mmap_raw);
    free(d->lib_buf);
    free(d->sock_buf);
    free(d->desc_pids);
    free(d->thread_buf);
    free(d->raw_status);
    free(d->raw_maps);
    free(d->raw_cgroup);
    free(d->pw_nodes);
    free(d->pw_links);
    free(d->pw_ports);
    free(d->mpris_players);
}

/* ── Broker cycle state ──────────────────────────────────────── */

struct broker_cycle {
    plugin_registry_t  *reg;
    void               *fdmon;

    /* Input: PIDs to gather for + what data is needed */
    pid_t              *pids;
    size_t              pid_count;
    evemon_data_needs_t needs;

    /* Output: per-PID gathered data */
    broker_pid_data_t  *results;
    size_t              result_count;

    /* Audio PID output: PIDs with active PipeWire audio streams.
     * Extracted from the PW graph snapshot in the worker thread,
     * delivered to the UI on the main thread via callback. */
    pid_t              *audio_pids;
    size_t              audio_pid_count;
};

/* ── Pthread worker state ──────────────────────────────────────── */

/*
 * Cancel flag + mutex.  The worker checks g_broker_cancelled under
 * g_broker_mutex at the start of each PID gather.
 * g_broker_thread is 0 when no worker is running.
 */
static pthread_mutex_t  g_broker_mutex     = PTHREAD_MUTEX_INITIALIZER;
static int              g_broker_cancelled = 0;
static pthread_t        g_broker_thread    = 0;
static int              g_broker_running   = 0;

/*
 * Frontend completion hook.
 * Set once at startup by the active frontend (e.g. GTK uses g_idle_add).
 * The hook receives a heap-allocated broker_cycle_t* and is responsible
 * for calling broker_dispatch_cycle() and then broker_cycle_free() on it
 * from the appropriate main-loop thread.
 *
 * broker_dispatch_cycle() is exported so frontends can call it:
 *   void broker_dispatch_cycle(broker_cycle_t *cycle);
 */
static broker_complete_cb g_broker_complete_cb   = NULL;
static void              *g_broker_complete_data = NULL;

/* Callback for delivering audio PIDs to the UI on the main thread */
static broker_audio_pids_cb g_audio_pids_cb   = NULL;
static void                *g_audio_pids_data = NULL;

/* ── /proc readers ───────────────────────────────────────────── */

/*
 * Read a whole file into a heap buffer.
 * Returns bytes read, or -1 on error.  Buffer is NUL-terminated.
 */
static ssize_t read_file_alloc(const char *path, char **out)
{
    *out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return -1; }

    for (;;) {
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); fclose(f); return -1; }
            buf = tmp;
        }
    }
    fclose(f);
    buf[len] = '\0';
    *out = buf;
    return (ssize_t)len;
}

/* Read /proc/<pid>/environ into an array of evemon_env_t */
static void gather_environ(pid_t pid, broker_pid_data_t *d)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);

    ssize_t len = read_file_alloc(path, &d->env_raw);
    if (len <= 0) return;

    /* Count NUL-separated entries */
    size_t count = 0;
    for (ssize_t i = 0; i < len; i++)
        if (d->env_raw[i] == '\0') count++;

    d->env_buf = calloc(count, sizeof(evemon_env_t));
    if (!d->env_buf) return;
    d->env_cap = count;

    size_t idx = 0;
    const char *p = d->env_raw;
    const char *end = d->env_raw + len;
    while (p < end && idx < count) {
        if (*p == '\0') { p++; continue; }
        d->env_buf[idx].text = p;
        d->env_buf[idx].category = 0;  /* classification left to plugin */
        idx++;
        p += strlen(p) + 1;
    }
    d->data.envs = d->env_buf;
    d->data.env_count = idx;
}

/* Read /proc/<pid>/maps and parse into mmap regions */
static void gather_maps(pid_t pid, broker_pid_data_t *d)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    ssize_t len = read_file_alloc(path, &d->mmap_raw);
    if (len <= 0) return;

    d->data.raw_maps = d->mmap_raw;

    /* Count lines */
    size_t lines = 0;
    for (ssize_t i = 0; i < len; i++)
        if (d->mmap_raw[i] == '\n') lines++;
    if (lines == 0) return;

    d->mmap_buf = calloc(lines, sizeof(evemon_mmap_t));
    if (!d->mmap_buf) return;
    d->mmap_cap = lines;

    /* Parse line by line (in-place, replace \n with \0) */
    size_t idx = 0;
    char *line = d->mmap_raw;
    for (char *c = d->mmap_raw; *c; c++) {
        if (*c == '\n') {
            *c = '\0';
            if (idx < lines) {
                d->mmap_buf[idx].text = line;
                d->mmap_buf[idx].category = 0;

                /* Parse size from "addr_start-addr_end" */
                unsigned long s = 0, e = 0;
                if (sscanf(line, "%lx-%lx", &s, &e) == 2)
                    d->mmap_buf[idx].size_kb = (e - s) / 1024;
                idx++;
            }
            line = c + 1;
        }
    }
    d->data.mmaps = d->mmap_buf;
    d->data.mmap_count = idx;
}

/* ── Library version / origin helpers ────────────────────────── */

/*
 * Extract version string from a .so filename.
 * e.g. "libfoo.so.1.2.3" → "1.2.3", "libfoo.so" → ""
 */
static void extract_so_version(const char *filename, char *ver, size_t vsz)
{
    ver[0] = '\0';
    const char *p = strstr(filename, ".so.");
    if (p) {
        p += 4; /* skip ".so." */
        snprintf(ver, vsz, "%s", p);
    }
}

/*
 * Determine a short origin label from the full path.
 */
static void extract_origin(const char *fp, char *out, size_t osz)
{
    out[0] = '\0';
    const char *p;

    /* Proton dist/files paths */
    if ((p = strstr(fp, "/dist/lib")) || (p = strstr(fp, "/files/lib"))) {
        const char *c = strstr(fp, "/common/");
        if (c) {
            c += 8;
            const char *s = strchr(c, '/');
            if (s) { size_t n = (size_t)(s - c); if (n >= osz) n = osz-1;
                     memcpy(out, c, n); out[n] = '\0'; return; }
        }
        snprintf(out, osz, "Proton"); return;
    }
    if (strstr(fp, "/wine/") || strstr(fp, "/Wine/")) {
        snprintf(out, osz, "Wine"); return;
    }
    if (strstr(fp, "steam-runtime") || strstr(fp, "SteamLinuxRuntime") ||
        strstr(fp, "pressure-vessel")) {
        if (strstr(fp, "sniper"))       snprintf(out, osz, "Runtime (sniper)");
        else if (strstr(fp, "soldier")) snprintf(out, osz, "Runtime (soldier)");
        else if (strstr(fp, "scout"))   snprintf(out, osz, "Runtime (scout)");
        else                            snprintf(out, osz, "Steam Runtime");
        return;
    }
    if (strstr(fp, "/steamapps/common/")) {
        const char *c = strstr(fp, "/common/");
        if (c) {
            c += 8;
            const char *s = strchr(c, '/');
            if (s) { size_t n = (size_t)(s - c); if (n >= osz) n = osz-1;
                     memcpy(out, c, n); out[n] = '\0'; return; }
        }
    }
    if (strstr(fp, "/compatdata/")) {
        snprintf(out, osz, "Wine Prefix"); return;
    }
}

/* Read /proc/<pid>/maps for shared libraries (r-x segments with .so/.dll) */
static void gather_libs(pid_t pid, broker_pid_data_t *d)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "r");
    if (!f) return;

    size_t cap = 64, count = 0;
    evemon_lib_t *buf = calloc(cap, sizeof(evemon_lib_t));
    if (!buf) { fclose(f); return; }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        /* Parse: addr_start-addr_end perms offset dev inode [pathname] */
        unsigned long addr_start = 0, addr_end = 0;
        char perms[8] = "----";
        unsigned long dummy_off; char devstr[16]; unsigned long inode;
        if (sscanf(line, "%lx-%lx %4s %lx %15s %lu",
                   &addr_start, &addr_end, perms, &dummy_off, devstr, &inode) < 5)
            continue;
        if (perms[2] != 'x') continue;

        /* Find pathname after 5th field */
        const char *p = line;
        for (int fld = 0; fld < 5 && *p; fld++) {
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
        }
        if (!*p || *p == '[') continue;

        /* Check extension: must be .so or .dll/.drv/.exe */
        const char *bname = strrchr(p, '/');
        bname = bname ? bname + 1 : p;

        int is_so  = strstr(bname, ".so") != NULL;
        int is_dll = 0;
        size_t blen = strlen(bname);
        if (blen >= 4) {
            const char *ext = bname + blen - 4;
            is_dll = (strcasecmp(ext, ".dll") == 0 ||
                      strcasecmp(ext, ".drv") == 0 ||
                      strcasecmp(ext, ".exe") == 0 ||
                      strcasecmp(ext, ".sys") == 0 ||
                      strcasecmp(ext, ".ocx") == 0);
        }
        if (!is_so && !is_dll) continue;

        size_t seg_kb = (addr_end - addr_start) / 1024;

        /* Dedup — merge sizes for multi-segment mappings */
        int found = 0;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(buf[i].path, p) == 0) {
                buf[i].size_kb += seg_kb;
                found = 1;
                break;
            }
        }
        if (found) continue;

        if (count >= cap) {
            cap *= 2;
            evemon_lib_t *nb = realloc(buf, cap * sizeof(evemon_lib_t));
            if (!nb) break;
            buf = nb;
        }

        evemon_lib_t *e = &buf[count];
        memset(e, 0, sizeof(*e));
        snprintf(e->path, sizeof(e->path), "%s", p);
        snprintf(e->name, sizeof(e->name), "%s", bname);
        e->size_kb  = seg_kb;
        e->category = 0;

        /* Extract version from .so filenames */
        if (is_so)
            extract_so_version(bname, e->version, sizeof(e->version));

        /* Extract origin from path */
        extract_origin(p, e->origin, sizeof(e->origin));

        count++;
    }
    fclose(f);

    if (count == 0) { free(buf); return; }

    d->lib_buf       = buf;
    d->lib_cap       = count;
    d->data.libs     = buf;
    d->data.lib_count = count;
}

/* ── Thread gathering ─────────────────────────────────────────── */

/*
 * Read per-thread data from /proc/<pid>/task/<tid>/stat and
 * /proc/<pid>/task/<tid>/status for all threads of a process.
 *
 * Fields gathered per thread:
 *   - TID, name, state, priority, nice, last CPU, utime, stime,
 *     starttime, voluntary/nonvoluntary context switches.
 *
 * CPU% is left at 0.0 — it requires cross-snapshot delta computation.
 * The plugin itself can show raw utime/stime for now; future work
 * can add per-thread delta tracking similar to per-process CPU%.
 */
static void gather_threads(pid_t pid, broker_pid_data_t *d)
{
    char task_dir[64];
    snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", pid);

    DIR *dp = opendir(task_dir);
    if (!dp) return;

    size_t cap = 32, count = 0;
    evemon_thread_t *buf = calloc(cap, sizeof(evemon_thread_t));
    if (!buf) { closedir(dp); return; }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;

        pid_t tid = (pid_t)atoi(de->d_name);
        if (tid <= 0) continue;

        if (count >= cap) {
            cap *= 2;
            evemon_thread_t *tmp = realloc(buf, cap * sizeof(evemon_thread_t));
            if (!tmp) break;
            buf = tmp;
        }

        evemon_thread_t *t = &buf[count];
        memset(t, 0, sizeof(*t));
        t->tid = tid;

        /* Read thread name from comm */
        {
            char path[128];
            snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", pid, tid);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(t->name, sizeof(t->name), f)) {
                    size_t len = strlen(t->name);
                    if (len > 0 && t->name[len - 1] == '\n')
                        t->name[len - 1] = '\0';
                }
                fclose(f);
            }
        }

        /* Parse /proc/<pid>/task/<tid>/stat for:
         *   state(3), priority(18), nice(19), utime(14), stime(15),
         *   starttime(22), processor(39)                              */
        {
            char path[128], statbuf[1024];
            snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(statbuf, sizeof(statbuf), f)) {
                    /* Skip past comm field (in parens, may contain spaces) */
                    const char *p = strrchr(statbuf, ')');
                    if (p) {
                        p++;  /* skip ')' */
                        char state_c;
                        long priority, nice;
                        unsigned long utime, stime;
                        unsigned long long starttime;

                        /* Fields 3..22 and 39 */
                        int n = sscanf(p,
                            " %c"           /* 3: state */
                            " %*d %*d %*d"  /* 4-6: ppid, pgrp, session */
                            " %*d %*d"      /* 7-8: tty_nr, tpgid */
                            " %*u"          /* 9: flags */
                            " %*u %*u"      /* 10-11: minflt, cminflt */
                            " %*u %*u"      /* 12-13: majflt, cmajflt */
                            " %lu %lu"      /* 14-15: utime, stime */
                            " %*d %*d"      /* 16-17: cutime, cstime */
                            " %ld %ld"      /* 18-19: priority, nice */
                            " %*d"          /* 20: num_threads */
                            " %*d"          /* 21: itrealvalue */
                            " %llu",        /* 22: starttime */
                            &state_c,
                            &utime, &stime,
                            &priority, &nice,
                            &starttime);
                        if (n >= 6) {
                            t->state = state_c;
                            t->utime = (unsigned long long)utime;
                            t->stime = (unsigned long long)stime;
                            t->priority = (int)priority;
                            t->nice = (int)nice;
                            t->starttime = starttime;
                        }

                        /* Field 39 (processor) — skip fields 23..38 */
                        const char *q = p;
                        /* We already consumed up to field 22.
                         * Scan forward to field 39 by skipping spaces. */
                        int fields_to_skip = 39 - 22 - 1; /* 16 more fields */
                        /* Re-parse from after starttime position.
                         * Easier: just scan the whole line for field 39. */
                        const char *r = p;
                        /* Count fields from position after ')': field 3 starts */
                        int fld = 3;
                        while (*r == ' ') r++;
                        while (*r && fld < 39) {
                            while (*r && *r != ' ') r++;
                            while (*r == ' ') r++;
                            fld++;
                        }
                        if (fld == 39 && *r) {
                            t->processor = atoi(r);
                        }
                        (void)q;
                        (void)fields_to_skip;
                    }
                }
                fclose(f);
            }
        }

        /* Read context switch counts from /proc/<pid>/task/<tid>/status */
        {
            char path[128], line[256];
            snprintf(path, sizeof(path), "/proc/%d/task/%d/status", pid, tid);
            FILE *f = fopen(path, "r");
            if (f) {
                while (fgets(line, sizeof(line), f)) {
                    if (strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
                        t->voluntary_ctxt_switches = strtoull(line + 24, NULL, 10);
                    } else if (strncmp(line, "nonvoluntary_ctxt_switches:", 27) == 0) {
                        t->nonvoluntary_ctxt_switches = strtoull(line + 27, NULL, 10);
                    }
                }
                fclose(f);
            }
        }

        count++;
    }
    closedir(dp);

    if (count == 0) { free(buf); return; }

    d->thread_buf = buf;
    d->thread_cap = cap;
    d->data.threads = buf;
    d->data.thread_count = count;
}

/* Read /proc/<pid>/status */
static void gather_status(pid_t pid, broker_pid_data_t *d)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    read_file_alloc(path, &d->raw_status);
    d->data.raw_status = d->raw_status;
}

/* Read cgroup v2 limits */
static void gather_cgroup(pid_t pid, broker_pid_data_t *d)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/cgroup", pid);

    /* Read raw cgroup for the raw_cgroup field */
    read_file_alloc(proc_path, &d->raw_cgroup);
    d->data.raw_cgroup = d->raw_cgroup;

    /* Parse cgroup v2 path */
    evemon_cgroup_t *cg = &d->cgroup_data;
    memset(cg, 0, sizeof(*cg));
    cg->mem_current = -1;
    cg->mem_max     = -1;
    cg->mem_high    = -1;
    cg->swap_max    = -1;
    cg->cpu_quota   = -1;
    cg->cpu_period  = -1;
    cg->pids_current = -1;
    cg->pids_max     = -1;

    if (!d->raw_cgroup) return;

    /* Find "0::<path>" line */
    const char *line = d->raw_cgroup;
    const char *cgroup_path = NULL;
    while (*line) {
        if (strncmp(line, "0::", 3) == 0) {
            cgroup_path = line + 3;
            break;
        }
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    if (!cgroup_path) return;

    /* Strip trailing newline */
    char cg_path[512];
    snprintf(cg_path, sizeof(cg_path), "%s", cgroup_path);
    char *nl = strchr(cg_path, '\n');
    if (nl) *nl = '\0';
    snprintf(cg->path, sizeof(cg->path), "%s", cg_path);

    /* H2: Reject cgroup paths containing ".." to prevent path traversal */
    if (strstr(cg_path, "..")) {
        fprintf(stderr, "evemon: rejecting cgroup path with '..': %s\n",
                cg_path);
        return;
    }

    /* Build /sys/fs/cgroup/<path> */
    char cg_dir[1024];
    snprintf(cg_dir, sizeof(cg_dir), "/sys/fs/cgroup%s", cg_path);

    /* Read controller files */
    char buf[256];
    FILE *f;

    /* Helper macro */
    #define CG_READ_INT(file, field) do { \
        char p2[1280]; snprintf(p2, sizeof(p2), "%s/%s", cg_dir, file); \
        f = fopen(p2, "r"); \
        if (f) { \
            if (fgets(buf, sizeof(buf), f)) { \
                char *nl2 = strchr(buf, '\n'); if (nl2) *nl2 = '\0'; \
                if (strcmp(buf, "max") != 0) cg->field = strtoll(buf, NULL, 10); \
            } \
            fclose(f); \
        } \
    } while(0)

    CG_READ_INT("memory.current", mem_current);
    CG_READ_INT("memory.max",     mem_max);
    CG_READ_INT("memory.high",    mem_high);
    CG_READ_INT("memory.swap.max", swap_max);
    CG_READ_INT("pids.current",   pids_current);
    CG_READ_INT("pids.max",       pids_max);

    #undef CG_READ_INT

    /* cpu.max: "<quota> <period>" or "max <period>" */
    {
        char p2[1280];
        snprintf(p2, sizeof(p2), "%s/cpu.max", cg_dir);
        f = fopen(p2, "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                char q[32], per[32];
                if (sscanf(buf, "%31s %31s", q, per) == 2) {
                    if (strcmp(q, "max") != 0)
                        cg->cpu_quota = strtoll(q, NULL, 10);
                    cg->cpu_period = strtoll(per, NULL, 10);
                }
            }
            fclose(f);
        }
    }

    /* io.max */
    {
        char p2[1280];
        snprintf(p2, sizeof(p2), "%s/io.max", cg_dir);
        f = fopen(p2, "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                char *nl2 = strchr(buf, '\n');
                if (nl2) *nl2 = '\0';
                snprintf(cg->io_max, sizeof(cg->io_max), "%s", buf);
            }
            fclose(f);
        }
    }

    d->data.cgroup = &d->cgroup_data;
}

/*
 * Collect descendant PIDs by scanning /proc and walking the ppid chain.
 * This is the portable approach — /proc/<pid>/task/<tid>/children
 * requires CONFIG_PROC_CHILDREN which many kernels don't enable.
 */
static void gather_descendants(pid_t pid, broker_pid_data_t *d)
{
    /*
     * Strategy: scan every numeric entry in /proc, read its ppid from
     * /proc/<candidate>/stat, and check if the ppid is either `pid`
     * or any PID we've already accepted as a descendant.  Iterate
     * until no new descendants are found (handles arbitrary depth).
     */
    size_t cap = 64, count = 0;
    pid_t *pids = calloc(cap, sizeof(pid_t));
    if (!pids) return;

    /* Build a list of candidate (pid, ppid) pairs from /proc */
    typedef struct { pid_t p; pid_t pp; } pp_entry_t;
    size_t pc_cap = 256, pc_count = 0;
    pp_entry_t *pc = malloc(pc_cap * sizeof(pp_entry_t));
    if (!pc) { free(pids); return; }

    DIR *proc = opendir("/proc");
    if (!proc) { free(pids); free(pc); return; }

    struct dirent *de;
    while ((de = readdir(proc)) != NULL) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pid_t candidate = (pid_t)atoi(de->d_name);
        if (candidate <= 0 || candidate == pid) continue;

        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", candidate);
        FILE *f = fopen(stat_path, "r");
        if (!f) continue;

        char buf[512];
        pid_t ppid = 0;
        if (fgets(buf, sizeof(buf), f)) {
            /* ppid is field 4; skip past "(comm)" which may contain spaces */
            const char *cp = strrchr(buf, ')');
            if (cp) {
                /* fields after ')': state(3) ppid(4) */
                char st;
                if (sscanf(cp + 1, " %c %d", &st, &ppid) < 2)
                    ppid = 0;
            }
        }
        fclose(f);
        if (ppid <= 0) continue;

        if (pc_count >= pc_cap) {
            pc_cap *= 2;
            pp_entry_t *tmp = realloc(pc, pc_cap * sizeof(pp_entry_t));
            if (!tmp) break;
            pc = tmp;
        }
        pc[pc_count].p  = candidate;
        pc[pc_count].pp = ppid;
        pc_count++;
    }
    closedir(proc);

    /*
     * Iteratively find descendants: on each pass, any candidate whose
     * ppid matches `pid` or an already-accepted descendant is added.
     * Repeat until a pass finds nothing new.
     */
    for (;;) {
        int found = 0;
        for (size_t i = 0; i < pc_count; i++) {
            if (pc[i].p == 0) continue;  /* already consumed */

            int is_desc = (pc[i].pp == pid);
            if (!is_desc) {
                for (size_t j = 0; j < count; j++) {
                    if (pids[j] == pc[i].pp) { is_desc = 1; break; }
                }
            }
            if (!is_desc) continue;

            if (count >= cap) {
                cap *= 2;
                pid_t *tmp = realloc(pids, cap * sizeof(pid_t));
                if (!tmp) break;
                pids = tmp;
            }
            pids[count++] = pc[i].p;
            pc[i].p = 0;  /* mark consumed */
            found = 1;
        }
        if (!found) break;
    }

    free(pc);

    d->desc_pids = pids;
    d->desc_count = count;
    d->desc_cap = cap;
    d->data.descendant_pids = pids;
    d->data.descendant_count = count;
}

/* ── Socket resolution helpers for gather_sockets ────────────── */

typedef enum {
    BSOCK_TCP, BSOCK_UDP, BSOCK_TCP6, BSOCK_UDP6, BSOCK_UNIX, BSOCK_COUNT
} bsock_kind_t;

typedef struct {
    unsigned long inode;
    bsock_kind_t  kind;
    uint32_t      local_addr;
    uint16_t      local_port;
    uint32_t      remote_addr;
    uint16_t      remote_port;
    unsigned char local_addr6[16];
    unsigned char remote_addr6[16];
    uint16_t      local_port6;
    uint16_t      remote_port6;
    int           state;         /* TCP state (1=ESTABLISHED,10=LISTEN) */
} bsock_entry_t;

typedef struct {
    bsock_entry_t *entries;
    size_t         count;
    size_t         cap;
} bsock_table_t;

static void bsock_table_init(bsock_table_t *t) {
    t->entries = NULL; t->count = 0; t->cap = 0;
}

static void bsock_table_free(bsock_table_t *t) {
    free(t->entries); t->entries = NULL; t->count = 0; t->cap = 0;
}

static bsock_entry_t *bsock_table_push(bsock_table_t *t) {
    if (t->count >= t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 256;
        bsock_entry_t *tmp = realloc(t->entries, nc * sizeof(*tmp));
        if (!tmp) return NULL;
        t->entries = tmp; t->cap = nc;
    }
    memset(&t->entries[t->count], 0, sizeof(bsock_entry_t));
    return &t->entries[t->count++];
}

static void bsock_parse_v4(const char *path, bsock_kind_t kind,
                           bsock_table_t *tbl)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        unsigned long inode = 0;
        uint32_t la, ra;
        unsigned int lp, rp, st;
        /* Format: sl local_addr:port remote_addr:port st ... inode ... */
        if (sscanf(line,
                   " %*d: %X:%X %X:%X %X %*X:%*X %*X:%*X %*X %*u %*u %lu",
                   &la, &lp, &ra, &rp, &st, &inode) < 6)
            continue;
        if (inode == 0) continue;
        bsock_entry_t *e = bsock_table_push(tbl);
        if (!e) break;
        e->inode = inode;
        e->kind = kind;
        e->local_addr = la;
        e->local_port = (uint16_t)lp;
        e->remote_addr = ra;
        e->remote_port = (uint16_t)rp;
        e->state = (int)st;
    }
    fclose(f);
}

static void bsock_parse_v6(const char *path, bsock_kind_t kind,
                           bsock_table_t *tbl)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        unsigned long inode = 0;
        unsigned int lp, rp, st;
        unsigned int la6[4], ra6[4];
        if (sscanf(line,
                   " %*d: %8X%8X%8X%8X:%X %8X%8X%8X%8X:%X %X "
                   "%*X:%*X %*X:%*X %*X %*u %*u %lu",
                   &la6[0], &la6[1], &la6[2], &la6[3], &lp,
                   &ra6[0], &ra6[1], &ra6[2], &ra6[3], &rp,
                   &st, &inode) < 12)
            continue;
        if (inode == 0) continue;
        bsock_entry_t *e = bsock_table_push(tbl);
        if (!e) break;
        e->inode = inode;
        e->kind = kind;
        e->local_port6 = (uint16_t)lp;
        e->remote_port6 = (uint16_t)rp;
        e->state = (int)st;
        /* /proc/net stores IPv6 addresses as 4 x 32-bit host-order ints */
        for (int i = 0; i < 4; i++) {
            uint32_t v = la6[i];
            memcpy(e->local_addr6 + i * 4, &v, 4);
            v = ra6[i];
            memcpy(e->remote_addr6 + i * 4, &v, 4);
        }
    }
    fclose(f);
}

static void bsock_table_build(bsock_table_t *tbl)
{
    bsock_table_init(tbl);
    bsock_parse_v4("/proc/net/tcp",  BSOCK_TCP,  tbl);
    bsock_parse_v4("/proc/net/udp",  BSOCK_UDP,  tbl);
    bsock_parse_v6("/proc/net/tcp6", BSOCK_TCP6, tbl);
    bsock_parse_v6("/proc/net/udp6", BSOCK_UDP6, tbl);
}

static const bsock_entry_t *bsock_find(const bsock_table_t *tbl,
                                        unsigned long inode)
{
    for (size_t i = 0; i < tbl->count; i++)
        if (tbl->entries[i].inode == inode) return &tbl->entries[i];
    return NULL;
}

/* Collect socket inodes from /proc/<pid>/fd, tracking the source PID and fd number */
static void collect_sock_inodes(pid_t pid,
                                unsigned long **out, pid_t **out_pids,
                                int **out_fdnums,
                                size_t *cnt, size_t *cap)
{
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", (int)pid);
    DIR *dp = opendir(dirpath);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        int fdnum = atoi(de->d_name);
        char link[288], target[512];
        snprintf(link, sizeof(link), "/proc/%d/fd/%s", (int)pid, de->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        if (strncmp(target, "socket:[", 8) != 0) continue;
        unsigned long ino = strtoul(target + 8, NULL, 10);
        if (ino == 0) continue;
        if (*cnt >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            unsigned long *tmp = realloc(*out, *cap * sizeof(**out));
            if (!tmp) continue;
            *out = tmp;
            pid_t *tmp2 = realloc(*out_pids, *cap * sizeof(pid_t));
            if (!tmp2) continue;
            *out_pids = tmp2;
            int *tmp3 = realloc(*out_fdnums, *cap * sizeof(int));
            if (!tmp3) continue;
            *out_fdnums = tmp3;
        }
        (*out)[*cnt]    = ino;
        (*out_pids)[*cnt] = pid;
        (*out_fdnums)[*cnt] = fdnum;
        (*cnt)++;
    }
    closedir(dp);
}

/* ── Per-inode TCP byte-counter cache ───────────────────────── */
/*
 * Two-level approach, tried in order each gather cycle:
 *
 *  1. INET_DIAG netlink dump (primary)
 *     One system-wide dump of all TCP/TCP6 sockets via
 *     SOCK_DIAG_BY_FAMILY returns struct tcp_info per socket, keyed
 *     by inode.  Available on all standard distro kernels
 *     (CONFIG_INET_DIAG + CONFIG_INET_TCP_DIAG).  No root required.
 *
 *  2. pidfd_open + pidfd_getfd + getsockopt(TCP_INFO) (fallback)
 *     Used when INET_DIAG is unavailable (custom kernels, containers).
 *     Probes each socket fd individually.  Works for same-user
 *     processes without root on kernel >= 5.6.
 *
 * Both paths populate the same inode-keyed cache and produce
 * send_delta / recv_delta by diffing cumulative counters across cycles.
 */

#define TCPCACHE_SIZE 8192

typedef struct {
    unsigned long inode;
    uint64_t      prev_acked;    /* tcpi_bytes_acked from last cycle  */
    uint64_t      prev_received; /* tcpi_bytes_received from last cycle */
    uint64_t      delta_send;
    uint64_t      delta_recv;
} tcp_cache_entry_t;

static tcp_cache_entry_t g_tcp_cache[TCPCACHE_SIZE];
static pthread_mutex_t   g_tcp_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -1 = not yet probed, 0 = unavailable, 1 = available */
static int g_inet_diag_available = -1;

static tcp_cache_entry_t *tcpcache_get(unsigned long inode)
{
    size_t slot = (size_t)(inode * 2654435761UL) % TCPCACHE_SIZE;
    for (size_t i = 0; i < TCPCACHE_SIZE; i++) {
        size_t s = (slot + i) % TCPCACHE_SIZE;
        if (g_tcp_cache[s].inode == inode)
            return &g_tcp_cache[s];
        if (g_tcp_cache[s].inode == 0) {
            g_tcp_cache[s].inode = inode;
            return &g_tcp_cache[s];
        }
    }
    return NULL;
}

static void tcpcache_update(unsigned long inode,
                            uint64_t acked, uint64_t received)
{
    tcp_cache_entry_t *ce = tcpcache_get(inode);
    if (!ce) return;
    if (ce->prev_acked > 0 || ce->prev_received > 0) {
        ce->delta_send = (acked    >= ce->prev_acked)
                       ? acked    - ce->prev_acked    : 0;
        ce->delta_recv = (received >= ce->prev_received)
                       ? received - ce->prev_received : 0;
    }
    ce->prev_acked    = acked;
    ce->prev_received = received;
}

/*
 * INET_DIAG primary path.
 * Dumps all TCP sockets for the given address family, updating the
 * inode cache with fresh byte counters.
 * Returns 1 on success, 0 if INET_DIAG is unavailable.
 */
static int inet_diag_dump(int nlfd, int family)
{
    struct {
        struct nlmsghdr         nlh;
        struct inet_diag_req_v2 r;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = sizeof(req);
    req.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = (uint32_t)family;
    req.r.sdiag_family   = (uint8_t)family;
    req.r.sdiag_protocol = IPPROTO_TCP;
    req.r.idiag_ext      = (1 << (INET_DIAG_INFO - 1));
    req.r.idiag_states   = 0xffffffff;

    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (sendto(nlfd, &req, sizeof(req), 0,
               (struct sockaddr *)&sa, sizeof(sa)) < 0)
        return 0;

    char buf[65536];
    for (;;) {
        int n = (int)recv(nlfd, buf, sizeof(buf), 0);
        if (n <= 0) return 0;
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE)  return 1;
            if (h->nlmsg_type == NLMSG_ERROR) return 0;
            if (h->nlmsg_type != SOCK_DIAG_BY_FAMILY) continue;

            struct inet_diag_msg *m =
                (struct inet_diag_msg *)NLMSG_DATA(h);
            int rta_len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*m));
            struct rtattr *attr = (struct rtattr *)(m + 1);
            for (; RTA_OK(attr, rta_len); attr = RTA_NEXT(attr, rta_len)) {
                if (attr->rta_type != INET_DIAG_INFO) continue;
                if (RTA_PAYLOAD(attr) < sizeof(struct tcp_info)) continue;
                struct tcp_info *ti = (struct tcp_info *)RTA_DATA(attr);
                tcpcache_update(m->idiag_inode,
                                ti->tcpi_bytes_acked,
                                ti->tcpi_bytes_received);
            }
        }
    }
}

/*
 * Run INET_DIAG for both AF_INET and AF_INET6.
 * Opens a fresh netlink socket each cycle (cheap; avoids stale state).
 * Returns 1 if successful, 0 if INET_DIAG is not available.
 */
static int inet_diag_refresh_cache(void)
{
    int nlfd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
                      NETLINK_INET_DIAG);
    if (nlfd < 0) {
        g_inet_diag_available = 0;
        return 0;
    }
    struct sockaddr_nl local = { .nl_family = AF_NETLINK };
    if (bind(nlfd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(nlfd);
        g_inet_diag_available = 0;
        return 0;
    }

    int ok4 = inet_diag_dump(nlfd, AF_INET);
    int ok6 = inet_diag_dump(nlfd, AF_INET6);
    close(nlfd);

    if (!ok4 && !ok6) {
        g_inet_diag_available = 0;
        return 0;
    }
    g_inet_diag_available = 1;
    return 1;
}

/*
 * pidfd fallback: probe a single socket fd from another process.
 * Returns 1 and fills *acked/*received on success.
 */
static int tcp_info_probe(pid_t pid, int fdnum,
                          uint64_t *acked, uint64_t *received)
{
    int pidfd = sys_pidfd_open(pid, 0);
    if (pidfd < 0) return 0;

    int sockfd = sys_pidfd_getfd(pidfd, fdnum, 0);
    close(pidfd);
    if (sockfd < 0) return 0;

    struct tcp_info ti;
    socklen_t tilen = sizeof(ti);
    int rc = getsockopt(sockfd, IPPROTO_TCP, TCP_INFO, &ti, &tilen);
    close(sockfd);
    if (rc != 0) return 0;

    *acked    = ti.tcpi_bytes_acked;
    *received = ti.tcpi_bytes_received;
    return 1;
}

/* Gather network sockets with eBPF throughput + pidfd TCP_INFO */
static void gather_sockets(pid_t pid, void *fdmon, broker_pid_data_t *d)
{
    /* 1. Collect socket inodes from this PID and descendants */
    unsigned long *inodes = NULL;
    pid_t *inode_pids = NULL;
    int   *inode_fdnums = NULL;
    size_t inode_count = 0, inode_cap = 0;

    collect_sock_inodes(pid, &inodes, &inode_pids, &inode_fdnums,
                        &inode_count, &inode_cap);

    /* Also collect from descendant PIDs */
    for (size_t i = 0; i < d->desc_count; i++)
        collect_sock_inodes(d->data.descendant_pids[i],
                            &inodes, &inode_pids, &inode_fdnums,
                            &inode_count, &inode_cap);

    if (inode_count == 0) {
        free(inodes);
        free(inode_pids);
        free(inode_fdnums);
        return;
    }

    /* 2. Build the system socket table from /proc/net */
    bsock_table_t socktbl;
    bsock_table_build(&socktbl);

    /* 2b. Refresh INET_DIAG cache (primary TCP/TCP6 speed source).
     *     One netlink round-trip populates g_tcp_cache for all sockets.
     *     Falls back gracefully if CONFIG_INET_DIAG is not available. */
    pthread_mutex_lock(&g_tcp_cache_mutex);
    inet_diag_refresh_cache();
    pthread_mutex_unlock(&g_tcp_cache_mutex);

    /* 3. Get per-socket throughput from eBPF (H5: heap-allocated) */
    size_t sock_io_cap = 1024;
    fdmon_sock_io_t *sock_io = calloc(sock_io_cap, sizeof(fdmon_sock_io_t));
    if (!sock_io) {
        free(inodes); free(inode_pids);
        bsock_table_free(&socktbl);
        return;
    }
    size_t sock_io_count = sock_io_cap;
    if (fdmon)
        fdmon_sock_io_list((fdmon_ctx_t *)fdmon, pid, sock_io, &sock_io_count);
    else
        sock_io_count = 0;

    /* Also from descendants */
    for (size_t i = 0; i < d->desc_count && sock_io_count < sock_io_cap; i++) {
        size_t remaining = sock_io_cap - sock_io_count;
        size_t dc = remaining < 256 ? remaining : 256;
        fdmon_sock_io_t *desc_io = calloc(dc, sizeof(fdmon_sock_io_t));
        if (!desc_io) break;
        if (fdmon)
            fdmon_sock_io_list((fdmon_ctx_t *)fdmon,
                               d->data.descendant_pids[i], desc_io, &dc);
        else
            dc = 0;
        for (size_t j = 0; j < dc && sock_io_count < sock_io_cap; j++)
            sock_io[sock_io_count++] = desc_io[j];
        free(desc_io);
    }

    /* 4. Resolve each inode and build the socket list */
    size_t cap = 32, count = 0;
    evemon_socket_t *socks = calloc(cap, sizeof(evemon_socket_t));
    if (!socks) { free(inodes); free(inode_pids); free(inode_fdnums); free(sock_io); bsock_table_free(&socktbl); return; }

    /* Deduplicate inodes (multiple fds can point to same socket) */
    for (size_t i = 0; i < inode_count; i++) {
        int dup = 0;
        for (size_t j = 0; j < i; j++)
            if (inodes[j] == inodes[i]) { dup = 1; break; }
        if (dup) continue;

        const bsock_entry_t *si = bsock_find(&socktbl, inodes[i]);
        if (!si) continue;  /* unix socket or unresolved — skip */

        if (count >= cap) {
            cap *= 2;
            evemon_socket_t *tmp = realloc(socks, cap * sizeof(*tmp));
            if (!tmp) break;
            socks = tmp;
        }

        evemon_socket_t *s = &socks[count];
        memset(s, 0, sizeof(*s));
        s->source_pid = inode_pids[i];

        /* Build description string */
        static const unsigned char v4mapped[12] =
            {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
        static const unsigned char zeroes[16] = {0};

        switch (si->kind) {
        case BSOCK_TCP:
        case BSOCK_UDP: {
            const char *proto = (si->kind == BSOCK_TCP) ? "TCP" : "UDP";
            char la[INET_ADDRSTRLEN], ra[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &si->local_addr, la, sizeof(la));
            inet_ntop(AF_INET, &si->remote_addr, ra, sizeof(ra));
            if (si->remote_addr == 0 && si->remote_port == 0)
                snprintf(s->desc, sizeof(s->desc), "%s %s:%u (listening)",
                         proto, la, si->local_port);
            else if (si->kind == BSOCK_TCP && si->state == 10)
                snprintf(s->desc, sizeof(s->desc), "%s %s:%u (listening)",
                         proto, la, si->local_port);
            else
                snprintf(s->desc, sizeof(s->desc), "%s %s:%u → %s:%u",
                         proto, la, si->local_port, ra, si->remote_port);
            break;
        }
        case BSOCK_TCP6:
        case BSOCK_UDP6: {
            const char *proto = (si->kind == BSOCK_TCP6) ? "TCP6" : "UDP6";
            char la[INET6_ADDRSTRLEN], ra[INET6_ADDRSTRLEN];
            /* Detect v4-mapped addresses and display as IPv4 */
            if (memcmp(si->local_addr6, v4mapped, 12) == 0)
                inet_ntop(AF_INET, si->local_addr6 + 12, la, sizeof(la));
            else
                inet_ntop(AF_INET6, si->local_addr6, la, sizeof(la));
            if (memcmp(si->remote_addr6, v4mapped, 12) == 0)
                inet_ntop(AF_INET, si->remote_addr6 + 12, ra, sizeof(ra));
            else
                inet_ntop(AF_INET6, si->remote_addr6, ra, sizeof(ra));

            if (memcmp(si->remote_addr6, zeroes, 16) == 0 &&
                si->remote_port6 == 0)
                snprintf(s->desc, sizeof(s->desc), "%s [%s]:%u (listening)",
                         proto, la, si->local_port6);
            else if ((si->kind == BSOCK_TCP6) && si->state == 10)
                snprintf(s->desc, sizeof(s->desc), "%s [%s]:%u (listening)",
                         proto, la, si->local_port6);
            else
                snprintf(s->desc, sizeof(s->desc), "%s [%s]:%u → [%s]:%u",
                         proto, la, si->local_port6, ra, si->remote_port6);
            break;
        }
        default:
            snprintf(s->desc, sizeof(s->desc), "socket:[%lu]", inodes[i]);
            break;
        }

        /* ── Speed data ──────────────────────────────────────────────
         *
         * Priority:
         *   1. INET_DIAG cache  – populated above the inode loop for all
         *      TCP/TCP6 sockets in one netlink round-trip (distro kernels).
         *   2. pidfd fallback   – per-fd getsockopt(TCP_INFO) for custom
         *      kernels without CONFIG_INET_DIAG (same-user processes only).
         *   3. eBPF             – covers UDP4 and any socket where both
         *      methods above failed (different-user, EPERM on pidfd_getfd).
         * ─────────────────────────────────────────────────────────────── */

        int speed_found = 0;

        if (si->kind == BSOCK_TCP || si->kind == BSOCK_TCP6) {
            /* Check INET_DIAG cache first (already refreshed above) */
            pthread_mutex_lock(&g_tcp_cache_mutex);
            tcp_cache_entry_t *ce = tcpcache_get(inodes[i]);
            if (ce && (ce->delta_send || ce->delta_recv)) {
                s->send_delta = ce->delta_send;
                s->recv_delta = ce->delta_recv;
                s->total      = ce->delta_send + ce->delta_recv;
                speed_found   = 1;
            }
            pthread_mutex_unlock(&g_tcp_cache_mutex);

            /* pidfd fallback when INET_DIAG unavailable */
            if (!speed_found && !g_inet_diag_available) {
                int is_connected = (si->kind == BSOCK_TCP)
                    ? (si->remote_addr != 0 || si->remote_port != 0)
                    : (memcmp(si->remote_addr6, zeroes, 16) != 0 ||
                       si->remote_port6 != 0);
                if (is_connected) {
                    uint64_t acked = 0, received = 0;
                    if (tcp_info_probe(inode_pids[i], inode_fdnums[i],
                                       &acked, &received)) {
                        pthread_mutex_lock(&g_tcp_cache_mutex);
                        tcpcache_update(inodes[i], acked, received);
                        tcp_cache_entry_t *ce2 = tcpcache_get(inodes[i]);
                        if (ce2) {
                            s->send_delta = ce2->delta_send;
                            s->recv_delta = ce2->delta_recv;
                            s->total      = ce2->delta_send + ce2->delta_recv;
                        }
                        pthread_mutex_unlock(&g_tcp_cache_mutex);
                        speed_found = 1;
                    }
                }
            }
        }

        /* eBPF fallback: UDP4/UDP6 data + any socket missed above.
         * Match by inode (primary) — works for all protocols/address families.
         * Fall back to IPv4 4-tuple if the inode is unavailable (old events). */
        if (!speed_found) {
            for (size_t k = 0; k < sock_io_count; k++) {
                const fdmon_sock_io_t *sio = &sock_io[k];
                int matched = 0;

                if (sio->inode && sio->inode == inodes[i]) {
                    /* Inode match — works for UDP4, UDP6, TCP6, everything */
                    matched = 1;
                } else if (!sio->inode) {
                    /* Legacy fallback: IPv4 4-tuple */
                    uint16_t bpf_rport_host = ntohs(sio->rport);
                    if ((si->kind == BSOCK_TCP || si->kind == BSOCK_UDP) &&
                        si->local_addr == sio->laddr &&
                        si->local_port == sio->lport &&
                        si->remote_addr == sio->raddr &&
                        si->remote_port == bpf_rport_host)
                        matched = 1;
                }

                if (matched) {
                    s->send_delta = sio->delta_send;
                    s->recv_delta = sio->delta_recv;
                    s->total = sio->delta_send + sio->delta_recv;
                    break;
                }
            }
        }

        count++;
    }

    free(inodes);
    free(inode_pids);
    free(inode_fdnums);
    free(sock_io);
    bsock_table_free(&socktbl);

    /* Sort by total throughput descending, then alphabetically */
    if (count > 1) {
        for (size_t i = 0; i < count - 1; i++) {
            for (size_t j = i + 1; j < count; j++) {
                int swap = 0;
                if (socks[j].total > socks[i].total)
                    swap = 1;
                else if (socks[j].total == socks[i].total &&
                         strcmp(socks[i].desc, socks[j].desc) > 0)
                    swap = 1;
                if (swap) {
                    evemon_socket_t tmp = socks[i];
                    socks[i] = socks[j];
                    socks[j] = tmp;
                }
            }
        }
    }

    d->sock_buf = socks;
    d->sock_cap = cap;
    d->data.sockets = socks;
    d->data.socket_count = count;
}

/* Gather file descriptors */
/* Read FDs from a single PID into the growing fds array */
static void gather_fds_for_pid(pid_t pid, pid_t source,
                               evemon_fd_t **fds, size_t *count,
                               size_t *cap)
{
    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", pid);

    DIR *dp = opendir(fd_dir);
    if (!dp) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;

        int fd_num = atoi(de->d_name);
        char link_path[128], target[512];
        snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%s",
                 pid, de->d_name);
        ssize_t len = readlink(link_path, target, sizeof(target) - 1);
        if (len <= 0) continue;
        target[len] = '\0';

        if (*count >= *cap) {
            *cap *= 2;
            evemon_fd_t *tmp = realloc(*fds, *cap * sizeof(evemon_fd_t));
            if (!tmp) break;
            *fds = tmp;
        }

        evemon_fd_t *e = &(*fds)[*count];
        e->fd = fd_num;
        snprintf(e->path, sizeof(e->path), "%s", target);
        e->desc[0] = '\0';
        if (strncmp(target, "/dev/", 5) == 0)
            label_device(target, e->desc, sizeof(e->desc));
        e->category = 0;
        e->net_sort_key = 0;
        e->source_pid = source;
        (*count)++;
    }
    closedir(dp);
}

static void gather_fds(pid_t pid, broker_pid_data_t *d)
{
    size_t cap = 64, count = 0;
    evemon_fd_t *fds = calloc(cap, sizeof(evemon_fd_t));
    if (!fds) return;

    /* Root PID's FDs */
    gather_fds_for_pid(pid, pid, &fds, &count, &cap);

    /* Descendant FDs (if descendants were gathered) */
    for (size_t i = 0; i < d->desc_count; i++)
        gather_fds_for_pid(d->data.descendant_pids[i],
                           d->data.descendant_pids[i],
                           &fds, &count, &cap);

    d->fd_buf = fds;
    d->fd_cap = cap;
    d->data.fds = fds;
    d->data.fd_count = count;
}

/* ── PipeWire graph snapshot → evemon_pw_* conversion ────────── */

#ifdef HAVE_PIPEWIRE
static void gather_pipewire(broker_pid_data_t *d, const pw_graph_t *graph)
{

    /* Convert pw_snap_node_t[] → evemon_pw_node_t[] */
    if (graph->node_count > 0) {
        d->pw_nodes = calloc(graph->node_count, sizeof(evemon_pw_node_t));
        if (d->pw_nodes) {
            d->pw_node_cap = graph->node_count;
            for (size_t i = 0; i < graph->node_count; i++) {
                const pw_snap_node_t *s = &graph->nodes[i];
                evemon_pw_node_t *n = &d->pw_nodes[i];
                n->id        = s->id;
                n->client_id = s->client_id;
                n->pid       = s->pid;
                memcpy(n->app_name,    s->app_name,    sizeof(n->app_name));
                memcpy(n->node_name,   s->node_name,   sizeof(n->node_name));
                memcpy(n->node_desc,   s->node_desc,   sizeof(n->node_desc));
                memcpy(n->media_class, s->media_class, sizeof(n->media_class));
                memcpy(n->media_name,  s->media_name,  sizeof(n->media_name));
            }
            d->data.pw_nodes = d->pw_nodes;
            d->data.pw_node_count = graph->node_count;

            for (size_t i = 0; i < graph->node_count; i++) {
                const evemon_pw_node_t *n = &d->pw_nodes[i];
                if (n->pid != d->pid) continue;
                evemon_log(LOG_AUDIO, "[pw] pid=%-6d id=%-5u  class=%-30s  media=%-30s  node=%s",
                           (int)n->pid, n->id,
                           n->media_class[0] ? n->media_class : "-",
                           n->media_name[0]  ? n->media_name  : "-",
                           n->node_name[0]   ? n->node_name   : "-");
            }
        }
    }

    /* Convert pw_snap_link_t[] → evemon_pw_link_t[] */
    if (graph->link_count > 0) {
        d->pw_links = calloc(graph->link_count, sizeof(evemon_pw_link_t));
        if (d->pw_links) {
            d->pw_link_cap = graph->link_count;
            for (size_t i = 0; i < graph->link_count; i++) {
                const pw_snap_link_t *s = &graph->links[i];
                evemon_pw_link_t *l = &d->pw_links[i];
                l->output_node = s->output_node_id;
                l->output_port = s->output_port_id;
                l->input_node  = s->input_node_id;
                l->input_port  = s->input_port_id;
            }
            d->data.pw_links = d->pw_links;
            d->data.pw_link_count = graph->link_count;
        }
    }

    /* Convert pw_snap_port_t[] → evemon_pw_port_t[] */
    if (graph->port_count > 0) {
        d->pw_ports = calloc(graph->port_count, sizeof(evemon_pw_port_t));
        if (d->pw_ports) {
            d->pw_port_cap = graph->port_count;
            for (size_t i = 0; i < graph->port_count; i++) {
                const pw_snap_port_t *s = &graph->ports[i];
                evemon_pw_port_t *p = &d->pw_ports[i];
                p->id      = s->id;
                p->node_id = s->node_id;
                memcpy(p->port_name,   s->port_name,   sizeof(p->port_name));
                memcpy(p->direction,   s->direction,    sizeof(p->direction));
                memcpy(p->format_dsp,  s->format_dsp,   sizeof(p->format_dsp));
            }
            d->data.pw_ports = d->pw_ports;
            d->data.pw_port_count = graph->port_count;
        }
    }
}
#endif /* HAVE_PIPEWIRE */

/* ── pthread worker thread ──────────────────────────────────── */

static int broker_is_cancelled(void)
{
    pthread_mutex_lock(&g_broker_mutex);
    int c = g_broker_cancelled;
    pthread_mutex_unlock(&g_broker_mutex);
    return c;
}

static void *broker_thread_func(void *arg)
{
    broker_cycle_t *cycle = arg;
    pthread_setname_np(pthread_self(), "ev-broker");

    if (broker_is_cancelled())
        goto cancelled;

#ifdef HAVE_PIPEWIRE
    /* Take one PipeWire graph snapshot for the entire cycle.
     * The graph is global (not per-PID), so sharing it across all
     * PIDs avoids N redundant PW connections per cycle. */
    pw_graph_t pw_graph;
    int pw_have_graph = 0;
    if (cycle->needs & evemon_NEED_PIPEWIRE)
        pw_have_graph = (pw_snapshot(&pw_graph) == 0);
#endif

    for (size_t i = 0; i < cycle->pid_count; i++) {
        if (broker_is_cancelled()) {
#ifdef HAVE_PIPEWIRE
            goto cleanup;   /* frees pw_graph if taken */
#else
            goto cancelled;
#endif
        }

        broker_pid_data_t *d = &cycle->results[i];
        d->pid = cycle->pids[i];
        memset(&d->data, 0, sizeof(d->data));
        d->data.pid = cycle->pids[i];

        evemon_data_needs_t needs = cycle->needs;

        if (needs & evemon_NEED_DESCENDANTS)
            gather_descendants(d->pid, d);

        if (needs & evemon_NEED_FDS)
            gather_fds(d->pid, d);

        if (needs & evemon_NEED_ENV)
            gather_environ(d->pid, d);

        if (needs & evemon_NEED_MMAP)
            gather_maps(d->pid, d);

        if (needs & evemon_NEED_LIBS)
            gather_libs(d->pid, d);

        if (needs & evemon_NEED_SOCKETS)
            gather_sockets(d->pid, cycle->fdmon, d);

        if (needs & evemon_NEED_CGROUP)
            gather_cgroup(d->pid, d);

        if (needs & evemon_NEED_STATUS)
            gather_status(d->pid, d);

        if (needs & evemon_NEED_THREADS)
            gather_threads(d->pid, d);

#ifdef HAVE_PIPEWIRE
        if ((needs & evemon_NEED_PIPEWIRE) && pw_have_graph)
            gather_pipewire(d, &pw_graph);
#endif

        /* MPRIS media metadata (uses GDBus, safe from worker thread) */
        if (needs & evemon_NEED_MPRIS) {
            evemon_mpris_data_t mpris_out;
            int rc = mpris_scan_for_pid(d->pid,
                                        d->data.descendant_pids,
                                        d->data.descendant_count,
                                        &mpris_out);
            evemon_log(LOG_DEBUG, "[BROKER MPRIS] pid=%d rc=%d players=%zu",
                       (int)d->pid, rc, mpris_out.player_count);
            if (rc == 0 && mpris_out.player_count > 0) {
                d->mpris_players = calloc(mpris_out.player_count,
                                          sizeof(evemon_mpris_player_t));
                if (d->mpris_players) {
                    memcpy(d->mpris_players, mpris_out.players,
                           mpris_out.player_count * sizeof(evemon_mpris_player_t));
                    d->mpris_player_count = mpris_out.player_count;
                    d->data.mpris_players = d->mpris_players;
                    d->data.mpris_player_count = mpris_out.player_count;
                }
            }
        }

        /* Store fdmon context for evemon_net_io_get() */
        d->data.fdmon = cycle->fdmon;
    }

#ifdef HAVE_PIPEWIRE
    if (broker_is_cancelled())
        goto cleanup;

    /* Extract system-wide audio PIDs from the PW graph before freeing it.
     * This replaces the old approach of calling pw_snapshot() a second
     * time on the main thread, which raced with this worker thread. */
    if (pw_have_graph) {
        size_t acap = 64;
        pid_t *apids = calloc(acap, sizeof(pid_t));
        size_t acount = 0;
        if (apids) {
            for (size_t ai = 0; ai < pw_graph.node_count; ai++) {
                const pw_snap_node_t *an = &pw_graph.nodes[ai];
                if (an->pid <= 0) continue;
                if (!strstr(an->media_class, "Audio") ||
                    !strstr(an->media_class, "Stream"))
                    continue;
                /* De-duplicate */
                int adup = 0;
                for (size_t aj = 0; aj < acount; aj++) {
                    if (apids[aj] == an->pid) { adup = 1; break; }
                }
                if (adup) continue;
                if (acount >= acap) {
                    acap *= 2;
                    pid_t *atmp = realloc(apids, acap * sizeof(pid_t));
                    if (!atmp) break;
                    apids = atmp;
                }
                apids[acount++] = an->pid;
            }
        }
        cycle->audio_pids      = apids;
        cycle->audio_pid_count = acount;

        pw_graph_free(&pw_graph);
    }

    goto done;
cleanup:
    if (pw_have_graph)
        pw_graph_free(&pw_graph);
done:
    ;
#endif /* HAVE_PIPEWIRE */

    cycle->result_count = cycle->pid_count;

cancelled:
    /* Reached either after a clean gather or after cancel.
     * Check the flag to decide whether to dispatch or just free. */
    {
        pthread_mutex_lock(&g_broker_mutex);
        int was_cancelled = g_broker_cancelled;
        g_broker_running = 0;
        pthread_mutex_unlock(&g_broker_mutex);

        if (!was_cancelled && g_broker_complete_cb)
            g_broker_complete_cb(cycle, g_broker_complete_data);
        else
            broker_cycle_free(cycle);
    }

    return NULL;
}

/* ── Main-thread dispatch (called by the frontend hook) ─────── */

/*
 * broker_dispatch_cycle() — call this on the main/UI thread after
 * receiving a completed cycle from the broker_complete_cb hook.
 * Dispatches data to plugins and delivers audio PIDs.
 * The caller is responsible for calling broker_cycle_free() afterwards.
 */
void broker_dispatch_cycle(broker_cycle_t *cycle)
{
    if (!cycle) return;

    /* Dispatch results to plugins */
    for (size_t i = 0; i < cycle->result_count; i++) {
        broker_pid_data_t *d = &cycle->results[i];
        plugin_dispatch_update(cycle->reg, d->pid, &d->data);
    }

    /* Deliver audio PIDs to the UI (main thread, safe to write) */
    if (g_audio_pids_cb && cycle->audio_pids)
        g_audio_pids_cb(cycle->audio_pids, cycle->audio_pid_count,
                        g_audio_pids_data);
}

void broker_cycle_free(broker_cycle_t *cycle)
{
    if (!cycle) return;

    for (size_t i = 0; i < cycle->pid_count; i++)
        broker_pid_data_free(&cycle->results[i]);
    free(cycle->results);
    free(cycle->pids);
    free(cycle->audio_pids);
    free(cycle);
}

/* ── Public API ──────────────────────────────────────────────── */

void broker_set_complete_callback(broker_complete_cb cb, void *user_data)
{
    g_broker_complete_cb   = cb;
    g_broker_complete_data = user_data;
}

void broker_start(plugin_registry_t *reg, void *fdmon)
{
    if (!reg || reg->count == 0)
        return;

    /* Cancel previous cycle — join so we don't orphan threads */
    broker_cancel();

    /* Collect unique tracked PIDs — dynamically sized */
    size_t pid_cap = reg->count > 0 ? reg->count : 16;
    pid_t *pids = calloc(pid_cap, sizeof(pid_t));
    if (!pids) return;
    size_t npids = plugin_collect_tracked_pids(reg, pids, pid_cap);

    /* Even when no plugin instance is tracking a PID (nothing
     * double-clicked yet), we still need to run the cycle if the
     * audio callback is registered so the worker thread can take a
     * PipeWire graph snapshot and extract system-wide audio PIDs. */
    int need_audio_only = (npids == 0 && g_audio_pids_cb != NULL
                           && (reg->combined_needs & evemon_NEED_PIPEWIRE));
    if (npids == 0 && !need_audio_only) {
        free(pids);
        return;
    }

    /* Allocate cycle */
    broker_cycle_t *cycle = calloc(1, sizeof(broker_cycle_t));
    if (!cycle) { free(pids); return; }

    cycle->reg   = reg;
    cycle->fdmon = fdmon;
    /* Narrow the gather to only what currently-active plugins need.
     * Plugins that implement wants_update() and return 0 are excluded,
     * so we skip expensive /proc reads for data no visible tab uses.
     * Falls back to combined_needs when all plugins want data.
     * The audio-only PipeWire snapshot always uses combined_needs so
     * the audio-PID callback fires even when no tab is visible. */
    cycle->needs = need_audio_only
                 ? reg->combined_needs
                 : plugin_registry_effective_needs(reg);

    if (npids > 0) {
        cycle->pids = calloc(npids, sizeof(pid_t));
        if (!cycle->pids) { free(cycle); free(pids); return; }
        memcpy(cycle->pids, pids, npids * sizeof(pid_t));

        cycle->results = calloc(npids, sizeof(broker_pid_data_t));
        if (!cycle->results) { free(cycle->pids); free(cycle); free(pids); return; }
    }
    cycle->pid_count = npids;
    free(pids);  /* local copy no longer needed */

    /* Arm the cancel flag and launch a detached pthread */
    pthread_mutex_lock(&g_broker_mutex);
    g_broker_cancelled = 0;
    g_broker_running   = 1;
    pthread_mutex_unlock(&g_broker_mutex);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&g_broker_thread, &attr,
                       broker_thread_func, cycle) != 0) {
        pthread_attr_destroy(&attr);
        pthread_mutex_lock(&g_broker_mutex);
        g_broker_running = 0;
        pthread_mutex_unlock(&g_broker_mutex);
        broker_cycle_free(cycle);
        return;
    }
    pthread_attr_destroy(&attr);
}

void broker_set_audio_callback(broker_audio_pids_cb cb, void *user_data)
{
    g_audio_pids_cb   = cb;
    g_audio_pids_data = user_data;
}

void broker_cancel(void)
{
    /* Signal the worker to stop */
    pthread_mutex_lock(&g_broker_mutex);
    int was_running = g_broker_running;
    if (was_running)
        g_broker_cancelled = 1;
    pthread_mutex_unlock(&g_broker_mutex);

    /* The worker is detached — we cannot join it.  Setting the cancel
     * flag is sufficient; the worker will see it on its next check and
     * free the cycle itself without calling the completion callback. */
    (void)was_running;
}

void broker_destroy(void)
{
    broker_cancel();

    /* Clear callbacks so no stale pointer is used if the worker
     * fires its completion after shutdown begins. */
    g_broker_complete_cb   = NULL;
    g_broker_complete_data = NULL;
    g_audio_pids_cb        = NULL;
    g_audio_pids_data      = NULL;
}
