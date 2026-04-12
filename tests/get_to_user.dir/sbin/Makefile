CC = bcc

CRT_DIR = ../../../crt
CRT_SRCS := $(wildcard $(CRT_DIR)/*.s)

SRCS := $(wildcard *.c)

# compile all .c files in the directory
init: $(SRCS) $(CRT_SRCS) Makefile
	$(CC) -crt $(CRT_DIR) $< -o $@ -bin

clean:
	rm -f init
