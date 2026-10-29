MAKEFLAGS += -j$(shell nproc)

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
LIBDIR = $(PREFIX)/lib/evemon
DATADIR = $(PREFIX)/share
EVEMON_DATADIR = $(DATADIR)/evemon
MILKDROP_PRESETDIR = $(EVEMON_DATADIR)/milkdrop/presets
SOFTWARE_DIRECTORY_DB = data/software-directory.sqlite

CC := gcc
CXX := g++
CLANG := clang
CFLAGS := -Wall -Wextra -std=c11 -O2 -g -pthread -D_GNU_SOURCE -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fno-strict-aliasing -fno-strict-overflow
CXXFLAGS := -Wall -Wextra -std=c++17 -O2 -g -pthread -D_GNU_SOURCE -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fno-strict-aliasing -fno-strict-overflow
HARDEN_LDFLAGS := -Wl,-z,relro,-z,now

SRC_DIR := src
BUILD_DIR ?= build
VERSION := 0.2.0

# Toolkit and library flags
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0 gio-2.0 2>/dev/null)
GLIB_LDFLAGS := $(shell pkg-config --libs glib-2.0 gio-2.0 2>/dev/null)
BPF_LDFLAGS := $(shell pkg-config --libs libbpf libelf 2>/dev/null)
JANSSON_CFLAGS := $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LDFLAGS := $(shell pkg-config --libs jansson 2>/dev/null)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 fontconfig x11 2>/dev/null)
GTK_LDFLAGS := $(shell pkg-config --libs gtk+-3.0 fontconfig x11 2>/dev/null)
GTK_WAYLAND_CFLAGS := $(shell pkg-config --cflags gdk-wayland-3.0 wayland-client 2>/dev/null)
GTK_WAYLAND_LDFLAGS := $(shell pkg-config --libs gdk-wayland-3.0 wayland-client 2>/dev/null)
WAYLAND_SCANNER := $(shell command -v wayland-scanner 2>/dev/null)
BACKGROUND_EFFECT_PROTOCOL := $(firstword $(wildcard /usr/share/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml /usr/local/share/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml))
PANGO_CFLAGS := $(shell pkg-config --cflags pangocairo gdk-pixbuf-2.0 2>/dev/null)
PANGO_LDFLAGS := $(shell pkg-config --libs pangocairo gdk-pixbuf-2.0 2>/dev/null)
SQLITE_LDFLAGS := $(shell pkg-config --libs sqlite3 2>/dev/null)
GSV_CFLAGS := $(shell pkg-config --cflags gtksourceview-4 2>/dev/null)
GSV_LDFLAGS := $(shell pkg-config --libs gtksourceview-4 2>/dev/null)
SOUP_CFLAGS := $(shell pkg-config --cflags libsoup-3.0 2>/dev/null)
SOUP_LDFLAGS := $(shell pkg-config --libs libsoup-3.0 2>/dev/null)
EPOXY_CFLAGS := $(shell pkg-config --cflags epoxy 2>/dev/null)
EPOXY_LDFLAGS := $(shell pkg-config --libs epoxy 2>/dev/null)
QT_CFLAGS := $(shell pkg-config --cflags Qt6Core Qt6Gui Qt6Widgets 2>/dev/null)
QT_LDFLAGS := $(shell pkg-config --libs Qt6Core Qt6Gui Qt6Widgets 2>/dev/null)
KWINDOWSYSTEM_CFLAGS := $(shell pkg-config --cflags KF6WindowSystem 2>/dev/null)
KWINDOWSYSTEM_LDFLAGS := $(shell pkg-config --libs KF6WindowSystem 2>/dev/null)
QT_OPENGL_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6OpenGLWidgets Qt6OpenGL 2>/dev/null)
QT_OPENGL_LDFLAGS := $(shell pkg-config --libs Qt6Widgets Qt6OpenGLWidgets Qt6OpenGL 2>/dev/null)
QT_LIBEXECDIR := $(shell pkg-config --variable=libexecdir Qt6Core 2>/dev/null)
QT_RCC ?= $(QT_LIBEXECDIR)/rcc

# PipeWire is optional and can be disabled explicitly with HAVE_PIPEWIRE=0.
ifneq ($(HAVE_PIPEWIRE),0)
PW_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null)
PW_LDFLAGS := $(shell pkg-config --libs libpipewire-0.3 2>/dev/null)
endif
PIPEWIRE_ENABLED := $(if $(PW_LDFLAGS),1,0)

# Complete pinned nvtop backend set.
NVTOP_DIR := third_party/nvtop
NVTOP_INCLUDE := $(NVTOP_DIR)/include
NVTOP_SRC_DIR := $(NVTOP_DIR)/src
NVTOP_AVAILABLE := $(if $(wildcard $(NVTOP_SRC_DIR)/extract_gpuinfo.c),1,0)
NVTOP_DRM_CFLAGS := $(shell pkg-config --cflags libdrm 2>/dev/null)
NVTOP_DRM_LDFLAGS := $(shell pkg-config --libs libdrm 2>/dev/null)
NVTOP_UDEV_CFLAGS := $(shell pkg-config --cflags libudev 2>/dev/null)
NVTOP_UDEV_LDFLAGS := $(shell pkg-config --libs libudev 2>/dev/null)
NVTOP_SRCS := $(SRC_DIR)/plugins/gpu_nvtop_adapter.c $(SRC_DIR)/plugins/gpu_dcmi_shim.c $(NVTOP_SRC_DIR)/extract_gpuinfo.c $(NVTOP_SRC_DIR)/time.c $(NVTOP_SRC_DIR)/get_process_info_linux.c $(NVTOP_SRC_DIR)/extract_processinfo_fdinfo.c $(NVTOP_SRC_DIR)/device_discovery_linux.c $(NVTOP_SRC_DIR)/extract_gpuinfo_nvidia.c $(NVTOP_SRC_DIR)/extract_gpuinfo_ascend.c $(NVTOP_SRC_DIR)/extract_gpuinfo_amdgpu.c $(NVTOP_SRC_DIR)/extract_gpuinfo_amdgpu_utils.c $(NVTOP_SRC_DIR)/extract_gpuinfo_intel.c $(NVTOP_SRC_DIR)/extract_gpuinfo_intel_i915.c $(NVTOP_SRC_DIR)/extract_gpuinfo_intel_xe.c $(NVTOP_SRC_DIR)/extract_gpuinfo_msm.c $(NVTOP_SRC_DIR)/extract_gpuinfo_msm_utils.c $(NVTOP_SRC_DIR)/extract_gpuinfo_panfrost.c $(NVTOP_SRC_DIR)/extract_gpuinfo_panfrost_utils.c $(NVTOP_SRC_DIR)/extract_gpuinfo_panthor.c $(NVTOP_SRC_DIR)/extract_gpuinfo_panthor_utils.c $(NVTOP_SRC_DIR)/extract_gpuinfo_mali_common.c $(NVTOP_SRC_DIR)/extract_gpuinfo_v3d.c $(NVTOP_SRC_DIR)/extract_gpuinfo_v3d_utils.c $(NVTOP_SRC_DIR)/extract_gpuinfo_tpu.c $(NVTOP_SRC_DIR)/extract_npuinfo_rockchip.c $(NVTOP_SRC_DIR)/extract_gpuinfo_metax.c $(NVTOP_SRC_DIR)/extract_gcuinfo_enflame.c

ANDROID_ARCHIVE := data/android-arm64/evemon-arm64.tar.gz
ANDROID_SHA256 := $(shell sha256sum "$(ANDROID_ARCHIVE)" 2>/dev/null | cut -d' ' -f1)

FEATURE_DEFINES := -DEVEMON_VERSION='"$(VERSION)"' -DEVEMON_LIBDIR='"$(LIBDIR)"' -DEVEMON_DATADIR='"$(EVEMON_DATADIR)"' -DEVEMON_DEFAULT_UI='"gtk"' -DEVEMON_HAVE_GTK=1 -DEVEMON_HAVE_QT=1 -DEVEMON_WITH_PIPEWIRE=$(PIPEWIRE_ENABLED) -DEVEMON_WITH_MILKDROP=$(PIPEWIRE_ENABLED) -DEVEMON_WITH_GPU=$(NVTOP_AVAILABLE) -DEVEMON_WITH_EBPF=1
ifneq ($(ANDROID_SHA256),)
FEATURE_DEFINES += -DEVEMON_ANDROID_SHA256='"$(ANDROID_SHA256)"'
endif
ifeq ($(PIPEWIRE_ENABLED),1)
FEATURE_DEFINES += -DHAVE_PIPEWIRE
endif
ifeq ($(NVTOP_AVAILABLE),1)
FEATURE_DEFINES += -DHAVE_NVTOP
endif
CORE_FEATURE_DEFINES = $(filter-out -DHAVE_PIPEWIRE,$(FEATURE_DEFINES))

# ── Core runtime and launcher ───────────────────────────────────
CORE_SRCS := $(SRC_DIR)/core/log.c $(SRC_DIR)/core/monitor.c $(SRC_DIR)/core/steam.c $(SRC_DIR)/core/profile.c $(SRC_DIR)/platform/fdmon.c $(SRC_DIR)/platform/fdmon_ebpf.c $(SRC_DIR)/plugins/event_bus.c $(SRC_DIR)/plugins/plugin_broker.c $(SRC_DIR)/plugins/plugin_loader.c $(SRC_DIR)/core/settings.c $(SRC_DIR)/media/mpris.c $(SRC_DIR)/core/format.c $(SRC_DIR)/app/runtime.c $(SRC_DIR)/ui/devices.c $(SRC_DIR)/platform/desktop_session.c $(SRC_DIR)/platform/compatibility.c $(SRC_DIR)/ipc/ipc_wire.c $(SRC_DIR)/ipc/query_client.c $(SRC_DIR)/ipc/query_server.c $(SRC_DIR)/ipc/query_snapshot.c $(SRC_DIR)/ipc/query_inspect.c $(SRC_DIR)/ipc/query_events.c $(SRC_DIR)/ipc/query_fdmon.c $(SRC_DIR)/ipc/query_gpu.c $(SRC_DIR)/ipc/query_host.c $(SRC_DIR)/ipc/query_about.c $(SRC_DIR)/app/client_session.c $(SRC_DIR)/remote/ssh_tunnel.c $(SRC_DIR)/remote/adb_tunnel.c
ifeq ($(NVTOP_AVAILABLE),1)
CORE_SRCS += $(NVTOP_SRCS)
endif
CORE_OBJS := $(patsubst %.c,$(BUILD_DIR)/core/%.o,$(CORE_SRCS))
CORE_LIB := $(BUILD_DIR)/libevemon_core.a
CORE_CFLAGS = $(CFLAGS) -fPIC -I$(SRC_DIR) $(GLIB_CFLAGS) $(JANSSON_CFLAGS) $(NVTOP_DRM_CFLAGS) $(NVTOP_UDEV_CFLAGS) -I$(NVTOP_INCLUDE) -I$(NVTOP_SRC_DIR) -DEVEMON_NO_GTK -DUSING_LIBUDEV $(CORE_FEATURE_DEFINES)
GPU_CORE_CFLAGS = $(filter-out -std=c11,$(CORE_CFLAGS)) -std=gnu11
CORE_LDFLAGS := $(GLIB_LDFLAGS) $(BPF_LDFLAGS) $(JANSSON_LDFLAGS) $(NVTOP_DRM_LDFLAGS) $(NVTOP_UDEV_LDFLAGS) -pthread -ldl -lm

APP_SRCS := $(SRC_DIR)/app/main.c
APP_OBJS := $(patsubst %.c,$(BUILD_DIR)/app/%.o,$(APP_SRCS))
TARGET := $(BUILD_DIR)/evemon
APP_CFLAGS = $(CFLAGS) -fPIE -I$(SRC_DIR) $(GLIB_CFLAGS) -DEVEMON_NO_GTK $(CORE_FEATURE_DEFINES)

# ── GTK frontend ────────────────────────────────────────────────
GTK_RETIRED := devices.c fd_scan.c env_scan.c mmap_scan.c lib_scan.c net_scan.c
GTK_UI_SRCS := $(filter-out $(addprefix $(SRC_DIR)/ui/,$(GTK_RETIRED)),$(wildcard $(SRC_DIR)/ui/*.c))
GTK_UI_OBJS := $(patsubst %.c,$(BUILD_DIR)/gtk/%.o,$(GTK_UI_SRCS))
GRES_XML := $(SRC_DIR)/ui/evemon.gresource.xml
GRES_C := $(BUILD_DIR)/generated/evemon_resources.c
GRES_O := $(BUILD_DIR)/gtk/generated/evemon_resources.o
GTK_MODULE := $(BUILD_DIR)/evemon-ui-gtk.so
GTK_FRONTEND_EXTRA_CFLAGS :=
GTK_FRONTEND_EXTRA_LDFLAGS :=
ifneq ($(strip $(GTK_WAYLAND_LDFLAGS)),)
ifneq ($(strip $(WAYLAND_SCANNER)),)
ifneq ($(strip $(BACKGROUND_EFFECT_PROTOCOL)),)
BLUR_HEADER := $(BUILD_DIR)/generated/evemon-background-effect-client.h
BLUR_CODE := $(BUILD_DIR)/generated/evemon-background-effect-protocol.c
BLUR_O := $(BUILD_DIR)/gtk/generated/evemon-background-effect-protocol.o
GTK_UI_OBJS += $(BLUR_O)
GTK_FRONTEND_EXTRA_CFLAGS += -DHAVE_GTK_WAYLAND_BLUR -I$(BUILD_DIR)/generated $(GTK_WAYLAND_CFLAGS)
GTK_FRONTEND_EXTRA_LDFLAGS += $(GTK_WAYLAND_LDFLAGS)
endif
endif
endif
GTK_FRONTEND_CFLAGS = $(CFLAGS) -fPIC -I$(SRC_DIR) $(GTK_CFLAGS) $(PANGO_CFLAGS) $(PW_CFLAGS) $(FEATURE_DEFINES) $(GTK_FRONTEND_EXTRA_CFLAGS)

# ── GTK plugins ─────────────────────────────────────────────────
PLUGIN_DIR := $(BUILD_DIR)/plugins
GTK_PLUGIN_NAMES := fd env devices mmap libs threads cgroup system_fd system_libs net gentoo proc_meta_service write_monitor write_monitor_ui
ifeq ($(PIPEWIRE_ENABLED),1)
GTK_PLUGIN_NAMES += pipewire audio_service milkdrop
endif
ifeq ($(NVTOP_AVAILABLE),1)
GTK_PLUGIN_NAMES += gpu
endif
GTK_PLUGIN_SOS := $(addprefix $(PLUGIN_DIR)/evemon_,$(addsuffix _plugin.so,$(GTK_PLUGIN_NAMES)))
PLUGIN_CFLAGS = $(CFLAGS) -fPIC -I$(SRC_DIR) $(GTK_CFLAGS) $(PANGO_CFLAGS) $(FEATURE_DEFINES)
PLUGIN_LDFLAGS := $(GTK_LDFLAGS) $(PANGO_LDFLAGS) $(HARDEN_LDFLAGS) -pthread -ldl -lm

# ── Qt frontend and inspectors ──────────────────────────────────
QT_CPP_SRCS := $(SRC_DIR)/qt/main.cpp $(SRC_DIR)/qt/process_model.cpp $(SRC_DIR)/qt/main_window.cpp $(SRC_DIR)/qt/main_menus.cpp $(SRC_DIR)/qt/remote_dialog.cpp $(SRC_DIR)/qt/theme.cpp $(SRC_DIR)/qt/scrollbars.cpp $(SRC_DIR)/qt/about_dialog.cpp $(SRC_DIR)/qt/compatibility_dialog.cpp $(SRC_DIR)/qt/inspector.cpp $(SRC_DIR)/qt/widget_bridge.cpp $(SRC_DIR)/qt/detail_panel.cpp $(SRC_DIR)/qt/write_view.cpp
QT_CPP_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/qt/%.o,$(QT_CPP_SRCS))
QT_INSPECTOR_NAMES := fd env devices mmap libs threads cgroup system_fd system_libs net gentoo
ifeq ($(PIPEWIRE_ENABLED),1)
QT_INSPECTOR_NAMES += pipewire audio_service
endif
ifeq ($(NVTOP_AVAILABLE),1)
QT_INSPECTOR_NAMES += gpu
endif
QT_INSPECTOR_DIR := $(BUILD_DIR)/qt-inspectors
QT_INSPECTOR_OBJS := $(addprefix $(QT_INSPECTOR_DIR)/,$(addsuffix _plugin.o,$(QT_INSPECTOR_NAMES)))
QT_INSPECTOR_EXTRA_OBJS := $(QT_INSPECTOR_DIR)/net_chart.o
ifeq ($(PIPEWIRE_ENABLED),1)
QT_INSPECTOR_EXTRA_OBJS += $(QT_INSPECTOR_DIR)/pipewire_view.o
endif
QT_AUDIO_SRCS :=
ifeq ($(PIPEWIRE_ENABLED),1)
QT_AUDIO_SRCS += $(SRC_DIR)/media/art_loader.c $(SRC_DIR)/qt/audio_services.c $(SRC_DIR)/ui/pipewire_scan.c $(SRC_DIR)/ui/pw_meter.c $(SRC_DIR)/ui/spectrogram.c
endif
QT_AUDIO_OBJS := $(patsubst %.c,$(BUILD_DIR)/qt-audio/%.o,$(QT_AUDIO_SRCS))
QT_QRC := $(BUILD_DIR)/generated/evemon-qt.qrc
QT_RESOURCE_CPP := $(BUILD_DIR)/generated/qrc_evemon.cpp
QT_RESOURCE_O := $(BUILD_DIR)/qt/generated/qrc_evemon.o
QT_MODULE := $(BUILD_DIR)/evemon-ui-qt.so
QT_PLUGIN_DIR := $(BUILD_DIR)/plugins-qt
QT_MILKDROP := $(QT_PLUGIN_DIR)/libevemon_qt_milkdrop.so
QT_FRONTEND_DEFINES := -DEVEMON_NO_GTK -DEVEMON_QT_PLUGIN_DIR='"$(LIBDIR)/plugins-qt"'
ifeq ($(PIPEWIRE_ENABLED),1)
QT_FRONTEND_DEFINES += -DHAVE_MILKDROP
endif
QT_FRONTEND_EXTRA_CFLAGS :=
QT_FRONTEND_EXTRA_LDFLAGS :=
ifneq ($(strip $(KWINDOWSYSTEM_LDFLAGS)),)
QT_FRONTEND_EXTRA_CFLAGS += $(KWINDOWSYSTEM_CFLAGS) -DHAVE_KWINDOWSYSTEM
QT_FRONTEND_EXTRA_LDFLAGS += $(KWINDOWSYSTEM_LDFLAGS)
endif
QT_FRONTEND_CXXFLAGS = $(CXXFLAGS) -fPIC -I$(SRC_DIR) $(QT_CFLAGS) $(GLIB_CFLAGS) $(PANGO_CFLAGS) $(PW_CFLAGS) $(FEATURE_DEFINES) $(QT_FRONTEND_DEFINES) $(QT_FRONTEND_EXTRA_CFLAGS)
QT_INSPECTOR_CFLAGS = $(CFLAGS) -fPIC -I$(SRC_DIR) $(GLIB_CFLAGS) $(PANGO_CFLAGS) $(PW_CFLAGS) $(FEATURE_DEFINES) -DEVEMON_QT_WIDGETS
QT_AUDIO_CFLAGS = $(QT_INSPECTOR_CFLAGS) $(SOUP_CFLAGS)

QT_MILKDROP_C_SRCS := $(SRC_DIR)/plugins/milkdrop_plugin.c $(SRC_DIR)/plugins/milkdrop_expression.c $(SRC_DIR)/plugins/milkdrop_presets.c $(SRC_DIR)/plugins/milkdrop_render.c $(SRC_DIR)/plugins/milkdrop_controls.c $(SRC_DIR)/plugins/milkdrop_overlay.c
QT_MILKDROP_C_OBJS := $(patsubst %.c,$(BUILD_DIR)/qt-milkdrop/%.o,$(QT_MILKDROP_C_SRCS))
QT_MILKDROP_CPP_OBJS := $(BUILD_DIR)/qt-milkdrop/$(SRC_DIR)/qt/milkdrop_view.o
QT_MILKDROP_CFLAGS = $(CFLAGS) -fPIC -ffunction-sections -fdata-sections -I$(SRC_DIR) $(GLIB_CFLAGS) $(PANGO_CFLAGS) $(PW_CFLAGS) $(EPOXY_CFLAGS) $(QT_OPENGL_CFLAGS) $(FEATURE_DEFINES) -DEVEMON_NO_GTK -DEVEMON_QT_MILKDROP
QT_MILKDROP_CXXFLAGS = $(CXXFLAGS) -fPIC -ffunction-sections -fdata-sections -I$(SRC_DIR) $(GLIB_CFLAGS) $(PANGO_CFLAGS) $(PW_CFLAGS) $(EPOXY_CFLAGS) $(QT_OPENGL_CFLAGS) $(FEATURE_DEFINES) -DEVEMON_NO_GTK -DEVEMON_QT_MILKDROP

# ── BPF and development data ────────────────────────────────────
BPF_SRC := $(SRC_DIR)/platform/fdmon_ebpf_kern.c
BPF_OBJ := $(BUILD_DIR)/fdmon_ebpf_kern.o
BPF_ARCH := $(if $(filter aarch64 arm64,$(shell uname -m)),arm64,x86)
BPF_CFLAGS := -O2 -target bpf -g -D__TARGET_ARCH_$(BPF_ARCH)
DEV_DATA_STAMP := $(BUILD_DIR)/share/evemon/.stamp

.PHONY: all gtk qt plugins clean run debug gdb asan ubsan install uninstall deb rpm nvtop require-android-archive

# Plain make is the complete desktop build: launcher, both frontends, all
# available GTK/Qt plugins, eBPF bytecode, and bundled runtime data.
all: require-android-archive $(CORE_LIB) $(TARGET) $(BPF_OBJ) gtk qt plugins $(DEV_DATA_STAMP)

gtk: $(GTK_MODULE) plugins

qt: $(QT_MODULE) $(QT_MILKDROP)

plugins: $(GTK_PLUGIN_SOS)

require-android-archive:
	@test -r "$(ANDROID_ARCHIVE)" || { echo "Missing Android runtime archive: $(ANDROID_ARCHIVE)" >&2; exit 1; }

$(BUILD_DIR)/core/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CORE_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/core/src/remote/adb_tunnel.o: $(ANDROID_ARCHIVE)

$(BUILD_DIR)/core/third_party/nvtop/%.o: third_party/nvtop/%.c
	@mkdir -p $(@D)
	$(CC) $(GPU_CORE_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/core/src/plugins/gpu_%.o: src/plugins/gpu_%.c
	@mkdir -p $(@D)
	$(CC) $(GPU_CORE_CFLAGS) -c -o $@ $<

$(CORE_LIB): $(CORE_OBJS) Makefile
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $(CORE_OBJS)

$(BUILD_DIR)/app/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(TARGET): $(APP_OBJS) $(CORE_LIB)
	$(CC) -fPIE -pie -rdynamic $(HARDEN_LDFLAGS) -o $@ $(APP_OBJS) -Wl,--whole-archive $(CORE_LIB) -Wl,--no-whole-archive $(CORE_LDFLAGS)

$(BUILD_DIR)/gtk/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(GTK_FRONTEND_CFLAGS) -c -o $@ $<

ifneq ($(strip $(BLUR_O)),)
$(BLUR_HEADER) $(BLUR_CODE) &: $(BACKGROUND_EFFECT_PROTOCOL)
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $(BLUR_HEADER)
	$(WAYLAND_SCANNER) private-code $< $(BLUR_CODE)

$(BUILD_DIR)/gtk/src/ui/ui_blur.o: $(BLUR_HEADER)

$(BLUR_O): $(BLUR_CODE) $(BLUR_HEADER)
	@mkdir -p $(@D)
	$(CC) $(GTK_FRONTEND_CFLAGS) -c -o $@ $(BLUR_CODE)
endif

$(GRES_C): $(GRES_XML) icon.png font-logos.ttf
	@mkdir -p $(@D)
	glib-compile-resources --sourcedir=$(SRC_DIR) --generate-source --target=$@ $<

$(GRES_O): $(GRES_C)
	@mkdir -p $(@D)
	$(CC) $(GTK_FRONTEND_CFLAGS) -c -o $@ $<

$(GTK_MODULE): $(GTK_UI_OBJS) $(GRES_O) $(CORE_LIB)
	$(CC) -shared $(HARDEN_LDFLAGS) -o $@ $(GTK_UI_OBJS) $(GRES_O) $(GTK_LDFLAGS) $(PANGO_LDFLAGS) $(PW_LDFLAGS) $(GTK_FRONTEND_EXTRA_LDFLAGS) -pthread -ldl -lm

$(PLUGIN_DIR)/evemon_%_plugin.so: $(SRC_DIR)/plugins/%_plugin.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $< $(PLUGIN_LDFLAGS)

$(PLUGIN_DIR)/evemon_net_plugin.so: $(SRC_DIR)/plugins/net_plugin.c $(SRC_DIR)/plugins/net_chart.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^) $(PLUGIN_LDFLAGS)

$(PLUGIN_DIR)/evemon_pipewire_plugin.so: $(SRC_DIR)/plugins/pipewire_plugin.c $(SRC_DIR)/plugins/pipewire_view.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $(filter %.c,$^) $(PLUGIN_LDFLAGS)

$(PLUGIN_DIR)/evemon_audio_service_plugin.so: $(SRC_DIR)/plugins/audio_service_plugin.c $(SRC_DIR)/media/art_loader.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(PLUGIN_CFLAGS) $(SOUP_CFLAGS) -shared -o $@ $(filter %.c,$^) $(PLUGIN_LDFLAGS) $(SOUP_LDFLAGS)

$(PLUGIN_DIR)/evemon_milkdrop_plugin.so: $(SRC_DIR)/plugins/milkdrop_plugin.c $(SRC_DIR)/plugins/milkdrop_expression.c $(SRC_DIR)/plugins/milkdrop_presets.c $(SRC_DIR)/plugins/milkdrop_render.c $(SRC_DIR)/plugins/milkdrop_controls.c $(SRC_DIR)/plugins/milkdrop_overlay.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(PLUGIN_CFLAGS) $(EPOXY_CFLAGS) -shared -o $@ $(filter %.c,$^) $(PLUGIN_LDFLAGS) $(EPOXY_LDFLAGS)

$(PLUGIN_DIR)/evemon_proc_meta_service_plugin.so: $(SRC_DIR)/plugins/proc_meta_service_plugin.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(PLUGIN_CFLAGS) -shared -o $@ $< $(PLUGIN_LDFLAGS) $(SQLITE_LDFLAGS)

$(PLUGIN_DIR)/evemon_write_monitor_ui_plugin.so: $(SRC_DIR)/plugins/write_monitor_ui_plugin.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(PLUGIN_CFLAGS) $(GSV_CFLAGS) -shared -o $@ $< $(PLUGIN_LDFLAGS) $(GSV_LDFLAGS)

define make_qt_inspector
$(QT_INSPECTOR_DIR)/$(1)_plugin.o: $(SRC_DIR)/plugins/$(1)_plugin.c
	@mkdir -p $$(@D)
	$(CC) $(QT_INSPECTOR_CFLAGS) -Devemon_plugin_init=evemon_qt_$(1)_init -c -o $$@ $$<
endef
$(foreach inspector,$(QT_INSPECTOR_NAMES),$(eval $(call make_qt_inspector,$(inspector))))

$(QT_INSPECTOR_DIR)/net_chart.o: $(SRC_DIR)/plugins/net_chart.c
	@mkdir -p $(@D)
	$(CC) $(QT_INSPECTOR_CFLAGS) -c -o $@ $<

$(QT_INSPECTOR_DIR)/pipewire_view.o: $(SRC_DIR)/plugins/pipewire_view.c
	@mkdir -p $(@D)
	$(CC) $(QT_INSPECTOR_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/qt-audio/$(SRC_DIR)/ui/pipewire_scan.o: QT_AUDIO_CFLAGS += -DEVEMON_NO_GTK

$(BUILD_DIR)/qt-audio/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(QT_AUDIO_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/qt/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(QT_FRONTEND_CXXFLAGS) -c -o $@ $<

$(QT_QRC): icon.png font-logos.ttf
	@mkdir -p $(@D)
	@{ echo '<RCC>'; echo '  <qresource prefix="/evemon">'; echo '    <file alias="icon.png">$(abspath icon.png)</file>'; echo '    <file alias="font-logos.ttf">$(abspath font-logos.ttf)</file>'; echo '  </qresource>'; echo '</RCC>'; } > $@

$(QT_RESOURCE_CPP): $(QT_QRC)
	$(QT_RCC) --name evemon -o $@ $<

$(QT_RESOURCE_O): $(QT_RESOURCE_CPP)
	@mkdir -p $(@D)
	$(CXX) $(QT_FRONTEND_CXXFLAGS) -c -o $@ $<

$(QT_MODULE): $(QT_CPP_OBJS) $(QT_INSPECTOR_OBJS) $(QT_INSPECTOR_EXTRA_OBJS) $(QT_AUDIO_OBJS) $(QT_RESOURCE_O) $(CORE_LIB)
	$(CXX) -shared $(HARDEN_LDFLAGS) -o $@ $(QT_CPP_OBJS) $(QT_INSPECTOR_OBJS) $(QT_INSPECTOR_EXTRA_OBJS) $(QT_AUDIO_OBJS) $(QT_RESOURCE_O) $(QT_LDFLAGS) $(QT_FRONTEND_EXTRA_LDFLAGS) $(GLIB_LDFLAGS) $(PANGO_LDFLAGS) $(SQLITE_LDFLAGS) $(PW_LDFLAGS) $(SOUP_LDFLAGS) -pthread -ldl -lm

$(BUILD_DIR)/qt-milkdrop/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(QT_MILKDROP_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/qt-milkdrop/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(QT_MILKDROP_CXXFLAGS) -c -o $@ $<

$(QT_MILKDROP): $(QT_MILKDROP_C_OBJS) $(QT_MILKDROP_CPP_OBJS) $(CORE_LIB)
	@mkdir -p $(@D)
	$(CXX) -shared $(HARDEN_LDFLAGS) -Wl,--as-needed,--gc-sections -o $@ $(QT_MILKDROP_CPP_OBJS) $(QT_MILKDROP_C_OBJS) $(QT_OPENGL_LDFLAGS) $(GLIB_LDFLAGS) $(PANGO_LDFLAGS) $(PW_LDFLAGS) $(EPOXY_LDFLAGS) -pthread -ldl -lm

$(BPF_OBJ): $(BPF_SRC)
	@mkdir -p $(@D)
	$(CLANG) $(BPF_CFLAGS) -c -o $@ $<

$(DEV_DATA_STAMP): $(SOFTWARE_DIRECTORY_DB) $(ANDROID_ARCHIVE) presets
	@mkdir -p $(@D)/android $(@D)/milkdrop
	@test -e $(@D)/software-directory.sqlite || test -L $(@D)/software-directory.sqlite || ln -s $(abspath $(SOFTWARE_DIRECTORY_DB)) $(@D)/software-directory.sqlite
	@test -e $(@D)/milkdrop/presets || test -L $(@D)/milkdrop/presets || ln -s $(abspath presets) $(@D)/milkdrop/presets
	@cmp -s $(ANDROID_ARCHIVE) $(@D)/android/evemon-arm64.tar.gz || cp -f $(ANDROID_ARCHIVE) $(@D)/android/evemon-arm64.tar.gz
	touch $@

nvtop:
	./scripts/fetch-nvtop.sh

clean:
	rm -rf "$(BUILD_DIR)"

run: all
	./$(TARGET)

# ── Debug and sanitizer builds ──────────────────────────────────
DEBUG_CFLAGS := $(subst -O2,-Og,$(CFLAGS))
DEBUG_CFLAGS := $(filter-out -D_FORTIFY_SOURCE=2,$(DEBUG_CFLAGS))
DEBUG_CXXFLAGS := $(subst -O2,-Og,$(CXXFLAGS))
DEBUG_CXXFLAGS := $(filter-out -D_FORTIFY_SOURCE=2,$(DEBUG_CXXFLAGS))
debug: CFLAGS := $(DEBUG_CFLAGS)
debug: CXXFLAGS := $(DEBUG_CXXFLAGS)
debug: all

gdb: debug
	gdb -q -ex run --args ./$(TARGET)

asan: CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan: CXXFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan: HARDEN_LDFLAGS += -fsanitize=address
asan: all

ubsan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
ubsan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
ubsan: HARDEN_LDFLAGS += -fsanitize=address,undefined
ubsan: all

# ── Packaging targets retained from the original build ─────────
DEB_VERSION ?= 0.2.0
DEB_TARGET ?= $(if $(target),$(target),debian12)
DEB_DEBUG ?= $(if $(filter 1 true yes,$(debug)),1,0)
DEB_DOCKERFILE_debian12 := packaging/debian12
DEB_DOCKERFILE_debian13 := packaging/debian13
DEB_DOCKERFILE_ubuntu2204 := packaging/ubuntu2204
DEB_DOCKERFILE_ubuntu2404 := packaging/ubuntu2404
DEB_IMAGE := evemon-builder-$(DEB_TARGET)
DEB_OUTDIR := $(CURDIR)/dist/$(DEB_TARGET)
RPM_VERSION ?= 0.2.0
RPM_RELEASE ?= 1
RPM_TARGET ?= $(if $(target),$(target),fedora42)
RPM_DEBUG ?= $(if $(filter 1 true yes,$(debug)),1,0)
RPM_DOCKERFILE_fedora42 := packaging/fedora42
RPM_DOCKERFILE_fedora41 := packaging/fedora41
RPM_IMAGE := evemon-builder-$(RPM_TARGET)
RPM_OUTDIR := $(CURDIR)/dist/$(RPM_TARGET)

deb:
	@DDIR="$(DEB_DOCKERFILE_$(DEB_TARGET))"; if [ -z "$$DDIR" ]; then echo "Unknown target '$(DEB_TARGET)'. Valid: debian12 debian13 ubuntu2204 ubuntu2404"; exit 1; fi; if [ ! -f "$$DDIR/Dockerfile" ]; then echo "Dockerfile not found: $$DDIR/Dockerfile"; exit 1; fi; mkdir -p "$(DEB_OUTDIR)"; docker build -t $(DEB_IMAGE) -f "$$DDIR/Dockerfile" .; docker run --rm -e VERSION=$(DEB_VERSION) -e DEBUG=$(DEB_DEBUG) -v "$(CURDIR):/src:ro" -v "$(DEB_OUTDIR):/out" $(DEB_IMAGE)

rpm:
	@DDIR="$(RPM_DOCKERFILE_$(RPM_TARGET))"; if [ -z "$$DDIR" ]; then echo "Unknown target '$(RPM_TARGET)'. Valid: fedora42 fedora41"; exit 1; fi; if [ ! -f "$$DDIR/Dockerfile" ]; then echo "Dockerfile not found: $$DDIR/Dockerfile"; exit 1; fi; mkdir -p "$(RPM_OUTDIR)"; docker build -t $(RPM_IMAGE) -f "$$DDIR/Dockerfile" .; docker run --rm -e VERSION=$(RPM_VERSION) -e RELEASE=$(RPM_RELEASE) -e DEBUG=$(RPM_DEBUG) -v "$(CURDIR):/src:ro" -v "$(RPM_OUTDIR):/out" $(RPM_IMAGE)

# ── Install ─────────────────────────────────────────────────────
install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/evemon
	install -m 755 $(GTK_MODULE) $(QT_MODULE) $(DESTDIR)$(LIBDIR)/
	install -d $(DESTDIR)$(LIBDIR)/plugins $(DESTDIR)$(LIBDIR)/plugins-qt
	rm -f $(DESTDIR)$(LIBDIR)/plugins/*.so $(DESTDIR)$(LIBDIR)/plugins-qt/*.so
	install -m 755 $(GTK_PLUGIN_SOS) $(DESTDIR)$(LIBDIR)/plugins/
	install -m 755 $(QT_MILKDROP) $(DESTDIR)$(LIBDIR)/plugins-qt/
	rm -f $(DESTDIR)$(LIBDIR)/libevemon_core.so*
	install -m 644 $(BPF_OBJ) $(DESTDIR)$(LIBDIR)/
	install -d $(DESTDIR)$(DATADIR)/applications
	install -m 644 evemon.desktop $(DESTDIR)$(DATADIR)/applications/evemon.desktop
	install -d $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps
	install -m 644 icon.png $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/evemon.png
	install -d $(DESTDIR)$(DATADIR)/pixmaps
	install -m 644 icon.png $(DESTDIR)$(DATADIR)/pixmaps/evemon.png
	install -d $(DESTDIR)$(MILKDROP_PRESETDIR) $(DESTDIR)$(EVEMON_DATADIR)/android
	install -m 644 presets/*.milk $(DESTDIR)$(MILKDROP_PRESETDIR)/
	install -m 644 $(SOFTWARE_DIRECTORY_DB) $(DESTDIR)$(EVEMON_DATADIR)/software-directory.sqlite
	install -m 644 $(ANDROID_ARCHIVE) $(DESTDIR)$(EVEMON_DATADIR)/android/evemon-arm64.tar.gz
ifeq ($(NVTOP_AVAILABLE),1)
	install -d $(DESTDIR)$(EVEMON_DATADIR)/licenses
	install -m 644 $(NVTOP_DIR)/COPYING $(DESTDIR)$(EVEMON_DATADIR)/licenses/nvtop-COPYING
endif
	$(if $(DESTDIR),,update-desktop-database $(DATADIR)/applications)
	$(if $(DESTDIR),,-gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor 2>/dev/null || true)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/evemon
	rm -f $(DESTDIR)$(DATADIR)/applications/evemon.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/evemon.png
	rm -f $(DESTDIR)$(DATADIR)/pixmaps/evemon.png
	rm -rf $(DESTDIR)$(LIBDIR)
	rm -rf $(DESTDIR)$(EVEMON_DATADIR)
	-gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null || true
