/*
 * monitor.c – background thread that reads /proc to build a process snapshot.
 *
 * The monitor thread loops every POLL_INTERVAL_MS, scans /proc for numeric
 * entries (PIDs), reads each process's comm and cmdline, and publishes a
 * new snapshot under the shared lock so the UI thread can consume it.
 */

#include "proc.h"
#include "steam.h"
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
#include <sys/stat.h>

#define POLL_INTERVAL_MS 2000

/* ── helpers ─────────────────────────────────────────────────── */

/*
 * Replace any byte sequences that are not valid UTF-8 with '?' in-place.
 * This prevents GTK/Pango from crashing on raw binary cmdline arguments
 * (e.g. Wine, JVM, or processes that embed binary data in argv).
 */
static void utf8_sanitize(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            /* ASCII — always valid */
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            /* 2-byte sequence: 110xxxxx 10xxxxxx */
            if ((p[1] & 0xC0) == 0x80 && (*p & 0x1F) >= 0x02) {
                p += 2;
            } else {
                *p++ = '?';
            }
        } else if ((*p & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                p += 3;
            } else {
                *p++ = '?';
            }
        } else if ((*p & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                (p[3] & 0xC0) == 0x80) {
                p += 4;
            } else {
                *p++ = '?';
            }
        } else {
            /* Invalid lead byte */
            *p++ = '?';
        }
    }
}

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

/*
 * Read /proc/<pid>/cmdline (NUL-delimited) into the strtab.
 * NUL separators are replaced with spaces.  Returns the str_off_t of
 * the stored string, or 0 on failure / empty (kernel thread).
 * The comm name is passed as a fallback for kernel threads.
 */
static str_off_t read_cmdline_to_strtab(pid_t pid, const char *comm,
                                        proc_strtab_t *strtab)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        goto fallback;

    /* Read the full cmdline, capped at 4 KiB to handle pathological cases */
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); goto fallback; }

    size_t used = 0;
    for (;;) {
        size_t n = fread(buf + used, 1, cap - used - 1, f);
        used += n;
        if (n == 0) break;
        if (used >= cap - 1) {
            /* grow if needed, cap at 16 KiB */
            if (cap >= 16384) break;
            size_t newcap = cap * 2;
            char *nb = realloc(buf, newcap);
            if (!nb) break;
            buf = nb;
            cap = newcap;
        }
    }
    fclose(f);

    if (used == 0) {
        free(buf);
        goto fallback;
    }

    /* NUL → space */
    for (size_t i = 0; i < used - 1; i++)
        if (buf[i] == '\0') buf[i] = ' ';
    buf[used] = '\0';

    utf8_sanitize(buf);

    str_off_t off = strtab_push(strtab, buf);
    free(buf);
    return off;

fallback:
    /* Kernel thread: show [comm] */
    if (comm && comm[0]) {
        char fb[PROC_NAME_MAX + 3];
        snprintf(fb, sizeof(fb), "[%s]", comm);
        return strtab_push(strtab, fb);
    }
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

/* Read the owning user of /proc/<pid> via Uid from status + getpwuid_r.
 * Writes result into buf and returns 0 on success. */
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

    struct passwd pwbuf, *result = NULL;
    char pwstore[1024];
    if (getpwuid_r(uid, &pwbuf, pwstore, sizeof(pwstore), &result) == 0
        && result)
        snprintf(buf, bufsz, "%s", result->pw_name);
    else
        snprintf(buf, bufsz, "%u", (unsigned)uid);

    return 0;
}

/* Read the current working directory of /proc/<pid>/cwd via readlink
 * and push it into the strtab.  Returns the str_off_t of the stored
 * string, or 0 on error.
 */
static str_off_t read_cwd_to_strtab(pid_t pid, proc_strtab_t *strtab)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cwd", pid);

    /* Try with a 4 KiB buffer first (PATH_MAX = 4096 on Linux) */
    char buf[4096];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0)
        return 0;
    buf[n] = '\0';

    if ((size_t)n < sizeof(buf) - 1) {
        /* Common case: fits in stack buffer */
        return strtab_push(strtab, buf);
    }

    /* Extremely long path — try a heap buffer */
    size_t cap = 8192;
    char *heap = malloc(cap);
    if (!heap)
        return strtab_push(strtab, buf);  /* use truncated copy */
    ssize_t hn = readlink(path, heap, cap - 1);
    str_off_t off;
    if (hn >= 0) {
        heap[hn] = '\0';
        off = strtab_push(strtab, heap);
    } else {
        off = strtab_push(strtab, buf);   /* fallback to truncated */
    }
    free(heap);
    return off;
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

/*
 * Detect whether a process is running inside a Linux container.
 *
 * Strategy (two-pronged):
 *  1. PID-namespace check – compare the inode of /proc/<pid>/ns/pid with
 *     that of /proc/1/ns/pid.  If they differ the process lives in a
 *     different PID namespace, which is the kernel-level definition of
 *     "in a container".
 *  2. Cgroup heuristic – scan /proc/<pid>/cgroup for well-known path
 *     fragments that container runtimes inject (docker, lxc, podman,
 *     kubepods, containerd, garden, buildkit, etc.).
 *
 * The cgroup check runs first because it can tell us *which* runtime is
 * in use.  The namespace check is a fallback for runtimes that don't
 * leave an obvious cgroup breadcrumb.
 *
 * Writes a short label into buf: "docker", "lxc", "podman", "k8s",
 * "containerd", "nspawn", "container" (generic), or "" (host).
 */
static void read_container(pid_t pid, char *buf, size_t bufsz)
{
    buf[0] = '\0';

    /* ── 1. cgroup heuristic ────────────────────────────────── */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);

    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024]; // k8s pods can have very long cgroup paths
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "docker") || strstr(line, "/moby/")) {
                snprintf(buf, bufsz, "docker");
                break;
            }
            if (strstr(line, "lxc")) {
                snprintf(buf, bufsz, "lxc");
                break;
            }
            if (strstr(line, "libpod")) {
                snprintf(buf, bufsz, "podman");
                break;
            }
            if (strstr(line, "kubepods") || strstr(line, "kubelet")) {
                snprintf(buf, bufsz, "k8s");
                break;
            }
            if (strstr(line, "containerd")) {
                snprintf(buf, bufsz, "containerd");
                break;
            }
            if (strstr(line, "machine.slice") ||
                strstr(line, "machine-")) {
                snprintf(buf, bufsz, "nspawn");
                break;
            }
            if (strstr(line, "garden")) {
                snprintf(buf, bufsz, "garden");
                break;
            }
            if (strstr(line, "buildkit")) {
                snprintf(buf, bufsz, "buildkit");
                break;
            }
        }
        fclose(f);
        if (buf[0] != '\0')
            return;   /* identified a specific runtime */
    }

    /* ── 2. PID-namespace check (fallback) ──────────────────── */
    static ino_t init_ns_ino = 0;
    if (init_ns_ino == 0) {
        struct stat st;
        if (stat("/proc/1/ns/pid", &st) == 0)
            init_ns_ino = st.st_ino;
    }
    if (init_ns_ino != 0) {
        snprintf(path, sizeof(path), "/proc/%d/ns/pid", pid);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_ino != init_ns_ino)
            snprintf(buf, bufsz, "container");
    }
}

/* ── service unit detection ──────────────────────────────────── */

typedef enum {
    INIT_UNKNOWN,
    INIT_SYSTEMD,
    INIT_OPENRC,
    INIT_OTHER,
} init_system_t;

static init_system_t g_init_system = INIT_UNKNOWN;

static init_system_t detect_init_system(void)
{
    struct stat st;
    if (stat("/run/systemd/system", &st) == 0 && S_ISDIR(st.st_mode))
        return INIT_SYSTEMD;
    if (stat("/run/openrc", &st) == 0 && S_ISDIR(st.st_mode))
        return INIT_OPENRC;
    return INIT_OTHER;
}

/*
 * For systemd: extract the service unit from /proc/<pid>/cgroup.
 * We look for a line like:
 *   0::/system.slice/sshd.service
 *   0::/user.slice/user-1000.slice/...
 * and extract the last .service (or .scope, .slice) component.
 *
 * Only called for direct children of init (ppid == 1) for performance.
 */
static void read_service_systemd(pid_t pid, char *buf, size_t bufsz)
{
    buf[0] = '\0';

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Prefer the unified hierarchy (v2): starts with "0::" */
        if (strncmp(line, "0::", 3) == 0) {
            /* Strip trailing newline */
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n')
                line[len - 1] = '\0';

            /* Find the last path component that ends in .service or .scope */
            char *cgroup_path = line + 3;  /* skip "0::" */

            /* Walk backwards for the last segment containing ".service" */
            char *svc = strstr(cgroup_path, ".service");
            if (!svc) svc = strstr(cgroup_path, ".scope");
            if (svc) {
                /* Find the start of this segment (after last '/') */
                char *seg_start = svc;
                while (seg_start > cgroup_path && *(seg_start - 1) != '/')
                    seg_start--;
                /* Find end of the unit name */
                char *seg_end = svc;
                while (*seg_end && *seg_end != '/' && *seg_end != '\n')
                    seg_end++;
                size_t seg_len = (size_t)(seg_end - seg_start);
                if (seg_len >= bufsz) seg_len = bufsz - 1;
                memcpy(buf, seg_start, seg_len);
                buf[seg_len] = '\0';
                fclose(f);
                return;
            }

            /* If it's a user slice like /user.slice/user-1000.slice/... */
            if (strstr(cgroup_path, "user.slice")) {
                /* Extract the meaningful slice */
                char *slice = strstr(cgroup_path, "user-");
                if (slice) {
                    char *end = strstr(slice, ".slice");
                    if (end) {
                        end += 6; /* include ".slice" */
                        size_t seg_len = (size_t)(end - slice);
                        if (seg_len >= bufsz) seg_len = bufsz - 1;
                        memcpy(buf, slice, seg_len);
                        buf[seg_len] = '\0';
                        fclose(f);
                        return;
                    }
                }
            }
            break;
        }
        /* Fallback for cgroup v1: look for name=systemd or systemd hierarchy */
        if (strstr(line, "name=systemd") || strstr(line, ":systemd:")) {
            char *p = strchr(line, ':');
            if (p) p = strchr(p + 1, ':');
            if (p) {
                p++;  /* skip second ':' */
                size_t len = strlen(p);
                if (len > 0 && p[len - 1] == '\n')
                    p[len - 1] = '\0';

                char *svc = strstr(p, ".service");
                if (!svc) svc = strstr(p, ".scope");
                if (svc) {
                    char *seg_start = svc;
                    while (seg_start > p && *(seg_start - 1) != '/')
                        seg_start--;
                    char *seg_end = svc;
                    while (*seg_end && *seg_end != '/' && *seg_end != '\n')
                        seg_end++;
                    size_t seg_len = (size_t)(seg_end - seg_start);
                    if (seg_len >= bufsz) seg_len = bufsz - 1;
                    memcpy(buf, seg_start, seg_len);
                    buf[seg_len] = '\0';
                    fclose(f);
                    return;
                }
            }
        }
    }
    fclose(f);
}

/*
 * OpenRC service PID map: built once per snapshot from /run/openrc/started/.
 * Each file in that directory is a service name; its content is the PID.
 */

#define SVC_MAP_SIZE 256
#define SVC_NAME_MAX 128

typedef struct {
    pid_t pid;
    char  name[SVC_NAME_MAX];
} svc_map_entry_t;

typedef struct {
    svc_map_entry_t entries[SVC_MAP_SIZE];
    size_t          count;
} svc_map_t;

static void svc_map_build_openrc(svc_map_t *map)
{
    map->count = 0;

    /*
     * /run/openrc/daemons/<service>/<instance> files contain metadata
     * including "pidfile=<path>".  We read the pidfile path, then read
     * the actual PID from it.
     */
    DIR *dp = opendir("/run/openrc/daemons");
    if (!dp)
        return;

    struct dirent *svc_de;
    while ((svc_de = readdir(dp)) != NULL && map->count < SVC_MAP_SIZE) {
        if (svc_de->d_name[0] == '.')
            continue;

        /* Open the per-service subdirectory */
        char svc_dir[384];
        snprintf(svc_dir, sizeof(svc_dir),
                 "/run/openrc/daemons/%s", svc_de->d_name);

        DIR *sdp = opendir(svc_dir);
        if (!sdp)
            continue;

        struct dirent *inst_de;
        while ((inst_de = readdir(sdp)) != NULL && map->count < SVC_MAP_SIZE) {
            if (inst_de->d_name[0] == '.')
                continue;

            char inst_path[512];
            snprintf(inst_path, sizeof(inst_path),
                     "%s/%s", svc_dir, inst_de->d_name);

            /* Parse the instance file for "pidfile=<path>" */
            FILE *f = fopen(inst_path, "r");
            if (!f)
                continue;

            char pidfile_path[256] = "";
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "pidfile=", 8) == 0) {
                    /* Strip trailing whitespace/newline */
                    char *val = line + 8;
                    size_t vlen = strlen(val);
                    while (vlen > 0 &&
                           (val[vlen-1] == '\n' || val[vlen-1] == '\r' ||
                            val[vlen-1] == ' '))
                        vlen--;
                    if (vlen >= sizeof(pidfile_path))
                        vlen = sizeof(pidfile_path) - 1;
                    memcpy(pidfile_path, val, vlen);
                    pidfile_path[vlen] = '\0';
                    break;
                }
            }
            fclose(f);

            if (!pidfile_path[0])
                continue;

            /* Read the PID from the pidfile */
            FILE *pf = fopen(pidfile_path, "r");
            if (!pf)
                continue;

            char pidbuf[32];
            pid_t pid = 0;
            if (fgets(pidbuf, sizeof(pidbuf), pf))
                pid = (pid_t)atoi(pidbuf);
            fclose(pf);

            if (pid > 0) {
                svc_map_entry_t *e = &map->entries[map->count++];
                e->pid = pid;
                snprintf(e->name, SVC_NAME_MAX, "%s", svc_de->d_name);
            }
        }
        closedir(sdp);
    }
    closedir(dp);
}

static const char *svc_map_lookup(const svc_map_t *map, pid_t pid)
{
    for (size_t i = 0; i < map->count; i++)
        if (map->entries[i].pid == pid)
            return map->entries[i].name;
    return NULL;
}

/*
 * Resolve the service name for a process.
 * Only called for direct children of init (ppid <= 1) for performance.
 * Writes the result into buf (size SVC_NAME_MAX) and returns a pointer
 * to buf (empty string if none).
 */
static const char *read_service(pid_t pid, pid_t ppid, char *buf,
                                 const svc_map_t *openrc_map)
{
    buf[0] = '\0';

    /* Only resolve for direct children of init */
    if (ppid != 0 && ppid != 1)
        return buf;

    switch (g_init_system) {
    case INIT_SYSTEMD:
        read_service_systemd(pid, buf, SVC_NAME_MAX);
        break;
    case INIT_OPENRC:
        if (openrc_map) {
            const char *name = svc_map_lookup(openrc_map, pid);
            if (name)
                snprintf(buf, SVC_NAME_MAX, "%s", name);
        }
        break;
    default:
        break;
    }
    return buf;
}

/*
 * Read the process start time from /proc/<pid>/stat field 22 (starttime)
 * and convert it to an epoch timestamp using the system boot time.
 * Returns epoch seconds, or 0 on failure.
 */
static unsigned long long read_start_time(pid_t pid)
{
    /* Get system boot time (cached after first call) */
    static time_t boot_time = 0;
    if (boot_time == 0) {
        FILE *f = fopen("/proc/stat", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "btime ", 6) == 0) {
                    boot_time = (time_t)atoll(line + 6);
                    break;
                }
            }
            fclose(f);
        }
        if (boot_time == 0) return 0;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[1024];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);

    /* Skip past the comm field (in parens, may contain spaces) */
    const char *p = strrchr(buf, ')');
    if (!p) return 0;
    p++;

    /* Fields after comm: state(3), ppid(4), pgrp(5), session(6),
     * tty_nr(7), tpgid(8), flags(9), minflt(10), cminflt(11),
     * majflt(12), cmajflt(13), utime(14), stime(15), cutime(16),
     * cstime(17), priority(18), nice(19), num_threads(20),
     * itrealvalue(21), starttime(22).                              */
    unsigned long long starttime = 0;
    int n = sscanf(p, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u"
                      " %*u %*u %*d %*d %*d %*d %*u %*d %llu",
                   &starttime);
    if (n != 1) return 0;

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck < 1) clk_tck = 100;

    return (unsigned long long)boot_time + starttime / (unsigned long long)clk_tck;
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

/*
 * Read read_bytes and write_bytes from /proc/<pid>/io.
 * These are cumulative counters of actual disk I/O (post-page-cache).
 * Stores results in *out_read and *out_write; returns 0 on success.
 */
static int read_io_bytes(pid_t pid,
                         unsigned long long *out_read,
                         unsigned long long *out_write)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/io", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    *out_read  = 0;
    *out_write = 0;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "read_bytes:", 11) == 0) {
            *out_read = strtoull(line + 11, NULL, 10);
            found++;
        } else if (strncmp(line, "write_bytes:", 12) == 0) {
            *out_write = strtoull(line + 12, NULL, 10);
            found++;
        }
        if (found == 2)
            break;
    }
    fclose(f);
    return (found == 2) ? 0 : -1;
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

/* ── previous I/O byte tracking for delta computation ────────── */

typedef struct {
    pid_t              pid;
    unsigned long long read_bytes;
    unsigned long long write_bytes;
    int                used;
} io_prev_entry_t;

static io_prev_entry_t g_prev_io[CPU_HT_SIZE];

/* ── per-PID I/O history ring buffer (persists across snapshots) ─ */

typedef struct {
    pid_t   pid;
    float   samples[IO_HISTORY_LEN];
    int     count;      /* number of valid samples (≤ IO_HISTORY_LEN) */
    int     used;
} io_hist_entry_t;

static io_hist_entry_t g_io_history[CPU_HT_SIZE];

static io_hist_entry_t *io_hist_get(io_hist_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % CPU_HT_SIZE;
    for (int k = 0; k < CPU_HT_SIZE; k++) {
        if (!ht[h].used) return NULL;
        if (ht[h].pid == pid) return &ht[h];
        h = (h + 1) % CPU_HT_SIZE;
    }
    return NULL;
}

static io_hist_entry_t *io_hist_get_or_create(io_hist_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % CPU_HT_SIZE;
    for (int k = 0; k < CPU_HT_SIZE; k++) {
        if (!ht[h].used) {
            ht[h].pid   = pid;
            ht[h].count = 0;
            ht[h].used  = 1;
            memset(ht[h].samples, 0, sizeof(ht[h].samples));
            return &ht[h];
        }
        if (ht[h].pid == pid)
            return &ht[h];
        h = (h + 1) % CPU_HT_SIZE;
    }
    return NULL;  /* table full */
}

static void io_hist_push(io_hist_entry_t *entry, float value)
{
    /* Shift samples left to make room for the new one at the end */
    if (entry->count >= IO_HISTORY_LEN) {
        memmove(entry->samples, entry->samples + 1,
                (IO_HISTORY_LEN - 1) * sizeof(float));
        entry->samples[IO_HISTORY_LEN - 1] = value;
    } else {
        entry->samples[entry->count++] = value;
    }
}

static void io_ht_get(const io_prev_entry_t *ht, pid_t pid,
                      unsigned long long *rd, unsigned long long *wr)
{
    *rd = 0;
    *wr = 0;
    unsigned h = (unsigned)pid % CPU_HT_SIZE;
    for (int k = 0; k < CPU_HT_SIZE; k++) {
        if (!ht[h].used) return;
        if (ht[h].pid == pid) {
            *rd = ht[h].read_bytes;
            *wr = ht[h].write_bytes;
            return;
        }
        h = (h + 1) % CPU_HT_SIZE;
    }
}

static void io_ht_set(io_prev_entry_t *ht, pid_t pid,
                      unsigned long long rd, unsigned long long wr)
{
    unsigned h = (unsigned)pid % CPU_HT_SIZE;
    for (int k = 0; k < CPU_HT_SIZE; k++) {
        if (!ht[h].used || ht[h].pid == pid) {
            ht[h].pid         = pid;
            ht[h].read_bytes  = rd;
            ht[h].write_bytes = wr;
            ht[h].used        = 1;
            return;
        }
        h = (h + 1) % CPU_HT_SIZE;
    }
    /* Table full — silently drop. */
}

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
    for (int k = 0; k < CPU_HT_SIZE; k++) {
        if (!ht[h].used || ht[h].pid == pid) {
            ht[h].pid   = pid;
            ht[h].ticks = ticks;
            ht[h].used  = 1;
            return;
        }
        h = (h + 1) % CPU_HT_SIZE;
    }
    /* Table full — silently drop. */
}

/* ── snapshot builder ────────────────────────────────────────── */

/*
 * Copy a proc_snapshot_t by duplicating the strtab buffer and entries.
 * The extras array is shallow-copied (steam pointers are zeroed; extras
 * are rebuilt during the Steam pass).
 * Used for the partial and early pre-Steam publish paths.
 */
static proc_snapshot_t snapshot_copy_pre_steam(const proc_snapshot_t *src)
{
    proc_snapshot_t dst;
    memset(&dst, 0, sizeof(dst));

    if (src->len == 0) return dst;

    /* Copy entries array */
    dst.entries = (proc_entry_t *)malloc(src->len * sizeof(proc_entry_t));
    if (!dst.entries) return dst;
    memcpy(dst.entries, src->entries, src->len * sizeof(proc_entry_t));
    dst.len = src->len;
    dst.cap = src->len;

    /* Copy strtab buffer */
    dst.strtab.buf = (char *)malloc(src->strtab.len);
    if (!dst.strtab.buf) { free(dst.entries); memset(&dst, 0, sizeof(dst)); return dst; }
    memcpy(dst.strtab.buf, src->strtab.buf, src->strtab.len);
    dst.strtab.len = src->strtab.len;
    dst.strtab.cap = src->strtab.len;

    /* Extras: none (steam pointers not yet populated) */
    dst.extras     = NULL;
    dst.extras_len = 0;
    dst.extras_cap = 0;

    return dst;
}

/*
 * out_published_early: set to 1 if the snapshot was already published
 * inside this function (early partial publish after /proc pass, before
 * the Steam enrichment pass).  The caller should still latch the
 * returned snapshot as the final version, but should not treat it as
 * the very first snapshot when counting publishes.
 */
static proc_snapshot_t build_snapshot(monitor_state_t *state,
                                      int *out_published_early)
{
    proc_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    int partial_published = 0;
    if (out_published_early) *out_published_early = 0;

    /* Detect the init system once */
    if (g_init_system == INIT_UNKNOWN)
        g_init_system = detect_init_system();

    /* For OpenRC, build the PID→service map once per snapshot */
    svc_map_t openrc_map = { .count = 0 };
    if (g_init_system == INIT_OPENRC)
        svc_map_build_openrc(&openrc_map);

    DIR *dp = opendir("/proc");
    if (!dp)
        return snap;

    /* Initialise the strtab (64 KiB initial capacity) */
    if (strtab_init(&snap.strtab, 65536) != 0) {
        closedir(dp);
        return snap;
    }

    uint32_t capacity = 512;
    snap.entries = (proc_entry_t *)malloc(capacity * sizeof(proc_entry_t));
    if (!snap.entries) {
        strtab_free(&snap.strtab);
        closedir(dp);
        memset(&snap, 0, sizeof(snap));
        return snap;
    }

    /* Scratch buffers for fields that still go through char[] helpers */
    char comm_buf[PROC_NAME_MAX];
    char user_buf[64];
    char container_buf[64];
    char svc_buf[SVC_NAME_MAX];

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_type != DT_DIR || !is_pid_dir(de->d_name))
            continue;

        pid_t pid = (pid_t)atoi(de->d_name);

        /* grow entries buffer if needed */
        if (snap.len >= capacity) {
            capacity *= 2;
            proc_entry_t *tmp = (proc_entry_t *)realloc(snap.entries,
                                        capacity * sizeof(proc_entry_t));
            if (!tmp)
                break;
            snap.entries = tmp;
        }

        proc_entry_t *e = &snap.entries[snap.len];
        memset(e, 0, sizeof(*e));
        e->pid        = pid;
        e->ppid       = read_ppid(pid);
        e->extra_idx  = UINT32_MAX;

        if (read_comm(pid, comm_buf, sizeof(comm_buf)) != 0)
            continue;   /* process vanished, skip */

        e->name_off = strtab_push(&snap.strtab, comm_buf);
        e->name_len = (str_len_t)strlen(comm_buf);

        /* cmdline */
        e->cmd_off = read_cmdline_to_strtab(pid, comm_buf, &snap.strtab);
        e->cmd_len = e->cmd_off
            ? (str_len_t)strlen(snap.strtab.buf + e->cmd_off)
            : 0;

        /* user */
        if (read_user(pid, user_buf, sizeof(user_buf)) != 0)
            snprintf(user_buf, sizeof(user_buf), "?");
        e->user_off = strtab_push(&snap.strtab, user_buf);
        e->user_len = (str_len_t)strlen(user_buf);

        /* cwd */
        e->cwd_off = read_cwd_to_strtab(pid, &snap.strtab);
        e->cwd_len = e->cwd_off
            ? (str_len_t)strlen(snap.strtab.buf + e->cwd_off)
            : 0;

        e->mem_rss_kb = (uint64_t)read_rss(pid);

        /* CPU ticks and percentage */
        e->cpu_ticks   = read_cpu_ticks(pid);
        e->cpu_percent = 0.0f;

        /* Disk I/O counters (cumulative) */
        {
            unsigned long long rd = 0, wr = 0;
            if (read_io_bytes(pid, &rd, &wr) != 0) rd = wr = 0;
            e->io_read_bytes  = rd;
            e->io_write_bytes = wr;
        }
        e->io_read_rate  = 0.0f;
        e->io_write_rate = 0.0f;
        e->io_history_len = 0;
        memset(e->io_history, 0, sizeof(e->io_history));

        /* Process start time */
        e->start_time = read_start_time(pid);

        /* Container detection */
        read_container(pid, container_buf, sizeof(container_buf));
        e->container_off = strtab_push(&snap.strtab, container_buf);
        e->container_len = (str_len_t)strlen(container_buf);

        /* Service unit (only for direct children of init) */
        read_service(pid, e->ppid, svc_buf, &openrc_map);
        e->service_off = strtab_push(&snap.strtab, svc_buf);
        e->service_len = (str_len_t)strlen(svc_buf);

        snap.len++;

        /* ── Early partial-snapshot publish ─────────────────────
         * If the caller asked for a preselect PID, publish as soon as
         * we have scanned both PID 1 (init) and the target PID.
         * Option C: the partial snapshot owns its own copy of the strtab. */
        if (state && state->preselect_pid > 0 &&
            !partial_published && snap.len >= 2) {
            int have_init   = 0;
            int have_target = 0;
            for (uint32_t _j = 0; _j < snap.len; _j++) {
                if (snap.entries[_j].pid == 1)                    have_init   = 1;
                if (snap.entries[_j].pid == state->preselect_pid) have_target = 1;
            }
            if (have_init && have_target) {
                partial_published = 1;
                proc_snapshot_t partial = snapshot_copy_pre_steam(&snap);
                if (partial.entries) {
                    pthread_mutex_lock(&state->lock);
                    proc_snapshot_free(&state->snapshot);
                    state->snapshot = partial;
                    pthread_cond_signal(&state->updated);
                    pthread_mutex_unlock(&state->lock);
                }
            }
        }
    }

    closedir(dp);

    /* ── Mitigation #2: publish after first /proc pass ───────────
     * Option C: early snapshot owns its own strtab copy.
     */
    if (state && steam_is_available() && snap.len > 0) {
        pthread_mutex_lock(&state->lock);
        int is_first = (state->snapshot.len == 0);
        pthread_mutex_unlock(&state->lock);

        if (is_first) {
            proc_snapshot_t early = snapshot_copy_pre_steam(&snap);
            if (early.entries) {
                pthread_mutex_lock(&state->lock);
                proc_snapshot_free(&state->snapshot);
                state->snapshot = early;
                pthread_cond_signal(&state->updated);
                pthread_mutex_unlock(&state->lock);
                if (out_published_early) *out_published_early = 1;

                extern struct timespec evemon_start_time;
                struct timespec _now;
                clock_gettime(CLOCK_MONOTONIC, &_now);
                double _e = (double)(_now.tv_sec  - evemon_start_time.tv_sec)
                          + (double)(_now.tv_nsec - evemon_start_time.tv_nsec) / 1e9;
                printf("[evemon] monitor: early snapshot published after /proc pass "
                       "(%.3f s, %u procs) — Steam enrichment pass starting\n",
                       _e, snap.len);
                fflush(stdout);
            }
        }
    }

    /* ── Steam/Proton metadata: second pass ──────────────────── */
    {
        static int steam_logged = 0;
        if (!steam_logged) {
            steam_logged = 1;
            if (steam_is_available())
                printf("[evemon] Steam detected — enabling Steam/Proton enrichment pass\n");
            else
                printf("[evemon] Steam not found — skipping Steam/Proton enrichment pass\n");
            fflush(stdout);
        }
    }
    if (steam_is_available()) {
        #define STEAM_HT_SIZE 8192
        typedef struct { pid_t pid; uint32_t idx; int used; } sht_entry_t;
        sht_entry_t *sht = (sht_entry_t *)calloc(STEAM_HT_SIZE, sizeof(sht_entry_t));
        if (sht) {
            for (uint32_t i = 0; i < snap.len; i++) {
                unsigned h = (unsigned)snap.entries[i].pid % STEAM_HT_SIZE;
                for (int k = 0; k < STEAM_HT_SIZE; k++) {
                    if (!sht[h].used) {
                        sht[h].pid  = snap.entries[i].pid;
                        sht[h].idx  = i;
                        sht[h].used = 1;
                        break;
                    }
                    h = (h + 1) % STEAM_HT_SIZE;
                }
            }

            for (int pass = 0; pass < 4; pass++) {
                int new_detections = 0;
                for (uint32_t i = 0; i < snap.len; i++) {
                    proc_entry_t *e = &snap.entries[i];
                    if (e->extra_idx != UINT32_MAX &&
                        snap.extras[e->extra_idx].steam)
                        continue;  /* already detected */

                    /* Find parent's steam info (if any) */
                    const steam_info_t *parent_si = NULL;
                    if (e->ppid > 0) {
                        unsigned h = (unsigned)e->ppid % STEAM_HT_SIZE;
                        for (int k = 0; k < STEAM_HT_SIZE; k++) {
                            if (!sht[h].used) break;
                            if (sht[h].pid == e->ppid) {
                                uint32_t pidx = sht[h].idx;
                                if (snap.entries[pidx].extra_idx != UINT32_MAX)
                                    parent_si = snap.extras[snap.entries[pidx].extra_idx].steam;
                                break;
                            }
                            h = (h + 1) % STEAM_HT_SIZE;
                        }
                    }

                    steam_info_t *si = steam_detect(
                        e->pid,
                        PROC_NAME(&snap, e),
                        PROC_CMDLINE(&snap, e),
                        parent_si);

                    if (si) {
                        /* Allocate or reuse extras slot */
                        if (e->extra_idx == UINT32_MAX) {
                            if (snap.extras_len >= snap.extras_cap) {
                                uint32_t newcap = snap.extras_cap
                                    ? snap.extras_cap * 2 : 64;
                                proc_extra_t *tmp = (proc_extra_t *)realloc(
                                    snap.extras,
                                    newcap * sizeof(proc_extra_t));
                                if (!tmp) { free(si); continue; }
                                snap.extras     = tmp;
                                snap.extras_cap = newcap;
                            }
                            e->extra_idx = snap.extras_len;
                            snap.extras[snap.extras_len].steam = NULL;
                            snap.extras_len++;
                        }
                        free(snap.extras[e->extra_idx].steam);
                        snap.extras[e->extra_idx].steam = si;
                        new_detections++;
                    }
                }
                if (new_detections == 0)
                    break;
            }

            free(sht);
        }
        #undef STEAM_HT_SIZE
    }

    return snap;
}

/* ── public API ──────────────────────────────────────────────── */

void proc_snapshot_free(proc_snapshot_t *snap)
{
    /* Free per-entry steam metadata in the extras array */
    if (snap->extras) {
        for (uint32_t i = 0; i < snap->extras_len; i++)
            free(snap->extras[i].steam);   /* NULL-safe */
        free(snap->extras);
        snap->extras     = NULL;
        snap->extras_len = 0;
        snap->extras_cap = 0;
    }

    /* Free the string blob */
    strtab_free(&snap->strtab);

    /* Free the entry array */
    free(snap->entries);
    snap->entries = NULL;
    snap->len     = 0;
    snap->cap     = 0;
}

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
    proc_snapshot_free(&state->snapshot);
    pthread_mutex_unlock(&state->lock);

    pthread_cond_destroy(&state->updated);
    pthread_mutex_destroy(&state->lock);
}

void *monitor_thread(void *arg)
{
    monitor_state_t *state = (monitor_state_t *)arg;
    pthread_setname_np(pthread_self(), "ev-monitor");

    while (1) {
        /* Check if we should keep running */
        pthread_mutex_lock(&state->lock);
        int running = state->running;
        pthread_mutex_unlock(&state->lock);

        if (!running)
            break;

        /* Build a fresh snapshot outside the lock */
        PROFILE_BEGIN(snapshot_build);
        int snap_published_early = 0;
        proc_snapshot_t snap = build_snapshot(state, &snap_published_early);
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
                for (uint32_t i = 0; i < snap.len; i++) {
                    unsigned long long prev = cpu_ht_get(g_prev_ticks,
                                                         snap.entries[i].pid);
                    if (prev > 0 && snap.entries[i].cpu_ticks >= prev) {
                        double delta = (double)(snap.entries[i].cpu_ticks - prev);
                        snap.entries[i].cpu_percent =
                            (float)((delta / total_ticks) * 100.0);
                    }

                    /* I/O rate (bytes/sec) from delta */
                    unsigned long long prev_rd, prev_wr;
                    io_ht_get(g_prev_io, snap.entries[i].pid,
                              &prev_rd, &prev_wr);
                    if (prev_rd > 0 || prev_wr > 0) {
                        if (snap.entries[i].io_read_bytes >= prev_rd)
                            snap.entries[i].io_read_rate =
                                (float)((double)(snap.entries[i].io_read_bytes - prev_rd)
                                / elapsed);
                        if (snap.entries[i].io_write_bytes >= prev_wr)
                            snap.entries[i].io_write_rate =
                                (float)((double)(snap.entries[i].io_write_bytes - prev_wr)
                                / elapsed);
                    }
                }
            }
        }

        /* Store current ticks as the "previous" for next iteration */
        memset(g_prev_ticks, 0, sizeof(g_prev_ticks));
        for (uint32_t i = 0; i < snap.len; i++)
            cpu_ht_set(g_prev_ticks, snap.entries[i].pid,
                       snap.entries[i].cpu_ticks);

        /* Store current I/O bytes as "previous" for next iteration */
        memset(g_prev_io, 0, sizeof(g_prev_io));
        for (uint32_t i = 0; i < snap.len; i++)
            io_ht_set(g_prev_io, snap.entries[i].pid,
                      snap.entries[i].io_read_bytes,
                      snap.entries[i].io_write_bytes);

        /* GC stale entries from g_io_history */
        for (int i = 0; i < CPU_HT_SIZE; i++) {
            if (!g_io_history[i].used) continue;
            if (cpu_ht_get(g_prev_ticks, g_io_history[i].pid) == 0 &&
                g_io_history[i].pid != 0)
                g_io_history[i].used = 0;
        }

        /* Update per-PID I/O history ring buffers and copy into snapshot */
        for (uint32_t i = 0; i < snap.len; i++) {
            float combined = snap.entries[i].io_read_rate
                           + snap.entries[i].io_write_rate;
            io_hist_entry_t *hist = io_hist_get_or_create(
                g_io_history, snap.entries[i].pid);
            if (hist) {
                io_hist_push(hist, combined);
                snap.entries[i].io_history_len = (uint8_t)hist->count;
                memcpy(snap.entries[i].io_history, hist->samples,
                       hist->count * sizeof(float));
            } else {
                snap.entries[i].io_history_len = 0;
            }
        }

        g_prev_valid = 1;
        prev_ts = now_ts;

        /* Latch per-PID network byte counters (eBPF tcp_sendmsg/recvmsg) */
        if (state->fdmon)
            fdmon_net_io_snapshot(state->fdmon);

        /* Swap it in */
        pthread_mutex_lock(&state->lock);
        int _first = ((state->snapshot.len == 0 || snap_published_early)
                      && snap.len > 0);
        proc_snapshot_free(&state->snapshot);
        state->snapshot = snap;
        pthread_cond_signal(&state->updated);
        pthread_mutex_unlock(&state->lock);

        if (_first) {
            extern struct timespec evemon_start_time;
            struct timespec _now;
            clock_gettime(CLOCK_MONOTONIC, &_now);
            double _e = (double)(_now.tv_sec  - evemon_start_time.tv_sec)
                      + (double)(_now.tv_nsec - evemon_start_time.tv_nsec) / 1e9;
            const char *_label = snap_published_early
                ? "[evemon] monitor: Steam-enriched snapshot ready %.3f s after startup (%u procs)\n"
                : "[evemon] monitor: first snapshot ready %.3f s after startup (%u procs)\n";
            printf(_label, _e, snap.len);
            fflush(stdout);
        }

        /* Sleep for the poll interval, but wake immediately on shutdown.
         * Use pthread_cond_timedwait so that broadcasting `updated`
         * (which the shutdown path does) unblocks us at once.          */
        {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec  += POLL_INTERVAL_MS / 1000;
            deadline.tv_nsec += (POLL_INTERVAL_MS % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec  += 1;
                deadline.tv_nsec -= 1000000000L;
            }

            pthread_mutex_lock(&state->lock);
            if (state->running)
                pthread_cond_timedwait(&state->updated, &state->lock, &deadline);
            pthread_mutex_unlock(&state->lock);
        }
    }

    return NULL;
}
