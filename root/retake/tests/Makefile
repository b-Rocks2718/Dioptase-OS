# The pure simulation is compiled unchanged by the host and guest harnesses.
HOST_CC ?= cc
BUILD_DIR := build
CORE := ../player.c ../takes.c
HEADERS := ../player.h ../takes.h host.h
HOST_FLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g
SANITIZERS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: test clean
test: $(BUILD_DIR)/core_test
	./$(BUILD_DIR)/core_test

$(BUILD_DIR)/core_test: test_core.c $(CORE) $(HEADERS) Makefile
	mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) $(SANITIZERS) $(CORE) test_core.c -o $@

clean:
	rm -rf $(BUILD_DIR)
