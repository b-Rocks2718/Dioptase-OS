# Build/test harness for Dioptase-OS test programs.
# Purpose: compile a root test C file plus kernel sources into a kernel-mode hex
# image, then optionally run the full emulator.

SHELL := /bin/sh
MAKEFLAGS += --no-print-directory

# Toolchain paths are relative to the Dioptase-OS directory.
BCC := ../Dioptase-Languages/Dioptase-C-Compiler/build/debug/bcc
BASM := ../Dioptase-Assembler/build/debug/basm
EMULATOR := ../Dioptase-Emulators/Dioptase-Emulator-Full/target/debug/Dioptase-Emulator-Full

# Build output locations.
BUILD_DIR := build
KERNEL_TMP_DIR := $(BUILD_DIR)/kernel

# Kernel sources to link into every test image.
KERNEL_C_SRCS := $(wildcard kernel/*.c)
KERNEL_C_TMPS := $(patsubst kernel/%.c,$(KERNEL_TMP_DIR)/%.s.tmp,$(KERNEL_C_SRCS))
KERNEL_ASM_SRCS := $(wildcard kernel/*.s)

# Test sources are any C files in the Dioptase-OS root.
TEST_C_SRCS := $(wildcard *.c)
TEST_NAMES := $(basename $(TEST_C_SRCS))
HEX_TARGETS := $(addsuffix .hex,$(TEST_NAMES))

.PHONY: all $(HEX_TARGETS) $(TEST_NAMES) clean

# Default target prints usage to avoid surprising emulator runs.
all:
	@echo "Specify a test target like: make <test>.hex or make <test>"

# Build alias so `make test.hex` produces build/test.hex.
$(HEX_TARGETS): %.hex: $(BUILD_DIR)/%.hex
	@:

# Run alias so `make test` builds and runs the emulator.
$(TEST_NAMES): %: $(BUILD_DIR)/%.hex
	"$(EMULATOR)" "$<"

# Assemble a kernel image from the test tmp, kernel C tmps, and kernel asm.
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.s.tmp $(KERNEL_C_TMPS) $(KERNEL_ASM_SRCS) | $(BUILD_DIR)
	@status=0; \
	"$(BASM)" -kernel -o "$@" $^ || status=$$?; \
	rm -f "$(BUILD_DIR)/$*.s.tmp" $(KERNEL_C_TMPS); \
	exit $$status

# Compile the root test C file to temporary assembly.
$(BUILD_DIR)/%.s.tmp: %.c | $(BUILD_DIR)
	"$(BCC)" -s -kernel -o "$@" "$<"

# Compile kernel C sources to temporary assembly.
$(KERNEL_TMP_DIR)/%.s.tmp: kernel/%.c | $(KERNEL_TMP_DIR)
	"$(BCC)" -s -kernel -o "$@" "$<"

# Build directories for outputs and temporary files.
$(BUILD_DIR):
	mkdir -p "$@"

$(KERNEL_TMP_DIR): | $(BUILD_DIR)
	mkdir -p "$@"

# Cleanup for build artifacts; does not delete source files.
clean:
	rm -f "$(BUILD_DIR)"/*.hex "$(BUILD_DIR)"/*.s.tmp $(KERNEL_C_TMPS)
