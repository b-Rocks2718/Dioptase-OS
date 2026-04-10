CC = bcc
CRT_DIR = ../../../crt

SRCS := $(wildcard *.c)

# compile all .c files in the directory
init: $(SRCS) Makefile
	$(CC) -crt $(CRT_DIR) $< -o $@ -bin

clean:
	rm -f init
