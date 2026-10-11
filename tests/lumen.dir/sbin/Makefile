# Build an init wrapper and the actual root application into the test image.
VERSION ?= release
OS_BCC_DEFINES ?=
TOOLCHAIN_ROOT ?= ../../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR := build
CRT_DIR := ../../../root/crt
LUMEN_DIR := ../../../root/lumen

CRT_STARTUP_SRC := $(CRT_DIR)/crt0.s
CRT_ASM_SRCS := $(wildcard $(CRT_DIR)/*.s)
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_HEADERS := $(wildcard $(CRT_DIR)/*.h $(CRT_DIR)/*/*.h)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.gen.s,$(CRT_C_SRCS))
# The flat executable enters its first instruction, so CRT startup comes first.
CRT_ASM_SRCS_ORDERED := $(CRT_STARTUP_SRC) $(filter-out $(CRT_STARTUP_SRC),$(CRT_ASM_SRCS))
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(BUILD_DIR)/init.s

.PHONY: all clean lumen

all: init lumen

init: $(LINK_ASM_SRCS) Makefile $(BASM) | $(BUILD_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

# Refresh the shipped binary on every test build so source changes cannot leave
# a stale copied executable in the fixture. Resolve tools before changing cwd.
lumen:
	$(MAKE) -C $(LUMEN_DIR) CC="$(abspath $(CC))" BASM="$(abspath $(BASM))" OS_BCC_DEFINES="$(OS_BCC_DEFINES)"
	cp ../../../root/sbin/lumen $@

$(BUILD_DIR)/init.s: init.c $(CRT_HEADERS) Makefile $(CC) | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c $(CRT_HEADERS) Makefile $(CC) | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f init lumen
	rm -rf $(BUILD_DIR)
