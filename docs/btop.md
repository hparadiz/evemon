# allmon vs btop

## Overview

**btop** is a polished **system-wide dashboard** — it excels at giving you a
birds-eye view of CPU, memory, network, disk, and GPU with beautiful graphs,
all in a terminal.

**allmon** is a **process-centric deep inspector** — it excels at understanding
*what a specific process is doing*: its full fd table, socket details, container
context, systemd service, Steam/Proton metadata, and where it sits in the
process hierarchy.  It trades system-wide dashboards for unique per-process
introspection features that btop doesn't offer at all (eBPF fd tracking,
container/service detection, Steam game resolution, process pinning, device
labelling).

They're complementary rather than competing: btop answers *"how is my system
doing?"* while allmon answers *"what is this process doing and why?"*

---

## Feature Comparison

| Aspect | **allmon** | **btop** |
|--------|-----------|----------|
| Language | C (GTK3) | C++ (terminal UI / TUI) |
| UI paradigm | **Graphical** (GTK3 window, mouse-driven) | **Terminal** (TUI with ncurses-like rendering) |
| Process view | **Hierarchical tree** — processes nested under parents, expand/collapse subtrees (like Sysinternals Process Explorer) | Flat sortable list (no tree hierarchy) |
| Focus | Deep **process inspection** — per-process detail sidebar, fd enumeration, socket resolution, device labelling, container/service detection | **System dashboard** — CPU graphs, memory bars, network I/O, disk I/O, plus a process list |

---

## Monitoring

| Aspect | **allmon** | **btop** |
|--------|-----------|----------|
| CPU monitoring | Per-process CPU% (per-core shelved for now) | Per-core graphs with historical sparklines, total CPU |
| Memory monitoring | System total/used in status bar, per-process RSS + group RSS | Visual memory bar with swap, cached, per-process mem |
| Network | Socket resolution per-process (TCP/UDP/Unix from `/proc/net/*`) | System-wide network throughput graphs |
| Disk I/O | Not tracked at system level; per-process fd open/close via **fanotify** or **eBPF** | System-wide disk I/O throughput |
| Sensors | ❌ No temperature/fan monitoring | ✅ CPU temp, fan speed, battery |
| GPU monitoring | Device labelling only (PCI vendor/device from sysfs) | ✅ GPU utilization, VRAM, temp (NVIDIA/AMD/Intel) |

---

## Unique to allmon

| Feature | Details |
|---------|---------|
| File descriptor monitoring | **fanotify** + **eBPF** backends track fd lifecycle (open/close) per process group, displayed in sidebar |
| Container detection | Detects Docker, Podman, LXC, Kubernetes, containerd, nspawn, etc. via cgroup + PID namespace |
| Service detection | Resolves systemd units and OpenRC services |
| Steam/Proton metadata | Detects Steam games, resolves App ID, game name, Proton version, Wine prefix, runtime layer |
| Device labelling | Resolves `/dev` nodes to human-readable names via sysfs + PCI IDs |
| Process pinning | Pin processes to the top of the tree for focused monitoring |
| Hierarchical process tree | Full parent/child nesting with expand/collapse and group-level CPU/RSS totals |

---

## Unique to btop

| Feature | Details |
|---------|---------|
| Per-core CPU graphs | Historical sparklines per core |
| Network throughput | System-wide upload/download graphs |
| Disk I/O throughput | System-wide read/write graphs |
| Temperature sensors | CPU temp, fan speed, battery |
| GPU dashboard | Utilization, VRAM, temperature (NVIDIA/AMD/Intel) |
| Cross-platform | Linux, macOS, FreeBSD |
| Zero dependencies | Standalone binary, no runtime deps |
| Built-in themes | Color themes that adapt to terminal capabilities |

---

## Other Differences

| Aspect | **allmon** | **btop** |
|--------|-----------|----------|
| Process actions | End Process / End Process Tree (SIGTERM), Copy Command | Send signals (multiple signal types) |
| Filtering | Ctrl+F name filter with live shadow store | Filter by name, command, user, PID |
| Theming | GTK3 theme switcher (discovers installed themes) | Built-in color themes, adapts to terminal |
| Privileges | Graceful degradation — works unprivileged for own processes; root/capabilities unlock fd monitoring, cross-user visibility | Works unprivileged with some sensor limits |
| Portability | **Linux only** (depends on `/proc`, `/sys`, fanotify, eBPF) | Linux, macOS, FreeBSD |
| Dependencies | GTK3, libbpf, libelf, zlib, fontconfig | Standalone binary (no runtime deps) |
