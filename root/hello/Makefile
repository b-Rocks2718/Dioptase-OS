# Build the in-guest bootstrap make helper in place. Unlike normal root
# programs, bmake belongs beside the hello sources it compiles rather than in
# /sbin.
VERSION ?= release
# Optional -D flags from the top-level OS Makefile (e.g. -DOS_RELEASE=1).
OS_BCC_DEFINES ?=
TOOLCHAIN_ROOT ?= ../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build

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

$(BMAKE_ASM): bmake.c $(CRT_HEADERS) Makefile $(CC) | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c $(CRT_HEADERS) Makefile $(CC) | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
