#!/usr/bin/env bash
set -euo pipefail

VERSION="${VERSION:-0.2.0}"
DEBUG="${DEBUG:-0}"

if [ "$(uname -m)" != aarch64 ]; then
    echo "The SteamOS recipe requires an ARM64 builder." >&2
    exit 1
fi

WORKDIR=/tmp/evemon-steamos-build
DESTDIR=/tmp/evemon-steamos-root
rm -rf "${WORKDIR}" "${DESTDIR}"
cp -a /src/. "${WORKDIR}"
cd "${WORKDIR}"
unset MAKEFLAGS

BUILD_TYPE=MinSizeRel
if [ "${DEBUG}" = 1 ]; then
    BUILD_TYPE=Debug
fi
cmake -S . -B build-package -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX=/home/steamos/.local -DCMAKE_INSTALL_LIBDIR=lib \
    -DEVEMON_GTK=OFF -DEVEMON_QT=ON -DEVEMON_DEFAULT_UI=qt \
    -DCMAKE_REQUIRE_FIND_PACKAGE_KF6WindowSystem=ON \
    -DCMAKE_REQUIRE_FIND_PACKAGE_KF6ColorScheme=ON
cmake --build build-package --parallel "$(nproc)"
if [ "${DEBUG}" = 1 ]; then
    DESTDIR="${DESTDIR}" cmake --install build-package
else
    DESTDIR="${DESTDIR}" cmake --install build-package --strip
fi

# The Frame provides glibc 2.39. Check every installed ELF, including UI/plugins.
while IFS= read -r -d '' binary; do
    required=$(readelf --version-info "${binary}" |
        sed -n 's/.*Name: GLIBC_\([0-9.]*\).*/\1/p' | sort -V | tail -n 1)
    if [ -n "${required}" ] &&
       [ "$(printf '%s\n' "${required}" 2.39 | sort -V | tail -n 1)" != 2.39 ]; then
        echo "${binary} requires glibc ${required}; SteamOS provides 2.39." >&2
        exit 1
    fi
done < <(find "${DESTDIR}/home/steamos/.local" -type f \
    \( -name evemon -o -name '*.so' \) -print0)

mkdir -p /out
tar -cJf "/out/evemon-${VERSION}-steamos-arm64.tar.xz" \
    -C "${DESTDIR}/home/steamos" .local
