#!/usr/bin/env bash
# packaging/build-rpm.sh
# Runs inside the Docker build container.
# Produces a .rpm in /out (bind-mounted from the host).
set -euo pipefail

VERSION="${VERSION:-0.1.0}"
ARCH="${ARCH:-$(uname -m)}"
DEBUG="${DEBUG:-0}"
RELEASE="${RELEASE:-1}"

echo "==> Building evemon ${VERSION}-${RELEASE} for ${ARCH} (DEBUG=${DEBUG})"

# ── 1. Copy source to a writable work directory ─────────────────
WORKDIR="/tmp/evemon-build"
rm -rf "${WORKDIR}"
cp -a /src/. "${WORKDIR}"
cd "${WORKDIR}"

# ── 2. Compile ──────────────────────────────────────────────────
unset MAKEFLAGS
make clean

if [ "${DEBUG}" = "1" ]; then
    make debug PREFIX=/usr
else
    make all PREFIX=/usr
fi

# ── 3. Stage install into a temp tree ───────────────────────────
DESTDIR="/tmp/evemon-root"
rm -rf "${DESTDIR}"
make install PREFIX=/usr DESTDIR="${DESTDIR}"

# ── 4. Write the spec file ──────────────────────────────────────
SPECDIR="/tmp/rpmbuild/SPECS"
mkdir -p "${SPECDIR}" /tmp/rpmbuild/{BUILD,RPMS,SRPMS,SOURCES}

cat > "${SPECDIR}/evemon.spec" <<EOF
Name:           evemon
Version:        ${VERSION}
Release:        ${RELEASE}%{?dist}
Summary:        Linux process monitor with deep per-process introspection
License:        MIT
URL:            https://github.com/akujin/evemon

BuildArch:      ${ARCH}

Requires: gtk3
Requires: fontconfig
Requires: libbpf
Requires: elfutils-libelf
Requires: zlib
Requires: jansson
Requires: sqlite-libs
Requires: pipewire-libs
Requires: libsoup3
Requires: gtksourceview4
Requires: libepoxy
Requires: libX11

%description
evemon drills into individual processes: file descriptors, network sockets,
environment variables, memory maps, shared libraries, cgroup limits,
container context, Steam/Proton metadata, and live PipeWire audio streams.
Powered by eBPF tracepoints and a plugin-based sidebar.

%install
cp -a ${DESTDIR}/. %{buildroot}/

%files
%{_bindir}/evemon
%{_datadir}/applications/evemon.desktop
%{_datadir}/icons/hicolor/256x256/apps/evemon.png
%{_datadir}/pixmaps/evemon.png
%dir %{_datadir}/evemon
%{_datadir}/evemon/software-directory.sqlite
%dir %{_datadir}/evemon/milkdrop
%{_datadir}/evemon/milkdrop/presets
%dir %{_libdir}/evemon
%dir %{_libdir}/evemon/plugins
%{_libdir}/evemon/*.o
%{_libdir}/evemon/plugins/*.so

%post
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi

%postun
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi
EOF

# ── 5. Build the RPM ────────────────────────────────────────────
rpmbuild \
    --define "_topdir /tmp/rpmbuild" \
    --define "_libdir /usr/lib" \
    --define "_buildroot /tmp/rpmbuild/BUILDROOT" \
    -bb "${SPECDIR}/evemon.spec"

# ── 6. Copy the result to /out ──────────────────────────────────
mkdir -p /out
find /tmp/rpmbuild/RPMS -name '*.rpm' -exec cp {} /out/ \;

echo "==> RPM(s) written to /out/"
ls /out/*.rpm
