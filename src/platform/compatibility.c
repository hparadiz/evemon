#include "compatibility.h"
#include "desktop_session.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <asm/unistd.h>
#include <unistd.h>
#include <errno.h>

/* Shared read-only feature diagnostics for both frontends. */
static void parse_kernel_version(const char *release,
                                 int *major, int *minor, int *patch)
{
    *major = *minor = *patch = 0;
    sscanf(release, "%d.%d.%d", major, minor, patch);
}

static bool kernel_ge(int maj, int min,
                           int need_maj, int need_min)
{
    if (maj > need_maj) return true;
    if (maj == need_maj && min >= need_min) return true;
    return false;
}

/* ── capability probe ─────────────────────────────────────────
 * Returns true if we can confirm the process holds the capability.
 * Falls back gracefully when libcap is unavailable at compile time.
 */
static bool probe_cap(int cap_value)
{
    /* Use the raw capget syscall — no dependency on libcap at link time. */
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid     = 0
    };
    struct __user_cap_data_struct data[2] = {};

    if (syscall(__NR_capget, &hdr, data) != 0)
        return false;

    int word  = cap_value / 32;
    int shift = cap_value % 32;
    if (word > 1) return false;
    return !!(data[word].effective & (1u << shift));
}

/* ── /proc/config.gz / boot/config probe ─────────────────────── */

static bool kconfig_option_enabled(const char *option)
{
    /* Try /proc/config.gz (uncommon) and /boot/config-<release> */
    struct utsname uts;
    uname(&uts);

    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path),
             "/boot/config-%s", uts.release);

    FILE *f = fopen(config_path, "r");
    if (!f) f = fopen("/proc/config", "r");
    if (!f) return false;  /* can't check – be optimistic elsewhere */

    char needle[128];
    snprintf(needle, sizeof(needle), "%s=y", option);
    char needle_m[128];
    snprintf(needle_m, sizeof(needle_m), "%s=m", option);

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (strcmp(line, needle) == 0 || strcmp(line, needle_m) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* ── collect all results ─────────────────────────────────────── */

/*
 * Run every compatibility check and append results to the array.
 * Returns the count of items written.
 * out[] must have capacity for at least 32 entries.
 */
int evemon_collect_compat_results(compat_result_t *out, int capacity)
{
    int n = 0;

#define PUSH(_sev, _feat, _stat, ...) \
    do { \
        if (n < capacity) { \
            out[n].severity = (_sev); \
            out[n].feature  = (_feat); \
            out[n].status   = (_stat); \
            snprintf(out[n].detail, sizeof(out[n].detail), __VA_ARGS__); \
            n++; \
        } \
    } while (0)

    /* ── Identity ─────────────────────────────────────────────── */

    uid_t uid  = getuid();
    uid_t euid = geteuid();

    char display_user[128];
    if (euid == 0 && desktop_session_uid() != 0)
        snprintf(display_user, sizeof(display_user), "%s (administrator)", desktop_session_user());
    else
        snprintf(display_user, sizeof(display_user), "%s (uid %u)", desktop_session_user(), (unsigned)uid);
    PUSH(COMPAT_INFO, "Running as", euid == 0 ? "root" : "user", "%s", display_user);

    /* ── Kernel version ───────────────────────────────────────── */

    struct utsname uts;
    uname(&uts);

    int kmaj = 0, kmin = 0, kpatch = 0;
    parse_kernel_version(uts.release, &kmaj, &kmin, &kpatch);

    {
        compat_severity_t sev;
        const char *status;
        char detail[512];

        if (kernel_ge(kmaj, kmin, 5, 8)) {
            sev    = COMPAT_OK;
            status = "good";
            snprintf(detail, sizeof(detail),
                     "%s — all features available (fine-grained capabilities "
                     "CAP_BPF + CAP_PERFMON supported).",
                     uts.release);
        } else if (kernel_ge(kmaj, kmin, 5, 5)) {
            sev    = COMPAT_WARN;
            status = "acceptable";
            snprintf(detail, sizeof(detail),
                     "%s — eBPF fd/write monitoring works but requires the "
                     "broad CAP_SYS_ADMIN instead of the narrower "
                     "CAP_BPF + CAP_PERFMON (added in 5.8).  "
                     "Recommend upgrading to 5.8+.",
                     uts.release);
        } else if (kernel_ge(kmaj, kmin, 5, 1)) {
            sev    = COMPAT_WARN;
            status = "limited";
            snprintf(detail, sizeof(detail),
                     "%s — eBPF fd/write monitoring unavailable "
                     "(requires bpf_probe_read_user, added in 5.5).  "
                     "fanotify fallback is active for file open/close "
                     "tracking only.  Upgrade to 5.8+ for full support.",
                     uts.release);
        } else if (kernel_ge(kmaj, kmin, 2, 6)) {
            sev    = COMPAT_FAIL;
            status = "too old";
            snprintf(detail, sizeof(detail),
                     "%s — fanotify requires 5.1+ and eBPF requires 5.5+.  "
                     "Only the core process tree will function.  "
                     "Upgrade strongly recommended.",
                     uts.release);
        } else {
            sev    = COMPAT_FAIL;
            status = "unsupported";
            snprintf(detail, sizeof(detail),
                     "%s — evemon minimum is Linux 2.6.24.  "
                     "This kernel is too old to run evemon at all.",
                     uts.release);
        }

        PUSH(sev, "Kernel version", status, "%s", detail);
    }

    /* ── /proc filesystem ─────────────────────────────────────── */

    {
        struct stat st;
        if (stat("/proc/self/status", &st) == 0) {
            PUSH(COMPAT_OK, "/proc filesystem", "present",
                 "procfs is mounted at /proc — required for all process "
                 "monitoring.");
        } else {
            PUSH(COMPAT_FAIL, "/proc filesystem", "missing",
                 "/proc is not accessible.  evemon cannot function without "
                 "procfs.  Mount it with: mount -t proc proc /proc");
        }
    }

    /* ── Own-process visibility ───────────────────────────────── */

    PUSH(COMPAT_OK, "Own-process monitoring", "available",
         "Process tree, CPU%%, and memory for your own processes always "
         "work regardless of privileges.");

    /* ── Other-user processes ─────────────────────────────────── */

    {
        /* Check for CAP_DAC_READ_SEARCH and CAP_SYS_PTRACE */
        bool has_dac   = (euid == 0) || probe_cap(2  /* CAP_DAC_READ_SEARCH */);
        bool has_ptrace= (euid == 0) || probe_cap(19 /* CAP_SYS_PTRACE */);

        if (has_dac && has_ptrace) {
            PUSH(COMPAT_OK, "Cross-user process info", "full access",
                 "Can read fd lists, working directories, and status for "
                 "processes owned by other users.");
        } else if (!has_dac && !has_ptrace) {
            PUSH(COMPAT_WARN, "Cross-user process info", "limited",
                 "Cannot read /proc/<pid>/fd or working directory for "
                 "processes belonging to other users.  Your own processes "
                 "are fully visible.  Run as root or grant CAP_DAC_READ_SEARCH "
                 "+ CAP_SYS_PTRACE to see all processes.");
        } else if (!has_dac) {
            PUSH(COMPAT_WARN, "Cross-user process info", "partial",
                 "fd lists for other users' processes are hidden "
                 "(needs CAP_DAC_READ_SEARCH).  Working directory is "
                 "available.");
        } else {
            PUSH(COMPAT_WARN, "Cross-user process info", "partial",
                 "Working directory / status for other users' processes "
                 "is blocked by ptrace_may_access (needs CAP_SYS_PTRACE).  "
                 "fd lists are available.");
        }
    }

    /* ── eBPF fd/write monitoring ─────────────────────────────── */

    {
        bool ebpf_kernel  = kernel_ge(kmaj, kmin, 5, 5);
        /* On 5.8+ we need CAP_BPF (39) + CAP_PERFMON (38).
         * On 5.5–5.7 we need CAP_SYS_ADMIN (21). */
        bool cap_bpf     = (euid == 0) || probe_cap(39 /* CAP_BPF */);
        bool cap_perfmon = (euid == 0) || probe_cap(38 /* CAP_PERFMON */);
        bool cap_admin   = (euid == 0) || probe_cap(21 /* CAP_SYS_ADMIN */);
        bool has_bpf_cap = kernel_ge(kmaj, kmin, 5, 8)
                               ? (cap_bpf && cap_perfmon)
                               : cap_admin;

        /* Check whether the bpf() syscall is actually usable */
        bool bpf_syscall_ok = false;
        {
            /* Try bpf(BPF_PROG_TYPE_UNSPEC=0, NULL, 0) – will return
             * -EINVAL (not -EPERM / -ENOSYS) if the kernel allows bpf. */
            long r = syscall(__NR_bpf, 0 /*BPF_MAP_CREATE*/, NULL, 0);
            bpf_syscall_ok = (r == -1 && errno == EINVAL) ||
                             (r == -1 && errno == EFAULT);
            /* EPERM → no capability.  ENOSYS → no BPF kernel support. */
        }

        /* Check sched tracepoints exist */
        bool tracepoints_ok =
            (access("/sys/kernel/debug/tracing/events/syscalls", F_OK) == 0) ||
            (access("/sys/kernel/tracing/events/syscalls",       F_OK) == 0);

        if (!ebpf_kernel) {
            PUSH(COMPAT_FAIL, "eBPF fd/write monitoring", "unavailable",
                 "Requires Linux 5.5+ (bpf_probe_read_user).  "
                 "Running kernel %d.%d.  Upgrade to 5.8+ for full support.",
                 kmaj, kmin);
        } else if (!has_bpf_cap || !bpf_syscall_ok) {
            if (kernel_ge(kmaj, kmin, 5, 8)) {
                PUSH(COMPAT_FAIL, "eBPF fd/write monitoring", "no permission",
                     "Needs CAP_BPF + CAP_PERFMON (kernel 5.8+).  "
                     "Use \"Restart as Administrator\" from the File menu, "
                     "or grant capabilities with: "
                     "setcap cap_bpf,cap_perfmon+ep /path/to/evemon");
            } else {
                PUSH(COMPAT_FAIL, "eBPF fd/write monitoring", "no permission",
                     "Kernel 5.5–5.7 needs CAP_SYS_ADMIN for bpf().  "
                     "Use \"Restart as Administrator\" from the File menu.  "
                     "Upgrading to kernel 5.8+ allows narrower "
                     "CAP_BPF + CAP_PERFMON instead.");
            }
        } else if (!tracepoints_ok) {
            PUSH(COMPAT_WARN, "eBPF fd/write monitoring", "limited",
                 "Capabilities are present but the syscall tracepoint "
                 "directory (/sys/kernel/tracing/events/syscalls) is not "
                 "accessible.  The debugfs/tracefs may not be mounted: "
                 "mount -t tracefs tracefs /sys/kernel/tracing");
        } else {
            PUSH(COMPAT_OK, "eBPF fd/write monitoring", "available",
                 "All eBPF prerequisites met.  File descriptor lifecycle, "
                 "write capture, and fork-propagation are fully functional.");
        }
    }

    /* ── fanotify fallback ────────────────────────────────────── */

    {
        bool fan_kernel = kernel_ge(kmaj, kmin, 5, 1);
        bool fan_cap    = (euid == 0) || probe_cap(21 /* CAP_SYS_ADMIN */);

        /* fanotify is only mentioned as fallback if eBPF is missing */
        if (!kernel_ge(kmaj, kmin, 5, 5) && fan_kernel) {
            if (fan_cap) {
                PUSH(COMPAT_OK, "fanotify fd tracking (fallback)", "available",
                     "eBPF is unavailable on this kernel, but fanotify can "
                     "track file open/close events mount-wide.  Note: "
                     "fanotify cannot see sockets, pipes, or anonymous fds.");
            } else {
                PUSH(COMPAT_FAIL, "fanotify fd tracking (fallback)", "no permission",
                     "fanotify_init() requires CAP_SYS_ADMIN.  "
                     "No fd-event backend will be active.  "
                     "Run as root or upgrade to kernel 5.8+ and grant "
                     "CAP_BPF + CAP_PERFMON for eBPF instead.");
            }
        }
    }

    /* ── Write monitoring (stdout/stderr capture) ─────────────── */

    {
        bool ebpf_ok = kernel_ge(kmaj, kmin, 5, 5);
        bool cap_ok  = kernel_ge(kmaj, kmin, 5, 8)
                         ? ((euid == 0) || (probe_cap(39) && probe_cap(38)))
                         : ((euid == 0) || probe_cap(21));

        if (ebpf_ok && cap_ok) {
            PUSH(COMPAT_OK, "Write monitoring (stdout/stderr)", "available",
                 "Real-time stdout/stderr capture from watched process trees "
                 "is fully functional via eBPF write tracepoints.");
        } else if (!ebpf_ok) {
            PUSH(COMPAT_FAIL, "Write monitoring (stdout/stderr)", "unavailable",
                 "Requires eBPF (Linux 5.5+).  Running kernel %d.%d.",
                 kmaj, kmin);
        } else {
            PUSH(COMPAT_FAIL, "Write monitoring (stdout/stderr)", "no permission",
                 "eBPF capability missing — see eBPF row above.");
        }
    }

    /* ── Socket resolution ────────────────────────────────────── */

    {
        bool tcp4 = (access("/proc/net/tcp",  R_OK) == 0);
        bool tcp6 = (access("/proc/net/tcp6", R_OK) == 0);
        bool udp  = (access("/proc/net/udp",  R_OK) == 0);
        bool unix_= (access("/proc/net/unix", R_OK) == 0);

        if (tcp4 && udp && unix_) {
            PUSH(COMPAT_OK, "Socket resolution", "available",
                 "TCP/UDP/Unix socket details available.%s",
                 tcp6 ? "  IPv6 supported." : "  IPv6 not available.");
        } else {
            PUSH(COMPAT_WARN, "Socket resolution", "partial",
                 "Some /proc/net entries are missing: tcp=%s tcp6=%s "
                 "udp=%s unix=%s.  Network details may be incomplete.",
                 tcp4 ? "yes" : "no",
                 tcp6 ? "yes" : "no",
                 udp  ? "yes" : "no",
                 unix_? "yes" : "no");
        }
    }

        /* ── eBPF network throughput ───────────────────────────────────── */

    {
        /*
         * Network throughput has two tiers:
         *
         *   TCP  — INET_DIAG netlink.  Works as any user for own sockets;
         *          needs CAP_NET_ADMIN for all processes.
         *
         *   UDP/UDP6 — eBPF kprobes on udp_sendmsg / udp_recvmsg /
         *              udpv6_sendmsg / udpv6_recvmsg.  Requires:
         *               • root / CAP_BPF + CAP_PERFMON
         *               • CONFIG_DEBUG_INFO_BTF=y  (for struct offsets)
         *
         * Without BTF the UDP inode-matching code reads the wrong struct
         * offset and always returns inode 0 → all UDP events are silently
         * dropped and throughput shows 0 B/s.
         */
        bool ebpf_kernel = kernel_ge(kmaj, kmin, 5, 5);
        bool cap_bpf_ok  = kernel_ge(kmaj, kmin, 5, 8)
                             ? ((euid == 0) || (probe_cap(39) && probe_cap(38)))
                             : ((euid == 0) || probe_cap(21));
        bool bpf_syscall_net = false;
        {
            long r = syscall(__NR_bpf, 0, NULL, 0);
            bpf_syscall_net = (r == -1 && (errno == EINVAL || errno == EFAULT));
        }

        /* /sys/kernel/btf/vmlinux exists iff CONFIG_DEBUG_INFO_BTF=y */
        bool btf_present = (access("/sys/kernel/btf/vmlinux", F_OK) == 0);

        /* INET_DIAG: available to all users for own sockets */
        bool cap_net_admin = (euid == 0) || probe_cap(12 /* CAP_NET_ADMIN */);

        bool tcp_full = cap_net_admin;
        bool udp_ok   = ebpf_kernel && cap_bpf_ok && bpf_syscall_net && btf_present;

        if (tcp_full && udp_ok) {
            PUSH(COMPAT_OK, "Network throughput", "full",
                 "TCP throughput via INET_DIAG (all processes).  "
                 "UDP/UDP6 (QUIC, WebRTC) throughput via eBPF kprobes: "
                 "udp_sendmsg, udp_recvmsg, udpv6_sendmsg, udpv6_recvmsg.  "
                 "BTF present — struct offsets resolved correctly.");
        } else if (!ebpf_kernel) {
            /* Can still do TCP */
            PUSH(COMPAT_WARN, "Network throughput", "TCP only",
                 "TCP throughput via INET_DIAG is %s.  "
                 "UDP/UDP6 eBPF speed requires Linux 5.5+ (running %d.%d).",
                 tcp_full ? "full (all processes)" : "own sockets only",
                 kmaj, kmin);
        } else if (!cap_bpf_ok || !bpf_syscall_net) {
            PUSH(COMPAT_WARN, "Network throughput", "TCP only",
                 "TCP throughput via INET_DIAG is %s.  "
                 "UDP/UDP6 eBPF speed is unavailable: missing CAP_BPF + CAP_PERFMON.  "
                 "Run as root or grant capabilities to see QUIC/WebRTC rates.",
                 tcp_full ? "full (all processes)" : "own sockets only");
        } else if (!btf_present) {
            PUSH(COMPAT_WARN, "Network throughput", "TCP only",
                 "TCP throughput via INET_DIAG is %s.  "
                 "UDP/UDP6 eBPF speed is unavailable: "
                 "/sys/kernel/btf/vmlinux not found.  "
                 "CONFIG_DEBUG_INFO_BTF=y is required for the eBPF kprobes "
                 "to resolve struct sock → sk_socket offsets correctly.  "
                 "Recompile the kernel with CONFIG_DEBUG_INFO_BTF=y.",
                 tcp_full ? "full (all processes)" : "own sockets only");
        } else {
            /* TCP only (no CAP_NET_ADMIN) */
            PUSH(COMPAT_WARN, "Network throughput", "own sockets only",
                 "TCP INET_DIAG and UDP/UDP6 eBPF are constrained to your "
                 "own process sockets.  Run as root or grant "
                 "CAP_NET_ADMIN to see all processes' network rates.");
        }
    }

    /* ── Container detection ──────────────────────────────────────── */

    {
        /* Check /proc/1/cgroup for cgroup presence */
        bool cgroup = (access("/proc/1/cgroup", R_OK) == 0);
        /* PID namespace inode needs ptrace for foreign processes */
        bool has_ptrace = (euid == 0) || probe_cap(19);

        if (cgroup && has_ptrace) {
            PUSH(COMPAT_OK, "Container detection", "full",
                 "cgroup parsing and PID-namespace inode comparison both "
                 "available.  Docker, Podman, LXC, Kubernetes containers "
                 "will be labelled.");
        } else if (cgroup) {
            PUSH(COMPAT_WARN, "Container detection", "partial",
                 "cgroup labels work for your own processes.  PID-namespace "
                 "inode comparison for foreign containers requires "
                 "CAP_SYS_PTRACE.  Container heuristic may miss some cases.");
        } else {
            PUSH(COMPAT_FAIL, "Container detection", "unavailable",
                 "/proc/<pid>/cgroup is not readable.  Container labels "
                 "will not appear.");
        }
    }

    /* ── systemd / OpenRC service resolution ─────────────────── */

    {
        bool systemd = (access("/run/systemd/system", F_OK) == 0);
        bool openrc  = (access("/run/openrc",         F_OK) == 0);

        if (systemd) {
            PUSH(COMPAT_OK, "Service resolution", "systemd",
                 "systemd detected.  Service unit names will appear in the "
                 "Service column for managed processes.");
        } else if (openrc) {
            PUSH(COMPAT_OK, "Service resolution", "OpenRC",
                 "OpenRC detected.  Service names will be resolved from "
                 "/run/openrc/daemons/.");
        } else {
            PUSH(COMPAT_INFO, "Service resolution", "n/a",
                 "Neither systemd nor OpenRC runtime directories found.  "
                 "Service column will be empty.");
        }
    }

    /* ── Device labelling ─────────────────────────────────────── */

    {
        bool sysfs = (access("/sys/class", R_OK) == 0);
        if (sysfs) {
            PUSH(COMPAT_OK, "Device labelling", "available",
                 "/sys/class is readable.  GPU, sound card, input device, "
                 "block device, and camera names will be resolved.");
        } else {
            PUSH(COMPAT_WARN, "Device labelling", "unavailable",
                 "/sys/class is not accessible.  Device names in the fd "
                 "list will fall back to raw /dev paths.");
        }
    }

    /* ── Signal (End Process) ─────────────────────────────────── */

    {
        bool cap_kill = (euid == 0) || probe_cap(5 /* CAP_KILL */);
        if (cap_kill) {
            PUSH(COMPAT_OK, "End Process / Send Signal", "available",
                 "Can send signals to any process.");
        } else {
            PUSH(COMPAT_WARN, "End Process / Send Signal", "own processes only",
                 "Can only send signals to your own processes.  "
                 "Attempting to end another user's process will fail silently.  "
                 "Run as root or grant CAP_KILL to signal any process.");
        }
    }

    /* ── PipeWire audio ───────────────────────────────────────── */

    {
#ifdef HAVE_PIPEWIRE
        char pw_sock[PATH_MAX] = "";
        const char *runtime = getenv("PIPEWIRE_RUNTIME_DIR");
        if (!runtime || !runtime[0]) runtime = getenv("XDG_RUNTIME_DIR");
        const char *remote = getenv("PIPEWIRE_REMOTE");
        if (!remote || !remote[0]) remote = "pipewire-0";
        if (remote[0] == '/') snprintf(pw_sock, sizeof(pw_sock), "%s", remote);
        else if (runtime && runtime[0]) snprintf(pw_sock, sizeof(pw_sock), "%s/%s", runtime, remote);
        else snprintf(pw_sock, sizeof(pw_sock), "/run/user/%u/%s", (unsigned)desktop_session_uid(), remote);
        const char *pw_how = "invoking user's session";

        if (pw_sock[0] && access(pw_sock, F_OK) == 0) {
            PUSH(COMPAT_OK, "PipeWire audio monitoring", "available",
                 "Socket found at %s (%s).  "
                 "Audio process graph, peak meters, and spectrogram "
                 "are fully functional.", pw_sock, pw_how);
        } else if (pw_sock[0]) {
            PUSH(COMPAT_WARN, "PipeWire audio monitoring", "no socket",
                 "PipeWire socket not found at %s.  "
                 "Audio plugins will load but show no data.  "
                 "Ensure PipeWire is running for the desktop session.",
                 pw_sock);
        } else {
            PUSH(COMPAT_WARN, "PipeWire audio monitoring", "no socket",
                 "Could not locate a PipeWire socket in /run/user/.  "
                 "Set PIPEWIRE_REMOTE or start PipeWire for your session.");
        }
#else
        PUSH(COMPAT_INFO, "PipeWire audio monitoring", "not built",
             "evemon was compiled without PipeWire support.  "
             "Rebuild with -DHAVE_PIPEWIRE to enable audio graphs "
             "and spectrogram visualisation.");
#endif
    }

    /* ── eBPF kernel config (informational if config accessible) ── */

    {
        /* Only show this block if we could open the kernel config at all */
        char cfg_path[PATH_MAX];
        struct utsname uts2;
        uname(&uts2);
        snprintf(cfg_path, sizeof(cfg_path), "/boot/config-%s", uts2.release);
        FILE *cfg_test = fopen(cfg_path, "r");
        if (!cfg_test) cfg_test = fopen("/proc/config", "r");

        if (cfg_test) {
            fclose(cfg_test);
            bool bpf_ok     = kconfig_option_enabled("CONFIG_BPF");
            bool bpf_sys    = kconfig_option_enabled("CONFIG_BPF_SYSCALL");
            bool bpf_ev     = kconfig_option_enabled("CONFIG_BPF_EVENTS");
            bool ftrace     = kconfig_option_enabled("CONFIG_FTRACE_SYSCALLS");
            bool perf       = kconfig_option_enabled("CONFIG_PERF_EVENTS");
            bool btf        = kconfig_option_enabled("CONFIG_DEBUG_INFO_BTF");

            bool all_ok = bpf_ok && bpf_sys && bpf_ev && ftrace && perf;
            if (all_ok && btf) {
                PUSH(COMPAT_OK, "eBPF kernel config", "complete",
                     "All required kernel options are present "
                     "(BPF, BPF_SYSCALL, BPF_EVENTS, FTRACE_SYSCALLS, "
                     "PERF_EVENTS, DEBUG_INFO_BTF).  "
                     "BTF is available — UDP/UDP6 struct offsets are "
                     "resolved correctly and QUIC/WebRTC throughput is fully "
                     "functional.");
            } else if (all_ok) {
                PUSH(COMPAT_WARN, "eBPF kernel config", "no BTF",
                     "Core eBPF options are present (BPF, BPF_SYSCALL, "
                     "BPF_EVENTS, FTRACE_SYSCALLS, PERF_EVENTS) but "
                     "CONFIG_DEBUG_INFO_BTF is not set.  "
                     "UDP/UDP6 eBPF kprobes require BTF to read "
                     "struct sock → sk_socket offsets; without it all "
                     "UDP throughput shows as 0 B/s.  "
                     "Recompile the kernel with CONFIG_DEBUG_INFO_BTF=y.");
            } else {
                PUSH(COMPAT_WARN, "eBPF kernel config", "incomplete",
                     "Some eBPF kernel options may be missing: "
                     "CONFIG_BPF=%s  CONFIG_BPF_SYSCALL=%s  "
                     "CONFIG_BPF_EVENTS=%s  CONFIG_FTRACE_SYSCALLS=%s  "
                     "CONFIG_PERF_EVENTS=%s  CONFIG_DEBUG_INFO_BTF=%s",
                     bpf_ok ? "y" : "?",
                     bpf_sys ? "y" : "?",
                     bpf_ev  ? "y" : "?",
                     ftrace  ? "y" : "?",
                     perf    ? "y" : "?",
                     btf    ? "y" : "?");
            }
        }
        /* If we can't read the kernel config, skip this row rather
         * than reporting a spurious failure. */
    }

#undef PUSH

    return n;
}

