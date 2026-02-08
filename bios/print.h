#ifndef PRINT_H
#define PRINT_H

extern short text_tiles[42]; // size is ignored, just there for compiler

void putchar(char c);

void putchar_color(char c, int color);

// print the number n to the console in decimal
// returns the number of characters printed
int print_num(int n);

int puts(char* str);

void clear_screen(void);

void vga_text_init(void);

void load_text_tiles(void);

#endif // PRINT_H