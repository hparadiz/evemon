# Building

## Dependencies

| Library | Debian / Ubuntu | Arch | Purpose |
|---------|----------------|------|---------|
| GTK 3 | `libgtk-3-dev` | `gtk3` | GTK frontend |
| Qt 6 | `qt6-base-dev` | `qt6-base` | Qt frontend |
| GLib 2 / GIO | `libglib2.0-dev` | `glib2` | GDBus (MPRIS), event loop |
| fontconfig | `libfontconfig1-dev` | `fontconfig` | Font loading |
| libbpf | `libbpf-dev` | `libbpf` | eBPF fd/network monitor |
| libelf | `libelf-dev` | `libelf` | eBPF ELF loading |
| zlib | `zlib1g-dev` | `zlib` | eBPF compression |
| jansson | `libjansson-dev` | `jansson` | JSON settings file |
| SQLite 3 | `libsqlite3-dev` | `sqlite` | Bundled software metadata database |
| clang | `clang` | `clang` | BPF kernel program compilation |
| glib-compile-resources | `libglib2.0-dev-bin` | `glib2` | Embedded resources (icon, font) |
| libepoxy | `libepoxy-dev` | `libepoxy` | OpenGL (MilkDrop plugin) |
| GtkSourceView 4 *(optional, GTK)* | `libgtksourceview-4-dev` | `gtksourceview4` | GTK Write Log code view |
| libsoup 3 *(optional)* | `libsoup-3.0-dev` | `libsoup3` | Album art HTTP download |
| PipeWire *(optional)* | `libpipewire-0.3-dev` | `pipewire` | Audio graph + spectrogram |

### Gentoo

For a full GTK + Qt desktop build, install these dependencies from a root
shell, reviewing Portage's proposed USE changes before accepting:

```sh
emerge --ask --noreplace \
  sys-devel/gcc dev-build/make dev-vcs/git virtual/pkgconfig \
  'llvm-core/clang[llvm_targets_BPF]' \
  dev-libs/glib dev-util/glib-utils dev-libs/libbpf virtual/libelf virtual/zlib \
  dev-libs/jansson dev-db/sqlite \
  'x11-libs/gtk+:3[X]' x11-libs/gtksourceview:4 \
  x11-libs/pango x11-libs/gdk-pixbuf media-libs/fontconfig x11-libs/libX11 \
  'dev-qt/qtbase:6[gui,widgets,opengl]' \
  media-video/pipewire net-libs/libsoup:3 media-libs/libepoxy
```

The BPF compiler needs Clang's `llvm_targets_BPF` USE flag. Qt needs `gui`,
`widgets`, and (for MilkDrop) `opengl`. CMake is only needed when using
`./configure` or `CONFIGURE_FLAGS`; install `dev-build/cmake` for those paths.
No Android SDK, NDK, cross compiler, or runtime archive is needed for a local
desktop build.

## Compile

One source tree, executable, monitoring core, and install tree serve all modes:

```sh
make
./build/evemon
```

Plain `make` needs no configuration step. It detects available dependencies and
builds the features it can, then prints a summary of the executable, frontends,
plugins, and omitted features with their missing dependencies. `make -j4` enables
four parallel jobs; the build does not override your chosen job count.

- GTK and Qt are selected independently. If neither can be built, the executable
  defaults to headless mode.
- Missing GtkSourceView or SQLite skips the corresponding GTK plugin. Qt needs
  SQLite for its frontend.
- GtkSourceView 4 is the code-view dependency of GTK's **Write Log** panel.
  If it is missing, both build paths omit that panel with a warning; the GTK
  frontend still builds. Qt's **Write Log** is native Qt and does not need GTK
  or GtkSourceView.
- Missing audio, OpenGL, nvtop, or Clang BPF support skips the affected optional
  feature. eBPF-disabled builds retain the fanotify fallback.
- Missing core dependencies (C compiler, archiver, pkg-config, GLib/GIO, libbpf,
  libelf, or jansson) finish successfully with a prominent **NOTHING BUILT**
  warning. Existing binaries are not presented as newly built. Install the
  dependencies and rerun `make` before trying to run or install the program.

Actual compiler and linker errors still fail the build. Dependency changes update
the build's feature definitions and remove stale outputs for skipped features.
Only the selected toolkit is loaded at runtime. Android runtime bundling is enabled
only when `data/android-arm64/evemon-arm64.tar.gz` exists. To omit it even when
the archive is present, use `make WITH_ANDROID=0`. `WITH_ANDROID=1` requires it.

GPU support is enabled when the pinned nvtop checkout, libdrm, and libudev are
available. To fetch the checkout, run
this from the evemon repository root before building:

```sh
make nvtop
make -j4
```

`make nvtop` runs `scripts/fetch-nvtop.sh`, checks out nvtop 3.3.2 into
`third_party/nvtop`, and verifies its pinned commit. GPU builds also require
libdrm and libudev (`x11-libs/libdrm virtual/libudev` on Gentoo).
Missing AMD libdrm headers omit the AMD backend while retaining the other GPU
backends. NVIDIA's SDK and driver libraries are not build dependencies.

The `Build without optional dependencies` GitHub Actions workflow compiles both
native Make and CMake builds in clean Debian containers. Its cases cover Qt
without GTK, GTK without Qt, minimal versions of each frontend, headless without
either toolkit or GPU dependencies, and GPU builds without AMD or NVIDIA
development/runtime files. Every case also omits the Android bundle.
Separate NVIDIA-only, Intel-only, and AMD-only builds compile the GPU plugin or
Qt inspector and verify that exactly the selected vendor backend is linked.
The NVIDIA-only container has neither DRM nor udev development files; Intel-only
has no AMD headers/library, and AMD-only has no Intel headers/library. Select
these builds with `make GPU_VENDOR=nvidia` (or `intel` / `amd`), or CMake's
`-DEVEMON_GPU_VENDOR=nvidia`. The default `all` keeps every available backend.

These jobs verify that the excluded dependencies really are absent, require the
expected executable and frontend/plugin outputs, and check for accidental GTK
or GPU-vendor link dependencies. They also compile the write-monitor service
with GIO alone, so a GTK dependency accidentally added to that service fails the
Qt-only and headless jobs. The shared runner is `scripts/ci-build-case.sh`; run it
only inside a container with the dependency set in
`.github/workflows/build-matrix.yml`. It compiles and inspects artifacts without
launching Evemon.

For custom builds, `./configure` is optional and requires CMake 3.21+. This path
uses explicit feature selections and reports missing required dependencies as
configuration errors:

```sh
./configure --with-qt --without-android
make -j4
```

Once configured, `make` and `make install` use that CMake build tree and respect
its selected options. To enable a later nvtop checkout in an already configured
tree, rerun `./configure` with `--with-gpu` and your other options.
You can also pass configuration options through `CONFIGURE_FLAGS`, for example
`make CONFIGURE_FLAGS=--with-qt` for Qt only or
`make CONFIGURE_FLAGS=--with-gtk` for GTK only. The earlier `evemon-qt` source
directory is not needed to build this tree.

A minimal headless build needs neither toolkit:

```sh
make -j4 BUILD_DIR=build-headless \
  CONFIGURE_FLAGS='--without-gtk --without-qt --without-pipewire --without-gpu'
```

A C compiler, pkg-config, GLib/GIO, jansson and libbpf/libelf
are required. The eBPF kernel program also requires clang; `--without-ebpf`
omits that program and disables the eBPF backend, retaining the fanotify fallback.
Qt builds additionally require a C++17 compiler, Qt 6.4+ Core, Gui,
Widgets, PangoCairo, GdkPixbuf and SQLite. MilkDrop uses Qt OpenGL/OpenGLWidgets
only when that module is loaded. Optional KDE Frameworks 6 WindowSystem enables
native compositor blur; ColorScheme supplies KDE palette roles. Qt still builds
without either library and reads installed `.colors` files itself. GTK builds require GTK 3, fontconfig, X11,
PangoCairo and optionally SQLite and GtkSourceView 4 for their plugins. The native
Make build detects PipeWire and libsoup 3 together; `HAVE_PIPEWIRE=0` disables
audio and MilkDrop. MilkDrop additionally needs libepoxy (and Qt OpenGL modules
for its Qt view). Configured builds enable audio and MilkDrop
by default; use `--without-pipewire` to omit both.

Qt's Write Log includes read-only output, stdout/stderr selection, descendant and
kernel-thread filters, a process/PID/timestamp gutter, wrapping, Clear, and
auto-scroll that pauses when you scroll up and jumps to the end when re-enabled.
Log highlighting is built in. The Syntax menu also offers code languages when
KDE Frameworks SyntaxHighlighting is available (`kde-frameworks/syntax-highlighting:6`
on Gentoo). This is optional: without it, Log and Plain Text remain available and
the build reports the omitted code-language definitions. Native builds can omit
it explicitly with `HAVE_QT_SYNTAX=0`. Write capture itself requires an active
eBPF backend; the Qt panel explains when that backend is unavailable.

Useful configuration options:

| Option | Effect |
|--------|--------|
| `--with-default-ui=gtk` / `qt` / `headless` | Default when no saved preference exists |
| `--without-pipewire` | Disable audio inspection and MilkDrop |
| `--without-milkdrop` | Disable visualization only |
| `--without-gpu` | Disable bundled nvtop GPU backends |
| `--with-gpu` | Require the pinned nvtop checkout |
| `--without-android` | Omit the Android runtime bundle |
| `--with-android` | Require the Android runtime archive |
| `--without-ebpf` | Disable eBPF tracing and kernel program compilation |
| `--enable-debug` / `--enable-lto` | Debug symbols and optimization choices |
| `--prefix=/usr` | Installation prefix |
| `BUILD_DIR=PATH` | Select a separate build directory on the Make command line |

Use `make install` to install the configured tree, or
`DESTDIR=/tmp/evemon-stage make install` to stage it. After `./configure`,
`make package` packages the configured install tree through CPack. `make deb`
and `make rpm` use the container packaging scripts and require Docker.
`make asan` / `make ubsan` enable sanitizers in
the current build directory; use a separate directory for diagnostic builds.

Build outputs include `build/evemon` with its core linked in, the selected
`build/evemon-ui-{gtk,qt}.so` modules, `build/fdmon_ebpf_kern.o`, and enabled
plugins under `build/plugins/` or `build/plugins-qt/`. Installed modules live under the configured library directory's `evemon/` subdirectory.
Presets and the software database live under `share/evemon/`.

## Select the interface

```sh
./build/evemon --ui qt          # override for this launch
./build/evemon --ui gtk
./build/evemon --set-ui qt      # save startup choice, then exit
./build/evemon --list-uis
```

When both frontends are built, select **View → Appearance → Interface** to switch.
The confirmation offers **Cancel** or **Restart**; Cancel preserves the running
interface and saved preference. Restart saves the shared `frontend` preference in
`$XDG_CONFIG_HOME/evemon/settings.json` (normally `~/.config/evemon/settings.json`)
and relaunches Evemon. Selection happens before toolkit
initialization. Restart first stops and joins the workers, closes the query
socket, and then executes the same launcher. An explicit `--ui` override is
dropped on restart so the saved selection can take effect. If a saved frontend
was excluded from a later build, startup reports this and uses the build default;
an explicitly requested unavailable frontend is an error.

The Qt view retains its existing Qt layout preferences for compatibility with the
previous port. Both frontends consume the same binary snapshots and inspector data.
Their main menus and plugin controls use the same names and ordering. Press Alt
to show the menu bar. Theme choices are discovered when the menu opens and apply
immediately; switching GTK/Qt itself requires a restart.

Run the UI as your normal desktop user. PipeWire graph inspection, peak meters,
spectrograms and media metadata use that user's audio/session bus and do not need
root. Privileged eBPF and access to other users' protected `/proc` entries remain
subject to kernel permissions.

**Restart as Administrator** uses `/usr/bin/kdesu -n --noignorebutton` and retains
the invoking user's desktop identity, display, audio endpoints, themes and
configuration directories. Settings written while elevated keep the user's
ownership. Shell prompt discovery in About also runs as the desktop user. Long
launch environments use a private, SHA-256-checked temporary script to fit the
installed phone verifier's 512-character command limit. The original window stays
open because kdesu acknowledging a launch does not prove the new window started.

## Headless evemon and binary clients

Every GUI connects to a separate headless Evemon process. A normal launch starts and
cleans up its own Evemon child automatically. To share one Evemon process between
frontends, start it explicitly and attach:

```sh
./build/evemon --headless --socket /tmp/evemon.sock --interval 500
./build/evemon --ui qt --connect /tmp/evemon.sock
./build/evemon --ui gtk --connect /tmp/evemon.sock
```

Use **File → Connect to Remote…** in either GUI to enter an SSH host, IP address,
or `user@host`. **Run Daemon on Host** is enabled by default. Evemon uploads only
its executable and optional eBPF object—no frontend or plugin
modules—to `$HOME/.cache/evemon/remote`, then starts the existing Evemon core
with `--headless`. It never invokes `sudo`: the files and daemon have exactly
the SSH account's identity, and are root-owned only when the SSH account itself
is root.
A copy installed this way is replaced when the connecting build is newer: an
older version, or the same version built later. The new build must first run on
that host; only then is that copy's daemon for the socket in use stopped, and
the previous build is put back if the new one will not serve. An Evemon that was
not installed this way (a system service, a distribution package, another
account's or a GUI's) is used as it is and is never stopped or replaced.
Leave **Socket override (optional)** blank to choose that account's secure
default socket (`/run/evemon/evemon.sock` for root, otherwise its private XDG or
`/tmp/evemon-UID` runtime directory). An override is an absolute path on the
remote machine.

**Self Shutdown Daemon after 5 min** is also enabled by default. The timeout
starts only when the last telemetry client disconnects, so an active view does
not expire midway through use. Disable **Run Daemon on Host** to require an
already-running Evemon; a blank socket then means `/run/evemon/evemon.sock`.
Evemon receives a full process snapshot before switching the view. An unavailable
daemon or failed SSH connection leaves the dialog ready to retry and keeps the
current view, title and connection intact. Cancel also keeps the current view.
Known failures end the attempt immediately. One fifteen-second deadline covers the
entire attempt, including SSH authentication, forwarding, protocol negotiation
and the first process snapshot; uploading a build to install or upgrade the
remote Evemon is allowed another minute. A timeout ends the attempt and re-enables
Connect; cancellation and timeout tear down the attempted connection's SSH
processes and sockets. The current view remains active throughout.

When the GUI runs as root, SSH runs with the original user's UID, groups, home,
SSH configuration and authentication session. If elevation strips the SSH agent,
askpass or session-bus variables, Evemon recovers the missing values from that
user's launching process ancestry, then from a matching desktop session. It
does not select another user's agent or retain a stale/root-owned agent socket.
SSH discovery and forwarding detach from the launching terminal and use the
desktop askpass helper for key/password prompts. Starting Evemon from a terminal
therefore cannot strand authentication on an unseen terminal prompt.

The local window title remains `ＥＶＥＭＯＮ`. Remote views append ` | hostname`
so KDE's window list and task previews identify the connected machine.

To launch directly through your existing SSH configuration:

```sh
./build/evemon --ui qt --remote kaiju
```

Evemon uses the same private daemon and five-minute idle timeout defaults,
creates an SSH Unix-socket tunnel, restarts the daemon when reconnecting if
needed, and cleans up its private tunnel when the GUI exits. Pass
`--remote-socket /absolute/path` for a custom server address. Native CLI users
can pass `--remote-no-daemon`, `--remote-idle MIN`, or `--remote-no-idle` to
change the lifecycle. The File menu and native `--remote` option require OpenSSH. Restarting or
switching UI toolkits keeps the currently selected remote host.
The SSH server must permit Unix-socket forwarding. A server configured to prohibit
forwarding cannot be used for remote views; ordinary remote `--query --binary`
commands remain available through SSH.
No TCP service or firewall changes are needed. The window names Evemon
host; process actions execute there with PID identity checks. Audio capture,
MilkDrop and the local Portage inspector are disabled for foreign hosts;
PipeWire graph inspection still uses the server's data.
Process filters and the selected PID are saved per remote host, so local
filters cannot hide a remote process list. When Evemon detects no supported
GPUs, the GPU plugin and GPU-only filter are disabled for that run; this does not
change the saved plugin preference.
The bundled AMD backend requires the `amdgpu` driver. Older cards using `radeon`
still support ordinary process monitoring but currently provide no GPU telemetry.

The normal system deployment runs Evemon as root. Its socket is root-owned
and private unless an administrator grants access through the filesystem.
Create the directory once and set its access and default ACLs, for example as
root:

```sh
mkdir -p -m 0700 /run/evemon
setfacl -m u:akujin:--x,d:u::rw-,d:u:akujin:rw-,d:g::---,d:m::rw-,d:o::--- /run/evemon
```

The user gets directory traversal and read/write access to each newly created
socket. Evemon preserves the directory and its ACLs on stop/restart; it
applies the directory's default ACL before listening. No account or permission
argument is passed to Evemon. An existing socket receives updated defaults on
the next Evemon restart. A socket grant authorizes Evemon's inspection and
process-control operations. The SSH account can remain unprivileged.
`/run` is volatile; use the host's filesystem/tmpfiles policy to recreate the
directory ACL after reboot. Never grant clients write access to the directory.

Both **system and user services** are supplied for **systemd and OpenRC**.
The build substitutes the configured install prefix in the service definitions;
`make install` places copies in `share/evemon/services`. It does not enable them.
For a headless-only installation use `--without-gtk --without-qt`. For a private
user installation add `--prefix="$HOME/.local/opt/evemon"` before building.

| Mode | Generated definition | Service destination | Enable/start |
|---|---|---|---|
| systemd system | `build/packaging/systemd/system/evemon.service` | `/etc/systemd/system/evemon.service` | `systemctl daemon-reload`, then `systemctl enable --now evemon.service` |
| systemd user | `build/packaging/systemd/user/evemon.service` | `~/.config/systemd/user/evemon.service` | `systemctl --user daemon-reload`, then `systemctl --user enable --now evemon.service` |
| OpenRC system | `build/packaging/openrc/evemon` | `/etc/init.d/evemon` (executable) | `rc-update add evemon default`, then `rc-service evemon start` |
| OpenRC user | `build/packaging/openrc/evemon` | `${XDG_CONFIG_HOME:-$HOME/.config}/rc/init.d/evemon` (executable) | `rc-update --user add evemon default`, then `openrc --user default` |

OpenRC user services require a version with `--user` support and a valid
`XDG_RUNTIME_DIR`. The script uses `supervise-daemon` in either mode, and puts
user PID/log files in that runtime directory. User services use the caller's UID
and private runtime socket; they do not elevate. System and user instances can
run concurrently. Connect to a user service with its explicit runtime socket.
Once the user runlevel is initialized, `rc-service --user evemon
start|stop|restart|status` manages this service individually.

Closing an attached GUI leaves the independent Evemon instance running. If the
Evemon restarts, clients retain the last snapshot, show a disconnected status
and reconnect automatically. The selected GTK/Qt interface still changes only
on restart. `--serve` exposes a GUI's managed Evemon instance at the default socket;
`--socket PATH` can select that managed Evemon instance's address.

```sh
./build/evemon --socket /tmp/evemon.sock --query status
./build/evemon --socket /tmp/evemon.sock --query snapshot
./build/evemon --socket /tmp/evemon.sock --query 1234
./build/evemon --socket /tmp/evemon.sock --query gpu
./build/evemon --socket /tmp/evemon.sock --query snapshot --binary > snapshot.evm
watch -n 0.2 './build/evemon --socket /tmp/evemon.sock --query status'
```

The transport is **versioned, framed binary over Unix sockets**. `--query` renders
the binary reply as readable terminal output; `--binary` preserves its wire
frame. The former JSON line protocol has been removed. Persistent clients can
subscribe to snapshots and tracing events or request process/inspector/GPU data.
See [the EVM1 protocol](protocol.md) for message layouts, limits and ownership.

`--interval` sets the delay between monitor scans (1–3600000 ms, default 1000).
Clients poll or subscribe at their own cadence without triggering extra scans.
Replies identify sample generation, timestamp, readiness and collection interval.
Process snapshots preserve Steam metadata, I/O history and network counters.
The root default is `/run/evemon/evemon.sock`, independent of the invoking
desktop user's environment. Unprivileged services default to
`$XDG_RUNTIME_DIR/evemon/query.sock`, with a private `/tmp/evemon-UID/` fallback.
Without a directory default ACL, sockets are mode 0600. Root Evemon instances using filesystem sockets
honor kernel-enforced ACL access; clients accept Evemon running as root or their own UID.
Unprivileged Evemon instances accept only their own UID.
On Linux, `--socket @name` selects an abstract Unix socket. It creates no socket
file; peer UID checks still apply. `--connect @name` and `--socket @name --query`
use the same address format. Choose a name specific to your user and evemon.

Evemon has separate sampling, socket and inspector workers. Slow clients
cannot block sampling or build an unbounded response queue. **MilkDrop and
spectrogram rendering remain GUI-only**; a headless-only build excludes MilkDrop
code/assets and GUI modules. PipeWire graph inspection and GPU collection are
optional evemon capabilities and can be disabled independently.

## Experimental Android Evemon

The ARM64 headless Evemon has been exercised on a Nexus 6P running Android 9
and Linux 3.10, as the ordinary ADB shell user. Its process snapshots, status,
thread inspector, concurrent requests and subscriptions reached a desktop Qt GUI
over USB. This is a Linux executable with a private glibc runtime, not an APK.
It does not require root, a chroot or changes to Android's security policy.

Desktop builds include a verified ARM64 archive containing headless Evemon,
the matching glibc loader, and its complete runtime
library closure. The enabled GTK/Qt inspector, system and service plugins remain
in the desktop package: they render and interpret telemetry locally and are never
loaded by the headless phone process. This keeps the device payload independent
of X11, Wayland and either GUI toolkit without dropping any desktop plugin.

Use `auto` when exactly one ready device is attached, or pass the serial shown by
`adb devices -l`:

```sh
./build/evemon --ui qt --android-adb-forward auto
./build/evemon --ui gtk --android-adb-forward 8XV7N16310003753
```

With **Run Daemon on Host** enabled (the default), Evemon checks the installed
bundle manifest, pushes and transactionally installs a changed bundle under
`/data/local/tmp/evemon`, then starts it with `--headless` on a
versioned per-shell-user abstract socket. It creates a private
`adb forward` filesystem socket, and removes only that forward when the GUI
exits. The Android process retains the ADB account's UID, GID, supplementary
groups and SELinux domain; no root mechanism is used. The normal five-minute
no-client shutdown is enabled. `--remote-socket @name`, `--remote-no-daemon`,
`--remote-idle MIN` and `--remote-no-idle` adjust those defaults.

Android deployment uses the bundled `data/android-arm64/evemon-arm64.tar.gz`
archive. The cross-compilation toolchain is in `cmake/android-arm64-toolchain.cmake`;
it takes `EVEMON_ANDROID_SYSROOT` and `EVEMON_ANDROID_CC` from the environment.
The archive uses Debian 12 ARM64 GLib/GIO, jansson, libbpf, libelf and glibc.

For the test bundle in `/data/local/tmp/evemon-test`, the equivalent manual
Evemon launch is:

```sh
cd /data/local/tmp/evemon-test
export HOME="$PWD" XDG_CONFIG_HOME="$PWD/config" XDG_RUNTIME_DIR="$PWD"
./lib/ld-linux-aarch64.so.1 --library-path ./lib ./evemon \
  --headless --socket @evemon-6p-2000 --interval 1000
```

This Android build denies filesystem socket creation but permits abstract Unix
sockets. The equivalent manual forward is useful for diagnostics (use
`adb -s SERIAL` when multiple devices are connected):

```sh
mkdir -m 700 /tmp/evemon-6p-link
adb forward --no-rebind localfilesystem:/tmp/evemon-6p-link/evemon.sock \
  localabstract:evemon-6p-2000
chmod 600 /tmp/evemon-6p-link/evemon.sock
./build/evemon --ui qt --connect /tmp/evemon-6p-link/evemon.sock
```

Evemon has the shell user's `/proc` visibility. This kernel does not offer
the modern eBPF and pidfd facilities needed for tracing and safe process signals;
those features are unavailable. GPU and audio collection were excluded from the
phone build. The full local process-control suite has not been run on Android.

## Run and diagnose

Ordinary monitoring works unprivileged. eBPF tracing and full cross-user `/proc`
visibility require appropriate capabilities or authorized administrator access;
unavailable tracing degrades gracefully. Neither frontend is initialized by
`--help`, `--query`, `--set-ui` or `--headless`.

| Flag | Description |
|------|-------------|
| `-d` / `--debug` | Verbose debug logging |
| `-a` / `--debug-audio` | Audio/PipeWire debug logging |
| `-s` / `--safe-mode` | Start without frontend plugins |
| `-h` / `--help` | Full command-line reference |
