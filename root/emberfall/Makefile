# Default to the repo-local toolchain so direct `make` in this directory does
# not depend on env.sh or a runner-global PATH setup.
VERSION ?= release
# Optional -D flags from the top-level OS Makefile (e.g. -DOS_RELEASE=1).
OS_BCC_DEFINES ?=
# Root programs are always compiled with bcc's optimizer, whether built here
# or through the top-level root-sbin target (which may pass -opt in
# OS_BCC_DEFINES as well). Compiled assembly depends on the bcc binary and on
# these flags (BCC_STAMP below), so a compiler or flag change rebuilds it.
BCC_FLAGS := $(filter-out -opt,$(OS_BCC_DEFINES)) -opt
TOOLCHAIN_ROOT ?= ../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build
BCC_STAMP := $(BUILD_DIR)/bcc_flags
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
C_HDRS := $(wildcard *.h)
LEGACY_C_ASMS := $(C_SRCS:.c=.s)
LEGACY_CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,crt_%.gen.s,$(CRT_C_SRCS))
C_ASMS := $(patsubst %.c,$(BUILD_DIR)/%.s,$(C_SRCS))
LOCAL_ASM_SRCS := $(filter-out $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS),$(wildcard *.s))
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(LOCAL_ASM_SRCS) $(C_ASMS)

.PHONY: all clean test $(PROGRAM)

all: $(PROGRAM)

$(PROGRAM): $(PROGRAM_OUTPUT)
	@:

# Compile each C source to assembly under build/ and place the final binary in
# ../sbin/ so the root filesystem keeps executables separate from sources.
$(PROGRAM_OUTPUT): $(LINK_ASM_SRCS) Makefile | $(BUILD_DIR) $(SBIN_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BUILD_DIR)/%.s: %.c Makefile $(C_HDRS) $(CC) $(BCC_STAMP) | $(BUILD_DIR)
	$(CC) $(BCC_FLAGS) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c Makefile $(CC) $(BCC_STAMP) | $(BUILD_DIR)
	$(CC) $(BCC_FLAGS) -s -o $@ $<

# The bcc flags the generated assembly was built with. make compares only
# timestamps, so assembly from an earlier build with other flags (say, without
# -opt) would otherwise be reused silently; this file changes, and the
# assembly that depends on it is rebuilt, only when BCC_FLAGS changes.
$(BCC_STAMP): FORCE | $(BUILD_DIR)
	@echo '$(BCC_FLAGS)' | cmp -s - $@ || echo '$(BCC_FLAGS)' > $@

.PHONY: FORCE
FORCE:

$(BUILD_DIR):
	mkdir -p $@

$(SBIN_DIR):
	mkdir -p $@

clean:
	rm -f $(PROGRAM) $(PROGRAM_OUTPUT)
	rm -f $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS)
	rm -rf $(BUILD_DIR)

# Deterministic host checks for the pure rules, no Dioptase toolchain needed.
test: | $(BUILD_DIR)
	gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
	  -fno-omit-frame-pointer -o $(BUILD_DIR)/game_test game.c tests/game_test.c
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD_DIR)/game_test
	@# Art checks build render.c against stub MMIO; -Werror is off because
	@# the CRT headers redeclare NULL under host libc headers.
	gcc -std=c11 -Wall -Wextra -fsanitize=address,undefined \
	  -fno-omit-frame-pointer -o $(BUILD_DIR)/dump_art tools/dump_art.c game.c
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD_DIR)/dump_art
