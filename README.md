# evemon

A graphical Linux process monitor focused on deep per-process introspection — built with C11, GTK 3, and eBPF.

> *"What is this process doing and why?"*

evemon drills into individual processes: their file descriptors, network sockets, environment, memory maps, shared libraries, cgroup limits, container context, Steam/Proton metadata, and even live PipeWire audio streams — all in one place.

---

## Features

### Process Tree
- Full parent/child hierarchy with expand/collapse
- Per-process and group-aggregate CPU%, RSS, and disk I/O columns
- Animated I/O sparklines per row (20-sample sliding window)
- Green fade-in for new processes, red fade-out for dying ones
- Pin any process to the top of the tree for persistent tracking
- Sort by any column (▲ = largest first for natural CPU/RSS ordering)
- Name filter (`Ctrl+F`) and Go-to-PID (`Ctrl+G`)

### Container & Service Detection
- **Containers** — Docker, Podman, LXC, Kubernetes, containerd, nspawn, Garden, Buildkit (cgroup + PID namespace heuristics)
- **Services** — systemd units (cgroup v1 + v2) and OpenRC services

### Steam / Proton
Detects Steam game launch trees via multi-pass parent-environment inheritance and resolves App ID, game name, Proton version, runtime layer (sniper, soldier, …), compat data path, and game directory.

### Plugin-Based Sidebar
Each inspector is a standalone `.so` plugin loaded at runtime via `dlopen`. A central **data broker** gathers `/proc` data once per tracked PID and distributes it to all active plugin instances — no duplicate syscalls.

Built-in plugins:

| Plugin | What it shows |
|--------|---------------|
| **File Descriptors** | Every open fd categorised into files, devices, sockets (TCP/UDP with addr:port), Unix sockets, pipes, eventfd/epoll/signalfd/timerfd/inotify. Device fds get human-readable sysfs labels (GPU, sound card, input device, …). Descendant-tree merging and duplicate grouping. |
| **Write Monitor** | Live capture of stdout/stderr from the selected process and its entire descendant tree, including processes that live for less than a millisecond. Powered by eBPF syscall tracepoints and an in-kernel fork propagation program — no userspace polling race. |
| **Network Sockets** | Per-connection throughput (send/recv bytes/s) powered by eBPF `tcp_sendmsg`/`tcp_recvmsg` tracepoints. |
| **Environment** | All variables from `/proc/<pid>/environ` classified into Paths, Display/Session, Locale, XDG, Steam/Proton, and Other. |
| **Memory Maps** | All regions from `/proc/<pid>/maps` — Code (r-x), Data (rw-), Heap, Stack, vDSO, Anonymous, and more. |
| **Shared Libraries** | Executable mappings categorised into Runtime, System, Application, Wine Built-in, and Windows DLL groups. Full Wine/Proton prefix awareness. |
| **cgroup Limits** | `memory.max`, `memory.current`, `cpu.max`, `pids.max`, `io.max`, etc. with percentage bars. Hidden when no explicit limits are set. |
| **PipeWire Audio** | Audio graph connections for the selected process with real-time L/R peak meters and an interactive FFT spectrogram. |
| **MilkDrop** | GPU-accelerated MilkDrop visualiser driven by the selected process's audio output via PipeWire. Renders in a dockable GTK surface using OpenGL. |

Plugins can be pinned to a specific PID, docked to any edge of the main tree, or floated as independent windows. Third-party plugins compile against a single public header ([`evemon_plugin.h`](src/evemon_plugin.h)).

### eBPF & Fanotify
- **eBPF backend** — attaches to syscall tracepoints for fd lifecycle (`sys_enter_openat`, `sys_exit_openat`, `sys_enter_close`), all write-family syscalls (`write`, `writev`, `pwrite64`, `sendto`, `sendmsg`, `sendmmsg`, `splice`, `sendfile`), and process lifecycle (`sched_process_fork`, `sched_process_exec`). Network throughput via `tcp_sendmsg` / `tcp_recvmsg` kprobes. Events delivered via perf buffer with per-CPU scratch maps to work around the 512-byte BPF stack limit.
- **Fanotify fallback** — used for fd monitoring when eBPF isn't available (e.g. no `CAP_BPF`).

### UI Polish
- GTK theme switcher (View → Theme)
- Font size control (`Ctrl+Plus` / `Ctrl+Minus` / `Ctrl+0`)
- Middle-click autoscroll with logarithmic velocity
- Drag-to-resize and double-click-to-collapse sidebar sections
- Neofetch-style About dialog with animated distro logo, system info, and HDR status
- Desktop-aware modifier detection (KDE Meta-as-primary)

---

## Screenshot

*(coming soon)*

---

## Building

### Dependencies

| Library | Debian / Ubuntu | Arch | Purpose |
|---------|----------------|------|---------|
| GTK 3 | `libgtk-3-dev` | `gtk3` | GUI |
| fontconfig | `libfontconfig1-dev` | `fontconfig` | Font loading |
| libbpf | `libbpf-dev` | `libbpf` | eBPF fd/network monitor |
| libelf | `libelf-dev` | `libelf` | eBPF ELF loading |
| zlib | `zlib1g-dev` | `zlib` | eBPF compression |
| clang | `clang` | `clang` | BPF kernel program compilation |
| glib-compile-resources | `libglib2.0-dev-bin` | `glib2` | Embedded resources (icon, font) |
| PipeWire *(optional)* | `libpipewire-0.3-dev` | `pipewire` | Audio graph + spectrogram |

### Compile

```sh
make
```

PipeWire support is auto-detected. To explicitly disable it:

```sh
make HAVE_PIPEWIRE=0
```

Build outputs:

```
build/evemon                  # main binary
build/fdmon_ebpf_kern.o       # eBPF kernel program (loaded at runtime)
build/plugins/evemon_*.so     # sidebar plugins
```

### Run

```sh
sudo ./build/evemon
```

Root (or `CAP_BPF` + `CAP_PERFMON`) is required for eBPF tracing and full `/proc` visibility across users. evemon degrades gracefully without privileges — you'll see your own processes and can use the fanotify fallback for fd tracking.

---

## Tree Columns

| Column | Description |
|--------|-------------|
| PID | Process ID |
| PPID | Parent process ID |
| User | Owning user |
| Name | Process name (Steam processes show a rich label, e.g. `reaper (Steam) · Deadlock [Proton ...]`) |
| CPU% | Per-process CPU utilisation |
| RSS | Resident set size |
| Group RSS | Sum of self + all descendant RSS |
| Group CPU% | Sum of self + all descendant CPU% |
| I/O Read | Disk read rate (bytes/s) |
| I/O Write | Disk write rate (bytes/s) |
| I/O Sparkline | Animated 20-sample bar chart of combined I/O history |
| Start Time | Process start time |
| Container | Container runtime label |
| Service | systemd unit or OpenRC service name |
| CWD | Current working directory |
| Command | Full command line |

---

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+F` / `Meta+F` | Toggle name filter |
| `Ctrl+G` / `Meta+G` | Go to PID |
| `Ctrl+=` / `Ctrl++` | Increase font size |
| `Ctrl+-` | Decrease font size |
| `Ctrl+0` | Reset font size |
| `Ctrl+Q` | Quit |
| `Alt` (tap) | Show menu bar |
| `Escape` | Close filter / return focus to tree |
| `←` / `→` | Collapse / expand selected row |
| Middle-click drag | Autoscroll |
| Double-click | Open sidebar for process |

### Context Menus

- **Right-click process** — Pin/Unpin, Copy Command, Send Signal (SIGTERM, SIGKILL, SIGSTOP, SIGCONT, …), Send Signal to Tree
- **Right-click status bar** — Toggle menu bar

---

## Architecture

```
```
┌──────────────────┐      mutex       ┌─────────────────────┐
│  Monitor Thread  │  ─────────────►   │    UI Thread (GTK)    │
│  (2s /proc scan) │  proc_snapshot_t │    1s tree refresh    │
└──────────────────┘                  └──────────┬──────────┘
         │                                       │
    fdmon (eBPF)                          ┌──────┬──────┐
    ├─ openat/close tracepoints              │ Data Broker │
    ├─ write/writev/sendto/... tracepoints   │  (GTask)    │
    ├─ sched_process_fork (propagate map)    └──────┬──────┘
    └─ tcp_sendmsg/recvmsg kprobes                    │
                              ┌──────┬──────┬────┬──────┬──────┬──────┬─────────┐
                              │  FD  │ Env  │ MMap   │ Libs │ Net  │Write│ PipeWire│
                              │ .so  │ .so  │  .so   │ .so  │ .so  │ .so │   .so   │
                              └──────┴──────┴────────┴──────┴──────┴─────┴─────────┘
```

- **Monitor thread** — scans `/proc` every 2 seconds, builds a `proc_snapshot_t`, computes CPU% and I/O rates via delta ticks with `CLOCK_MONOTONIC`, detects containers/services/Steam metadata, publishes under a mutex.
- **UI thread** — GTK 3 main loop. Diff-based tree updates (remove dead → update in-place → insert new) preserve scroll, selection, and expand state.
- **Data broker** — single GTask worker that gathers `/proc` data for all tracked PIDs, deduplicating reads across plugins via an OR'd `data_needs` bitmask. Plugins receive a read-only `evemon_proc_data_t` and render on the main thread.
- **eBPF backend** — `fdmon_ebpf_kern.c` compiled to BPF bytecode with clang. Tracepoints cover fd lifecycle, all write-family syscalls, and process fork/exec. The `sched_process_fork` program propagates the `monitored_pids` map entry from parent to child in kernel space — enabling zero-latency capture of sub-millisecond worker processes. Events delivered via perf buffer.
- **Fanotify backend** — fallback for fd monitoring when eBPF is unavailable.
```

- **Monitor thread** — scans `/proc` every 2 seconds, builds a `proc_snapshot_t`, computes CPU% and I/O rates via delta ticks with `CLOCK_MONOTONIC`, detects containers/services/Steam metadata, publishes under a mutex.
- **UI thread** — GTK 3 main loop. Diff-based tree updates (remove dead → update in-place → insert new) preserve scroll, selection, and expand state.
- **Data broker** — single GTask worker that gathers `/proc` data for all tracked PIDs, deduplicating reads across plugins via an OR'd `data_needs` bitmask. Plugins receive a read-only `evemon_proc_data_t` and render on the main thread.
- **eBPF backend** — `fdmon_ebpf_kern.c` compiled to BPF bytecode with clang, attaches to syscall tracepoints and kprobes. Events delivered via perf buffer.
- **Fanotify backend** — fallback for fd monitoring when eBPF is unavailable.

---

## Status Bar

| Left | Right |
|------|-------|
| Process count · CPU count · memory used/total · UI render time (last/avg/max) | Uptime · logged-in users · 1/5/15 min load averages |

---

## Writing Plugins

Plugins are shared objects that export a single entry point:

```c
evemon_plugin_t *evemon_plugin_init(void);
```

The returned descriptor declares what data the plugin needs (`evemon_NEED_FDS`, `evemon_NEED_ENV`, etc.) and provides four callbacks: `create_widget`, `update`, `clear`, and `destroy`. The host handles all `/proc` I/O, threading, and layout — plugins just render.

See [`src/evemon_plugin.h`](src/evemon_plugin.h) for the full ABI and [`docs/plugins.md`](docs/plugins.md) for the architecture document with a complete example plugin.

Compile and drop into `build/plugins/`:

```sh
gcc -shared -fPIC -o evemon_myplugin.so myplugin.c $(pkg-config --cflags --libs gtk+-3.0)
```

---

## Credits

- **[font-logos](https://github.com/lukas-w/font-logos)** (v1.3.0) by Lukas W. — Linux distribution logo icon font used in the About dialog. Licensed under the [Unlicense](https://github.com/lukas-w/font-logos/blob/master/LICENSE).

---

## License

*(to be decided)*
