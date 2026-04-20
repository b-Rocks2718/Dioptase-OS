# Default to the repo-local toolchain so direct `make` in this directory does
# not depend on env.sh or a runner-global PATH setup.
VERSION ?= release
TOOLCHAIN_ROOT ?= ../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build
PROGRAM ?= $(notdir $(CURDIR))
SBIN_DIR := ../sbin
PROGRAM_OUTPUT := $(SBIN_DIR)/$(PROGRAM)

# root/<program> is one level shallower than tests/<name>.dir/sbin.
CRT_DIR = ../../crt
CRT_STARTUP_SRC := $(CRT_DIR)/crt0.s
CRT_ASM_SRCS := $(wildcard $(CRT_DIR)/*.s)
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.gen.s,$(CRT_C_SRCS))
# Keep crt0.s first so _start becomes the entry point in the flat binary.
CRT_ASM_SRCS_ORDERED := $(CRT_STARTUP_SRC) \
	$(filter-out $(CRT_STARTUP_SRC),$(CRT_ASM_SRCS))

C_SRCS := $(wildcard *.c)
LEGACY_C_ASMS := $(C_SRCS:.c=.s)
LEGACY_CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,crt_%.gen.s,$(CRT_C_SRCS))
C_ASMS := $(patsubst %.c,$(BUILD_DIR)/%.s,$(C_SRCS))
LOCAL_ASM_SRCS := $(filter-out $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS),$(wildcard *.s))
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(LOCAL_ASM_SRCS) $(C_ASMS)

.PHONY: all clean $(PROGRAM)

all: $(PROGRAM)

$(PROGRAM): $(PROGRAM_OUTPUT)
	@:

# Compile each C source to assembly under build/ and place the final binary in
# ../sbin/ so the root filesystem keeps executables separate from sources.
$(PROGRAM_OUTPUT): $(LINK_ASM_SRCS) Makefile | $(BUILD_DIR) $(SBIN_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BUILD_DIR)/%.s: %.c Makefile | $(BUILD_DIR)
	$(CC) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c Makefile | $(BUILD_DIR)
	$(CC) -s -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

$(SBIN_DIR):
	mkdir -p $@

clean:
	rm -f $(PROGRAM) $(PROGRAM_OUTPUT)
	rm -f $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS)
	rm -rf $(BUILD_DIR)
