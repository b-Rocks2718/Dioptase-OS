# Default to the repo-local toolchain so direct `make` in this directory does
# not depend on env.sh or a runner-global PATH setup.
VERSION ?= release
# Optional -D flags from the top-level OS Makefile (e.g. -DOS_RELEASE=1).
OS_BCC_DEFINES ?= -DOS_RELEASE=1 -opt
TOOLCHAIN_ROOT ?= ../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build
PROGRAM ?= $(notdir $(CURDIR))
SBIN_DIR := ../sbin
PROGRAM_OUTPUT := $(SBIN_DIR)/$(PROGRAM)

# root/<program> is one level shallower than tests/<name>.dir/sbin.
CRT_DIR = ../crt
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

.PHONY: all clean $(PROGRAM) FORCE host-check host-sanitize previews

all: $(PROGRAM)

$(PROGRAM): $(PROGRAM_OUTPUT)
	@:

# Compile each C source to assembly under build/ and place the final binary in
# ../sbin/ so the root filesystem keeps executables separate from sources.
$(PROGRAM_OUTPUT): $(LINK_ASM_SRCS) Makefile $(BASM) | $(BUILD_DIR) $(SBIN_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BUILD_DIR)/%.s: %.c $(wildcard *.h) Makefile $(BUILD_DIR)/config.stamp $(CC) | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c $(wildcard $(CRT_DIR)/*.h $(CRT_DIR)/*/*.h) Makefile $(BUILD_DIR)/config.stamp $(CC) | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

$(SBIN_DIR):
	mkdir -p $@

clean:
	rm -f $(PROGRAM) $(PROGRAM_OUTPUT)
	rm -f $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS)
	rm -rf $(BUILD_DIR)

# Keep application and CRT assembly consistent when optimization/assert flags change.
$(BUILD_DIR)/config.stamp: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(CC)' '$(OS_BCC_DEFINES)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp

FORCE:

HOST_CC ?= cc
HOST_FLAGS := -std=c11 -O2 -Wall -Wextra -Werror -pedantic
PORTABLE_SRCS := game.c render.c music.c

$(BUILD_DIR)/check: tools/check.c $(PORTABLE_SRCS) $(wildcard *.h) | $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) -o $@ tools/check.c $(PORTABLE_SRCS)

$(BUILD_DIR)/preview: tools/preview.c $(PORTABLE_SRCS) $(wildcard *.h) | $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) -o $@ tools/preview.c $(PORTABLE_SRCS)

host-check: $(BUILD_DIR)/check
	$(BUILD_DIR)/check

# Optional host diagnostic build; sanitizer flags are never used by bcc.
host-sanitize: | $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $(BUILD_DIR)/check-sanitize tools/check.c $(PORTABLE_SRCS)
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/check-sanitize

# Requires the guest capture files produced by the OS lumen.test integration run.
previews: $(BUILD_DIR)/preview
	python3 tools/verify_capture.py --guest-dir ../../tests/lumen.out.dir --preview $(BUILD_DIR)/preview --output $(BUILD_DIR)/captures
