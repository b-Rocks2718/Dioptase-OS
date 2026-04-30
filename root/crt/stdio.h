#ifndef STDIO_H
#define STDIO_H

#include "stddef.h"
#include "unistd.h"

/*
 * Dioptase userland does not expose a libc FILE object. This bootstrap layer
 * stores each stream as one integer file descriptor so compiler code can keep
 * using fopen/fclose/fputc/fputs/fwrite while the remaining host-style stdio
 * call sites are ported to the CRT print/sys helpers.
 * The bootstrap compiler does not accept typedef, so FILE stays a macro alias.
 */
#define FILE int

extern FILE __stdin_storage;
extern FILE __stdout_storage;
extern FILE __stderr_storage;

#define stdin  (&__stdin_storage)
#define stdout (&__stdout_storage)
#define stderr (&__stderr_storage)

/*
 * These helpers behave like the usual stdio byte/string output entry points.
 * Raw file-descriptor formatting helpers live in print.h instead of stdio.h.
 */
void putchar(char c);
unsigned puts(char* str);

FILE* fopen(char* path, char* mode);
int fclose(FILE* stream);
int fputc(int c, FILE* stream);
int fputs(char* str, FILE* stream);
size_t fread(void* ptr, size_t size, size_t count, FILE* stream);
size_t fwrite(void* ptr, size_t size, size_t count, FILE* stream);
int fseek(FILE* stream, int offset, int whence);
int ftell(FILE* stream);

#endif // STDIO_H
