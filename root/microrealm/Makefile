VERSION ?= release
OS_BCC_DEFINES ?=
BCC_FLAGS := $(filter-out -opt,$(OS_BCC_DEFINES)) -opt
TOOLCHAIN_ROOT ?= ../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build
BCC_STAMP := $(BUILD_DIR)/bcc_flags
PROGRAM ?= $(notdir $(CURDIR))
SBIN_DIR := ../sbin
PROGRAM_OUTPUT := $(SBIN_DIR)/$(PROGRAM)

CRT_DIR = ../crt
CRT_STARTUP_SRC := $(CRT_DIR)/crt0.s
CRT_ASM_SRCS := $(wildcard $(CRT_DIR)/*.s)
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.gen.s,$(CRT_C_SRCS))
CRT_ASM_SRCS_ORDERED := $(CRT_STARTUP_SRC) \
	$(filter-out $(CRT_STARTUP_SRC),$(CRT_ASM_SRCS))

C_SRCS := $(wildcard *.c)
LEGACY_C_ASMS := $(C_SRCS:.c=.s)
LEGACY_CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,crt_%.gen.s,$(CRT_C_SRCS))
C_ASMS := $(patsubst %.c,$(BUILD_DIR)/%.s,$(C_SRCS))
LOCAL_ASM_SRCS := $(filter-out $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS),$(wildcard *.s))
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(LOCAL_ASM_SRCS) $(C_ASMS)

.PHONY: all clean $(PROGRAM)

all: $(PROGRAM)

$(PROGRAM): $(PROGRAM_OUTPUT)
	@:

$(PROGRAM_OUTPUT): $(LINK_ASM_SRCS) Makefile | $(BUILD_DIR) $(SBIN_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BUILD_DIR)/%.s: %.c $(wildcard *.h) Makefile $(CC) $(BCC_STAMP) | $(BUILD_DIR)
	$(CC) $(BCC_FLAGS) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c Makefile $(CC) $(BCC_STAMP) | $(BUILD_DIR)
	$(CC) $(BCC_FLAGS) -s -o $@ $<

$(BCC_STAMP): FORCE | $(BUILD_DIR)
	@echo '$(BCC_FLAGS)' | cmp -s - $@ || echo '$(BCC_FLAGS)' > $@

$(BUILD_DIR):
	mkdir -p $@

$(SBIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(PROGRAM_OUTPUT)

FORCE:
