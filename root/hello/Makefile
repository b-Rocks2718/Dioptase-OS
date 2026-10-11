# Build the in-guest bootstrap make helper in place. Unlike normal root
# programs, bmake belongs beside the hello sources it compiles rather than in
# /sbin.
VERSION ?= release
# Optional -D flags from the top-level OS Makefile (e.g. -DOS_RELEASE=1).
OS_BCC_DEFINES ?=
# Root programs are always compiled with bcc's optimizer, whether built here
# or through the top-level root-sbin target (which may pass -opt in
# OS_BCC_DEFINES as well).
BCC_FLAGS := $(filter-out -opt,$(OS_BCC_DEFINES)) -opt
TOOLCHAIN_ROOT ?= ../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build
BCC_STAMP := $(BUILD_DIR)/bcc_flags

CRT_DIR = ../crt
CRT_STARTUP_SRC := $(CRT_DIR)/crt0.s
CRT_ASM_SRCS := $(wildcard $(CRT_DIR)/*.s)
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_HEADERS := $(wildcard $(CRT_DIR)/*.h $(CRT_DIR)/*/*.h)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.gen.s,$(CRT_C_SRCS))
CRT_ASM_SRCS_ORDERED := $(CRT_STARTUP_SRC) \
	$(filter-out $(CRT_STARTUP_SRC),$(CRT_ASM_SRCS))
BMAKE_ASM := $(BUILD_DIR)/bmake.s
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(BMAKE_ASM)

.PHONY: all clean

all: bmake

bmake: $(LINK_ASM_SRCS) Makefile $(BASM)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BMAKE_ASM): bmake.c $(CRT_HEADERS) Makefile $(CC) $(BCC_STAMP) | $(BUILD_DIR)
	$(CC) $(BCC_FLAGS) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c $(CRT_HEADERS) Makefile $(CC) $(BCC_STAMP) | $(BUILD_DIR)
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

clean:
	rm -rf $(BUILD_DIR)
