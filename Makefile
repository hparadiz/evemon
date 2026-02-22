CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -pthread -D_GNU_SOURCE
LDFLAGS := -pthread -lm

# GTK3 flags
GTK_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0)
GTK_LDFLAGS := $(shell pkg-config --libs gtk+-3.0)

# Fontconfig (explicit init to suppress startup warning)
FC_CFLAGS   := $(shell pkg-config --cflags fontconfig)
FC_LDFLAGS  := $(shell pkg-config --libs fontconfig)

CFLAGS  += $(GTK_CFLAGS) $(FC_CFLAGS)
LDFLAGS += $(GTK_LDFLAGS) $(FC_LDFLAGS)

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
SRCS += $(wildcard $(SRC_DIR)/ui/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS += $(GRES_O)

# BPF object (compiled with clang, loaded at runtime)
BPF_SRC := $(SRC_DIR)/fdmon_ebpf_kern.c
BPF_OBJ := $(BUILD_DIR)/fdmon_ebpf_kern.o

TARGET := $(BUILD_DIR)/allmon

.PHONY: all clean run

all: $(TARGET) $(BPF_OBJ)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/ui/%.o: $(SRC_DIR)/ui/%.c | $(BUILD_DIR)/ui
	$(CC) $(CFLAGS) -c -o $@ $<

# GResource — compile XML + icon.png into a C source, then into .o
$(GRES_C): $(GRES_XML) icon.png | $(BUILD_DIR)
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

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET) $(BPF_OBJ)
	./$(TARGET)
