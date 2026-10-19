# Run the game's recoverable save codec under the standard OS user-program test runner.
VERSION ?= release
TOOLCHAIN_ROOT ?= ../../../../
OS_BCC_DEFINES ?=
BCC := $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM := $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
CORE_DIR := ../../../root/retake
CRT_DIR := ../../../root/crt
BUILD_DIR := build
FLAGS := $(filter-out -opt,$(OS_BCC_DEFINES)) -opt
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.s,$(CRT_C_SRCS))
CRT_ASM_SRCS := $(CRT_DIR)/crt0.s $(filter-out $(CRT_DIR)/crt0.s,$(wildcard $(CRT_DIR)/*.s))
CORE_ASMS := $(BUILD_DIR)/save.s $(BUILD_DIR)/init.s

.PHONY: all clean FORCE
all: init

init: $(CRT_C_ASMS) $(CORE_ASMS) $(CRT_ASM_SRCS) $(BASM)
	$(BASM) -bin -o $@ $(CRT_ASM_SRCS) $(CRT_C_ASMS) $(CORE_ASMS)

$(BUILD_DIR)/init.s: init.c $(CORE_DIR)/save.h $(CORE_DIR)/game.h $(CORE_DIR)/player.h $(CORE_DIR)/takes.h $(BCC) $(BUILD_DIR)/flags
	$(BCC) $(FLAGS) -s -o $@ $<

$(BUILD_DIR)/save.s: $(CORE_DIR)/save.c $(CORE_DIR)/save.h $(CORE_DIR)/game.h $(CORE_DIR)/pak.h $(BCC) $(BUILD_DIR)/flags
	$(BCC) $(FLAGS) -s -o $@ $<

$(BUILD_DIR)/crt_%.s: $(CRT_DIR)/%.c $(wildcard $(CRT_DIR)/*.h) $(BCC) $(BUILD_DIR)/flags
	$(BCC) $(FLAGS) -s -o $@ $<

# A configuration change invalidates generated assembly without forcing all
# CRT sources to rebuild on every repeated emulator test.
$(BUILD_DIR)/flags: FORCE
	@mkdir -p $(BUILD_DIR)
	@echo '$(FLAGS)' | cmp -s - $@ || echo '$(FLAGS)' > $@

FORCE:

clean:
	rm -rf $(BUILD_DIR) init
