# Build/test harness for Dioptase-OS test programs.
# Purpose: build a standalone BIOS hex image, then build a kernel binary image
# from a root test C file plus kernel sources, and optionally run the emulator.

# ----------------------------------- configuration ---------------------------------------- #

# emulator config
NUM_CORES ?= 1
SCHEDULER ?= free # free, rr, or random
EMU_VGA ?= no
TRACE_INTS ?= no

# memory map
TEXT_LOAD_ADDR := 0x10000
DATA_LOAD_ADDR := 0x90000
RODATA_LOAD_ADDR := 0xA0000
BSS_LOAD_ADDR := 0xF0000

# ext2 filesystem config
BLOCK_SIZE := 1024 # 1024, 2048, or 4096

# Test config
TEST_RUNS ?= 1
TIMEOUT_SECONDS ?= 3
VERSION ?= release

# ------------------------------------------------------------------------------------------ #

EMU_FLAGS := --cores $(NUM_CORES) --sched $(SCHEDULER)

ifeq ($(EMU_VGA),yes)
EMU_FLAGS += --vga
USE_VGA_DEFINE := 1
else
USE_VGA_DEFINE := 0
endif

ifeq ($(TRACE_INTS),yes)
EMU_FLAGS += --trace-ints
endif

SHELL := /bin/sh
MAKEFLAGS += --no-print-directory

# Toolchain paths are relative to the Dioptase-OS directory.
BCC := ../Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM := ../Dioptase-Assembler/build/$(VERSION)/basm
EMULATOR := ../Dioptase-Emulators/Dioptase-Emulator-Full/target/$(VERSION)/Dioptase-Emulator-Full
EMULATOR_DIR := ../Dioptase-Emulators/Dioptase-Emulator-Full

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

# Kernel disk images are raw 512-byte sectors, independent of ext2 block size.
KERNEL_BLOCK_SIZE := 512
KERNEL_START_BLOCK := 1
KERNEL_START_OFFSET := $(shell expr $(KERNEL_START_BLOCK) \* $(KERNEL_BLOCK_SIZE))
SD_IMAGE_MIN_BLOCKS := 2048
SD_IMAGE_EXTRA_BLOCKS := 256

# Test sources live under tests/.
TEST_C_SRCS := $(wildcard tests/*.c)
TEST_NAMES := $(basename $(notdir $(TEST_C_SRCS)))
# Only tests with checked-in .ok baselines are valid for aggregate output checks.
TEST_OK_FILES := $(wildcard tests/*.ok)
TEST_OK_NAMES := $(basename $(notdir $(TEST_OK_FILES)))
TEST_OK_SUMMARY_TARGETS := $(addsuffix .summary-test,$(TEST_OK_NAMES))
BIOS_HEX := $(BUILD_DIR)/bios.hex
BIN_TARGETS := $(addsuffix .bin,$(TEST_NAMES))
HEX_TARGETS := $(addsuffix .hex,$(TEST_NAMES))
LABEL_TARGETS := $(addsuffix .labels,$(TEST_NAMES))

# Build the emulator argv in shell positional parameters. When tests/<name>.dir
# exists, package it as an ext2 filesystem image and attach it as SD1.
define prepare_emulator_cmd
sd1_dir="tests/$*.dir"; \
set -- "$(EMULATOR)" "$(BIOS_HEX)" --sd0 "$(BUILD_DIR)/$*.bin"; \
if [ -d "$$sd1_dir" ]; then \
  sd1_image="$(BUILD_DIR)/$*.sd1.ext2"; \
  dir_kib=$$(du -sk "$$sd1_dir" | cut -f1); \
  dir_blocks=$$(((dir_kib * 1024 + $(BLOCK_SIZE) - 1) / $(BLOCK_SIZE))); \
  fs_blocks=$$((dir_blocks + $(SD_IMAGE_EXTRA_BLOCKS))); \
  if [ $$fs_blocks -lt $(SD_IMAGE_MIN_BLOCKS) ]; then \
    fs_blocks=$(SD_IMAGE_MIN_BLOCKS); \
  fi; \
  rm -f "$$sd1_image"; \
  mkfs.ext2 -q -d "$$sd1_dir" -b $(BLOCK_SIZE) -m 0 -r 0 -O none "$$sd1_image" "$$fs_blocks" > /dev/null || exit $$?; \
  set -- "$$@" --sd1 "$$sd1_image"; \
fi; \
set -- "$$@" $(EMU_FLAGS);
endef

# Treat config.s files as phony so config define changes always rebuild images.
.PHONY: all bios.hex bios.labels $(BIN_TARGETS) $(HEX_TARGETS) $(LABEL_TARGETS) \
  $(TEST_NAMES) test test-no-ok clean bios/config.s kernel/config.s kernel/mbr.s
# Keep generated assembly outputs for inspection.
.PRECIOUS: $(BUILD_DIR)/%.s $(BIOS_ASM_DIR)/%.s $(KERNEL_ASM_DIR)/%.s

# Default target prints usage to avoid surprising emulator runs.
all:
	@echo "Specify a target like: make test, make bios.hex, make <test>.bin, or make <test>"

# Aggregate baseline-checked summary targets so `make -j` can run them in parallel.
test: $(if $(TEST_OK_SUMMARY_TARGETS),$(TEST_OK_SUMMARY_TARGETS),test-no-ok)

test-no-ok:
	@echo "No tests with .ok baselines were found under tests/."

# Build alias so `make bios.hex` produces build/bios.hex.
bios.hex: $(BIOS_HEX)
	@:

# Build alias so `make bios.labels` produces build/bios.labels.
bios.labels: $(BUILD_DIR)/bios.labels
	@:

# Build alias so `make test.bin` produces build/test.bin.
$(BIN_TARGETS): %.bin: $(BUILD_DIR)/%.bin
	@:

# Build alias so `make test.hex` produces build/test.hex.
$(HEX_TARGETS): %.hex: $(BUILD_DIR)/%.hex
	@:

# Build alias so `make test.labels` produces build/test.labels.
$(LABEL_TARGETS): %.labels: $(BUILD_DIR)/%.labels
	@:

# Run alias so `make test` builds BIOS + kernel and runs the emulator.
$(TEST_NAMES): %: $(BIOS_HEX) $(BUILD_DIR)/%.bin $(EMULATOR)
	@$(prepare_emulator_cmd) \
	"$$@"

# Test alias so `make testname.test` runs the emulator multiple times and compares output.
# Keep per-run logging here for interactive debugging of a single test.
%.test: $(BIOS_HEX) $(BUILD_DIR)/%.bin $(EMULATOR)
	@$(prepare_emulator_cmd) \
	runs=$(TEST_RUNS); \
	success=0; \
	test_name="$*"; \
	i=1; \
	while [ $$i -le $$runs ]; do \
	  raw="tests/$*.raw"; \
	  out="tests/$*.out"; \
	  ok="tests/$*.ok"; \
	  rm -f "$$raw" "$$out"; \
	  status=0; \
	  timeout "$(TIMEOUT_SECONDS)" "$$@" > "$$raw" || status=$$?; \
	  grep '^\*\*\*' "$$raw" > "$$out" || true; \
	  if [ $$status -eq 124 ]; then \
	    echo "[$$test_name] run $$i/$$runs: fail (timeout)"; \
	  elif grep -q "Warning" "$$raw" || grep -q "Spurious" "$$raw" || grep -q "PANIC" "$$raw"; then \
	    echo "[$$test_name] run $$i/$$runs: fail (warning)"; \
	  elif [ $$status -ne 0 ]; then \
	    echo "[$$test_name] run $$i/$$runs: fail (exit $$status)"; \
	  elif [ -f "$$ok" ] && cmp -s "$$out" "$$ok"; then \
	    success=$$((success + 1)); \
	    echo "[$$test_name] run $$i/$$runs: pass"; \
	  else \
	    echo "[$$test_name] run $$i/$$runs: fail"; \
	  fi; \
	  i=$$((i + 1)); \
	done; \
	echo "[$$test_name] summary: $$success/$$runs"

# Quiet aggregate target used by `make test`; prints once after all runs complete.
%.summary-test: $(BIOS_HEX) $(BUILD_DIR)/%.bin $(EMULATOR)
	@$(prepare_emulator_cmd) \
	runs=$(TEST_RUNS); \
	success=0; \
	timeout_failures=0; \
	warning_failures=0; \
	exit_failures=0; \
	mismatch_failures=0; \
	test_name="$*"; \
	i=1; \
	while [ $$i -le $$runs ]; do \
	  raw="tests/$*.raw"; \
	  out="tests/$*.out"; \
	  ok="tests/$*.ok"; \
	  rm -f "$$raw" "$$out"; \
	  status=0; \
	  timeout "$(TIMEOUT_SECONDS)" "$$@" > "$$raw" || status=$$?; \
	  grep '^\*\*\*' "$$raw" > "$$out" || true; \
	  if [ $$status -eq 124 ]; then \
	    timeout_failures=$$((timeout_failures + 1)); \
	  elif grep -q "Warning" "$$raw" || grep -q "Spurious" "$$raw" || grep -q "PANIC" "$$raw"; then \
	    warning_failures=$$((warning_failures + 1)); \
	  elif [ $$status -ne 0 ]; then \
	    exit_failures=$$((exit_failures + 1)); \
	  elif [ -f "$$ok" ] && cmp -s "$$out" "$$ok"; then \
	    success=$$((success + 1)); \
	  else \
	    mismatch_failures=$$((mismatch_failures + 1)); \
	  fi; \
	  i=$$((i + 1)); \
	done; \
	if [ $$success -eq $$runs ]; then \
	  echo "[$$test_name] pass: $$success/$$runs"; \
	else \
	  reason_summary=""; \
	  if [ $$timeout_failures -gt 0 ]; then reason_summary="$$reason_summary $$timeout_failures timeout"; fi; \
	  if [ $$warning_failures -gt 0 ]; then reason_summary="$$reason_summary $$warning_failures warning"; fi; \
	  if [ $$exit_failures -gt 0 ]; then reason_summary="$$reason_summary $$exit_failures exit"; fi; \
	  if [ $$mismatch_failures -gt 0 ]; then reason_summary="$$reason_summary $$mismatch_failures mismatch"; fi; \
	  reason_summary=$${reason_summary# }; \
	  echo "[$$test_name] fail: $$success/$$runs ($$reason_summary)"; \
	fi

# Test alias so `make testname.fail` stops on the first failure.
%.fail: $(BIOS_HEX) $(BUILD_DIR)/%.bin $(EMULATOR)
	@$(prepare_emulator_cmd) \
	runs=$(TEST_RUNS); \
	success=0; \
	failure_reason=""; \
	failure_run=0; \
	test_name="$*"; \
	i=1; \
	while [ $$i -le $$runs ]; do \
	  raw="tests/$*.raw"; \
	  out="tests/$*.out"; \
	  ok="tests/$*.ok"; \
	  rm -f "$$raw" "$$out"; \
	  status=0; \
	  timeout "$(TIMEOUT_SECONDS)" "$$@" > "$$raw" || status=$$?; \
	  grep '^\*\*\*' "$$raw" > "$$out" || true; \
	  if [ $$status -eq 124 ]; then \
	    failure_reason="timeout"; \
	    failure_run=$$i; \
	    break; \
	  elif grep -q "Warning" "$$raw" || grep -q "Spurious" "$$raw" ||  grep -q "PANIC" "$$raw"; then \
	    failure_reason="warning"; \
	    failure_run=$$i; \
	    break; \
	  elif [ $$status -ne 0 ]; then \
	    failure_reason="exit $$status"; \
	    failure_run=$$i; \
	    break; \
	  elif [ -f "$$ok" ] && cmp -s "$$out" "$$ok"; then \
	    success=$$((success + 1)); \
	  else \
	    failure_reason="mismatch"; \
	    failure_run=$$i; \
	    break; \
	  fi; \
	  i=$$((i + 1)); \
	done; \
	if [ -z "$$failure_reason" ]; then \
	  echo "[$$test_name] pass: $$success/$$runs"; \
	else \
	  echo "[$$test_name] fail: $$success/$$runs ($$failure_reason on run $$failure_run/$$runs)"; \
	fi
# Assemble a BIOS image from BIOS C asm and BIOS asm.
# init.s must be first so its .origin establishes the bios entry point.
$(BIOS_HEX): $(BIOS_C_ASMS) $(BIOS_ASM_SRCS_ORDERED) | $(BUILD_DIR) $(BASM)
	@status=0; \
	"$(BASM)" -kernel -o "$@" $(BIOS_ASM_INIT) $(BIOS_C_ASMS) \
	  $(filter-out $(BIOS_ASM_INIT),$(BIOS_ASM_SRCS_ORDERED)) -DNUM_CORES=$(NUM_CORES) -DUSE_VGA=$(USE_VGA_DEFINE) -g || status=$$?; \
	if [ $$status -ne 0 ]; then exit $$status; fi; \
	grep '^#' "$@" > "$(BUILD_DIR)/bios.labels" || true; \
	cat $(BIOS_ASM_INIT) $(BIOS_C_ASMS) $(filter-out $(BIOS_ASM_INIT),$(BIOS_ASM_SRCS_ORDERED)) > "$(BUILD_DIR)/bios.all.s"; \
	exit $$status

# Assemble a kernel binary from the test asm, kernel C asm, and kernel asm.
# mbr.s must be first so it lands at address 0; init.s must come next for .origin 0x400.
# This is a two-pass build: pass 1 emits a temp hex file to compute section offsets,
# then pass 2 reassembles with correct MBR macros.
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.s $(KERNEL_C_ASMS) $(KERNEL_ASM_SRCS_ORDERED) | $(BUILD_DIR) $(BASM)
	@status=0; \
	tmp_hex="$(BUILD_DIR)/$*.tmp.hex"; \
	"$(BASM)" -kernel -g -o "$$tmp_hex" $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT) $(BUILD_DIR)/$*.s \
	  $(KERNEL_C_ASMS) $(filter-out $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT),$(KERNEL_ASM_SRCS_ORDERED)) \
	  -DTEXT_START_BLOCK=0 -DTEXT_NUM_BLOCKS=0 -DTEXT_LOAD_ADDR=$(TEXT_LOAD_ADDR) \
	  -DDATA_START_BLOCK=0 -DDATA_NUM_BLOCKS=0 -DDATA_LOAD_ADDR=$(DATA_LOAD_ADDR) \
	  -DRODATA_START_BLOCK=0 -DRODATA_NUM_BLOCKS=0 -DRODATA_LOAD_ADDR=$(RODATA_LOAD_ADDR) \
	  -DBSS_NUM_BLOCKS=0 -DBSS_LOAD_ADDR=$(BSS_LOAD_ADDR) -DNUM_CORES=$(NUM_CORES) \
	  -DUSE_VGA=$(USE_VGA_DEFINE) \
	  || status=$$?; \
	if [ $$status -ne 0 ]; then exit $$status; fi; \
	set -- $$(grep '^@' "$$tmp_hex" | head -n 6 | sed 's/^@//'); \
	if [ $$# -ne 6 ]; then \
	  echo "Kernel build error: expected 6 section markers in $$tmp_hex, found $$#." >&2; \
	  exit 1; \
	fi; \
	implicit_base=$$((0x$$1 * 4)); \
	text_base=$$((0x$$2 * 4)); \
	rodata_base=$$((0x$$3 * 4)); \
	data_base=$$((0x$$4 * 4)); \
	bss_base=$$((0x$$5 * 4)); \
	end_base=$$((0x$$6 * 4)); \
	if [ $$((text_base % $(KERNEL_BLOCK_SIZE))) -ne 0 ] || \
	   [ $$((rodata_base % $(KERNEL_BLOCK_SIZE))) -ne 0 ] || \
	   [ $$((data_base % $(KERNEL_BLOCK_SIZE))) -ne 0 ] || \
	   [ $$((bss_base % $(KERNEL_BLOCK_SIZE))) -ne 0 ] || \
	   [ $$((end_base % $(KERNEL_BLOCK_SIZE))) -ne 0 ]; then \
	  echo "Kernel build error: section bases are not aligned to $(KERNEL_BLOCK_SIZE) bytes." >&2; \
	  exit 1; \
	fi; \
	if [ $$rodata_base -lt $$text_base ] || [ $$data_base -lt $$rodata_base ] || [ $$bss_base -lt $$data_base ] || [ $$end_base -lt $$bss_base ]; then \
	  echo "Kernel build error: section bases are not ordered text->rodata->data->bss->end." >&2; \
	  exit 1; \
	fi; \
	text_start_block=$$((text_base / $(KERNEL_BLOCK_SIZE))); \
	rodata_start_block=$$((rodata_base / $(KERNEL_BLOCK_SIZE))); \
	data_start_block=$$((data_base / $(KERNEL_BLOCK_SIZE))); \
	text_num_blocks=$$(((rodata_base - text_base) / $(KERNEL_BLOCK_SIZE))); \
	rodata_num_blocks=$$(((data_base - rodata_base) / $(KERNEL_BLOCK_SIZE))); \
	data_num_blocks=$$(((bss_base - data_base) / $(KERNEL_BLOCK_SIZE))); \
	bss_num_blocks=$$(((end_base - bss_base) / $(KERNEL_BLOCK_SIZE))); \
	"$(BASM)" -kernel -bin -o "$@" $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT) $(BUILD_DIR)/$*.s \
	  $(KERNEL_C_ASMS) $(filter-out $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT),$(KERNEL_ASM_SRCS_ORDERED)) \
	  -DTEXT_START_BLOCK=$$text_start_block -DTEXT_NUM_BLOCKS=$$text_num_blocks -DTEXT_LOAD_ADDR=$(TEXT_LOAD_ADDR) \
	  -DDATA_START_BLOCK=$$data_start_block -DDATA_NUM_BLOCKS=$$data_num_blocks -DDATA_LOAD_ADDR=$(DATA_LOAD_ADDR) \
	  -DRODATA_START_BLOCK=$$rodata_start_block -DRODATA_NUM_BLOCKS=$$rodata_num_blocks -DRODATA_LOAD_ADDR=$(RODATA_LOAD_ADDR) \
	  -DBSS_NUM_BLOCKS=$$bss_num_blocks -DBSS_LOAD_ADDR=$(BSS_LOAD_ADDR) -DNUM_CORES=$(NUM_CORES) \
	  -DUSE_VGA=$(USE_VGA_DEFINE) \
	  || status=$$?; \
	if [ $$status -ne 0 ]; then exit $$status; fi; \
	"$(BASM)" -kernel -g -o "$(BUILD_DIR)/$*.hex" $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT) $(BUILD_DIR)/$*.s \
	  $(KERNEL_C_ASMS) $(filter-out $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT),$(KERNEL_ASM_SRCS_ORDERED)) \
	  -DTEXT_START_BLOCK=$$text_start_block -DTEXT_NUM_BLOCKS=$$text_num_blocks -DTEXT_LOAD_ADDR=$(TEXT_LOAD_ADDR) \
	  -DDATA_START_BLOCK=$$data_start_block -DDATA_NUM_BLOCKS=$$data_num_blocks -DDATA_LOAD_ADDR=$(DATA_LOAD_ADDR) \
	  -DRODATA_START_BLOCK=$$rodata_start_block -DRODATA_NUM_BLOCKS=$$rodata_num_blocks -DRODATA_LOAD_ADDR=$(RODATA_LOAD_ADDR) \
	  -DBSS_NUM_BLOCKS=$$bss_num_blocks -DBSS_LOAD_ADDR=$(BSS_LOAD_ADDR) -DNUM_CORES=$(NUM_CORES) \
	  -DUSE_VGA=$(USE_VGA_DEFINE) \
	  || status=$$?; \
	if [ $$status -ne 0 ]; then exit $$status; fi; \
	grep '^#' "$(BUILD_DIR)/$*.hex" > "$(BUILD_DIR)/$*.labels" || true; \
	cat $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT) $(BUILD_DIR)/$*.s $(KERNEL_C_ASMS) \
	  $(filter-out $(KERNEL_ASM_MBR) $(KERNEL_ASM_INIT),$(KERNEL_ASM_SRCS_ORDERED)) > "$(BUILD_DIR)/$*.all.s"; \
	rm -f "$$tmp_hex"; \
	exit $$status

# Compile the root test C file to assembly.
$(BUILD_DIR)/%.s: tests/%.c $(BCC) | $(BUILD_DIR)
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
	rm -f "$(BUILD_DIR)"/*.hex "$(BUILD_DIR)"/*.bin "$(BUILD_DIR)"/*.s "$(BUILD_DIR)"/*.labels \
	  "$(BUILD_DIR)"/*.sd1.ext2 $(BIOS_C_ASMS) $(KERNEL_C_ASMS) tests/*.raw tests/*.out
