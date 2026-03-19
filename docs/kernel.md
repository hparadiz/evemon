# Linux Kernel Requirements

evemon relies on several kernel subsystems, syscalls, and pseudo-filesystem
interfaces.  This document lists every requirement, grouped by feature tier.

---

## Feature Tiers

| Tier | What it enables | Min kernel | Capabilities needed |
|------|-----------------|------------|---------------------|
| **Core** | Process tree, CPU%, memory, containers, services | 2.6.24 | `CAP_SYS_PTRACE`, `CAP_DAC_READ_SEARCH` |
| **FD monitoring — fanotify** | File open/close tracking (mount-wide) | 5.1 | `CAP_SYS_ADMIN` |
| **FD monitoring — eBPF** | Full fd lifecycle incl. sockets/pipes | 5.5 | `CAP_BPF` + `CAP_PERFMON` (≥ 5.8) or `CAP_SYS_ADMIN` |
| **Write monitoring — eBPF** | Capture stdout/stderr of selected process tree in real time | 5.5 | `CAP_BPF` + `CAP_PERFMON` (≥ 5.8) or `CAP_SYS_ADMIN` |
| **Orphan-stdout capture** | Auto-intercept writes from exec'd processes with non-TTY stdout | 5.5 | `CAP_BPF` + `CAP_PERFMON` |
| **Network throughput — TCP** | Per-socket send/recv bytes/s via INET_DIAG + eBPF kprobes | 5.5 | `CAP_BPF` + `CAP_PERFMON` |
| **Network throughput — UDP/IPv6** | Per-socket send/recv bytes/s for UDP4, UDP6, QUIC, WebRTC | 5.5 | `CAP_BPF` + `CAP_PERFMON` |
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
| `CONFIG_DEBUG_INFO_BTF` | eBPF network throughput | **Required** for correct struct offset resolution at load time. Without this, socket inode reading silently returns 0 and UDP/IPv6 throughput will show zero. |
| `CONFIG_DEBUG_INFO_BTF_MODULES` | eBPF (modules) | Recommended alongside `DEBUG_INFO_BTF` |

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
| `/proc/<pid>/environ` | Environment variables (Steam detection, plugin broker) |
| `/proc/<pid>/maps` | Memory-mapped regions (mmap plugin, shared-library list) |
| `/proc/<pid>/task/` | Thread directory enumeration |
| `/proc/<pid>/task/<tid>/comm` | Per-thread name |
| `/proc/<pid>/task/<tid>/stat` | Per-thread CPU ticks, state, priority, nice, last CPU (fields 14–15, 18–19, 39) |
| `/proc/<pid>/task/<tid>/status` | Per-thread voluntary/involuntary context switches |
| `/proc/<pid>/io` | Cumulative `read_bytes` / `write_bytes` for disk I/O rate |
| `/proc/<pid>/fd/` | Open file descriptor enumeration |
| `/proc/<pid>/fd/<n>` | Resolve fd target path (readlink) |
| `/proc/1/ns/pid` | Init PID namespace reference inode |
| `/proc/self/exe` | Locate eBPF object file at runtime |
| `/proc/self/fd/<n>` | Resolve fanotify event metadata fd |
| `/proc/sys/kernel/pid_max` | Resize `monitored_pids` BPF map to match live kernel limit |
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
| `/sys/fs/cgroup/<path>/` | cgroup v2 resource limit files: `memory.current`, `memory.max`, `memory.high`, `memory.swap.max`, `cpu.max`, `pids.current`, `pids.max`, `io.max` |

---

## `/run` Paths Read

| Path | Purpose |
|------|---------|
| `/run/systemd/system` | Detect systemd init (stat) |
| `/run/openrc` | Detect OpenRC init (stat) |
| `/run/openrc/daemons/<svc>/<inst>` | OpenRC service → PID mapping |
| `/run/user/<uid>/pipewire-0` | PipeWire socket discovery for per-user sessions |

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
| `clock_gettime` | Monotonic + realtime clocks for profiling and startup timing | Core |
| `sysconf` | `_SC_CLK_TCK`, `_SC_NPROCESSORS_ONLN` | Core |
| `kill` | Send SIGTERM / SIGKILL to processes | UI |
| `getutxent` / `utmpx` | Count logged-in users for the status bar | UI |

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

FD and write monitoring programs are `BPF_PROG_TYPE_TRACEPOINT`.
Network throughput programs are `BPF_PROG_TYPE_KPROBE` (kprobe/kretprobe).

#### FD monitoring programs

| SEC annotation | Tracepoint | Purpose |
|----------------|------------|---------|
| `tracepoint/syscalls/sys_enter_openat` | `sys_enter_openat` | Save filename ptr on openat entry |
| `tracepoint/syscalls/sys_exit_openat` | `sys_exit_openat` | Emit open event with resolved path |
| `tracepoint/syscalls/sys_enter_close` | `sys_enter_close` | Emit close event |

#### Write monitoring programs

Hook all write-family syscalls for monitored pids. Each program does a
single hash lookup of the current tgid in `monitored_pids`; if the pid is
not present it returns 0 immediately (negligible overhead system-wide).

| SEC annotation | Syscall | Notes |
|----------------|---------|-------|
| `tracepoint/syscalls/sys_enter_write` | `write` | Copies payload inline into per-CPU scratch |
| `tracepoint/syscalls/sys_enter_writev` | `writev` | Walks iovec, copies first non-empty segment |
| `tracepoint/syscalls/sys_enter_pwrite64` | `pwrite64` | Same as write but with offset |
| `tracepoint/syscalls/sys_enter_sendto` | `sendto` | Socket write treated as fd write |
| `tracepoint/syscalls/sys_enter_sendmsg` | `sendmsg` | Zero-payload notification |
| `tracepoint/syscalls/sys_enter_sendmmsg` | `sendmmsg` | Zero-payload notification |
| `tracepoint/syscalls/sys_enter_splice` | `splice` | Uses `fd_out`; no user buffer to copy |
| `tracepoint/syscalls/sys_enter_sendfile` | `sendfile` | Uses `out_fd`; no user buffer to copy |

#### Process lifecycle programs

| SEC annotation | Tracepoint | Purpose |
|----------------|------------|---------|
| `tracepoint/sched/sched_process_exit` | `sched_process_exit` | Remove exited PID from `monitored_pids` map so dead PIDs never accumulate. Only fires on group-leader exit (pid == tgid) |
| `tracepoint/sched/sched_process_exec` | `sched_process_exec` | Emit `FDMON_BPF_EXEC` event so userspace can re-confirm child map entries and perform orphan-stdout detection |
| `tracepoint/sched/sched_process_fork` | `sched_process_fork` | Propagate parent's `monitored_pids` entry to child — zero latency, in kernel |

The fork program is the key to capturing output from short-lived children
(lifetime < 10 ms). It runs synchronously with the fork syscall, so the
child's map entry exists before the child's first `write()` can fire.

#### Network throughput programs (kprobes)

These kprobes fire on every send/recv call and emit byte counts keyed by
socket inode. Matching is done by inode (read from `sock → sk_socket → file
→ f_inode → i_ino`) rather than IPv4 4-tuple, so UDP6, QUIC, and any other
protocol where the IPv4 address fields are zero are handled correctly.

`CONFIG_DEBUG_INFO_BTF=y` is required for the struct offsets to be
resolved correctly at load time.

The inode read uses fixed kernel struct offsets (verified via pahole/BTF):

| Field | Struct | Offset |
|-------|--------|--------|
| `sk_socket` | `struct sock` | 288 |
| `file` | `struct socket` | 16 |
| `f_inode` | `struct file` | 32 |
| `i_ino` | `struct inode` | 64 |

| SEC annotation | Kernel function | Direction | Notes |
|----------------|-----------------|-----------|-------|
| `kprobe/tcp_sendmsg` | `tcp_sendmsg` | send | IPv4 + v4-mapped IPv6 TCP |
| `kretprobe/tcp_recvmsg` | `tcp_recvmsg` | recv | Return value = bytes received |
| `kprobe/udp_sendmsg` | `udp_sendmsg` | send | IPv4 UDP |
| `kretprobe/udp_recvmsg` | `udp_recvmsg` | recv | IPv4 UDP |
| `kprobe/udpv6_sendmsg` | `udpv6_sendmsg` | send | IPv6 UDP (separate kernel function from IPv4) |
| `kretprobe/udpv6_recvmsg` | `udpv6_recvmsg` | recv | IPv6 UDP — covers QUIC, WebRTC, and all other UDP6 traffic |

TCP throughput is additionally cross-checked via INET_DIAG netlink
(`sock_diag`) as the primary source when available; eBPF serves as
the fallback and as the sole source for UDP (INET_DIAG has no UDP
byte counters).

---

### BPF maps

| Map | Type | Key | Value | Max entries | Purpose |
|-----|------|-----|-------|-------------|----------|
| `events` | `PERF_EVENT_ARRAY` | CPU id | fd | per-CPU | Deliver events to userspace perf buffer |
| `open_args` | `HASH` | `__u32` tid | `__u64` filename ptr | 8192 | Pass filename ptr from enter to exit tracepoint |
| `monitored_pids` | `HASH` | `__u32` pid (tgid) | `__u8` fd_mask | dynamic (resized from `/proc/sys/kernel/pid_max` at load time; BPF default 32768) | Which pids to capture and which fds (bit0=fd1, bit1=fd2) |
| `write_event_scratch` | `PERCPU_ARRAY` | `__u32` 0 | `struct fdmon_bpf_event` | 1 | Per-CPU scratch to hold write payload (up to 4096 bytes of `write.data`) — avoids 512-byte BPF stack limit |
| `recvmsg_args` | `HASH` | `__u32` tid | `__u64` sock ptr | 4096 | Stash `struct sock *` between kprobe entry and kretprobe exit for tcp/udp/udpv6 recvmsg |

#### `monitored_pids` fd_mask encoding

```
bit 0 = fd 1 (stdout)
bit 1 = fd 2 (stderr)
```

Userspace sets the mask for the root process via `fdmon_watch_parent_fds()`.
The fork tracepoint copies it to every child automatically.

---

### BPF helpers used

| Helper | Min kernel | Used by |
|--------|------------|---------|
| `bpf_get_current_pid_tgid` | 4.2 | All programs |
| `bpf_map_update_elem` | 3.19 | All programs |
| `bpf_map_lookup_elem` | 3.19 | All programs |
| `bpf_map_delete_elem` | 3.19 | FD monitor |
| `bpf_ktime_get_ns` | 4.1 | Write + FD programs |
| `bpf_probe_read_user_str` | **5.5** | FD open (filename) |
| `bpf_probe_read_user` | 5.5 | Write programs (payload copy) |
| `bpf_perf_event_output` | 4.4 | All programs |

The highest requirement remains `bpf_probe_read_user_str` / `bpf_probe_read_user`
at **kernel 5.5**, which sets the floor for the entire eBPF backend.

---

### Build requirements

The BPF object is compiled with:

```
clang -O2 -target bpf -g -D__TARGET_ARCH_x86
```

and loaded at runtime from beside the main executable.  Linked against
`-lbpf -lelf -lz` (libbpf).

The main binary is compiled with gcc (`-std=c11 -O2 -pthread -D_GNU_SOURCE`)
and linked with GTK 3, GLib/GIO, fontconfig, jansson (JSON settings),
libepoxy (OpenGL for MilkDrop), and optionally libpipewire-0.3 and
libsoup-3.0.  The binary is hardened: `-fstack-protector-strong`,
`-D_FORTIFY_SOURCE=2`, `-fPIE`, `-pie`, `-Wl,-z,relro,-z,now`.

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
| + UDP/IPv6 network throughput | **5.5** + `CONFIG_DEBUG_INFO_BTF=y` | Struct offset resolution for socket inode reading |

**Recommended: Linux 5.8+** for fine-grained capabilities (`CAP_BPF`,
`CAP_PERFMON`) instead of blanket `CAP_SYS_ADMIN`.

**Recommended kernel config for full functionality:**
```
CONFIG_DEBUG_INFO_BTF=y
CONFIG_DEBUG_INFO_BTF_MODULES=y
```
Without `CONFIG_DEBUG_INFO_BTF`, socket inode reads in the eBPF programs
return 0 and UDP/IPv6 throughput will always show zero.
