# Build/test harness for Dioptase-OS test programs.
# Purpose: compile a root test C file plus kernel sources into a kernel-mode hex
# image, then optionally run the full emulator.

SHELL := /bin/sh
MAKEFLAGS += --no-print-directory

# version can be set to "debug" or "release"
VERSION ?= release

# Toolchain paths are relative to the Dioptase-OS directory.
BCC := ../Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM := ../Dioptase-Assembler/build/$(VERSION)/basm
EMULATOR := ../Dioptase-Emulators/Dioptase-Emulator-Full/target/$(VERSION)/Dioptase-Emulator-Full

# configuration
NUM_CORES ?= 1

# Build output locations.
BUILD_DIR := build
KERNEL_ASM_DIR := $(BUILD_DIR)/kernel

# Kernel sources to link into every test image.
KERNEL_C_SRCS := $(wildcard kernel/*.c)
KERNEL_C_ASMS := $(patsubst kernel/%.c,$(KERNEL_ASM_DIR)/%.s,$(KERNEL_C_SRCS))
KERNEL_ASM_SRCS := $(wildcard kernel/*.s)

# Test sources are any C files in the Dioptase-OS root.
TEST_C_SRCS := $(wildcard *.c)
TEST_NAMES := $(basename $(TEST_C_SRCS))
HEX_TARGETS := $(addsuffix .hex,$(TEST_NAMES))

# Treat config.s as phony so NUM_CORES changes always rebuild kernel images.
.PHONY: all $(HEX_TARGETS) $(TEST_NAMES) clean kernel/config.s
# Keep generated assembly outputs for inspection.
.PRECIOUS: $(BUILD_DIR)/%.s $(KERNEL_ASM_DIR)/%.s

# Default target prints usage to avoid surprising emulator runs.
all:
	@echo "Specify a test target like: make <test>.hex or make <test>"

# Build alias so `make test.hex` produces build/test.hex.
$(HEX_TARGETS): %.hex: $(BUILD_DIR)/%.hex
	@:

# Run alias so `make test` builds and runs the emulator.
$(TEST_NAMES): %: $(BUILD_DIR)/%.hex
	"$(EMULATOR)" "$<" --cores $(NUM_CORES)

# Assemble a kernel image from the test asm, kernel C asm, and kernel asm.
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.s $(KERNEL_C_ASMS) $(KERNEL_ASM_SRCS) | $(BUILD_DIR)
	@status=0; \
	"$(BASM)" -kernel -o "$@" $^ -DNUM_CORES=$(NUM_CORES) -g || status=$$?; \
	exit $$status

# Compile the root test C file to assembly.
$(BUILD_DIR)/%.s: %.c | $(BUILD_DIR)
	"$(BCC)" -s -kernel -o "$@" "$<" -g

# Compile kernel C sources to assembly.
$(KERNEL_ASM_DIR)/%.s: kernel/%.c | $(KERNEL_ASM_DIR)
	"$(BCC)" -s -kernel -o "$@" "$<" -g

# Build directories for outputs and temporary files.
$(BUILD_DIR):
	mkdir -p "$@"

$(KERNEL_ASM_DIR): | $(BUILD_DIR)
	mkdir -p "$@"

# Cleanup for build artifacts; does not delete source files.
clean:
	rm -f "$(BUILD_DIR)"/*.hex "$(BUILD_DIR)"/*.s $(KERNEL_C_ASMS)
