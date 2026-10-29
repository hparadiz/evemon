FROM debian:13

ENV DEBIAN_FRONTEND=noninteractive

# Qt 6.8 and KDE Frameworks 6.13 target the Frame's Qt 6.8/KF 6.14 runtime.
# build-steamos.sh rejects binaries requiring a newer glibc than the Frame.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config xz-utils \
    qt6-base-dev qt6-base-dev-tools libqt6opengl6-dev \
    libkf6windowsystem-dev libkf6colorscheme-dev libkf6config-dev \
    libglib2.0-dev libjansson-dev libsqlite3-dev libbpf-dev libelf-dev \
    libdrm-dev libudev-dev libncurses-dev libpipewire-0.3-dev libsoup-3.0-dev \
    libpango1.0-dev libgdk-pixbuf-2.0-dev libepoxy-dev \
    clang llvm linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
ENTRYPOINT ["bash", "/src/packaging/build-steamos.sh"]
