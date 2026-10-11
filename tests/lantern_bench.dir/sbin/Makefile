# Headless lantern bench. Not part of `make test` (no .ok file): jiffies vary
# with the host, and the ASCII picture is for a person to read.
# `make lantern_bench` from the OS tree boots this init and exits.
VERSION ?= release
OS_BCC_DEFINES ?=
TOOLCHAIN_ROOT ?= ../../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build

CRT_DIR = ../../../root/crt
LANTERN_DIR = ../../../root/lantern
CRT_STARTUP_SRC := $(CRT_DIR)/crt0.s
CRT_ASM_SRCS := $(wildcard $(CRT_DIR)/*.s)
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.gen.s,$(CRT_C_SRCS))
CRT_ASM_SRCS_ORDERED := $(CRT_STARTUP_SRC) \
	$(filter-out $(CRT_STARTUP_SRC),$(CRT_ASM_SRCS))

LANTERN_ASMS := $(BUILD_DIR)/lantern.s $(BUILD_DIR)/render.s $(BUILD_DIR)/world.s $(BUILD_DIR)/sound.s
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(LANTERN_ASMS)

.PHONY: all clean

all: init

init: $(LINK_ASM_SRCS) Makefile | $(BUILD_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BUILD_DIR)/lantern.s: $(LANTERN_DIR)/lantern.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -DLANTERN_FORCE_BENCH -s -o $@ $<

$(BUILD_DIR)/render.s: $(LANTERN_DIR)/render.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/world.s: $(LANTERN_DIR)/world.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/sound.s: $(LANTERN_DIR)/sound.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f init
	rm -rf $(BUILD_DIR)
