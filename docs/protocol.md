# Evemon binary protocol (EVM1)

GTK and Qt are clients of a separate headless Evemon process. Both use the same C
client, snapshot decoder and inspector decoder. Evemon owns process
sampling, the common inspector broker, GPU telemetry and eBPF registrations.
Window/widget construction, themes, icons, desktop integration, audio capture,
spectrograms and MilkDrop stay in the frontend. No renderer or GUI toolkit is
loaded by `--headless`. The transport uses Unix sockets. Remote hosts use
OpenSSH stream-local forwarding; Evemon opens no TCP listener and relies on SSH
for authentication and encryption. `evemon --remote HOST --ui qt` manages that tunnel.
Host identity is checked before loading the frontend. Foreign-host sessions
disable local audio capture, MilkDrop, local MPRIS integration, and the local
Portage plugin. PipeWire graph data remains available from Evemon.

An ordinary GUI launch manages a private Evemon child. `--connect PATH`
attaches to an independently managed Evemon instance and never stops it on GUI exit.
The GUI's `ev-client` thread receives snapshots; `ev-events` receives tracing
notifications and manages that client's tracing registrations. Inspector and GPU
requests run off the GUI thread. Disconnection preserves the last snapshot,
marks it stale in the status bar and retries the connection. Reconnection
replaces the snapshot and reapplies tracing registrations. A frontend restart
restarts its managed child; an attached Evemon instance remains running.

## Framing and negotiation

Root Evemon defaults to `/run/evemon/evemon.sock`. Its root-owned
directory is initially mode 0700; administrators control traversal and inherited
socket grants through directory access/default ACLs. Without a default ACL, the
socket is **0600**. With one, Evemon applies that ACL before `listen` (Linux Unix
`bind` otherwise masks its permissions). Directory ACLs survive Evemon restarts;
Evemon never changes the directory's existing permissions. No permission
configuration is carried in Evemon arguments.

Root Evemon on a filesystem socket relies on the kernel's socket access checks, including
ACLs, to authorize clients. Clients verify Evemon runs as root or their own UID.
Unprivileged Evemon instances retain same-UID client authentication. Granting access to a
root socket grants the full protocol, including process controls. User services
use `$XDG_RUNTIME_DIR/evemon/query.sock` (private `/tmp/evemon-UID/` fallback).

A socket argument beginning with `@`
selects Linux's abstract namespace: the leading `@` becomes a NUL byte, and the
remaining name is sent without a trailing NUL. Abstract sockets create no file
and have no filesystem permission bits. Abstract transports therefore retain
strict same-UID `SO_PEERCRED` checks, including for root. The invoking desktop user's environment continues
to select PipeWire, D-Bus and configuration directories during elevated launches.

Every frame starts with this 16-byte header, in network (big-endian) byte order:

| Offset | Type | Meaning |
|---|---|---|
| 0 | 4 bytes | ASCII `EVM1` |
| 4 | u16 | Protocol version, currently 1 |
| 6 | u16 | Message type |
| 8 | u32 | Request identifier, echoed in replies |
| 12 | u32 | Payload byte count, excluding this header |

A peer must first send `HELLO` with an empty payload. Bad magic, unsupported
versions and oversized frames close the connection. There is no JSON or line
protocol fallback. Ordinary replies set bit `0x8000` in the request type. Errors
use type `0xffff`, the original request identifier, a u32 error code and a string.
Codes: 1 invalid request, 2 snapshot not ready, 3 process not found, 4 unsupported
operation, 5 evemon error, 6 inspector queue full.

All scalar fields have explicit widths. Integers use big endian; signed fields
use two's complement of the stated width. Floating values use IEEE-754 binary64.
Strings are a u32 byte length followed by those bytes, with no terminator; an
embedded NUL is invalid. Linux strings may contain non-UTF-8 bytes; a display
client should repair those for presentation. No native C struct, pointer, padding,
`size_t` or platform-dependent enum layout is transmitted.

## Messages

| Type | Name | Request payload | Reply payload |
|---|---|---|---|
| 1 | HELLO | empty | u32 capabilities, u32 maximum reply size, u64 evemon instance ID, u32 tracing backend, application version string |
| 2 | STATUS | empty | common snapshot header |
| 3 | SNAPSHOT | empty | common header, u32 row count, process records |
| 4 | PROCESS | u32 PID | common header, one process record preceded by row count 1 |
| 5 | SUBSCRIBE | u32 delivery period in milliseconds | u32 accepted period; subsequent SNAPSHOT replies use this identifier |
| 6 | INSPECT | u32 PID, u32 needs mask | inspector bundle |
| 7 | CONTROL | u32 count, tracing registrations | empty acknowledgement |
| 8 | EVENTS | u32 enable (0 or 1) | u32 enable; subsequent event batches use this identifier |
| 9 | GPU | empty | GPU snapshot, or unsupported if GPU support was excluded |
| 10 | HOST | empty | hostname, boot ID, PID namespace (3 strings), evemon UID (u32) |
| 11 | SIGNAL | PID, signal (2 × u32), kernel start ticks, evemon instance (2 × u64) | Linux errno (u32), zero on success |
| 12 | ABOUT | empty | hostname, distro ID, distro name, kernel, evemon user, login shell, CPU model, GPU names (8 strings); CPU count (u32); uptime seconds, total memory bytes, available memory bytes (3 × u64) |

Capabilities: snapshot=1, inspector=2, PipeWire graph=4, MPRIS=8, tracing event
transport=16, GPU=32, process signals=64, About system information=128. The tracing transport can exist without an active tracing
backend. Backend values: 0 unavailable, 1 fanotify, 2 eBPF. Permission failures
are not disguised as a working eBPF backend.

Connections persist across requests. Requests may be fragmented across writes
or coalesced into a single write. Clients must parse frames, not socket read
boundaries, and match replies by type and identifier. Subscription frames and
request replies may interleave. Sending SUBSCRIBE with period 0 cancels the
snapshot subscription; 1–3600000 enables it. The period controls delivery of the
latest cached sample, **not** collection. Sampling remains controlled by the
Evemon instance's `--interval`. Faster consumers receive repeated generations.

## Process snapshots

The common snapshot header is 104 bytes:

| Fields in order | Representation |
|---|---|
| generation, sample monotonic timestamp in nanoseconds | 2 × u64 |
| scan interval in milliseconds, complete/ready flag, total process count | 3 × u32 |
| uptime seconds, total memory bytes, free memory bytes, available memory bytes | 4 × u64 |
| 1/5/15 minute load averages | 3 × binary64 |
| CPU count, logged-in user count, tracing backend | 3 × u32 |
| evemon monotonic timestamp at serialization, nanoseconds | u64 |

A snapshot adds a u32 returned-row count. Each row contains, in order:

1. PID and PPID (2 × u32).
2. RSS KiB, CPU ticks, process start time in epoch seconds, exact kernel start
   ticks, evemon instance ID, cumulative disk read/write bytes, and latest
   network sent/received interval bytes (9 × u64).
3. CPU percent and disk read/write bytes per second (3 × binary64).
4. I/O history length and reserved flags (2 × u32), then all 20 history samples
   (20 × binary64). Only the first `history length` samples are populated.
5. Name, command line, user, current directory, container and service (6 strings).
6. Steam metadata present flag (u32). If set: Steam and Wine flags (2 × u32),
   followed by app ID, game name, Proton version, Proton directory, compat-data
   directory, game directory, runtime layer and display label (8 strings).

CPU percent retains Evemon's whole-machine normalization. Start time retains the
monitor's existing value. Network fields are interval deltas, not cumulative
counters. A ready=false snapshot can contain startup rows before enrichment is
complete. Generation and sample timestamp change on publication, not on a query.
Clients compute sample age from the two evemon timestamps, then map that age
to their own monotonic clock; host boot times need not match.

## Inspector, GPU and event payloads

The inspector needs mask preserves the plugin ABI: FDs=1, environment=2,
mappings=4, sockets=8, cgroups=16, status=32, libraries=64, PipeWire graph=128,
descendants=256, threads=512, MPRIS=1024. Unknown bits are rejected. Unrequested
arrays are empty. Optional PipeWire data is empty when that capability was not
compiled or the user's server is unavailable.

Inspector bundles begin with PID/PPID (u32 each), name/command/user/cwd (strings),
CPU percent (binary64) and RSS KiB (u64). They then contain counted arrays in this
order: descendant PIDs, FDs, environment entries, mappings, libraries, sockets,
threads, PipeWire nodes, links, ports, and MPRIS players. Each count is u32.
A u32 cgroup-present flag and optional cgroup record follow, then raw status,
raw mappings and raw cgroup strings. The explicit field order of each record is
maintained in [`query_inspect.c`](../src/ipc/query_inspect.c). Kernel counters and
sizes use u64, IDs/categories/booleans use u32, rates use binary64, and each string
is length-prefixed. GUI clients reconstruct owned data for the existing plugin
ABI; pointers never leave the process.

GPU replies contain a u64 monotonic timestamp, u32 device count and device
records, then u32 process count and process records. Fields are explicit in
[`query_gpu.c`](../src/ipc/query_gpu.c). Device/process validity masks distinguish
unavailable metrics from zero. GPU collection shares the inspector worker and
caches samples for 250 ms across clients.
The GPU capability advertises compiled protocol support, not device availability.
A successful reply with zero devices disables the frontend GPU plugin and its
GPU-only filter for that session. Transport failures retain the normal retry
behavior and do not persist a disabled plugin preference.

ABOUT runs on the inspector worker and is requested asynchronously when a remote
About dialog opens. Its identity, distro logo and hardware information come from
Evemon. Shell/user describe Evemon account; remote desktop,
resolution and HDR are unavailable because headless Evemon has no desktop session.
The GUI never substitutes its local machine's information. Older Evemon versions
without this capability show an explicit unavailable message.

A CONTROL payload replaces that connection's complete tracing set. Each of up to
128 records is three u32 values: kind, PID, value. Kind 1 monitors a PID/FD pair;
kind 2 watches a PID and descendants with stdout/stderr mask 1/2/3; kind 3 enables
orphan stdout tracing and requires PID=value=0. Evemon combines all
connections' sets; disconnect releases only that connection's contribution.
Unsupported tracing returns an error.

An event batch is u64 dropped-event count, u32 event count, then records. Kind 1
(write) carries u32 TID, TGID, FD, truncation flag and byte count, followed by raw
write bytes (up to 4096, including NULs). Kind 2 (exec) carries u32 PID and PPID,
then executable path string. Evemon retains 256 events; overruns are
reported, never silently hidden. Batches are sent every 50 ms while subscribed.

SIGNAL pins the target with `pidfd_open`, verifies its exact kernel start ticks
and evemon instance, then delivers through `pidfd_send_signal`. Stale identities
return `ESTALE`; PID 0/1 and invalid signals are rejected. Kernel permission errors
are returned unchanged. There is no local-client `kill(pid)` fallback. Both GUIs
capture the identities of the selected processes before confirmation/delivery.

## Bounds and shutdown

There are at most 16 connections, 4096 request bytes and 32 MiB per response.
Each peer has at most one outgoing frame. Slow consumers cannot accumulate an
unbounded queue. Incomplete requests and blocked writes expire after five
seconds; an idle negotiated connection or subscription remains connected.
Inspection has a bounded 16-job queue and its own `ev-inspect` thread, independent
of `ev-query`, `ev-monitor` and either GUI main loop. Snapshot copies are taken
under the monitor lock and encoded/sent outside it.

SIGINT and SIGTERM stop the service, join workers and unlink only the socket inode
created by that service. Active sockets cannot be taken over; stale sockets are
reclaimed only when owned by the current UID and not listening. Ordinary GUI
shutdown also waits for its managed Evemon instance, removes its private directory and
leaves independent Evemon instances alone.
Abstract names use exclusive kernel binding instead of filesystem locks and are
released when the listening socket closes; there is no socket file to unlink.
