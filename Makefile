CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2 -pthread -D_GNU_SOURCE \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
           -Wformat -Wformat-security -fPIE
LDFLAGS := -pthread -lm -pie -Wl,-z,relro,-z,now \
           -rdynamic -ldl

# GTK3 flags
GTK_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0)
GTK_LDFLAGS := $(shell pkg-config --libs gtk+-3.0)

# Fontconfig (explicit init to suppress startup warning)
FC_CFLAGS   := $(shell pkg-config --cflags fontconfig)
FC_LDFLAGS  := $(shell pkg-config --libs fontconfig)

CFLAGS  += $(GTK_CFLAGS) $(FC_CFLAGS)
LDFLAGS += $(GTK_LDFLAGS) $(FC_LDFLAGS)

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
GRES_XML := $(SRC_DIR)/allmon.gresource.xml
GRES_C   := $(BUILD_DIR)/allmon_resources.c
GRES_O   := $(BUILD_DIR)/allmon_resources.o

# Exclude the BPF kernel program from gcc compilation
SRCS := $(filter-out $(SRC_DIR)/fdmon_ebpf_kern.c, $(wildcard $(SRC_DIR)/*.c))

# Retired sidebar scan modules (replaced by the plugin system).
# They still live in src/ui/ for reference but are no longer compiled.
# Note: pipewire_scan.c is kept — it provides pw_snapshot()/pw_graph_free()
# used by the plugin broker.
RETIRED_SCANS := fd_scan.c env_scan.c mmap_scan.c lib_scan.c net_scan.c \
                 cgroup_scan.c
SRCS += $(filter-out $(addprefix $(SRC_DIR)/ui/,$(RETIRED_SCANS)), \
                     $(wildcard $(SRC_DIR)/ui/*.c))
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS += $(GRES_O)

# BPF object (compiled with clang, loaded at runtime)
BPF_SRC := $(SRC_DIR)/fdmon_ebpf_kern.c
BPF_OBJ := $(BUILD_DIR)/fdmon_ebpf_kern.o

TARGET := $(BUILD_DIR)/allmon

.PHONY: all clean run

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
PLUGIN_CFLAGS  := -Wall -Wextra -std=c11 -O2 -D_GNU_SOURCE -fPIC -shared \
                  $(GTK_CFLAGS) $(FC_CFLAGS)
PLUGIN_LDFLAGS := $(GTK_LDFLAGS) $(FC_LDFLAGS)

PLUGIN_SRCS := $(wildcard $(SRC_DIR)/plugins/*.c)
PLUGIN_SOS  := $(patsubst $(SRC_DIR)/plugins/%.c,$(PLUGIN_DIR)/allmon_%.so,$(PLUGIN_SRCS))

$(PLUGIN_DIR):
	mkdir -p $(PLUGIN_DIR)

$(PLUGIN_DIR)/allmon_%.so: $(SRC_DIR)/plugins/%.c $(SRC_DIR)/allmon_plugin.h | $(PLUGIN_DIR)
	$(CC) $(PLUGIN_CFLAGS) -o $@ $< $(PLUGIN_LDFLAGS)

# PipeWire plugin needs PW flags (only built when PW is available)
ifneq ($(PW_LDFLAGS),)
$(PLUGIN_DIR)/allmon_pipewire_plugin.so: $(SRC_DIR)/plugins/pipewire_plugin.c $(SRC_DIR)/allmon_plugin.h | $(PLUGIN_DIR)
	$(CC) $(PLUGIN_CFLAGS) $(PW_CFLAGS) -DHAVE_PIPEWIRE -o $@ $< $(PLUGIN_LDFLAGS) $(PW_LDFLAGS)
else
# If PipeWire is not available, skip the PipeWire plugin
PLUGIN_SOS := $(filter-out $(PLUGIN_DIR)/allmon_pipewire_plugin.so,$(PLUGIN_SOS))
endif

plugins: $(PLUGIN_SOS)

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET) $(BPF_OBJ) plugins
	./$(TARGET)
