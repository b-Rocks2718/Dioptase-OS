#include "print.h"
#include "atomic.h"
#include "config.h"
#include "debug.h"
#include "interrupts.h"
#include "machine.h"
#include "vga.h"

/* docs/mem_map.md: byte-wide UART transmit register. */
#define UART_TX_ADDR 0x7FE5802

static struct PreemptSpinLock print_lock = { 0 };
static char* UART_PADDR = (char*)UART_TX_ADDR;

#define DECIMAL_BASE 10u
#define HEX_BASE 16u
#define MAX_INT_DEC_DIGITS 10      // ABI: int is 4 bytes, so the largest decimal magnitude has 10 digits.
#define MAX_UNSIGNED_HEX_DIGITS 8  // ABI: unsigned is 4 bytes, so hexadecimal output needs at most 8 digits.
#define DEFAULT_TEXT_COLOR 0xFF    // RGB332 white.
#define TILE_COLOR_SHIFT 8         // The framebuffer entry's high byte is RGB332 color.
#define TILE_ENTRY_BYTE_MASK 0xFFu // Both tile-index and tile-color fields are one byte.
#define TILE_HEIGHT_PIXELS 8       // Text tiles are 8x8 pixels.
#define TILE_PIXELS_PER_TILE 64
#define SOLID_TEXT_TILE_INDEX 127
#define TRANSPARENT_TILE_INDEX 255
#define DYNAMIC_TEXT_PIXEL 0xC000  // docs/mem_map.md: replace with the entry's color byte.
#define TRANSPARENT_TILE_PIXEL 0xF000
#define CONSOLE_NO_OWNER (-1)

/*
 * Console ownership and concurrency contract
 * ------------------------------------------
 * `print_lock` owns every access in this module to current_text_color,
 * vga_index, scrolling, pending_row_entry, TILE_FB, TILEMAP, TILE_SCALE,
 * TILE_HSCROLL, and TILE_VSCROLL. These objects are global across all cores. An
 * unlocked helper may therefore be called only by a caller that holds
 * print_lock.
 *
 * The architecture's memory model is sequentially consistent, and
 * PreemptSpinLock uses the project's sequentially-consistent atomic exchange,
 * so acquisition orders all state and MMIO writes after the preceding owner's
 * release. The outer owner keeps preemption disabled, which prevents a TCB
 * switch or migration while console_owner_core identifies this CPU.
 *
 * Interrupts are masked only around an atomic acquisition attempt and
 * owner/depth publication or release. A failed contender restores its caller's
 * IMR before retrying; the caller's exact IMR also remains restored while the
 * possibly long formatted output, framebuffer clear, or tileset load runs. If
 * an interrupt-side diagnostic preempts an owner on the same core, it observes
 * the published core ID and enters recursively instead of spinning on a lock
 * whose owner it interrupted. Recursive output is sent to UART and does not
 * touch partially updated VGA cursor/MMIO state. Other cores remain excluded
 * until the outermost release.
 *
 * TILE_FB is the 80x60, 16-bit tile-entry MMIO range documented at
 * 0x07FBD000..0x07FBF57F. putchar_color_unlocked() maintains vga_index in
 * [0, FB_NUM_TILES), ensuring every cursor-relative write remains in that
 * range. TILEMAP and TILE_VSCROLL have the ownership and side effects described
 * in docs/mem_map.md. Direct user mappings of these device ranges necessarily
 * bypass this kernel lock; callers using those mappings must coordinate with
 * console output themselves.
 */
static int current_text_color = DEFAULT_TEXT_COLOR;
static int vga_index = 0;
static bool scrolling = false;
// True after either an explicit newline or a circular-buffer wrap moves the
// cursor to the start of a row. The next character consumes this transition.
static bool pending_row_entry = false;

/*
 * These fields are read by every core before it owns print_lock, so all access
 * uses the project's custom sequentially-consistent atomic operations. A
 * nonnegative owner always has depth >= 1 and owns print_lock. Publication is
 * depth first, owner last; release clears owner first, depth second, then the
 * atomic lock. Same-core interrupts cannot observe either transition because
 * IMR is disabled around them.
 */
static int console_owner_core = CONSOLE_NO_OWNER;
static int console_lock_depth = 0;

static unsigned puts_unlocked(char* str);
static unsigned printf_unlocked(char* fmt, void* arr);
static unsigned printf_uart_unlocked(char* fmt, void* arr);
static unsigned print_signed_unlocked(int n);
static unsigned print_unsigned_unlocked(unsigned n);
static unsigned print_hex_unlocked(unsigned n, bool uppercase);
static void clear_screen_unlocked(void);
static void make_tiles_transparent_unlocked(void);
static void load_text_tiles_unlocked(void);
static void load_text_tiles_colored_unlocked(short fg_color, short bg_color);

/*
 * Acquire console ownership in kernel mode on any core count.
 *
 * Preconditions:
 * - The caller executes in kernel mode. It may be ordinary thread context or
 *   an interrupt nested over a console owner on this same core.
 * - Console operations do not block while the outer ownership is active.
 *
 * Postconditions:
 * - This core owns one recursion level. The outermost level has preemption
 *   disabled until its matching release, so console_owner_core stays valid.
 * - The caller's exact IMR has been restored for the console operation itself.
 * - The returned entry IMR must be passed to the matching release.
 */
static unsigned console_lock_acquire(void){
  unsigned interrupt_state = interrupts_disable();

  while (true){
    int core = (int)get_core_id();
    if (__atomic_load_n(&console_owner_core) == core){
      int previous_depth = __atomic_fetch_add(&console_lock_depth, 1);
      assert(previous_depth > 0,
        "console lock: recursive owner has invalid depth.\n");
      interrupts_restore(interrupt_state);
      return interrupt_state;
    }

    /*
     * The try-acquire keeps preemption disabled only on success. IMR remains
     * masked through owner publication, closing the only window in which a
     * same-core interrupt could see a held lock without recognizing itself as
     * the recursive owner.
     */
    if (preempt_spin_lock_try_acquire(&print_lock)){
      assert(__atomic_load_n(&console_owner_core) == CONSOLE_NO_OWNER,
        "console lock: acquired atomic lock with a published owner.\n");
      assert(__atomic_load_n(&console_lock_depth) == 0,
        "console lock: acquired atomic lock with nonzero recursion depth.\n");
      __atomic_store_n(&console_lock_depth, 1);
      __atomic_store_n(&console_owner_core, core);

      interrupts_restore(interrupt_state);
      return interrupt_state;
    }

    /*
     * Let pending interrupts and PIT preemption run between failed attempts.
     * An interrupt-side printer that enters here is waiting only for a remote
     * owner; after that owner releases it may acquire, print, and return before
     * this interrupted contender resumes. The success/publication window above
     * stays noninterruptible, so it cannot mistake this core for a remote owner.
     */
    interrupts_restore(interrupt_state);
    interrupts_disable();
  }
}

/*
 * Release console ownership and restore the caller's CPU state.
 *
 * Precondition: this core owns one recursion level, and interrupt_state is the
 * IMR returned by its matching acquisition.
 *
 * Postcondition: the caller's entry IMR is restored. A recursive release leaves
 * outer ownership and preemption state intact. The outermost release clears
 * ownership, publishes all preceding console/MMIO writes through the atomic
 * lock release, and restores the outer caller's prior preemption state.
 */
static void console_lock_release(unsigned interrupt_state){
  interrupts_disable();

  int core = (int)get_core_id();
  int owner = __atomic_load_n(&console_owner_core);
  int depth = __atomic_load_n(&console_lock_depth);
  assert(owner == core,
    "console lock: release attempted by a core that does not own the console.\n");
  assert(depth > 0,
    "console lock: owner has invalid recursion depth during release.\n");

  if (depth > 1){
    __atomic_store_n(&console_lock_depth, depth - 1);
  } else {
    __atomic_store_n(&console_owner_core, CONSOLE_NO_OWNER);
    __atomic_store_n(&console_lock_depth, 0);
    preempt_spin_lock_release(&print_lock);
  }

  interrupts_restore(interrupt_state);
}

// Direct UART output is deliberately lock-free for the fatal panic path.
void putchar_uart(char c){
  *UART_PADDR = c;
}

/* Caller must hold print_lock; color is RGB332. */
static void putchar_color_unlocked(char c, int color){
  if (CONFIG.use_vga){
    /*
     * The depth is published with interrupts masked. Depth greater than one
     * therefore identifies a diagnostic nested over a same-core owner. Send
     * that byte to UART: the interrupted outer operation may be between any two
     * cursor/MMIO updates and must remain the only VGA state machine writer.
     */
    if (__atomic_load_n(&console_lock_depth) > 1){
      *UART_PADDR = c;
      return;
    }

    if (pending_row_entry){
      if (scrolling){
        // Entering a reused circular-buffer row must clear its old tiles and
        // advance the hardware viewport exactly once. A full final row can
        // reach this state without a newline, so wrap and newline share this
        // pending transition.
        for (int i = 0; i < TILE_ROW_WIDTH; ++i){
          TILE_FB[vga_index + i] = 0;
        }
        *TILE_VSCROLL = *TILE_VSCROLL - TILE_HEIGHT_PIXELS;
      }
      pending_row_entry = false;
    }

    if (c == '\n'){
      vga_index++;
      // round up to next row
      vga_index = ((vga_index + TILE_ROW_WIDTH - 1) / TILE_ROW_WIDTH) * TILE_ROW_WIDTH;

      pending_row_entry = true;
    } else {
      unsigned entry =
        (((unsigned)color & TILE_ENTRY_BYTE_MASK) << TILE_COLOR_SHIFT)
        | ((unsigned)c & TILE_ENTRY_BYTE_MASK);
      TILE_FB[vga_index++] = (short)entry;
    }

    if (vga_index >= FB_NUM_TILES) {
      // The circular cursor reached the top row. Defer clearing and viewport
      // movement until a following character actually enters that row.
      vga_index -= FB_NUM_TILES;
      scrolling = true;
      pending_row_entry = true;
    }

  } else {
    // just write to UART, ignoring color
    *UART_PADDR = c;
  }
}

/* Caller must hold print_lock. */
static void putchar_unlocked(char c){
  putchar_color_unlocked(c, current_text_color);
}

// Write one character while serializing console and VGA state.
void putchar(char c){
  unsigned interrupt_state = console_lock_acquire();
  putchar_unlocked(c);
  console_lock_release(interrupt_state);
}

// Write one character using the requested RGB332 text color.
void putchar_color(char c, int color){
  unsigned interrupt_state = console_lock_acquire();
  putchar_color_unlocked(c, color);
  console_lock_release(interrupt_state);
}

// Write up to count characters to the active console.
unsigned console_write(char* buffer, unsigned count){
  unsigned interrupt_state = console_lock_acquire();
  for (unsigned i = 0; i < count; ++i){
    putchar_unlocked(buffer[i]);
  }
  console_lock_release(interrupt_state);
  return count;
}

// Set the active console's RGB332 foreground color.
void console_set_text_color(int color){
  unsigned interrupt_state = console_lock_acquire();
  current_text_color = color;
  console_lock_release(interrupt_state);
}

// Set the VGA tile scale used by subsequent console output.
void console_set_tile_scale(int scale){
  unsigned interrupt_state = console_lock_acquire();
  *TILE_SCALE = scale;
  console_lock_release(interrupt_state);
}

// Set the VGA tile vertical scroll offset.
void console_set_tile_vscroll(int scroll){
  unsigned interrupt_state = console_lock_acquire();
  *TILE_VSCROLL = scroll;
  console_lock_release(interrupt_state);
}

// Set the VGA tile horizontal scroll offset.
void console_set_tile_hscroll(int scroll){
  unsigned interrupt_state = console_lock_acquire();
  *TILE_HSCROLL = scroll;
  console_lock_release(interrupt_state);
}

// Adjust the VGA tile vertical scroll offset by delta.
void console_move_tile_vscroll(int delta){
  unsigned interrupt_state = console_lock_acquire();

  // The hardware register is MMIO, so no CPU atomic RMW exists for it. The
  // global console owner excludes every other core, and interrupt diagnostics
  // recursively use UART without touching this VGA state.
  *TILE_VSCROLL += delta;

  console_lock_release(interrupt_state);
}

// Adjust the VGA tile horizontal scroll offset by delta.
void console_move_tile_hscroll(int delta){
  unsigned interrupt_state = console_lock_acquire();

  // See console_move_tile_vscroll(): ownership covers the complete 16-bit MMIO
  // read/modify/write while same-core nested diagnostics use UART.
  *TILE_HSCROLL += delta;

  console_lock_release(interrupt_state);
}

// Return the active console's current RGB332 text color.
int console_get_text_color(void){
  unsigned interrupt_state = console_lock_acquire();
  int color = current_text_color;
  console_lock_release(interrupt_state);
  return color;
}

// Return whether c is an ASCII decimal digit.
bool isnum(char c){
  return ('0' <= c && c <= '9');
}

/* Caller must hold print_lock. */
static unsigned puts_unlocked(char* str){
  unsigned count = 0;
  while (*str != '\0'){
    putchar_unlocked(*str);
    ++str;
    ++count;
  }
  return count;
}

// Write a null-terminated string while holding the console lock.
unsigned puts(char* str){
  unsigned interrupt_state = console_lock_acquire();
  unsigned count = puts_unlocked(str);
  console_lock_release(interrupt_state);
  return count;
}

// Direct, panic-safe UART output; intentionally bypasses print_lock.
unsigned puts_uart(char* str){
  unsigned count = 0;
  while (*str != '\0'){
    putchar_uart(*str);
    ++str;
    ++count;
  }
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output
unsigned say(char* fmt, void* arr){
  return printf(fmt, arr);
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output
// ignores CONFIG.use_vga
unsigned say_uart(char* fmt, void* arr){
  return printf_uart(fmt, arr);
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output and allows specifying text color
unsigned say_color(char* fmt, void* arr, int color){
  unsigned interrupt_state = console_lock_acquire();
  int old_color = current_text_color;
  current_text_color = color;
  unsigned count = printf_unlocked(fmt, arr);
  current_text_color = old_color;
  console_lock_release(interrupt_state);
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// caller must hold print_lock
static unsigned printf_unlocked(char* fmt, void* arr){
  unsigned count = 0;
  unsigned i = 0;
  while (*fmt != '\0'){
    if (*fmt == '%') {
      if (*(fmt + 1) == 'd') {
        ++fmt;
        count += print_signed_unlocked(((int*)arr)[i++]);
      } else if (*(fmt + 1) == 'u') {
        ++fmt;
        count += print_unsigned_unlocked(((unsigned*)arr)[i++]);
      } else if (*(fmt + 1) == 'x') {
        ++fmt;
        count += print_hex_unlocked(((unsigned*)arr)[i++], false);
      } else if (*(fmt + 1) == 'X') {
        ++fmt;
        count += print_hex_unlocked(((unsigned*)arr)[i++], true);
      } else if (*(fmt + 1) == 's') {
        ++fmt;
        count += puts_unlocked((char*)((void**)arr)[i++]);
      } else if (*(fmt + 1) == 'c') {
        ++fmt;
        putchar_unlocked(((unsigned*)arr)[i++]);
        ++count;
      } else if (*(fmt + 1) == '%') {
        ++fmt;
        putchar_unlocked('%');
        ++count;
      } else {
        // unsupported format specifier, print as is
        putchar_unlocked(*fmt);
        ++count;
      }
    } else {
      putchar_unlocked(*fmt);
      ++count;
    }
    ++fmt;
  }
  return count;
}

// Format and write text to the active console.
unsigned printf(char* fmt, void* arr){
  unsigned interrupt_state = console_lock_acquire();
  unsigned count = printf_unlocked(fmt, arr);
  console_lock_release(interrupt_state);
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// caller must hold print_lock; ignores CONFIG.use_vga
static unsigned printf_uart_unlocked(char* fmt, void* arr){
  unsigned count = 0;
  unsigned i = 0;
  while (*fmt != '\0'){
    if (*fmt == '%') {
      if (*(fmt + 1) == 'd') {
        ++fmt;
        count += print_signed_uart(((int*)arr)[i++]);
      } else if (*(fmt + 1) == 'u') {
        ++fmt;
        count += print_unsigned_uart(((unsigned*)arr)[i++]);
      } else if (*(fmt + 1) == 'x') {
        ++fmt;
        count += print_hex_uart(((unsigned*)arr)[i++], false);
      } else if (*(fmt + 1) == 'X') {
        ++fmt;
        count += print_hex_uart(((unsigned*)arr)[i++], true);
      } else if (*(fmt + 1) == 's') {
        ++fmt;
        count += puts_uart((char*)((void**)arr)[i++]);
      } else if (*(fmt + 1) == 'c') {
        ++fmt;
        putchar_uart(((unsigned*)arr)[i++]);
        ++count;
      } else if (*(fmt + 1) == '%') {
        ++fmt;
        putchar_uart('%');
        ++count;
      } else {
        // unsupported format specifier, print as is
        putchar_uart(*fmt);
        ++count;
      }
    } else {
      putchar_uart(*fmt);
      ++count;
    }
    ++fmt;
  }
  return count;
}

// Format text directly to UART without consulting VGA state.
unsigned printf_uart(char* fmt, void* arr){
  unsigned interrupt_state = console_lock_acquire();
  unsigned count = printf_uart_unlocked(fmt, arr);
  console_lock_release(interrupt_state);
  return count;
}

// print the number n to the console as a signed decimal
// returns the number of characters printed
// caller must hold print_lock
static unsigned print_signed_unlocked(int n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned magnitude;
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar_unlocked('0');
    return 1;
  }

  if(n < 0){
    putchar_unlocked('-');
    magnitude = 0u - (unsigned)n;
  } else {
    magnitude = (unsigned)n;
  }

  while (magnitude != 0){
    digits[len++] = (char)('0' + (magnitude % DECIMAL_BASE));
    magnitude /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar_unlocked(digits[--len]);
  }
  
  // return number of characters printed
  return (n < 0) ? (count + 1) : count;
}

// Format one signed integer to the active console.
unsigned print_signed(int n){
  unsigned interrupt_state = console_lock_acquire();
  unsigned count = print_signed_unlocked(n);
  console_lock_release(interrupt_state);
  return count;
}

// print the number n to the console as a signed decimal
// returns the number of characters printed
// ignores CONFIG.use_vga
unsigned print_signed_uart(int n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned magnitude;
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar_uart('0');
    return 1;
  }

  if(n < 0){
    putchar_uart('-');
    magnitude = 0u - (unsigned)n;
  } else {
    magnitude = (unsigned)n;
  }

  while (magnitude != 0){
    digits[len++] = (char)('0' + (magnitude % DECIMAL_BASE));
    magnitude /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar_uart(digits[--len]);
  }
  
  // return number of characters printed
  return (n < 0) ? (count + 1) : count;
}

// print the number n to the console as an unsigned decimal
// returns the number of characters printed
// caller must hold print_lock
static unsigned print_unsigned_unlocked(unsigned n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar_unlocked('0');
    return 1;
  }

  while (n != 0){
    digits[len++] = (char)('0' + (n % DECIMAL_BASE));
    n /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar_unlocked(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}

// Format one unsigned integer to the active console.
unsigned print_unsigned(unsigned n){
  unsigned interrupt_state = console_lock_acquire();
  unsigned count = print_unsigned_unlocked(n);
  console_lock_release(interrupt_state);
  return count;
}

// print the number n to the console as an unsigned decimal
// returns the number of characters printed
// ignores CONFIG.use_vga
unsigned print_unsigned_uart(unsigned n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar_uart('0');
    return 1;
  }

  while (n != 0){
    digits[len++] = (char)('0' + (n % DECIMAL_BASE));
    n /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar_uart(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}

// print the number n to the console as a hexadecimal
// returns the number of characters printed
// caller must hold print_lock
static unsigned print_hex_unlocked(unsigned n, bool uppercase){
  char digits[MAX_UNSIGNED_HEX_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar_unlocked('0');
    return 1;
  }

  while (n != 0){
    unsigned digit = n % HEX_BASE;

    if (digit < DECIMAL_BASE){
      digits[len++] = (char)('0' + digit);
    } else {
      digits[len++] = (char)((uppercase ? 'A' : 'a') + (digit - DECIMAL_BASE));
    }

    n /= HEX_BASE;
  }

  count = len;
  while (len != 0){
    putchar_unlocked(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}

// Format one unsigned integer in hexadecimal on the active console.
unsigned print_hex(unsigned n, bool uppercase){
  unsigned interrupt_state = console_lock_acquire();
  unsigned count = print_hex_unlocked(n, uppercase);
  console_lock_release(interrupt_state);
  return count;
}

// print the number n to the console as a hexadecimal
// returns the number of characters printed
// ignores CONFIG.use_vga
unsigned print_hex_uart(unsigned n, bool uppercase){
  char digits[MAX_UNSIGNED_HEX_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar_uart('0');
    return 1;
  }

  while (n != 0){
    unsigned digit = n % HEX_BASE;

    if (digit < DECIMAL_BASE){
      digits[len++] = (char)('0' + digit);
    } else {
      digits[len++] = (char)((uppercase ? 'A' : 'a') + (digit - DECIMAL_BASE));
    }

    n /= HEX_BASE;
  }

  count = len;
  while (len != 0){
    putchar_uart(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}

// load text mode tiles and initialize VGA text mode
void vga_text_init(void){
  if (CONFIG.use_vga){
    unsigned interrupt_state = console_lock_acquire();
    load_text_tiles_unlocked(); // text tileset is included in kernel image
    *TILE_SCALE = 0;
    *TILE_VSCROLL = 0;
    console_lock_release(interrupt_state);
  }
}

/* Caller must hold print_lock. */
static void clear_screen_unlocked(void){
  for (int i = 0; i < FB_NUM_TILES; ++i){
    TILE_FB[i] = 0;
  }

  // Same-core nested diagnostics use UART, so this cursor reset cannot race
  // them even though the caller's original IMR remains installed.
  vga_index = 0;
  scrolling = false;
  pending_row_entry = false;
}

// clear the screen of all text characters and reset scroll/cursor state
void clear_screen(void){
  unsigned interrupt_state = console_lock_acquire();
  clear_screen_unlocked();
  console_lock_release(interrupt_state);
}

// Configure the active tile set so its transparent color is enabled.
void console_make_tiles_transparent(void){
  unsigned interrupt_state = console_lock_acquire();
  make_tiles_transparent_unlocked();
  console_lock_release(interrupt_state);
}

/* Caller must hold print_lock; the caller's original IMR remains installed. */
static void make_tiles_transparent_unlocked(void){
  for (int i = 0; i < FB_NUM_TILES; ++i){
    TILE_FB[i] = TRANSPARENT;
  }
}

/* Caller must hold print_lock. */
static void load_text_tiles_unlocked(void){
  // text_tiles lists pixels that hardware replaces with the entry's color.
  for (int i = 0; i < TILEMAP_PIXELS; ++i){
    TILEMAP[i] = 0;
  }

  clear_screen_unlocked();

  int i = 0;
  int offset = text_tiles[i];
  while (offset != 0){
    TILEMAP[offset] = DYNAMIC_TEXT_PIXEL;
    offset = text_tiles[i++];
  }

  for (int i = 0; i < TILE_PIXELS_PER_TILE; ++i){
    TILEMAP[SOLID_TEXT_TILE_INDEX * TILE_PIXELS_PER_TILE + i] =
      DYNAMIC_TEXT_PIXEL;
  }

  for (int i = 0; i < TILE_PIXELS_PER_TILE; ++i){
    TILEMAP[TRANSPARENT_TILE_INDEX * TILE_PIXELS_PER_TILE + i] =
      TRANSPARENT_TILE_PIXEL;
  }

}

// set the current tileset to the text mode tileset and clear the screen
void load_text_tiles(void){
  unsigned interrupt_state = console_lock_acquire();
  load_text_tiles_unlocked();
  console_lock_release(interrupt_state);
}

/* Caller must hold print_lock. */
static void load_text_tiles_colored_unlocked(short fg_color, short bg_color){
  // text_tiles lists pixels that should use the requested foreground color.
  for (int i = 0; i < TILEMAP_PIXELS; ++i){
    TILEMAP[i] = bg_color;
  }

  clear_screen_unlocked();

  int i = 0;
  int offset = text_tiles[i];
  while (offset != 0){
    TILEMAP[offset] = fg_color;
    offset = text_tiles[i++];
  }

  for (int i = 0; i < TILE_PIXELS_PER_TILE; ++i){
    TILEMAP[TRANSPARENT_TILE_INDEX * TILE_PIXELS_PER_TILE + i] =
      TRANSPARENT_TILE_PIXEL;
  }

}

// set the current tileset to the text mode tileset with the given text and background colors,
// then clear the screen
void load_text_tiles_colored(short fg_color, short bg_color){
  unsigned interrupt_state = console_lock_acquire();
  load_text_tiles_colored_unlocked(fg_color, bg_color);
  console_lock_release(interrupt_state);
}
