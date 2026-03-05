#!/usr/bin/env bash
# packaging/build-deb.sh
# Runs inside the Docker build container.
# Produces a .deb in /out (bind-mounted from the host).
set -euo pipefail

VERSION="${VERSION:-0.1.0}"
ARCH="${ARCH:-$(dpkg --print-architecture)}"
PKG="evemon_${VERSION}_${ARCH}"
DESTDIR="/tmp/${PKG}"

echo "==> Building evemon ${VERSION} for ${ARCH}"

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
make all PREFIX=/usr BPF_CFLAGS="-O2 -target bpf -g -D__TARGET_ARCH_x86 -I/usr/include/${ARCH_TRIPLE}"

# ── 2. Stage install into a temp tree ──────────────────────────
rm -rf "${DESTDIR}"
make install PREFIX=/usr DESTDIR="${DESTDIR}"

# ── 3. Write DEBIAN/control ────────────────────────────────────
mkdir -p "${DESTDIR}/DEBIAN"

INSTALLED_SIZE=$(du -sk "${DESTDIR}" | awk '{print $1}')

cat > "${DESTDIR}/DEBIAN/control" <<EOF
Package: evemon
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: evemon contributors
Installed-Size: ${INSTALLED_SIZE}
Depends: libgtk-3-0, libfontconfig1, libbpf1, libelf1, zlib1g, libjansson4, libpipewire-0.3-0, libsoup-3.0-0, libgtksourceview-4-0, libepoxy0, libx11-6
Section: utils
Priority: optional
Homepage: https://github.com/akujin/evemon
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
