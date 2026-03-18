MAKEFLAGS += -j$(shell nproc)

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
LIBDIR = $(PREFIX)/lib/evemon
DATADIR = $(PREFIX)/share

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2 -pthread -D_GNU_SOURCE \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
           -Wformat -Wformat-security -fPIE \
           -DEVEMON_LIBDIR='"$(LIBDIR)"'
LDFLAGS := -pthread -lm -pie -Wl,-z,relro,-z,now \
           -rdynamic -ldl

# GTK3 flags
GTK_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0)
GTK_LDFLAGS := $(shell pkg-config --libs gtk+-3.0)

# X11 (needed for _MOTIF_WM_HINTS in the About splash)
X11_LDFLAGS := $(shell pkg-config --libs x11 2>/dev/null)

# Fontconfig (explicit init to suppress startup warning)
FC_CFLAGS   := $(shell pkg-config --cflags fontconfig)
FC_LDFLAGS  := $(shell pkg-config --libs fontconfig)

# Jansson (JSON settings file)
JANSSON_CFLAGS  := $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LDFLAGS := $(shell pkg-config --libs   jansson 2>/dev/null)

CFLAGS  += $(GTK_CFLAGS) $(FC_CFLAGS) $(JANSSON_CFLAGS)
LDFLAGS += $(GTK_LDFLAGS) $(FC_LDFLAGS) $(X11_LDFLAGS) $(JANSSON_LDFLAGS)

# PipeWire (optional — audio connection info in sidebar)
# Auto-detect: if libpipewire-0.3 is installed, enable the feature.
# Disable explicitly with:  make HAVE_PIPEWIRE=0
ifneq ($(HAVE_PIPEWIRE),0)
  PW_CFLAGS  := $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null)
  PW_LDFLAGS := $(shell pkg-config --libs   libpipewire-0.3 2>/dev/null)
  ifneq ($(PW_LDFLAGS),)
    CFLAGS  += $(PW_CFLAGS) -DHAVE_PIPEWIRE
    LDFLAGS += $(PW_LDFLAGS)
  endif
endif

# libbpf (for eBPF fd-monitor backend)
LDFLAGS += -lbpf -lelf -lz

# BPF kernel program compilation
CLANG   := clang
BPF_CFLAGS := -O2 -target bpf -g -D__TARGET_ARCH_x86

SRC_DIR := src
BUILD_DIR := build

# GResource (embeds icon.png into the binary)
GRES_XML := $(SRC_DIR)/evemon.gresource.xml
GRES_C   := $(BUILD_DIR)/evemon_resources.c
GRES_O   := $(BUILD_DIR)/evemon_resources.o

# ── libevemon_core: toolkit-free sources ────────────────────────
# These compile without any GTK dependency and will eventually be
# linked by all frontends (GTK, headless, terminal).
CORE_SRCS := \
    $(SRC_DIR)/log.c \
    $(SRC_DIR)/monitor.c \
    $(SRC_DIR)/plugin_broker.c \
    $(SRC_DIR)/plugin_loader.c \
    $(SRC_DIR)/event_bus.c \
    $(SRC_DIR)/fdmon.c \
    $(SRC_DIR)/fdmon_ebpf.c \
    $(SRC_DIR)/steam.c \
    $(SRC_DIR)/settings.c \
    $(SRC_DIR)/profile.c \
    $(SRC_DIR)/mpris.c

CORE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))
CORE_LIB  := $(BUILD_DIR)/libevemon_core.a

# GLib (needed by core: mpris.c uses GDBus/GLib, settings.c uses jansson but
# settings.h pulls in glib via evemon_plugin.h indirectly).
# GLib is a subset of GTK — it has no display dependency.
GLIB_CFLAGS  := $(shell pkg-config --cflags glib-2.0 gio-2.0 2>/dev/null)
GLIB_LDFLAGS := $(shell pkg-config --libs   glib-2.0 gio-2.0 2>/dev/null)

# Core objects use base CFLAGS only (no GTK), and declare EVEMON_NO_GTK
# so evemon_plugin.h substitutes void* for GtkWidget*/GdkPixbuf*.
# Use lazy assignment (=) so that debug:/gdb: CFLAGS overrides propagate.
CORE_CFLAGS = $(filter-out $(GTK_CFLAGS),$(CFLAGS)) $(GLIB_CFLAGS) -DEVEMON_NO_GTK

# ── GTK frontend + UI sources ────────────────────────────────────
# Exclude the BPF kernel program, art_loader (plugin-only), and all
# core sources (now in libevemon_core.a) from the per-file SRCS list.
CORE_BASENAMES := $(notdir $(CORE_SRCS))
ALL_MAIN_SRCS  := $(filter-out $(SRC_DIR)/fdmon_ebpf_kern.c $(SRC_DIR)/art_loader.c, $(wildcard $(SRC_DIR)/*.c))
SRCS := $(filter-out $(addprefix $(SRC_DIR)/,$(CORE_BASENAMES)), $(ALL_MAIN_SRCS))

# Retired sidebar scan modules (replaced by the plugin system).
# They still live in src/ui/ for reference but are no longer compiled.
# Note: pipewire_scan.c is kept — it provides pw_snapshot()/pw_graph_free()
# used by the plugin broker.
RETIRED_SCANS := fd_scan.c env_scan.c mmap_scan.c lib_scan.c net_scan.c
SRCS += $(filter-out $(addprefix $(SRC_DIR)/ui/,$(RETIRED_SCANS)), \
                     $(wildcard $(SRC_DIR)/ui/*.c))
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS += $(GRES_O)

# BPF object (compiled with clang, loaded at runtime)
BPF_SRC := $(SRC_DIR)/fdmon_ebpf_kern.c
BPF_OBJ := $(BUILD_DIR)/fdmon_ebpf_kern.o

TARGET := $(BUILD_DIR)/evemon

# ── Packaging targets ───────────────────────────────────────────
# Usage:  make deb target=debian12
#         make deb target=debian13
#         make deb target=ubuntu2204
#         make deb target=ubuntu2404
# Output: dist/<target>/evemon_<version>_<arch>.deb

DEB_VERSION ?= 0.1.0
DEB_TARGET  ?= $(if $(target),$(target),debian12)
DEB_DEBUG   ?= $(if $(filter 1 true yes,$(debug)),1,0)

# Map target name → Dockerfile directory
DEB_DOCKERFILE_debian12   := packaging/debian12
DEB_DOCKERFILE_debian13   := packaging/debian13
DEB_DOCKERFILE_ubuntu2204 := packaging/ubuntu2204
DEB_DOCKERFILE_ubuntu2404 := packaging/ubuntu2404

DEB_IMAGE   := evemon-builder-$(DEB_TARGET)
DEB_OUTDIR  := $(CURDIR)/dist/$(DEB_TARGET)

# ── RPM packaging variables ─────────────────────────────────────
# Usage:  make rpm target=fedora42
#         make rpm target=fedora41
# Output: dist/<target>/evemon-<version>-<release>.<arch>.rpm

RPM_VERSION ?= 0.1.0
RPM_RELEASE ?= 1
RPM_TARGET  ?= $(if $(target),$(target),fedora42)
RPM_DEBUG   ?= $(if $(filter 1 true yes,$(debug)),1,0)

# Map target name → Dockerfile directory
RPM_DOCKERFILE_fedora42 := packaging/fedora42
RPM_DOCKERFILE_fedora41 := packaging/fedora41

RPM_IMAGE  := evemon-builder-$(RPM_TARGET)
RPM_OUTDIR := $(CURDIR)/dist/$(RPM_TARGET)

.PHONY: all clean run debug gdb install uninstall deb rpm

all: $(CORE_LIB) $(TARGET) $(BPF_OBJ) plugins

$(CORE_LIB): $(CORE_OBJS) | $(BUILD_DIR)
	ar rcs $@ $^

# Core objects: compiled without GTK flags
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CORE_CFLAGS) -c -o $@ $<

# GTK frontend binary links libevemon_core.a
$(TARGET): $(OBJS) $(CORE_LIB)
	$(CC) -o $@ $(OBJS) -L$(BUILD_DIR) -levemon_core $(LDFLAGS)

$(BUILD_DIR)/ui/%.o: $(SRC_DIR)/ui/%.c | $(BUILD_DIR)/ui
	$(CC) $(CFLAGS) -c -o $@ $<

# main.c and other non-core top-level sources need full CFLAGS (GTK etc.)
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# GResource — compile XML + icon.png + font-logos.ttf into a C source, then into .o
$(GRES_C): $(GRES_XML) icon.png font-logos.ttf | $(BUILD_DIR)
	glib-compile-resources --sourcedir=$(SRC_DIR) --generate-source --target=$@ $<

$(GRES_O): $(GRES_C)
	$(CC) $(CFLAGS) -c -o $@ $<

# BPF kernel program — compiled to BPF bytecode with clang
$(BPF_OBJ): $(BPF_SRC) | $(BUILD_DIR)
	$(CLANG) $(BPF_CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/ui:
	mkdir -p $(BUILD_DIR)/ui

# ── Plugin shared objects ────────────────────────────────────────
PLUGIN_DIR     := $(BUILD_DIR)/plugins
SOUP_CFLAGS    := $(shell pkg-config --cflags libsoup-3.0 2>/dev/null)
SOUP_LDFLAGS   := $(shell pkg-config --libs   libsoup-3.0 2>/dev/null)
PLUGIN_CFLAGS  := -Wall -Wextra -std=c11 -O2 -D_GNU_SOURCE -fPIC -shared \
                  -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
                  -Wformat -Wformat-security \
                  $(GTK_CFLAGS) $(FC_CFLAGS)
PLUGIN_LDFLAGS := $(GTK_LDFLAGS) $(FC_LDFLAGS)

PLUGIN_SRCS := $(filter-out $(SRC_DIR)/plugins/json_service_plugin.c, $(wildcard $(SRC_DIR)/plugins/*.c))
PLUGIN_SOS  := $(patsubst $(SRC_DIR)/plugins/%.c,$(PLUGIN_DIR)/evemon_%.so,$(PLUGIN_SRCS))

$(PLUGIN_DIR):
	mkdir -p $(PLUGIN_DIR)

$(PLUGIN_DIR)/evemon_%.so: $(SRC_DIR)/plugins/%.c $(SRC_DIR)/evemon_plugin.h | $(PLUGIN_DIR)
	$(CC) $(PLUGIN_CFLAGS) -o $@ $< $(PLUGIN_LDFLAGS)

# PipeWire plugin needs PW flags (only built when PW is available)
ifneq ($(PW_LDFLAGS),)
$(PLUGIN_DIR)/evemon_pipewire_plugin.so: $(SRC_DIR)/plugins/pipewire_plugin.c $(SRC_DIR)/evemon_plugin.h | $(PLUGIN_DIR)
	$(CC) $(PLUGIN_CFLAGS) $(PW_CFLAGS) -DHAVE_PIPEWIRE -o $@ $(SRC_DIR)/plugins/pipewire_plugin.c $(PLUGIN_LDFLAGS) $(PW_LDFLAGS)
else
# If PipeWire is not available, skip the PipeWire plugin
PLUGIN_SOS := $(filter-out $(PLUGIN_DIR)/evemon_pipewire_plugin.so,$(PLUGIN_SOS))
endif

# MilkDrop plugin needs -lm for math and -lepoxy for OpenGL via GtkGLArea
$(PLUGIN_DIR)/evemon_milkdrop_plugin.so: $(SRC_DIR)/plugins/milkdrop_plugin.c $(SRC_DIR)/evemon_plugin.h | $(PLUGIN_DIR)
	$(CC) $(PLUGIN_CFLAGS) -o $@ $(SRC_DIR)/plugins/milkdrop_plugin.c $(PLUGIN_LDFLAGS) -lm -lepoxy
	$(CC) $(PLUGIN_CFLAGS) -o $@ $(SRC_DIR)/plugins/milkdrop_plugin.c $(PLUGIN_LDFLAGS) -lm -lepoxy

# Audio service headless plugin — owns album art loading (art_loader.c + libsoup)
$(PLUGIN_DIR)/evemon_audio_service_plugin.so: $(SRC_DIR)/plugins/audio_service_plugin.c $(SRC_DIR)/art_loader.c $(SRC_DIR)/evemon_plugin.h | $(PLUGIN_DIR)
	$(CC) $(PLUGIN_CFLAGS) $(SOUP_CFLAGS) -o $@ $(SRC_DIR)/plugins/audio_service_plugin.c $(SRC_DIR)/art_loader.c $(PLUGIN_LDFLAGS) $(SOUP_LDFLAGS)

# write_monitor_ui plugin needs GtkSourceView-4
GSV_CFLAGS  := $(shell pkg-config --cflags gtksourceview-4 2>/dev/null)
GSV_LDFLAGS := $(shell pkg-config --libs   gtksourceview-4 2>/dev/null)
$(PLUGIN_DIR)/evemon_write_monitor_ui_plugin.so: $(SRC_DIR)/plugins/write_monitor_ui_plugin.c $(SRC_DIR)/evemon_plugin.h | $(PLUGIN_DIR)
	$(CC) $(PLUGIN_CFLAGS) $(GSV_CFLAGS) -o $@ $< $(PLUGIN_LDFLAGS) $(GSV_LDFLAGS)

plugins: $(PLUGIN_SOS)

deb:
	@if [ -z "$(DEB_TARGET)" ]; then \
	    echo "Usage: make deb target=<debian12|debian13|ubuntu2204|ubuntu2404>"; \
	    exit 1; \
	fi
	@DDIR="$(DEB_DOCKERFILE_$(DEB_TARGET))"; \
	if [ -z "$$DDIR" ]; then \
	    echo "Unknown target '$(DEB_TARGET)'. Valid: debian12 debian13 ubuntu2204 ubuntu2404"; \
	    exit 1; \
	fi; \
	if [ ! -f "$$DDIR/Dockerfile" ]; then \
	    echo "Dockerfile not found: $$DDIR/Dockerfile"; \
	    exit 1; \
	fi; \
	mkdir -p "$(DEB_OUTDIR)"; \
	echo "==> Building Docker image $(DEB_IMAGE) from $$DDIR"; \
	docker build -t $(DEB_IMAGE) -f "$$DDIR/Dockerfile" .; \
	echo "==> Running package build for target=$(DEB_TARGET)"; \
	docker run --rm \
	    -e VERSION=$(DEB_VERSION) \
	    -e DEBUG=$(DEB_DEBUG) \
	    -v "$(CURDIR):/src:ro" \
	    -v "$(DEB_OUTDIR):/out" \
	    $(DEB_IMAGE); \
	echo "==> .deb written to dist/$(DEB_TARGET)/"; \
	if [ "$(DEB_DEBUG)" = "1" ]; then \
	    echo "==> .ddeb written to dist/$(DEB_TARGET)/"; \
	fi

rpm:
	@if [ -z "$(RPM_TARGET)" ]; then \
	    echo "Usage: make rpm target=<fedora42|fedora41>"; \
	    exit 1; \
	fi
	@DDIR="$(RPM_DOCKERFILE_$(RPM_TARGET))"; \
	if [ -z "$$DDIR" ]; then \
	    echo "Unknown target '$(RPM_TARGET)'. Valid: fedora42 fedora41"; \
	    exit 1; \
	fi; \
	if [ ! -f "$$DDIR/Dockerfile" ]; then \
	    echo "Dockerfile not found: $$DDIR/Dockerfile"; \
	    exit 1; \
	fi; \
	mkdir -p "$(RPM_OUTDIR)"; \
	echo "==> Building Docker image $(RPM_IMAGE) from $$DDIR"; \
	docker build -t $(RPM_IMAGE) -f "$$DDIR/Dockerfile" .; \
	echo "==> Running package build for target=$(RPM_TARGET)"; \
	docker run --rm \
	    -e VERSION=$(RPM_VERSION) \
	    -e RELEASE=$(RPM_RELEASE) \
	    -e DEBUG=$(RPM_DEBUG) \
	    -v "$(CURDIR):/src:ro" \
	    -v "$(RPM_OUTDIR):/out" \
	    $(RPM_IMAGE); \
	echo "==> .rpm written to dist/$(RPM_TARGET)/"

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET) $(BPF_OBJ) plugins
	./$(TARGET)

# ── Debug build ───────────────────────────────────────────────────
# Usage:  make debug        — build with debug symbols
#         make gdb          — build debug + launch under gdb
#         sudo make gdb     — same, as root (needed for eBPF / procfs)

DEBUG_CFLAGS  := $(subst -O2,-Og -g3,$(CFLAGS))
DEBUG_CFLAGS  := $(filter-out -D_FORTIFY_SOURCE=2,$(DEBUG_CFLAGS))
DEBUG_LDFLAGS := $(LDFLAGS)
DEBUG_PLUGIN_CFLAGS := $(subst -O2,-Og -g3,$(PLUGIN_CFLAGS))
DEBUG_PLUGIN_CFLAGS := $(filter-out -D_FORTIFY_SOURCE=2,$(DEBUG_PLUGIN_CFLAGS))

debug: CFLAGS  := $(DEBUG_CFLAGS)
debug: LDFLAGS := $(DEBUG_LDFLAGS)
debug: PLUGIN_CFLAGS := $(DEBUG_PLUGIN_CFLAGS)
debug: all

gdb: debug
	gdb -q -ex run --args ./$(TARGET)

comma := ,
ASAN_CFLAGS  := $(subst -O2,-Og -g3,$(CFLAGS)) -fsanitize=address -fno-omit-frame-pointer
ASAN_CFLAGS  := $(filter-out -D_FORTIFY_SOURCE=2 -fstack-protector-strong -pie,$(ASAN_CFLAGS))
ASAN_LDFLAGS := $(filter-out -pie -Wl$(comma)-z$(comma)relro$(comma)-z$(comma)now,$(LDFLAGS)) -fsanitize=address
asan: CFLAGS  := $(ASAN_CFLAGS)
asan: LDFLAGS := $(ASAN_LDFLAGS)
asan: PLUGIN_CFLAGS := $(subst -O2,-Og -g3,$(PLUGIN_CFLAGS)) -fsanitize=address -fno-omit-frame-pointer
asan: all

UBSAN_CFLAGS  := $(subst -O2,-Og -g3,$(CFLAGS)) -fsanitize=undefined,address -fno-omit-frame-pointer -fno-sanitize-recover=all
UBSAN_CFLAGS  := $(filter-out -D_FORTIFY_SOURCE=2 -fstack-protector-strong -pie,$(UBSAN_CFLAGS))
UBSAN_LDFLAGS := $(filter-out -pie -Wl$(comma)-z$(comma)relro$(comma)-z$(comma)now,$(LDFLAGS)) -fsanitize=undefined,address
ubsan: CFLAGS  := $(UBSAN_CFLAGS)
ubsan: LDFLAGS := $(UBSAN_LDFLAGS)
ubsan: PLUGIN_CFLAGS := $(subst -O2,-Og -g3,$(PLUGIN_CFLAGS)) -fsanitize=undefined,address -fno-omit-frame-pointer
ubsan: all

# ── Install ──────────────────────────────────────────────────────

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/evemon
	install -d $(DESTDIR)$(DATADIR)/applications
	install -m 644 evemon.desktop $(DESTDIR)$(DATADIR)/applications/evemon.desktop
	install -d $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps
	install -m 644 icon.png $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/evemon.png
	install -d $(DESTDIR)$(DATADIR)/pixmaps
	install -m 644 icon.png $(DESTDIR)$(DATADIR)/pixmaps/evemon.png
	install -d $(DESTDIR)$(LIBDIR)/plugins
	# Remove all stale .so files before installing so no old-ABI plugins linger
	rm -f $(DESTDIR)$(LIBDIR)/plugins/*.so
	install -m 644 $(BPF_OBJ) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(PLUGIN_SOS) $(DESTDIR)$(LIBDIR)/plugins/
	$(if $(DESTDIR),,update-desktop-database $(DATADIR)/applications)
	$(if $(DESTDIR),,-gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor 2>/dev/null || true)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/evemon
	rm -f $(DESTDIR)$(DATADIR)/applications/evemon.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/evemon.png
	rm -f $(DESTDIR)$(DATADIR)/pixmaps/evemon.png
	rm -rf $(DESTDIR)$(LIBDIR)
	-gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null || true
