#!/bin/sh
# Run inside the clean dependency container selected by build-matrix.yml.
set -eu

case_name=${1:?build case required}
builder=${2:?make or cmake required}
build_dir=${3:-/tmp/evemon-build-case}
jobs=${BUILD_JOBS:-2}
gtk=0 qt=0 gpu=0 amd=0 intel=0 nvidia=0 audio=0 ebpf=0
vendor=all
case "$case_name" in
    qt-no-gtk) qt=1; audio=1; ebpf=1 ;;
    gtk-no-qt) gtk=1; audio=1; ebpf=1 ;;
    qt-minimal) qt=1 ;;
    gtk-minimal) gtk=1 ;;
    headless) ;;
    gpu-no-amdgpu) gpu=1; intel=1; nvidia=1 ;;
    gpu-no-nvidia) gpu=1; amd=1; intel=1; nvidia=1 ;;
    nvidia-only) qt=1; gpu=1; nvidia=1; vendor=nvidia ;;
    intel-only) gtk=1; gpu=1; intel=1; vendor=intel ;;
    amd-only) gtk=1; gpu=1; amd=1; vendor=amd ;;
    *) echo "Unknown build case: $case_name" >&2; exit 2 ;;
esac

fail() { echo "FAIL: $*" >&2; exit 1; }
absent_package() {
    if pkg-config --exists "$1"; then fail "$1 is installed; this is not an isolated absence test"; fi
}
absent_library() {
    if ldconfig -p | grep -E "$1"; then fail "forbidden runtime library is installed: $1"; fi
}
absent_symbol() {
    if nm "$build_dir/evemon" | grep -w "$1"; then fail "unexpected backend: $1"; fi
}
present_symbol() {
    nm "$build_dir/evemon" | grep -qw "$1" || fail "backend was silently omitted: $1"
}

test ! -e "$build_dir" || fail "use a fresh build directory"
test ! -e data/android-arm64/evemon-arm64.tar.gz || fail "Android bundle must be absent"
if [ "$gtk" = 0 ]; then
    absent_package gtk+-3.0
    absent_package gtksourceview-4
    absent_library 'libgtk-(3|4)\.so|libgtksourceview-'
    test ! -d /usr/include/gtk-3.0 || fail "GTK headers are installed"
fi
if [ "$qt" = 0 ]; then
    absent_package Qt6Widgets
    absent_library 'libQt[56](Core|Gui|Widgets)\.so'
fi
if [ "$gpu" = 0 ] || [ "$vendor" = nvidia ]; then absent_package libdrm; absent_package libudev; fi
if [ "$gpu" = 1 ]; then
    absent_package ncurses
    test ! -f /usr/include/ncurses.h || fail "ncurses development headers must be absent"
fi
if [ "$audio" = 0 ]; then absent_package libpipewire-0.3; absent_package libsoup-3.0; fi
if [ "$ebpf" = 0 ] && command -v clang >/dev/null 2>&1; then fail "Clang must be absent"; fi
if [ "$case_name" = gtk-minimal ]; then
    absent_package gtksourceview-4
    absent_package sqlite3
fi
absent_library 'libnvidia-ml\.so|libcuda\.so'
test ! -f /usr/include/nvml.h || fail "NVIDIA SDK header is installed"
if [ "$case_name" = gpu-no-amdgpu ] || [ "$vendor" = intel ]; then
    test ! -f /usr/include/libdrm/amdgpu.h || fail "AMD headers are installed"
    test ! -f /usr/include/libdrm/amdgpu_drm.h || fail "AMD DRM headers are installed"
    absent_library 'libdrm_amdgpu\.so'
fi
if [ "$vendor" = amd ]; then
    test ! -f /usr/include/libdrm/i915_drm.h || fail "Intel i915 headers are installed"
    test ! -f /usr/include/libdrm/xe_drm.h || fail "Intel Xe headers are installed"
    absent_library 'libdrm_intel\.so'
fi

case "$builder" in
    make)
        make -j"$jobs" BUILD_DIR="$build_dir" GPU_VENDOR="$vendor"
        for feature in "ui=$gtk:$qt" "gpu=$gpu" "amdgpu=$amd" "audio=$audio" "ebpf=$ebpf" 'android=0:'; do
            grep -qx "$feature" "$build_dir/build-config" || fail "expected $feature"
        done
        ;;
    cmake)
        cmake -S . -B "$build_dir" -DEVEMON_GTK="$gtk" -DEVEMON_QT="$qt" \
            -DEVEMON_GPU="$gpu" -DEVEMON_GPU_VENDOR="$vendor" -DEVEMON_PIPEWIRE="$audio" \
            -DEVEMON_MILKDROP="$audio" -DEVEMON_EBPF="$ebpf"
        cmake --build "$build_dir" --parallel "$jobs"
        grep -qx 'EVEMON_ANDROID:BOOL=OFF' "$build_dir/CMakeCache.txt" || fail "Android bundle was not auto-disabled"
        ;;
    *) fail "unknown builder: $builder" ;;
esac

test -x "$build_dir/evemon" || fail "no executable was built"
for frontend in gtk qt; do
    case "$frontend" in gtk) enabled=$gtk ;; qt) enabled=$qt ;; esac
    module=$build_dir/evemon-ui-$frontend.so
    if [ "$enabled" = 1 ]; then test -f "$module" || fail "$frontend frontend missing"
    else test ! -e "$module" || fail "unexpected $frontend frontend"; fi
done
if [ "$nvidia" = 1 ]; then present_symbol gpu_vendor_nvidia
else absent_symbol gpu_vendor_nvidia; fi
if [ "$intel" = 1 ]; then present_symbol gpu_vendor_intel
else absent_symbol gpu_vendor_intel; fi
if [ "$amd" = 1 ]; then present_symbol gpu_vendor_amdgpu
else absent_symbol gpu_vendor_amdgpu; fi
if [ "$vendor" != all ]; then
    backend=$vendor
    if [ "$vendor" = amd ]; then backend=amdgpu; fi
    vendors=$(nm "$build_dir/evemon" | awk '$NF ~ /^gpu_vendor_/ {print $NF}')
    test "$vendors" = "gpu_vendor_$backend" || fail "vendor isolation failed: $vendors"
fi
if [ "$gpu:$gtk" = 1:1 ]; then
    test -f "$build_dir/plugins/evemon_gpu_plugin.so" || fail "GTK GPU plugin missing"
fi
if [ "$gpu:$qt" = 1:1 ]; then
    nm "$build_dir/evemon-ui-qt.so" | grep -qw evemon_qt_gpu_init || fail "Qt GPU inspector missing"
fi
if [ "$qt" = 1 ]; then
    nm -C "$build_dir/evemon-ui-qt.so" | grep -q 'WriteView::WriteView' || fail "Qt Write Log missing"
fi
if [ "$case_name" = gtk-no-qt ]; then
    test -f "$build_dir/plugins/evemon_write_monitor_ui_plugin.so" || fail "GTK Write Log missing"
fi
if [ "$case_name" = gtk-minimal ]; then
    test ! -e "$build_dir/plugins/evemon_write_monitor_ui_plugin.so" || fail "unexpected GtkSourceView plugin"
    test ! -e "$build_dir/plugins/evemon_proc_meta_service_plugin.so" || fail "unexpected SQLite plugin"
fi
test ! -e "$build_dir/share/evemon/android/evemon-arm64.tar.gz" || fail "unexpected Android bundle"

# Regression: the write-monitor service must compile with GIO alone, including
# in the Qt-only and headless containers where GTK is genuinely not installed.
cc -shared -fPIC -Isrc $(pkg-config --cflags gio-2.0) \
    src/plugins/write_monitor_plugin.c $(pkg-config --libs gio-2.0) \
    -o "$build_dir/write-monitor-service.so"
for output in "$build_dir/evemon" "$build_dir"/*.so "$build_dir"/plugins*/*.so; do
    test -f "$output" || continue
    needed=$(readelf -d "$output")
    if printf '%s\n' "$needed" | grep -E '\(NEEDED\).*(libnvidia-ml|libcuda|libdrm_amdgpu)\.'; then
        fail "GPU vendor runtime became a mandatory link dependency: $output"
    fi
    if [ "$vendor" = nvidia ] && [ "$output" = "$build_dir/evemon" ]; then
        if printf '%s\n' "$needed" | grep -E '\(NEEDED\).*(libdrm|libudev)\.'; then
            fail "DRM/udev leaked into the NVIDIA-only backend"
        fi
    fi
    if [ "$gtk" = 0 ]; then
        if printf '%s\n' "$needed" | grep -E '\(NEEDED\).*(libgtk-|libgtksourceview-|libgdk-[34])'; then
            fail "GTK leaked into $output"
        fi
        if nm -D --undefined-only "$output" | grep -E ' (gtk_|gtk_source_|gdk_)' | grep -v ' gdk_pixbuf_'; then
            fail "unresolved GTK symbols in $output"
        fi
    fi
done
echo "PASS: $case_name ($builder); executable, selected frontends, Write Log, and dependency isolation verified"
