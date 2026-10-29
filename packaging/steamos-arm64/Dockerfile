FROM debian:13 AS bootstrap

ENV DEBIAN_FRONTEND=noninteractive

# Debian only supplies pacman to assemble the SteamOS root filesystem.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates pacman-package-manager \
    && rm -rf /var/lib/apt/lists/*

COPY packaging/steamos-arm64/pacman.conf /tmp/steamos-pacman.conf

# Match the Frame's SteamOS 0.4.5 packages. Dependencies also come from Valve's
# release repositories, including Qt's host tools and the C/C++ compiler.
RUN mkdir -p /steamos/var/lib/pacman /tmp/pacman-cache \
    && pacman --config /tmp/steamos-pacman.conf --root /steamos \
        --dbpath /steamos/var/lib/pacman --cachedir /tmp/pacman-cache \
        --noconfirm --noprogressbar --noscriptlet -Sy \
        filesystem bash coreutils findutils grep sed tar xz make cmake pkgconf \
        pacman ca-certificates binutils clang llvm linux-api-headers \
        'glibc=2.39-2' 'gcc=15.1.1+r500+gb1b8d8ce3eea-1.2' \
        'gcc-libs=15.1.1+r500+gb1b8d8ce3eea-1.2' \
        'qt6-base=6.8.0-2.1' 'kconfig=6.14.0-1.1' \
        'kcolorscheme=6.14.0-1.1' 'kwindowsystem=6.14.0-1.1' \
        'glib2=2.84.3-3' 'libbpf=1.5.1-1' 'libdrm=2.4.134-1' \
        'libpipewire=1:1.6.8-1.2' \
        libelf jansson sqlite systemd-libs ncurses libsoup3 pango gdk-pixbuf2 libepoxy

# No Debian libraries or tools enter the environment used to build Evemon.
FROM scratch
COPY --from=bootstrap /steamos/ /
COPY --from=bootstrap /tmp/steamos-pacman.conf /etc/pacman.conf
ENV PATH=/usr/bin LANG=C.UTF-8
RUN ldconfig

WORKDIR /src
ENTRYPOINT ["bash", "/src/packaging/build-steamos.sh"]
