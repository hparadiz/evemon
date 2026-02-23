# Linux Kernel Requirements

allmon relies on several kernel subsystems, syscalls, and pseudo-filesystem
interfaces.  This document lists every requirement, grouped by feature tier.

---

## Feature Tiers

| Tier | What it enables | Min kernel | Capabilities needed |
|------|-----------------|------------|---------------------|
| **Core** | Process tree, CPU%, memory, containers, services | 2.6.24 | `CAP_SYS_PTRACE`, `CAP_DAC_READ_SEARCH` |
| **FD monitoring — fanotify** | File open/close tracking (mount-wide) | 5.1 | `CAP_SYS_ADMIN` |
| **FD monitoring — eBPF** | Full fd lifecycle incl. sockets/pipes | 5.5 | `CAP_BPF` + `CAP_PERFMON` (≥ 5.8) or `CAP_SYS_ADMIN` |
| **Socket resolution** | TCP/UDP/Unix socket detail in sidebar | 2.6+ | — |
| **Device labelling** | Human-readable names for /dev nodes | 2.6.16 | — |

Running as root satisfies all capability requirements, but it is **not
required**.  When running as a normal (non-root) user the following features
are unavailable or degraded:

| Feature | Why it fails without root | Capability needed |
|---------|--------------------------|-------------------|
| FD enumeration for other users' processes | Cannot read `/proc/<pid>/fd` | `CAP_DAC_READ_SEARCH` |
| Working directory & status of other users' processes | Blocked by `ptrace_may_access` | `CAP_SYS_PTRACE` |
| fanotify file-descriptor monitoring | `fanotify_init()` requires admin | `CAP_SYS_ADMIN` |
| eBPF file-descriptor monitoring (kernel < 5.8) | `bpf()` requires admin | `CAP_SYS_ADMIN` |
| eBPF file-descriptor monitoring (kernel ≥ 5.8) | `bpf()` and perf buffers | `CAP_BPF` + `CAP_PERFMON` |
| End Process (send signals) | Cannot signal other users' processes | `CAP_KILL` |
| Container detection via PID namespace | Cannot read `/proc/<pid>/ns/pid` for foreign processes | `CAP_SYS_PTRACE` |

The core process tree, CPU%, and memory columns for the **current user's own
processes** will still work without any special privileges.  Socket resolution,
device labelling, and `/sys` queries are also unaffected.

---

## Kernel Config Options

### Essential

| Option | Required by | Notes |
|--------|-------------|-------|
| `CONFIG_PROC_FS` | Core process monitoring | All `/proc` reads depend on this |
| `CONFIG_SYSFS` | Device labelling | `/sys/class/*` queries |

### eBPF backend

| Option | Required by | Notes |
|--------|-------------|-------|
| `CONFIG_BPF` | eBPF fd monitor | Base BPF support |
| `CONFIG_BPF_SYSCALL` | eBPF fd monitor | `bpf()` syscall |
| `CONFIG_BPF_EVENTS` | eBPF fd monitor | Tracepoint BPF programs |
| `CONFIG_FTRACE_SYSCALLS` | eBPF fd monitor | `tracepoint/syscalls/*` attachment |
| `CONFIG_TRACEPOINTS` | eBPF fd monitor | Tracepoint infrastructure |
| `CONFIG_PERF_EVENTS` | eBPF fd monitor | Perf ring-buffer output to userspace |
| `CONFIG_DEBUG_INFO_BTF` | eBPF fd monitor | Recommended for CO-RE / libbpf |

### fanotify backend

| Option | Required by | Notes |
|--------|-------------|-------|
| `CONFIG_FANOTIFY` | fanotify fd monitor | `fanotify_init` / `fanotify_mark` |

### Optional (graceful degradation)

| Option | Required by | Notes |
|--------|-------------|-------|
| `CONFIG_CGROUPS` | Container & service detection | `/proc/<pid>/cgroup` parsing |
| `CONFIG_PID_NS` | Container detection | PID namespace inode comparison |
| `CONFIG_INET` | Socket resolution | `/proc/net/tcp`, `/proc/net/udp` |
| `CONFIG_IPV6` | IPv6 socket resolution | `/proc/net/tcp6`, `/proc/net/udp6` |
| `CONFIG_UNIX` | Unix socket resolution | `/proc/net/unix` |
| `CONFIG_NET` | All socket resolution | Prerequisite for the above |

---

## `/proc` Paths Read

| Path | Purpose |
|------|---------|
| `/proc/` (directory scan) | Enumerate all PIDs |
| `/proc/<pid>/comm` | Process name |
| `/proc/<pid>/cmdline` | Full command line |
| `/proc/<pid>/status` | PPid, Uid, VmRSS |
| `/proc/<pid>/stat` | CPU ticks (utime, stime), start time, PPID |
| `/proc/<pid>/cgroup` | Container runtime detection, systemd/OpenRC service unit |
| `/proc/<pid>/cwd` | Working directory (readlink) |
| `/proc/<pid>/ns/pid` | PID namespace inode (container heuristic) |
| `/proc/<pid>/fd/` | Open file descriptor enumeration |
| `/proc/<pid>/fd/<n>` | Resolve fd target path (readlink) |
| `/proc/1/ns/pid` | Init PID namespace reference inode |
| `/proc/self/exe` | Locate eBPF object file at runtime |
| `/proc/self/fd/<n>` | Resolve fanotify event metadata fd |
| `/proc/stat` | System boot time (`btime`) |
| `/proc/uptime` | System uptime |
| `/proc/loadavg` | Load averages |
| `/proc/net/tcp` | IPv4 TCP socket table |
| `/proc/net/udp` | IPv4 UDP socket table |
| `/proc/net/tcp6` | IPv6 TCP socket table |
| `/proc/net/udp6` | IPv6 UDP socket table |
| `/proc/net/unix` | Unix domain socket table |

---

## `/sys` Paths Read

| Path | Purpose |
|------|---------|
| `/sys/class/drm/<node>/device/uevent` | GPU PCI vendor/device ID |
| `/sys/class/sound/card<N>/id` | Sound card identity |
| `/sys/class/input/<dev>/device/name` | Input device name |
| `/sys/block/<dev>/device/model` | Block device model |
| `/sys/class/video4linux/<dev>/name` | Camera/video device name |

---

## `/run` Paths Read

| Path | Purpose |
|------|---------|
| `/run/systemd/system` | Detect systemd init (stat) |
| `/run/openrc` | Detect OpenRC init (stat) |
| `/run/openrc/daemons/<svc>/<inst>` | OpenRC service → PID mapping |

---

## Syscalls

| Syscall | Used for | Backend |
|---------|----------|---------|
| `fanotify_init` | Create fanotify notification group | fanotify |
| `fanotify_mark` | Watch root mount for open/close events | fanotify |
| `bpf` (via libbpf) | Load programs, create maps, attach tracepoints | eBPF |
| `perf_event_open` (via libbpf) | Set up per-CPU perf ring buffers | eBPF |
| `poll` | Wait for fanotify events | fanotify |
| `read` | Consume fanotify event metadata | fanotify |
| `readlink` | Resolve `/proc` symlinks (fd, cwd, exe) | Core |
| `opendir` / `readdir` | Scan `/proc`, `/proc/<pid>/fd`, `/run/openrc` | Core |
| `stat` | Namespace inodes, init system detection | Core |
| `getpwuid_r` | UID → username resolution | Core |
| `sigaction` | Handle SIGINT / SIGTERM for clean shutdown | Core |
| `clock_gettime` | Monotonic + realtime clocks for profiling | Core |
| `sysconf` | `_SC_CLK_TCK`, `_SC_NPROCESSORS_ONLN` | Core |
| `kill` | Send SIGTERM / SIGKILL to processes | UI |

---

## Capabilities

| Capability | Required for | Notes |
|------------|-------------|-------|
| `CAP_SYS_ADMIN` | `fanotify_init()`, `bpf()` on kernels < 5.8 | Broad; running as root satisfies this |
| `CAP_BPF` | `bpf()` syscall | Kernel ≥ 5.8; narrower alternative to `CAP_SYS_ADMIN` |
| `CAP_PERFMON` | `perf_event_open()` for perf buffers | Kernel ≥ 5.8 |
| `CAP_DAC_READ_SEARCH` | Read `/proc/<pid>/fd` for other users | Needed for fd enumeration |
| `CAP_SYS_PTRACE` | Read `/proc/<pid>/cwd`, status of foreign processes | Bypasses `ptrace_may_access` |
| `CAP_KILL` | Send signals to processes from the UI | Required for End Process |

---

## eBPF Program Details

### Program type

All three BPF programs are `BPF_PROG_TYPE_TRACEPOINT`:

| SEC annotation | Tracepoint |
|----------------|------------|
| `tracepoint/syscalls/sys_enter_openat` | Capture filename on `openat` entry |
| `tracepoint/syscalls/sys_exit_openat` | Emit open event with resolved path |
| `tracepoint/syscalls/sys_enter_close` | Emit close event |

### BPF maps

| Map | Type | Key | Value | Max entries |
|-----|------|-----|-------|-------------|
| `events` | `PERF_EVENT_ARRAY` | CPU id | fd | per-CPU (auto) |
| `open_args` | `HASH` | `__u32` (tid) | `__u64` (filename ptr) | 8192 |

### BPF helpers used

| Helper | Min kernel |
|--------|------------|
| `bpf_get_current_pid_tgid` | 4.2 |
| `bpf_map_update_elem` | 3.19 |
| `bpf_map_lookup_elem` | 3.19 |
| `bpf_map_delete_elem` | 3.19 |
| `bpf_ktime_get_ns` | 4.1 |
| `bpf_probe_read_user_str` | **5.5** |
| `bpf_perf_event_output` | 4.4 |

The highest requirement is `bpf_probe_read_user_str` at **kernel 5.5**, which
sets the floor for the eBPF backend.

### Build requirements

The BPF object is compiled with:

```
clang -O2 -target bpf -g -D__TARGET_ARCH_x86
```

and loaded at runtime from beside the main executable.  Linked against
`-lbpf -lelf -lz` (libbpf).

---

## Cgroup Support

Both cgroup v1 and v2 are supported for container detection and systemd
service resolution:

| Version | Detection method |
|---------|-----------------|
| cgroup v2 | Lines starting with `0::` in `/proc/<pid>/cgroup` |
| cgroup v1 | `name=systemd` or `:systemd:` hierarchy lines |

Container runtimes detected by cgroup path keywords:
`docker`, `moby`, `lxc`, `libpod` (Podman), `kubepods`/`kubelet` (Kubernetes),
`containerd`, `machine.slice`/`machine-` (systemd-nspawn), `garden`,
`buildkit`.

---

## Minimum Kernel Version Summary

| Backend | Minimum | Bottleneck |
|---------|---------|------------|
| Core process monitor | **2.6.24** | `/proc/<pid>/cgroup` |
| + fanotify fd tracking | **5.1** | `FAN_UNLIMITED_QUEUE` |
| + eBPF fd tracking | **5.5** | `bpf_probe_read_user_str` |

**Recommended: Linux 5.8+** for fine-grained capabilities (`CAP_BPF`,
`CAP_PERFMON`) instead of blanket `CAP_SYS_ADMIN`.
