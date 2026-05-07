# Default to the repo-local toolchain so direct `make` in this directory does
# not depend on env.sh or a runner-global PATH setup.
VERSION ?= release
TOOLCHAIN_ROOT ?= ../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BUILD_DIR = build

C_SRCS := $(wildcard *.c)
ASM_SRCS := $(wildcard *.s)

C_BASENAMES := $(basename $(notdir $(C_SRCS)))
ASM_BASENAMES := $(basename $(notdir $(ASM_SRCS)))

MERGED_BASENAMES := $(sort $(filter $(ASM_BASENAMES),$(C_BASENAMES)))
C_ONLY_BASENAMES := $(sort $(filter-out $(ASM_BASENAMES),$(C_BASENAMES)))
ASM_ONLY_BASENAMES := $(sort $(filter-out $(C_BASENAMES),$(ASM_BASENAMES)))

MERGED_ASMS := $(addprefix $(BUILD_DIR)/,$(addsuffix .s,$(MERGED_BASENAMES)))
C_ONLY_ASMS := $(addprefix $(BUILD_DIR)/,$(addsuffix .s,$(C_ONLY_BASENAMES)))
ASM_ONLY_ASMS := $(addprefix $(BUILD_DIR)/,$(addsuffix .s,$(ASM_ONLY_BASENAMES)))
ALL_ASMS := $(ASM_ONLY_ASMS) $(MERGED_ASMS) $(C_ONLY_ASMS)

.PHONY: all clean

all: $(ALL_ASMS)

$(ASM_ONLY_ASMS): $(BUILD_DIR)/%.s: %.s Makefile | $(BUILD_DIR)
	cp $< $@

$(C_ONLY_ASMS): $(BUILD_DIR)/%.s: %.c Makefile | $(BUILD_DIR)
	$(CC) -s -o $@ $<

# When both foo.c and foo.s exist, the generated build/foo.s needs both pieces.
# Compile the C source first, then append the handwritten assembly so callers
# still consume a single foo.s output in build/.
$(MERGED_ASMS): $(BUILD_DIR)/%.s: %.c %.s Makefile | $(BUILD_DIR)
	$(CC) -s -o $@.tmp $<
	cat $(word 2,$^) >> $@.tmp
	mv $@.tmp $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
