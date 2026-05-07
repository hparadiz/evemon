#!/usr/bin/env bash
# packaging/build-deb.sh
# Runs inside the Docker build container.
# Produces a .deb in /out (bind-mounted from the host).
set -euo pipefail

VERSION="${VERSION:-0.1.0}"
ARCH="${ARCH:-$(dpkg --print-architecture)}"
DEBUG="${DEBUG:-0}"
PKG="evemon_${VERSION}_${ARCH}"
DESTDIR="/tmp/${PKG}"

echo "==> Building evemon ${VERSION} for ${ARCH} (DEBUG=${DEBUG})"

# ── 1. Copy source to a writable work directory ─────────────────
# /src is bind-mounted read-only; we need a writable tree to build in.
WORKDIR="/tmp/evemon-build"
rm -rf "${WORKDIR}"
cp -a /src/. "${WORKDIR}"
cd "${WORKDIR}"

# ── 2. Compile ──────────────────────────────────────────────────
# Unset MAKEFLAGS so the host's make flags (e.g. -j, target=...) don't
# leak into these recursive invocations via the environment.
unset MAKEFLAGS
make clean
# On Debian/Ubuntu, clang's BPF target can't find asm/types.h from the
# multiarch include path without help.  Point it at the arch-specific dir.
ARCH_TRIPLE=$(gcc -dumpmachine)
MAKE_EXTRA="PREFIX=/usr BPF_CFLAGS=\"-O2 -target bpf -g -D__TARGET_ARCH_x86 -I/usr/include/${ARCH_TRIPLE}\""

if [ "${DEBUG}" = "1" ]; then
    eval make debug "${MAKE_EXTRA}"
else
    eval make all "${MAKE_EXTRA}"
fi

# ── 2. Stage install into a temp tree ──────────────────────────
rm -rf "${DESTDIR}"
make install PREFIX=/usr DESTDIR="${DESTDIR}"

# ── 2b. Split debug symbols into a .ddeb (debug=1 only) ────────
if [ "${DEBUG}" = "1" ]; then
    DBGPKG="evemon-dbgsym_${VERSION}_${ARCH}"
    DBGDIR="/tmp/${DBGPKG}"
    DBGSYM_DIR="${DBGDIR}/usr/lib/debug/.build-id"

    rm -rf "${DBGDIR}"
    mkdir -p "${DBGSYM_DIR}"

    # Split one binary: extract debug info, strip the installed copy,
    # add a gnu_debuglink.  The debuglink path baked into the stripped
    # binary must be the *installed* absolute path, not the staging path.
    # objcopy embeds whatever path you pass as the last argument, so we
    # pass only the basename and run objcopy from the directory that
    # mirrors the installed layout inside DBGDIR.
    split_debug() {
        local bin="$1"
        local BUILD_ID
        BUILD_ID=$(readelf -n "${bin}" 2>/dev/null \
            | awk '/Build ID:/{print $NF}' | head -1)

        if [ -n "${BUILD_ID}" ]; then
            local id_dir="${DBGSYM_DIR}/${BUILD_ID:0:2}"
            local debug_name="${BUILD_ID:2}.debug"
            mkdir -p "${id_dir}"
            # Extract all debug sections into the .debug file
            objcopy --only-keep-debug "${bin}" "${id_dir}/${debug_name}"
            # Strip the installed binary down to just code + symbols needed for backtraces
            strip --strip-unneeded "${bin}"
            # Embed a debuglink — run from id_dir so objcopy records only the
            # basename; gdb resolves it via the build-id path automatically.
            (cd "${id_dir}" && objcopy --add-gnu-debuglink="${debug_name}" "${bin}")
        else
            echo "WARNING: no Build ID in ${bin} — embedding full debug info"
            # No build-id: fall back to keeping the binary unstripped
        fi
    }

    split_debug "${DESTDIR}/usr/bin/evemon"

    # Also split the plugin .so files
    find "${DESTDIR}/usr/lib/evemon/plugins" -name '*.so' | while read -r so; do
        split_debug "${so}"
    done

    # Write DEBIAN/control for the .ddeb
    DBGINSTALLED=$(du -sk "${DBGDIR}" | awk '{print $1}')
    mkdir -p "${DBGDIR}/DEBIAN"
    cat > "${DBGDIR}/DEBIAN/control" <<EOF
Package: evemon-dbgsym
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: evemon contributors
Installed-Size: ${DBGINSTALLED}
Depends: evemon (= ${VERSION})
Section: debug
Priority: optional
Description: Debug symbols for evemon
 This package contains detached debug symbols for the evemon binary
 and its plugins, built with -Og -g3.
EOF

    fakeroot dpkg-deb --build "${DBGDIR}" "/out/${DBGPKG}.ddeb"
    echo "==> Debug symbols package ready: /out/${DBGPKG}.ddeb"
fi

# ── 3. Write DEBIAN/control ────────────────────────────────────
mkdir -p "${DESTDIR}/DEBIAN"

INSTALLED_SIZE=$(du -sk "${DESTDIR}" | awk '{print $1}')

cat > "${DESTDIR}/DEBIAN/control" <<EOF
Package: evemon
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: evemon contributors
Installed-Size: ${INSTALLED_SIZE}
Depends: libgtk-3-0, libfontconfig1, libbpf1, libelf1, zlib1g, libjansson4, libsqlite3-0, libpipewire-0.3-0, libsoup-3.0-0, libgtksourceview-4-0, libepoxy0, libx11-6
Section: utils
Priority: optional
Homepage: https://github.com/hparadiz/evemon
Description: Linux process monitor with deep per-process introspection
 evemon drills into individual processes: file descriptors, network sockets,
 environment variables, memory maps, shared libraries, cgroup limits,
 container context, Steam/Proton metadata, and live PipeWire audio streams.
 Powered by eBPF tracepoints and a plugin-based sidebar.
EOF

# ── 4. Write DEBIAN/postinst ───────────────────────────────────
cat > "${DESTDIR}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi
EOF
chmod 0755 "${DESTDIR}/DEBIAN/postinst"

# ── 5. Write DEBIAN/postrm ─────────────────────────────────────
cat > "${DESTDIR}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi
EOF
chmod 0755 "${DESTDIR}/DEBIAN/postrm"

# ── 6. Build the .deb ──────────────────────────────────────────
mkdir -p /out
fakeroot dpkg-deb --build "${DESTDIR}" "/out/${PKG}.deb"

echo "==> Package ready: /out/${PKG}.deb"
