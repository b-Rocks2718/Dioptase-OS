# Default to the repo-local toolchain so direct `make` in this directory does
# not depend on env.sh or a runner-global PATH setup.
VERSION ?= release
# Optional -D flags from the top-level OS Makefile (e.g. -DOS_RELEASE=1).
OS_BCC_DEFINES ?=
TOOLCHAIN_ROOT ?= ../../../../
CC = $(TOOLCHAIN_ROOT)Dioptase-Languages/Dioptase-C-Compiler/build/$(VERSION)/bcc
BASM = $(TOOLCHAIN_ROOT)Dioptase-Assembler/build/$(VERSION)/basm
BUILD_DIR = build

CRT_DIR = ../../../root/crt
CRT_STARTUP_SRC := $(CRT_DIR)/crt0.s
CRT_ASM_SRCS := $(wildcard $(CRT_DIR)/*.s)
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.gen.s,$(CRT_C_SRCS))
CRT_ASM_SRCS_ORDERED := $(CRT_STARTUP_SRC) \
	$(filter-out $(CRT_STARTUP_SRC),$(CRT_ASM_SRCS))

SHELL_DIR = ../../../root/shell
SHELL_C_SRCS := $(SHELL_DIR)/dirs.c $(SHELL_DIR)/shell.c
SHELL_C_ASMS := $(patsubst $(SHELL_DIR)/%.c,$(BUILD_DIR)/shell_%.s,$(SHELL_C_SRCS))

C_SRCS := $(wildcard *.c)
C_ASMS := $(patsubst %.c,$(BUILD_DIR)/%.s,$(C_SRCS))
LOCAL_ASM_SRCS := $(filter-out $(C_SRCS:.c=.s),$(wildcard *.s))
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(SHELL_C_ASMS) \
	$(LOCAL_ASM_SRCS) $(C_ASMS)

.PHONY: all clean

all: init

init: $(LINK_ASM_SRCS) Makefile | $(BUILD_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BUILD_DIR)/%.s: %.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/init.s: $(SHELL_DIR)/shell.h $(SHELL_DIR)/dirs.h

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/shell_%.s: $(SHELL_DIR)/%.c Makefile | $(BUILD_DIR)
	$(CC) $(OS_BCC_DEFINES) -s -o $@ $<

$(BUILD_DIR)/shell_dirs.s: $(SHELL_DIR)/dirs.h

# Link the production command handlers without the interactive shell entry.
$(BUILD_DIR)/shell_shell.s: $(SHELL_DIR)/shell.c $(SHELL_DIR)/shell.h \
	$(SHELL_DIR)/dirs.h Makefile | $(BUILD_DIR)
	$(CC) -DSHELL_LIBRARY_ONLY -s -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f init
	rm -rf $(BUILD_DIR)
