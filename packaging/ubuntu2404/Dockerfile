FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build essentials
    gcc \
    make \
    pkg-config \
    # GTK 3
    libgtk-3-dev \
    # fontconfig
    libfontconfig1-dev \
    # libbpf / eBPF
    libbpf-dev \
    libelf-dev \
    zlib1g-dev \
    # BPF kernel program compilation
    clang \
    llvm \
    linux-headers-generic \
    # GLib resources (glib-compile-resources)
    libglib2.0-dev \
    libglib2.0-dev-bin \
    # Jansson (JSON settings)
    libjansson-dev \
    # PipeWire (optional but include it)
    libpipewire-0.3-dev \
    # libsoup 3 (album art in audio_service_plugin)
    libsoup-3.0-dev \
    # GtkSourceView 4 (write_monitor_ui_plugin)
    libgtksourceview-4-dev \
    # OpenGL via epoxy (milkdrop_plugin)
    libepoxy-dev \
    # X11 (About splash _MOTIF_WM_HINTS)
    libx11-dev \
    # Packaging tools
    dpkg-dev \
    fakeroot \
    binutils \
    desktop-file-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

ENTRYPOINT ["/src/packaging/build-deb.sh"]
