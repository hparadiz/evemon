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

# Exclude the BPF kernel program and plugin-only modules from gcc compilation
SRCS := $(filter-out $(SRC_DIR)/fdmon_ebpf_kern.c $(SRC_DIR)/art_loader.c, $(wildcard $(SRC_DIR)/*.c))

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

.PHONY: all clean run debug gdb install uninstall

all: $(TARGET) $(BPF_OBJ) plugins

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/ui/%.o: $(SRC_DIR)/ui/%.c | $(BUILD_DIR)/ui
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

plugins: $(PLUGIN_SOS)

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
	sudo gdb -q -ex run ./$(TARGET)

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
	install -m 644 $(BPF_OBJ) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(PLUGIN_SOS) $(DESTDIR)$(LIBDIR)/plugins/
	update-desktop-database $(DESTDIR)$(DATADIR)/applications
	-gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/evemon
	rm -f $(DESTDIR)$(DATADIR)/applications/evemon.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/evemon.png
	rm -f $(DESTDIR)$(DATADIR)/pixmaps/evemon.png
	rm -rf $(DESTDIR)$(LIBDIR)
	-gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null || true
