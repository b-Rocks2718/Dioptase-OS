# The pure simulation is compiled unchanged by the host and guest harnesses.
HOST_CC ?= cc
BUILD_DIR := build
CORE := ../player.c ../takes.c
HEADERS := ../player.h ../takes.h host.h
HOST_FLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g
SANITIZERS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: test clean
test: $(BUILD_DIR)/core_test $(BUILD_DIR)/game_test $(BUILD_DIR)/save_test
	./$(BUILD_DIR)/core_test
	./$(BUILD_DIR)/game_test
	./$(BUILD_DIR)/save_test

$(BUILD_DIR)/save_test: test_save.c save_io.h sim.c sim.h ../save.c ../save.h ../game.c ../game.h ../retake.h ../pak.h $(CORE) $(HEADERS) Makefile
	mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) $(SANITIZERS) $(CORE) test_save.c -o $@

$(BUILD_DIR)/game_test: test_game.c sim.c sim.h ../game.c ../game.h ../retake.h ../pak.h $(CORE) $(HEADERS) Makefile
	mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) $(SANITIZERS) $(CORE) test_game.c -o $@

# ctypes authoring uses the same sources; committed routes run with UBSan too.
$(BUILD_DIR)/sim.so: sim.c sim.h ../game.c ../game.h ../retake.h ../pak.h $(CORE) $(HEADERS) Makefile
	mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) -O2 -fPIC -shared -fsanitize=undefined -fno-sanitize-recover=all $(CORE) sim.c -o $@

$(BUILD_DIR)/core_test: test_core.c $(CORE) $(HEADERS) Makefile
	mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(HOST_FLAGS) $(SANITIZERS) $(CORE) test_core.c -o $@

clean:
	rm -rf $(BUILD_DIR)
