CC = bcc
BASM = basm
BUILD_DIR = build

CRT_DIR = ../../../crt
CRT_STARTUP_SRC := $(CRT_DIR)/crt0.s
CRT_ASM_SRCS := $(wildcard $(CRT_DIR)/*.s)
CRT_C_SRCS := $(wildcard $(CRT_DIR)/*.c)
CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,$(BUILD_DIR)/crt_%.gen.s,$(CRT_C_SRCS))
# Keep crt0.s first so _start becomes the entry point in the flat binary.
CRT_ASM_SRCS_ORDERED := $(CRT_STARTUP_SRC) \
	$(filter-out $(CRT_STARTUP_SRC),$(CRT_ASM_SRCS))

C_SRCS := $(wildcard *.c)
LEGACY_C_ASMS := $(C_SRCS:.c=.s)
LEGACY_CRT_C_ASMS := $(patsubst $(CRT_DIR)/%.c,crt_%.gen.s,$(CRT_C_SRCS))
C_ASMS := $(patsubst %.c,$(BUILD_DIR)/%.s,$(C_SRCS))
LOCAL_ASM_SRCS := $(filter-out $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS),$(wildcard *.s))
LINK_ASM_SRCS := $(CRT_ASM_SRCS_ORDERED) $(CRT_C_ASMS) $(LOCAL_ASM_SRCS) $(C_ASMS)

.PHONY: all clean

all: init

init: $(LINK_ASM_SRCS) Makefile | $(BUILD_DIR)
	$(BASM) -bin -o $@ $(LINK_ASM_SRCS)

$(BUILD_DIR)/%.s: %.c Makefile | $(BUILD_DIR)
	$(CC) -s -o $@ $<

$(BUILD_DIR)/crt_%.gen.s: $(CRT_DIR)/%.c Makefile | $(BUILD_DIR)
	$(CC) -s -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f init
	rm -f $(LEGACY_C_ASMS) $(LEGACY_CRT_C_ASMS)
	rm -rf $(BUILD_DIR)
