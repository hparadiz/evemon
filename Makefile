BUILD_DIR ?= build
.DEFAULT_GOAL := all
CONFIGURED_BUILD := $(and $(wildcard $(BUILD_DIR)/CMakeCache.txt),$(wildcard $(BUILD_DIR)/CMakeFiles/Makefile.cmake $(BUILD_DIR)/build.ninja))
USE_CMAKE_BUILD := $(strip $(CONFIGURE_FLAGS)$(CONFIGURED_BUILD))

# Configuration is optional. Once requested, use that build tree rather than
# silently ignoring its selected frontends, features and install paths.
.PHONY: configure nvtop
configure:
	./configure --build-dir="$(BUILD_DIR)" $(CONFIGURE_FLAGS)

nvtop:
	./scripts/fetch-nvtop.sh

ifneq ($(USE_CMAKE_BUILD),)
.PHONY: all gtk qt plugins install clean run debug gdb asan ubsan package
ifneq ($(strip $(CONFIGURE_FLAGS)),)
all debug asan ubsan: configure
endif

all:
	+cmake --build "$(BUILD_DIR)"
	@cat "$(BUILD_DIR)/build-summary.txt"

gtk qt plugins: all

install: all
	cmake --install "$(BUILD_DIR)"

clean:
	@if test -f "$(BUILD_DIR)/CMakeCache.txt"; then cmake --build "$(BUILD_DIR)" --target clean; fi

run: all
	"$(BUILD_DIR)/evemon"

debug:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug
	+cmake --build "$(BUILD_DIR)"

gdb: debug
	gdb -q -ex run --args "$(BUILD_DIR)/evemon"

asan: CMAKE_SANITIZERS := address
ubsan: CMAKE_SANITIZERS := address,undefined
asan ubsan:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-fsanitize=$(CMAKE_SANITIZERS) -fno-omit-frame-pointer" \
		-DCMAKE_CXX_FLAGS="-fsanitize=$(CMAKE_SANITIZERS) -fno-omit-frame-pointer"
	+cmake --build "$(BUILD_DIR)"

package: all
	+cmake --build "$(BUILD_DIR)" --target package

else

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
LIBDIR = $(PREFIX)/lib/evemon
DATADIR = $(PREFIX)/share
EVEMON_DATADIR = $(DATADIR)/evemon
MILKDROP_PRESETDIR = $(EVEMON_DATADIR)/milkdrop/presets
SOFTWARE_DIRECTORY_DB = data/software-directory.sqlite

ifeq ($(origin CC),default)
CC := cc
endif
ifeq ($(origin CXX),default)
CXX := c++
endif
CC ?= cc
CXX ?= c++
CLANG ?= clang
PKG_CONFIG ?= pkg-config
GLIB_COMPILE_RESOURCES ?= glib-compile-resources
CFLAGS := -Wall -Wextra -std=c11 -O2 -g -pthread -D_GNU_SOURCE -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fno-strict-aliasing -fno-strict-overflow
CXXFLAGS := -Wall -Wextra -std=c++17 -O2 -g -pthread -D_GNU_SOURCE -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fno-strict-aliasing -fno-strict-overflow
HARDEN_LDFLAGS := -Wl,-z,relro,-z,now

SRC_DIR := src
VERSION := 0.2.0

# Toolkit and library flags
GLIB_CFLAGS := $(shell $(PKG_CONFIG) --cflags glib-2.0 gio-2.0 2>/dev/null)
GLIB_LDFLAGS := $(shell $(PKG_CONFIG) --libs glib-2.0 gio-2.0 2>/dev/null)
BPF_LDFLAGS := $(shell $(PKG_CONFIG) --libs libbpf libelf 2>/dev/null)
JANSSON_CFLAGS := $(shell $(PKG_CONFIG) --cflags jansson 2>/dev/null)
JANSSON_LDFLAGS := $(shell $(PKG_CONFIG) --libs jansson 2>/dev/null)
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 fontconfig x11 2>/dev/null)
GTK_LDFLAGS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 fontconfig x11 2>/dev/null)
GTK_WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags gdk-wayland-3.0 wayland-client 2>/dev/null)
GTK_WAYLAND_LDFLAGS := $(shell $(PKG_CONFIG) --libs gdk-wayland-3.0 wayland-client 2>/dev/null)
WAYLAND_SCANNER := $(shell command -v wayland-scanner 2>/dev/null)
BACKGROUND_EFFECT_PROTOCOL := $(firstword $(wildcard /usr/share/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml /usr/local/share/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml))
PANGO_CFLAGS := $(shell $(PKG_CONFIG) --cflags pangocairo gdk-pixbuf-2.0 2>/dev/null)
PANGO_LDFLAGS := $(shell $(PKG_CONFIG) --libs pangocairo gdk-pixbuf-2.0 2>/dev/null)
SQLITE_LDFLAGS := $(shell $(PKG_CONFIG) --libs sqlite3 2>/dev/null)
GSV_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtksourceview-4 2>/dev/null)
GSV_LDFLAGS := $(shell $(PKG_CONFIG) --libs gtksourceview-4 2>/dev/null)
SOUP_CFLAGS := $(shell $(PKG_CONFIG) --cflags libsoup-3.0 2>/dev/null)
SOUP_LDFLAGS := $(shell $(PKG_CONFIG) --libs libsoup-3.0 2>/dev/null)
EPOXY_CFLAGS := $(shell $(PKG_CONFIG) --cflags epoxy 2>/dev/null)
EPOXY_LDFLAGS := $(shell $(PKG_CONFIG) --libs epoxy 2>/dev/null)
QT_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt6Core Qt6Gui Qt6Widgets 2>/dev/null)
QT_LDFLAGS := $(shell $(PKG_CONFIG) --libs Qt6Core Qt6Gui Qt6Widgets 2>/dev/null)
KWINDOWSYSTEM_CFLAGS := $(shell $(PKG_CONFIG) --cflags KF6WindowSystem 2>/dev/null)
KWINDOWSYSTEM_LDFLAGS := $(shell $(PKG_CONFIG) --libs KF6WindowSystem 2>/dev/null)
QT_OPENGL_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt6Widgets Qt6OpenGLWidgets Qt6OpenGL 2>/dev/null)
QT_OPENGL_LDFLAGS := $(shell $(PKG_CONFIG) --libs Qt6Widgets Qt6OpenGLWidgets Qt6OpenGL 2>/dev/null)
QT_LIBEXECDIR := $(shell $(PKG_CONFIG) --variable=libexecdir Qt6Core 2>/dev/null)
QT_RCC ?= $(QT_LIBEXECDIR)/rcc

# Missing optional dependencies select a smaller build. These are detection
# checks only; errors from an actual compile or link are never suppressed.
missing_packages = $(strip $(foreach package,$(1),$(if $(shell $(PKG_CONFIG) --exists $(package) 2>/dev/null && echo yes),,$(package))))
missing_tool = $(if $(shell command -v $(firstword $(1)) 2>/dev/null),,$(firstword $(1)))
CORE_MISSING := $(strip $(call missing_tool,$(CC)) $(call missing_tool,$(AR)) $(call missing_tool,$(PKG_CONFIG)) $(call missing_packages,gio-2.0 libbpf libelf jansson))
CORE_ENABLED := $(if $(CORE_MISSING),0,1)
GUI_ASSETS_MISSING := $(foreach asset,icon.png font-logos.ttf,$(if $(wildcard $(asset)),,$(asset)))
GTK_MISSING := $(strip $(call missing_packages,gtk+-3.0 fontconfig x11 pangocairo gdk-pixbuf-2.0) $(call missing_tool,$(GLIB_COMPILE_RESOURCES)) $(GUI_ASSETS_MISSING))
GTK_ENABLED := $(if $(GTK_MISSING),0,1)
QT_MISSING := $(strip $(call missing_packages,Qt6Core Qt6Gui Qt6Widgets pangocairo gdk-pixbuf-2.0 sqlite3) $(call missing_tool,$(CXX)) $(if $(call missing_tool,$(QT_RCC)),Qt6-rcc) $(GUI_ASSETS_MISSING))
ifeq ($(QT_MISSING),)
QT_MISSING := $(if $(shell $(PKG_CONFIG) --atleast-version=6.4 Qt6Core 2>/dev/null && echo yes),,Qt6Core>=6.4)
endif
QT_ENABLED := $(if $(QT_MISSING),0,1)
# KDE ships CMake metadata but no pkg-config file for SyntaxHighlighting.
# Probe this optional library without making CMake a native-build dependency.
QT_SYNTAX_CFLAGS ?= -I$(shell $(PKG_CONFIG) --variable=prefix Qt6Core 2>/dev/null)/include/KF6/KSyntaxHighlighting
QT_SYNTAX_LDFLAGS ?= -lKF6SyntaxHighlighting
QT_SYNTAX_ENABLED := 0
ifeq ($(QT_ENABLED),1)
ifneq ($(HAVE_QT_SYNTAX),0)
QT_SYNTAX_ENABLED := $(shell printf '%s\n' '#include <KSyntaxHighlighting/Repository>' 'int main() { KSyntaxHighlighting::Repository repository; }' | $(CXX) -std=c++17 -fPIC $(QT_CFLAGS) $(QT_SYNTAX_CFLAGS) -x c++ - -x none $(QT_LDFLAGS) $(QT_SYNTAX_LDFLAGS) -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0)
endif
endif
UI_ENABLED := $(if $(filter 1,$(GTK_ENABLED) $(QT_ENABLED)),1,0)
DEFAULT_UI := $(if $(filter 1,$(GTK_ENABLED)),gtk,$(if $(filter 1,$(QT_ENABLED)),qt,headless))
GSV_MISSING := $(call missing_packages,gtksourceview-4)
SQLITE_MISSING := $(call missing_packages,sqlite3)

# PipeWire is optional and can be disabled explicitly with HAVE_PIPEWIRE=0.
PIPEWIRE_MISSING := $(call missing_packages,libpipewire-0.3 libsoup-3.0)
PIPEWIRE_ENABLED := 0
ifneq ($(HAVE_PIPEWIRE),0)
ifeq ($(PIPEWIRE_MISSING),)
ifeq ($(UI_ENABLED),1)
PW_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PW_LDFLAGS := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)
PIPEWIRE_ENABLED := 1
endif
endif
endif
MILKDROP_MISSING := $(call missing_packages,epoxy)
GTK_MILKDROP_ENABLED := $(if $(filter 1,$(GTK_ENABLED)),$(if $(filter 1,$(PIPEWIRE_ENABLED)),$(if $(MILKDROP_MISSING),0,1),0),0)
QT_MILKDROP_MISSING := $(call missing_packages,epoxy Qt6OpenGL Qt6OpenGLWidgets)
QT_MILKDROP_ENABLED := $(if $(filter 1,$(QT_ENABLED)),$(if $(filter 1,$(PIPEWIRE_ENABLED)),$(if $(QT_MILKDROP_MISSING),0,1),0),0)
MILKDROP_ENABLED := $(if $(filter 1,$(GTK_MILKDROP_ENABLED) $(QT_MILKDROP_ENABLED)),1,0)

# Build every available backend by default; CI also isolates individual vendors.
GPU_VENDOR ?= all
NVTOP_DIR := third_party/nvtop
NVTOP_INCLUDE := $(NVTOP_DIR)/include
NVTOP_SRC_DIR := $(NVTOP_DIR)/src
GPU_PACKAGES := $(if $(filter nvidia,$(GPU_VENDOR)),,libdrm libudev)
GPU_MISSING := $(strip $(if $(wildcard $(NVTOP_SRC_DIR)/extract_gpuinfo.c),,third_party/nvtop) $(call missing_packages,$(GPU_PACKAGES)))
NVTOP_AVAILABLE := $(if $(GPU_MISSING),0,1)
AMDGPU_AVAILABLE := 0
ifeq ($(NVTOP_AVAILABLE),1)
ifneq ($(GPU_VENDOR),nvidia)
NVTOP_DRM_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null)
NVTOP_DRM_LDFLAGS := $(shell $(PKG_CONFIG) --libs libdrm 2>/dev/null)
NVTOP_UDEV_CFLAGS := $(shell $(PKG_CONFIG) --cflags libudev 2>/dev/null)
NVTOP_UDEV_LDFLAGS := $(shell $(PKG_CONFIG) --libs libudev 2>/dev/null)
endif
ifneq ($(filter all amd,$(GPU_VENDOR)),)
AMDGPU_AVAILABLE := $(shell printf '%s\n' '#include <libdrm/amdgpu.h>' '#include <libdrm/amdgpu_drm.h>' | $(CC) $(NVTOP_DRM_CFLAGS) -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)
endif
endif
NVTOP_SRCS := $(SRC_DIR)/plugins/gpu_nvtop_adapter.c $(NVTOP_SRC_DIR)/extract_gpuinfo.c $(NVTOP_SRC_DIR)/time.c $(NVTOP_SRC_DIR)/get_process_info_linux.c $(NVTOP_SRC_DIR)/extract_processinfo_fdinfo.c
ifeq ($(GPU_VENDOR),all)
NVTOP_BACKENDS := nvidia ascend intel intel_i915 intel_xe msm msm_utils panfrost panfrost_utils panthor panthor_utils mali_common v3d v3d_utils tpu metax
NVTOP_SRCS += $(SRC_DIR)/plugins/gpu_dcmi_shim.c $(NVTOP_SRC_DIR)/extract_npuinfo_rockchip.c $(NVTOP_SRC_DIR)/extract_gcuinfo_enflame.c
else ifeq ($(GPU_VENDOR),nvidia)
NVTOP_BACKENDS := nvidia
else ifeq ($(GPU_VENDOR),intel)
NVTOP_BACKENDS := intel intel_i915 intel_xe
else ifeq ($(GPU_VENDOR),amd)
NVTOP_BACKENDS :=
ifeq ($(NVTOP_AVAILABLE):$(AMDGPU_AVAILABLE),1:0)
$(error GPU_VENDOR=amd requires AMD libdrm headers)
endif
else
$(error GPU_VENDOR must be all, nvidia, intel or amd)
endif
ifneq ($(GPU_VENDOR),nvidia)
NVTOP_SRCS += $(NVTOP_SRC_DIR)/device_discovery_linux.c
endif
ifeq ($(AMDGPU_AVAILABLE),1)
NVTOP_BACKENDS += amdgpu amdgpu_utils
endif
NVTOP_SRCS += $(addprefix $(NVTOP_SRC_DIR)/extract_gpuinfo_,$(addsuffix .c,$(NVTOP_BACKENDS)))

WITH_ANDROID ?= auto
ANDROID_ARCHIVE ?= data/android-arm64/evemon-arm64.tar.gz
ANDROID_ENABLED := $(if $(filter auto,$(WITH_ANDROID)),$(if $(wildcard $(ANDROID_ARCHIVE)),1,0),$(WITH_ANDROID))
ifeq ($(filter $(ANDROID_ENABLED),0 1),)
$(error WITH_ANDROID must be auto, 0 or 1)
endif
ANDROID_DEP :=
ANDROID_SHA256 :=
ifeq ($(ANDROID_ENABLED),1)
ANDROID_DEP := $(ANDROID_ARCHIVE)
ANDROID_SHA256 := $(shell sha256sum "$(ANDROID_ARCHIVE)" 2>/dev/null | cut -d' ' -f1)
endif
BUILD_CONFIG := $(BUILD_DIR)/build-config

EBPF_MISSING := $(call missing_tool,$(CLANG))
ifeq ($(EBPF_MISSING),)
EBPF_MISSING := $(if $(shell $(CLANG) -print-targets 2>/dev/null | grep -q '^[[:space:]]*bpf[[:space:]]' && echo yes),,Clang-BPF-target)
endif
EBPF_ENABLED := $(if $(EBPF_MISSING),0,1)

FEATURE_DEFINES := -DEVEMON_VERSION='"$(VERSION)"' -DEVEMON_LIBDIR='"$(LIBDIR)"' -DEVEMON_DATADIR='"$(EVEMON_DATADIR)"' -DEVEMON_DEFAULT_UI='"$(DEFAULT_UI)"' -DEVEMON_HAVE_GTK=$(GTK_ENABLED) -DEVEMON_HAVE_QT=$(QT_ENABLED) -DEVEMON_WITH_PIPEWIRE=$(PIPEWIRE_ENABLED) -DEVEMON_WITH_MILKDROP=$(MILKDROP_ENABLED) -DEVEMON_WITH_GPU=$(NVTOP_AVAILABLE) -DEVEMON_WITH_EBPF=$(EBPF_ENABLED)
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
CORE_SRCS := $(SRC_DIR)/core/log.c $(SRC_DIR)/core/monitor.c $(SRC_DIR)/core/steam.c $(SRC_DIR)/core/profile.c $(SRC_DIR)/platform/fdmon.c $(SRC_DIR)/platform/fdmon_ebpf.c $(SRC_DIR)/plugins/event_bus.c $(SRC_DIR)/plugins/plugin_broker.c $(SRC_DIR)/plugins/plugin_loader.c $(SRC_DIR)/core/settings.c $(SRC_DIR)/media/mpris.c $(SRC_DIR)/core/format.c $(SRC_DIR)/app/runtime.c $(SRC_DIR)/ui/devices.c $(SRC_DIR)/platform/desktop_session.c $(SRC_DIR)/platform/compatibility.c $(SRC_DIR)/ipc/ipc_wire.c $(SRC_DIR)/ipc/query_client.c $(SRC_DIR)/ipc/query_server.c $(SRC_DIR)/ipc/query_snapshot.c $(SRC_DIR)/ipc/query_inspect.c $(SRC_DIR)/ipc/query_events.c $(SRC_DIR)/ipc/query_fdmon.c $(SRC_DIR)/ipc/query_gpu.c $(SRC_DIR)/ipc/query_host.c $(SRC_DIR)/ipc/query_about.c $(SRC_DIR)/ipc/query_dpkg.c $(SRC_DIR)/plugins/dpkg_lookup.c $(SRC_DIR)/app/client_session.c $(SRC_DIR)/remote/ssh_tunnel.c $(SRC_DIR)/remote/adb_tunnel.c
ifeq ($(NVTOP_AVAILABLE),1)
CORE_SRCS += $(NVTOP_SRCS)
endif
CORE_OBJS := $(patsubst %.c,$(BUILD_DIR)/core/%.o,$(CORE_SRCS))
CORE_LIB := $(BUILD_DIR)/libevemon_core.a
CORE_CFLAGS = $(CFLAGS) -fPIC -I$(SRC_DIR) $(GLIB_CFLAGS) $(JANSSON_CFLAGS) $(NVTOP_DRM_CFLAGS) $(NVTOP_UDEV_CFLAGS) -I$(NVTOP_INCLUDE) -I$(NVTOP_SRC_DIR) -DEVEMON_NO_GTK -DUSING_LIBUDEV $(CORE_FEATURE_DEFINES)
GPU_CORE_CFLAGS = -I$(SRC_DIR)/plugins/nvtop_compat $(filter-out -std=c11,$(CORE_CFLAGS)) -std=gnu11
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
GTK_PLUGIN_NAMES := fd env devices mmap libs threads cgroup system_fd system_libs net gentoo dpkg write_monitor
ifeq ($(SQLITE_MISSING),)
GTK_PLUGIN_NAMES += proc_meta_service
endif
ifeq ($(GSV_MISSING),)
GTK_PLUGIN_NAMES += write_monitor_ui
endif
ifeq ($(PIPEWIRE_ENABLED),1)
GTK_PLUGIN_NAMES += pipewire audio_service
endif
ifeq ($(GTK_MILKDROP_ENABLED),1)
GTK_PLUGIN_NAMES += milkdrop
endif
ifeq ($(NVTOP_AVAILABLE),1)
GTK_PLUGIN_NAMES += gpu
endif
GTK_ALL_PLUGIN_NAMES := fd env devices mmap libs threads cgroup system_fd system_libs net gentoo dpkg write_monitor proc_meta_service write_monitor_ui pipewire audio_service milkdrop gpu
ifneq ($(GTK_ENABLED),1)
GTK_PLUGIN_NAMES :=
endif
GTK_PLUGIN_SOS := $(addprefix $(PLUGIN_DIR)/evemon_,$(addsuffix _plugin.so,$(GTK_PLUGIN_NAMES)))
PLUGIN_CFLAGS = $(CFLAGS) -fPIC -I$(SRC_DIR) $(GTK_CFLAGS) $(PANGO_CFLAGS) $(FEATURE_DEFINES)
PLUGIN_LDFLAGS := $(GTK_LDFLAGS) $(PANGO_LDFLAGS) $(HARDEN_LDFLAGS) -pthread -ldl -lm

# ── Qt frontend and inspectors ──────────────────────────────────
QT_CPP_SRCS := $(SRC_DIR)/qt/main.cpp $(SRC_DIR)/qt/process_model.cpp $(SRC_DIR)/qt/main_window.cpp $(SRC_DIR)/qt/main_menus.cpp $(SRC_DIR)/qt/remote_dialog.cpp $(SRC_DIR)/qt/theme.cpp $(SRC_DIR)/qt/scrollbars.cpp $(SRC_DIR)/qt/about_dialog.cpp $(SRC_DIR)/qt/compatibility_dialog.cpp $(SRC_DIR)/qt/inspector.cpp $(SRC_DIR)/qt/widget_bridge.cpp $(SRC_DIR)/qt/detail_panel.cpp $(SRC_DIR)/qt/write_view.cpp
QT_CPP_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/qt/%.o,$(QT_CPP_SRCS))
QT_INSPECTOR_NAMES := fd env devices mmap libs threads cgroup system_fd system_libs net gentoo dpkg
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
QT_MILKDROP :=
ifeq ($(QT_MILKDROP_ENABLED),1)
QT_MILKDROP := $(QT_PLUGIN_DIR)/libevemon_qt_milkdrop.so
endif
QT_FRONTEND_DEFINES := -DEVEMON_NO_GTK -DEVEMON_QT_PLUGIN_DIR='"$(LIBDIR)/plugins-qt"'
ifeq ($(QT_MILKDROP_ENABLED),1)
QT_FRONTEND_DEFINES += -DHAVE_MILKDROP
endif
QT_FRONTEND_EXTRA_CFLAGS :=
QT_FRONTEND_EXTRA_LDFLAGS :=
ifeq ($(QT_SYNTAX_ENABLED),1)
QT_FRONTEND_EXTRA_CFLAGS += $(QT_SYNTAX_CFLAGS) -DHAVE_QT_SYNTAX
QT_FRONTEND_EXTRA_LDFLAGS += $(QT_SYNTAX_LDFLAGS)
endif
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
BPF_MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null)
BPF_CFLAGS := -O2 -target bpf -g -D__TARGET_ARCH_$(BPF_ARCH)
ifneq ($(wildcard /usr/include/$(BPF_MULTIARCH)/asm/types.h),)
BPF_CFLAGS += -I/usr/include/$(BPF_MULTIARCH)
endif
DEV_DATA_STAMP := $(BUILD_DIR)/share/evemon/.stamp

.PHONY: all gtk qt plugins clean run debug gdb asan ubsan install uninstall require-android-archive FORCE

GTK_TARGETS := $(if $(filter 1,$(GTK_ENABLED)),$(GTK_MODULE) $(GTK_PLUGIN_SOS))
QT_TARGETS := $(if $(filter 1,$(QT_ENABLED)),$(QT_MODULE) $(QT_MILKDROP))
BUILD_OUTPUTS := $(TARGET) $(GTK_TARGETS) $(QT_TARGETS) $(if $(filter 1,$(EBPF_ENABLED)),$(BPF_OBJ)) $(if $(filter 1,$(UI_ENABLED)),$(DEV_DATA_STAMP))

# Print the report after every selected output finishes, including under -j.
all: $(if $(filter 1,$(CORE_ENABLED)),require-android-archive $(BUILD_OUTPUTS))
ifeq ($(CORE_ENABLED),1)
	@rm -f $(addprefix $(PLUGIN_DIR)/evemon_,$(addsuffix _plugin.so,$(filter-out $(GTK_PLUGIN_NAMES),$(GTK_ALL_PLUGIN_NAMES))))
ifneq ($(GTK_ENABLED),1)
	@rm -f $(GTK_MODULE)
endif
ifneq ($(QT_ENABLED),1)
	@rm -f $(QT_MODULE)
endif
ifneq ($(QT_MILKDROP_ENABLED),1)
	@rm -f $(QT_PLUGIN_DIR)/libevemon_qt_milkdrop.so
endif
ifneq ($(EBPF_ENABLED),1)
	@rm -f $(BPF_OBJ)
endif
	@printf '\nEvemon build summary (built or up to date)\n'
	@printf '%s\n' '  Executable: $(TARGET) (headless support included)' '  Default interface: $(DEFAULT_UI)'
	@printf '%s\n' '  $(if $(filter 1,$(GTK_ENABLED)),GTK frontend: $(GTK_MODULE),WARNING: GTK frontend skipped; missing: $(GTK_MISSING))'
	@printf '%s\n' '  $(if $(filter 1,$(QT_ENABLED)),Qt frontend: $(QT_MODULE),WARNING: Qt frontend skipped; missing: $(QT_MISSING))'
ifeq ($(GTK_ENABLED),1)
	@printf '%s\n' '  GTK plugins: $(GTK_PLUGIN_NAMES)'
ifneq ($(GSV_MISSING),)
	@printf '%s\n' '  WARNING: GTK Write Log skipped; missing: $(GSV_MISSING)'
endif
ifneq ($(SQLITE_MISSING),)
	@printf '%s\n' '  WARNING: GTK software metadata plugin skipped; missing: $(SQLITE_MISSING)'
endif
endif
ifeq ($(QT_ENABLED),1)
	@printf '%s\n' '  Qt inspectors: $(QT_INSPECTOR_NAMES)'
	@printf '%s\n' '  Qt Write Log: native code view with built-in log highlighting'
ifeq ($(QT_SYNTAX_ENABLED),1)
	@printf '%s\n' '  Qt code-language highlighting: KF6SyntaxHighlighting'
else
	@printf '%s\n' '  WARNING: extra Qt code languages unavailable; $(if $(filter 0,$(HAVE_QT_SYNTAX)),disabled by HAVE_QT_SYNTAX=0,optional KF6SyntaxHighlighting was not found or could not link); Write Log remains available'
endif
endif
	@printf '%s\n' '  $(if $(filter 1,$(EBPF_ENABLED)),eBPF kernel program: $(BPF_OBJ),WARNING: eBPF tracing skipped; missing: $(EBPF_MISSING); fanotify fallback remains available)'
	@printf '%s\n' '  $(if $(filter 1,$(PIPEWIRE_ENABLED)),PipeWire audio: enabled,WARNING: PipeWire audio skipped; $(if $(filter 0,$(HAVE_PIPEWIRE)),disabled by HAVE_PIPEWIRE=0,$(if $(PIPEWIRE_MISSING),missing: $(PIPEWIRE_MISSING),no frontend available)))'
	@printf '%s\n' '  $(if $(filter 1,$(GTK_MILKDROP_ENABLED)),GTK MilkDrop: enabled,WARNING: GTK MilkDrop skipped; $(if $(filter 0,$(GTK_ENABLED)),GTK unavailable,$(if $(filter 0,$(PIPEWIRE_ENABLED)),audio unavailable,missing: $(MILKDROP_MISSING))))'
	@printf '%s\n' '  $(if $(filter 1,$(QT_MILKDROP_ENABLED)),Qt MilkDrop: enabled,WARNING: Qt MilkDrop skipped; $(if $(filter 0,$(QT_ENABLED)),Qt unavailable,$(if $(filter 0,$(PIPEWIRE_ENABLED)),audio unavailable,missing: $(QT_MILKDROP_MISSING))))'
	@printf '%s\n' '  $(if $(filter 1,$(NVTOP_AVAILABLE)),GPU backends: enabled,WARNING: GPU backends skipped; missing: $(GPU_MISSING)$(if $(wildcard $(NVTOP_SRC_DIR)/extract_gpuinfo.c),, (fetch sources with make nvtop)))'
ifneq ($(filter all amd,$(GPU_VENDOR)),)
ifeq ($(NVTOP_AVAILABLE):$(AMDGPU_AVAILABLE),1:0)
	@printf '%s\n' '  WARNING: AMDGPU backend skipped; AMD libdrm headers unavailable; other GPU backends remain enabled'
endif
endif
	@printf '%s\n' '  $(if $(filter 1,$(UI_ENABLED)),$(if $(filter 1,$(ANDROID_ENABLED)),Android runtime: bundled,WARNING: Android runtime omitted; $(if $(filter 0,$(WITH_ANDROID)),disabled by WITH_ANDROID=0,missing: $(ANDROID_ARCHIVE))),Android runtime: omitted for headless build)'
ifeq ($(wildcard $(SOFTWARE_DIRECTORY_DB)),)
	@printf '%s\n' '  WARNING: software metadata database unavailable: $(SOFTWARE_DIRECTORY_DB)'
endif
ifeq ($(wildcard presets/*.milk),)
	@printf '%s\n' '  WARNING: MilkDrop presets unavailable: presets/*.milk'
endif
	@printf '%s\n' '  Dependency instructions: docs/building.md'
else
	@printf '\nWARNING: NOTHING BUILT. Missing core build dependencies: %s\n' '$(CORE_MISSING)'
	@printf '%s\n' 'Existing build artifacts were not rebuilt. Install the missing dependencies and rerun make.' 'See docs/building.md for package names.'
endif

gtk: $(if $(filter 1,$(CORE_ENABLED)),$(GTK_TARGETS))

qt: $(if $(filter 1,$(CORE_ENABLED)),$(QT_TARGETS))

plugins: $(if $(filter 1,$(CORE_ENABLED)),$(GTK_PLUGIN_SOS))

require-android-archive:
ifeq ($(ANDROID_ENABLED),1)
	@test -r "$(ANDROID_ARCHIVE)" || { echo "Missing Android runtime archive: $(ANDROID_ARCHIVE) (use WITH_ANDROID=0 to omit it)" >&2; exit 1; }
endif

# Dependency changes must also change compiler feature defines in existing
# objects. A content stamp keeps unchanged builds incremental.
$(BUILD_CONFIG): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' 'ui=$(GTK_ENABLED):$(QT_ENABLED)' 'qt-syntax=$(QT_SYNTAX_ENABLED)' 'audio=$(PIPEWIRE_ENABLED)' 'milkdrop=$(GTK_MILKDROP_ENABLED):$(QT_MILKDROP_ENABLED)' 'gpu=$(NVTOP_AVAILABLE)' 'gpu-vendor=$(GPU_VENDOR)' 'amdgpu=$(AMDGPU_AVAILABLE)' 'ebpf=$(EBPF_ENABLED)' 'android=$(ANDROID_ENABLED):$(ANDROID_SHA256)' 'plugins=$(GTK_PLUGIN_NAMES)' > $@.tmp
	@cmp -s $@.tmp $@ || mv -f $@.tmp $@
	@rm -f $@.tmp

$(CORE_OBJS) $(APP_OBJS) $(GTK_UI_OBJS) $(GRES_O) $(QT_CPP_OBJS) $(QT_INSPECTOR_OBJS) $(QT_INSPECTOR_EXTRA_OBJS) $(QT_AUDIO_OBJS) $(QT_RESOURCE_O) $(QT_MILKDROP_C_OBJS) $(QT_MILKDROP_CPP_OBJS): $(BUILD_CONFIG)

$(BUILD_DIR)/core/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CORE_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/core/src/remote/adb_tunnel.o: $(ANDROID_DEP)

$(BUILD_DIR)/core/third_party/nvtop/%.o: third_party/nvtop/%.c
	@mkdir -p $(@D)
	$(CC) $(GPU_CORE_CFLAGS) -c -o $@ $<

$(filter $(BUILD_DIR)/core/third_party/nvtop/%,$(CORE_OBJS)): $(SRC_DIR)/plugins/nvtop_compat/nvtop/interface_internal_common.h

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
	$(GLIB_COMPILE_RESOURCES) --sourcedir=$(SRC_DIR) --generate-source --target=$@ $<

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

$(PLUGIN_DIR)/evemon_write_monitor_plugin.so: $(SRC_DIR)/plugins/write_monitor_plugin.c $(CORE_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fPIC -I$(SRC_DIR) $(GLIB_CFLAGS) -shared -o $@ $< $(GLIB_LDFLAGS) $(HARDEN_LDFLAGS)

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

ifeq ($(QT_MILKDROP_ENABLED),1)
$(QT_MILKDROP): $(QT_MILKDROP_C_OBJS) $(QT_MILKDROP_CPP_OBJS) $(CORE_LIB)
	@mkdir -p $(@D)
	$(CXX) -shared $(HARDEN_LDFLAGS) -Wl,--as-needed,--gc-sections -o $@ $(QT_MILKDROP_CPP_OBJS) $(QT_MILKDROP_C_OBJS) $(QT_OPENGL_LDFLAGS) $(GLIB_LDFLAGS) $(PANGO_LDFLAGS) $(PW_LDFLAGS) $(EPOXY_LDFLAGS) -pthread -ldl -lm
endif

$(BPF_OBJ): $(BPF_SRC)
	@mkdir -p $(@D)
	$(CLANG) $(BPF_CFLAGS) -c -o $@ $<

$(DEV_DATA_STAMP): $(wildcard $(SOFTWARE_DIRECTORY_DB) presets) $(ANDROID_DEP) $(BUILD_CONFIG)
	@mkdir -p $(@D)/milkdrop
ifneq ($(wildcard $(SOFTWARE_DIRECTORY_DB)),)
	@test -e $(@D)/software-directory.sqlite || test -L $(@D)/software-directory.sqlite || ln -s $(abspath $(SOFTWARE_DIRECTORY_DB)) $(@D)/software-directory.sqlite
endif
ifneq ($(wildcard presets),)
	@test -e $(@D)/milkdrop/presets || test -L $(@D)/milkdrop/presets || ln -s $(abspath presets) $(@D)/milkdrop/presets
endif
ifeq ($(ANDROID_ENABLED),1)
	@mkdir -p $(@D)/android
	@cmp -s $(ANDROID_ARCHIVE) $(@D)/android/evemon-arm64.tar.gz || cp -f $(ANDROID_ARCHIVE) $(@D)/android/evemon-arm64.tar.gz
else
	@rm -f $(@D)/android/evemon-arm64.tar.gz
endif
	touch $@

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

endif

# ── Packaging targets retained from the original build ─────────
.PHONY: deb rpm
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
ifeq ($(USE_CMAKE_BUILD),)
install: all
ifeq ($(CORE_ENABLED),1)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/evemon
ifeq ($(GTK_ENABLED),1)
	install -m 755 $(GTK_MODULE) $(DESTDIR)$(LIBDIR)/
	install -d $(DESTDIR)$(LIBDIR)/plugins $(DESTDIR)$(LIBDIR)/plugins-qt
	rm -f $(DESTDIR)$(LIBDIR)/plugins/*.so $(DESTDIR)$(LIBDIR)/plugins-qt/*.so
	install -m 755 $(GTK_PLUGIN_SOS) $(DESTDIR)$(LIBDIR)/plugins/
endif
ifeq ($(QT_ENABLED),1)
	install -m 755 $(QT_MODULE) $(DESTDIR)$(LIBDIR)/
endif
ifeq ($(QT_MILKDROP_ENABLED),1)
	install -d $(DESTDIR)$(LIBDIR)/plugins-qt
	install -m 755 $(QT_MILKDROP) $(DESTDIR)$(LIBDIR)/plugins-qt/
endif
	rm -f $(DESTDIR)$(LIBDIR)/libevemon_core.so*
ifeq ($(EBPF_ENABLED),1)
	install -m 644 $(BPF_OBJ) $(DESTDIR)$(LIBDIR)/
endif
ifeq ($(UI_ENABLED),1)
	install -d $(DESTDIR)$(DATADIR)/applications
	install -m 644 evemon.desktop $(DESTDIR)$(DATADIR)/applications/evemon.desktop
	install -d $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps
	install -m 644 icon.png $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/evemon.png
	install -d $(DESTDIR)$(DATADIR)/pixmaps
	install -m 644 icon.png $(DESTDIR)$(DATADIR)/pixmaps/evemon.png
ifneq ($(wildcard presets/*.milk),)
	install -d $(DESTDIR)$(MILKDROP_PRESETDIR)
	install -m 644 presets/*.milk $(DESTDIR)$(MILKDROP_PRESETDIR)/
endif
ifneq ($(wildcard $(SOFTWARE_DIRECTORY_DB)),)
	install -d $(DESTDIR)$(EVEMON_DATADIR)
	install -m 644 $(SOFTWARE_DIRECTORY_DB) $(DESTDIR)$(EVEMON_DATADIR)/software-directory.sqlite
endif
ifeq ($(ANDROID_ENABLED),1)
	install -d $(DESTDIR)$(EVEMON_DATADIR)/android
	install -m 644 $(ANDROID_ARCHIVE) $(DESTDIR)$(EVEMON_DATADIR)/android/evemon-arm64.tar.gz
endif
	$(if $(DESTDIR),,update-desktop-database $(DATADIR)/applications)
	$(if $(DESTDIR),,-gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor 2>/dev/null || true)
endif
ifeq ($(NVTOP_AVAILABLE),1)
	install -d $(DESTDIR)$(EVEMON_DATADIR)/licenses
	install -m 644 $(NVTOP_DIR)/COPYING $(DESTDIR)$(EVEMON_DATADIR)/licenses/nvtop-COPYING
endif
else
	@echo 'Cannot install: no executable was built.' >&2
	@exit 1
endif

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/evemon
	rm -f $(DESTDIR)$(DATADIR)/applications/evemon.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/evemon.png
	rm -f $(DESTDIR)$(DATADIR)/pixmaps/evemon.png
	rm -rf $(DESTDIR)$(LIBDIR)
	rm -rf $(DESTDIR)$(EVEMON_DATADIR)
	-gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null || true
endif
