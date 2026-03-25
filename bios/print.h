#ifndef PRINT_H
#define PRINT_H

// The assembler exports a NUL-terminated list of tilemap offsets; the C side
// only needs an array declaration to reference the symbol.
extern short text_tiles[42];

// Print a single character to the BIOS console using the current text color.
void putchar(char c);

// Print one character with an explicit RGB332 tile color.
// In headless mode the BIOS falls back to UART and ignores the color.
void putchar_color(char c, int color);

// print the number n to the console in decimal
// returns the number of characters printed
int print_num(int n);

// Print a NUL-terminated string to the BIOS console.
int puts(char* str);

// Clear the tile framebuffer and reset BIOS scroll/cursor state.
void clear_screen(void);

// Load BIOS text glyphs and initialize the VGA text layer when enabled.
void vga_text_init(void);

// Program the tilemap with BIOS text glyphs and reserve the transparent tile.
void load_text_tiles(void);

#endif // PRINT_H
