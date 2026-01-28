# Build/test harness for Dioptase-OS test programs.
# Purpose: build a standalone BIOS hex image, then build a kernel binary image
# from a root test C file plus kernel sources, and optionally run the emulator.

SHELL := /bin/sh
MAKEFLAGS += --no-print-directory

# version can be set to "debug" or "release"
VERSION ?= release

# Toolchain paths are relative to the Dioptase-OS directory.
BCC := ../Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM := ../Dioptase-Assembler/build/$(VERSION)/basm
EMULATOR := ../Dioptase-Emulators/Dioptase-Emulator-Full/target/$(VERSION)/Dioptase-Emulator-Full
EMULATOR_DIR := ../Dioptase-Emulators/Dioptase-Emulator-Full

# configuration
NUM_CORES ?= 1

# Build output locations.
BUILD_DIR := build
BIOS_ASM_DIR := $(BUILD_DIR)/bios
KERNEL_ASM_DIR := $(BUILD_DIR)/kernel

# BIOS sources to link into the BIOS image.
BIOS_C_SRCS := $(wildcard bios/*.c)
BIOS_C_ASMS := $(patsubst bios/%.c,$(BIOS_ASM_DIR)/%.s,$(BIOS_C_SRCS))
BIOS_ASM_SRCS := $(wildcard bios/*.s)
BIOS_ASM_INIT := $(wildcard bios/init.s)
BIOS_ASM_SRCS_ORDERED := $(BIOS_ASM_INIT) $(filter-out $(BIOS_ASM_INIT),$(BIOS_ASM_SRCS))

# Kernel sources to link into every test image.
KERNEL_C_SRCS := $(wildcard kernel/*.c)
KERNEL_C_ASMS := $(patsubst kernel/%.c,$(KERNEL_ASM_DIR)/%.s,$(KERNEL_C_SRCS))
KERNEL_ASM_SRCS := $(wildcard kernel/*.s)
KERNEL_ASM_MBR := $(wildcard kernel/mbr.s)
KERNEL_ASM_INIT := $(wildcard kernel/init.s)
KERNEL_ASM_SRCS_ORDERED := $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT) \
  $(filter-out $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT),$(KERNEL_ASM_SRCS))

KERNEL_BLOCK_SIZE := 512
KERNEL_START_BLOCK := 1
KERNEL_START_OFFSET := $(shell expr $(KERNEL_START_BLOCK) \* $(KERNEL_BLOCK_SIZE))

# Test sources are any C files in the Dioptase-OS root.
TEST_C_SRCS := $(wildcard *.c)
TEST_NAMES := $(basename $(TEST_C_SRCS))
BIOS_HEX := $(BUILD_DIR)/bios.hex
BIN_TARGETS := $(addsuffix .bin,$(TEST_NAMES))

# Treat config.s as phony so NUM_CORES changes always rebuild kernel images.
.PHONY: all bios.hex $(BIN_TARGETS) $(TEST_NAMES) clean kernel/config.s kernel/mbr.s
# Keep generated assembly outputs for inspection.
.PRECIOUS: $(BUILD_DIR)/%.s $(BIOS_ASM_DIR)/%.s $(KERNEL_ASM_DIR)/%.s

# Default target prints usage to avoid surprising emulator runs.
all:
	@echo "Specify a target like: make bios.hex, make <test>.bin, or make <test>"

# Build alias so `make bios.hex` produces build/bios.hex.
bios.hex: $(BIOS_HEX)
	@:

# Build alias so `make test.bin` produces build/test.bin.
$(BIN_TARGETS): %.bin: $(BUILD_DIR)/%.bin
	@:

# Run alias so `make test` builds BIOS + kernel and runs the emulator.
$(TEST_NAMES): %: $(BIOS_HEX) $(BUILD_DIR)/%.bin $(EMULATOR)
	"$(EMULATOR)" "$(BIOS_HEX)" --sd0 "$(BUILD_DIR)/$*.bin" --cores $(NUM_CORES)

# Assemble a BIOS image from BIOS C asm and BIOS asm.
# init.s must be first so its .origin establishes the bios entry point.
$(BIOS_HEX): $(BIOS_C_ASMS) $(BIOS_ASM_SRCS_ORDERED) | $(BUILD_DIR) $(BASM)
	@status=0; \
	"$(BASM)" -kernel -o "$@" $(BIOS_ASM_INIT) $(BIOS_C_ASMS) \
	  $(filter-out $(BIOS_ASM_INIT),$(BIOS_ASM_SRCS_ORDERED)) -DNUM_CORES=$(NUM_CORES) -g || status=$$?; \
	exit $$status

# Assemble a kernel binary from the test asm, kernel C asm, and kernel asm.
# mbr.s must be first so it lands at address 0; init.s must come next for .origin 0x400.
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.s $(KERNEL_C_ASMS) $(KERNEL_ASM_SRCS_ORDERED) | $(BUILD_DIR) $(BASM)
	@status=0; \
	tmp_bin="$(BUILD_DIR)/$*.tmp.bin"; \
	"$(BASM)" -kernel -bin -o "$$tmp_bin" $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT) $(BUILD_DIR)/$*.s \
	  $(KERNEL_C_ASMS) $(filter-out $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT),$(KERNEL_ASM_SRCS_ORDERED)) \
	  -DKERNEL_NUM_BLOCKS=0 -DKERNEL_START_BLOCK=$(KERNEL_START_BLOCK) -DNUM_CORES=$(NUM_CORES) \
	  || status=$$?; \
	if [ $$status -ne 0 ]; then exit $$status; fi; \
	tmp_size=$$(wc -c < "$$tmp_bin"); \
	if [ $$tmp_size -lt $(KERNEL_START_OFFSET) ]; then \
	  echo "Kernel build error: image size $$tmp_size is smaller than start offset $(KERNEL_START_OFFSET)" >&2; \
	  exit 1; \
	fi; \
	payload_bytes=$$((tmp_size - $(KERNEL_START_OFFSET))); \
	kernel_blocks=$$(( (payload_bytes + $(KERNEL_BLOCK_SIZE) - 1) / $(KERNEL_BLOCK_SIZE) )); \
	"$(BASM)" -kernel -bin -o "$@" $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT) $(BUILD_DIR)/$*.s \
	  $(KERNEL_C_ASMS) $(filter-out $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT),$(KERNEL_ASM_SRCS_ORDERED)) \
	  -DKERNEL_NUM_BLOCKS=$$kernel_blocks -DKERNEL_START_BLOCK=$(KERNEL_START_BLOCK) -DNUM_CORES=$(NUM_CORES) \
	  || status=$$?; \
	rm -f "$$tmp_bin"; \
	exit $$status

# Compile the root test C file to assembly.
$(BUILD_DIR)/%.s: %.c $(BCC) | $(BUILD_DIR)
	"$(BCC)" -s -kernel -o "$@" "$<" -g

# Compile bios C sources to assembly.
$(BIOS_ASM_DIR)/%.s: bios/%.c $(BCC) | $(BIOS_ASM_DIR)
	"$(BCC)" -s -kernel -o "$@" "$<" -g

# Compile kernel C sources to assembly.
$(KERNEL_ASM_DIR)/%.s: kernel/%.c $(BCC) | $(KERNEL_ASM_DIR)
	"$(BCC)" -s -kernel -o "$@" "$<" -g

# Build directories for outputs and temporary files.
$(BUILD_DIR):
	mkdir -p "$@"

$(BIOS_ASM_DIR): | $(BUILD_DIR)
	mkdir -p "$@"

$(KERNEL_ASM_DIR): | $(BUILD_DIR)
	mkdir -p "$@"

$(BCC):
	$(MAKE) -C ../Dioptase-Languages/Dioptase-C-Compiler $(VERSION)

$(BASM):
	$(MAKE) -C ../Dioptase-Assembler $(VERSION)

$(EMULATOR):
	@build_cmd="cargo build"; \
	if [ "$(VERSION)" = "release" ]; then build_cmd="cargo build --release"; fi; \
	$$build_cmd --manifest-path "$(EMULATOR_DIR)/Cargo.toml"

# Cleanup for build artifacts; does not delete source files.
clean:
	rm -f "$(BUILD_DIR)"/*.hex "$(BUILD_DIR)"/*.bin "$(BUILD_DIR)"/*.s \
	  $(BIOS_C_ASMS) $(KERNEL_C_ASMS)
