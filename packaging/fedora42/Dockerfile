FROM fedora:42

RUN dnf install -y \
    # Build essentials
    gcc \
    make \
    pkg-config \
    # GTK 3
    gtk3-devel \
    # fontconfig
    fontconfig-devel \
    # libbpf / eBPF
    libbpf-devel \
    elfutils-libelf-devel \
    zlib-devel \
    # BPF kernel program compilation
    clang \
    llvm \
    kernel-headers \
    # GLib resources (glib-compile-resources)
    glib2-devel \
    # Jansson (JSON settings)
    jansson-devel \
    # PipeWire (optional but include it)
    pipewire-devel \
    # libsoup 3 (album art in audio_service_plugin)
    libsoup3-devel \
    # GtkSourceView 4 (write_monitor_ui_plugin)
    gtksourceview4-devel \
    # OpenGL via epoxy (milkdrop_plugin)
    libepoxy-devel \
    # X11 (About splash _MOTIF_WM_HINTS)
    libX11-devel \
    # Packaging tools
    rpm-build \
    desktop-file-utils \
    && dnf clean all

WORKDIR /src

ENTRYPOINT ["/src/packaging/build-rpm.sh"]
