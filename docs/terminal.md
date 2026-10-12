## Terminal

### Display model

- The terminal renders to the tile framebuffer returned by `get_tile_fb()`.
- The terminal reads shell/program output from `STDIN` and renders those bytes.
- The terminal reads raw keyboard events with `getkey()` and writes translated
  input bytes to `STDOUT`, which `/sbin/init` connects to the shell's `STDIN`.
- In raw keyboard mode it instead writes every key press and release as a
  two-byte record (see "Raw keyboard mode").
- The visible screen is `80` columns by `60` rows.
- Each character cell is one `8x8` tile.
- The low byte of each tile framebuffer entry is the tile index.
- The high byte of each tile framebuffer entry is the tile color.
- The terminal loads the built-in text tileset with `load_text_tiles()`.
- The terminal sets tile scale to `0` and initializes tile horizontal and
  vertical scroll to `0`.

### Text rendering

- All bytes except newline and recognized escape sequences are rendered as tile
  indices directly.
- Carriage return (`\r`) moves the cursor to column `0` of the current visible
  row.
- Backspace (`\b`) moves the cursor left by one column within the current
  visible row. It does not erase the character already stored on screen.
- Horizontal tab (`\t`) expands to spaces until the next `8`-column tab stop.
- Text color is controlled by the current SGR state. The terminal currently only
  models one color byte for text output.
- The cursor position is tracked in visible screen coordinates, not raw
  framebuffer coordinates. This matters once the screen has scrolled.

### Keyboard translation

- Key release events update terminal modifier state and do not produce input
  bytes.
- Left and right Shift modify printable letters, number-row keys, and supported
  punctuation before the byte is written to `STDOUT`.
- Left and right Ctrl translate Ctrl-letter combinations into ASCII control
  bytes.
- Ctrl-C calls `signal_foreground(SIGNAL_TERMINATE)`. If no live
  foreground child is set, Ctrl-C writes ASCII ETX (`0x03`) to `STDOUT` so the
  shell prompt can cancel its current input buffer.
- Enter writes newline (`\n`).
- Printable ASCII, Escape, Backspace, Delete, and Tab are forwarded as one
  byte.
- Arrow keys are forwarded using the existing Dioptase `KEY_LEFT`,
  `KEY_RIGHT`, `KEY_UP`, and `KEY_DOWN` byte values.
- Other raw keycodes are ignored.

### Raw keyboard mode

Programs that need to know how long keys are held (games) can ask the terminal
to stop translating keys with the Dioptase-private DEC-style mode `68`:
`ESC [ ? 68 h` turns raw mode on and `ESC [ ? 68 l` turns it off
(`TERMINAL_RAW_KEYBOARD_ON` / `TERMINAL_RAW_KEYBOARD_OFF` in
`root/crt/terminal.h`). The mode takes effect when the terminal processes the
sequence; key events already forwarded stay in the input pipe as they were.

While raw mode is on, every keyboard event, presses and releases alike, is
written to `STDOUT` as a two-byte record:

| Byte | Meaning |
| --- | --- |
| `0` | Keycode as returned by `getkey()` without the release bit (see `../../docs/mem_map.md`; names in `root/crt/ps2.h`) |
| `1` | `1` (`TERMINAL_RAW_RELEASE`) for a release, `0` for a press |

No Shift or Ctrl translation is applied; programs see the modifier keys as
records of their own. If the input pipe has no room for a whole record, the
terminal drops that event instead of blocking, so a program that stops reading
cannot stall Ctrl-C. The terminal still tracks modifier state, and Ctrl-C is
the one exception: it is not forwarded but turns raw mode off and then behaves
as in cooked mode (it terminates the foreground child, or writes `0x03` if
there is none). Turning raw mode off first means a killed program cannot leave
the keyboard raw, and the releases that follow the Ctrl-C are dropped as
cooked input instead of reaching the shell. A program that handles
`SIGNAL_TERMINATE` and keeps running must turn raw mode on again.

Raw mode is also turned off by the shell after every foreground program exits
(see `shell.md`) and by foreground display recovery.

#### Keyboard sync

`ESC [ 68 n` (`TERMINAL_KEYBOARD_SYNC`) asks the terminal to write two `0xFF`
bytes to the input pipe when it processes the request. Because the terminal
handles program output in order and writes key input to the same pipe, the
reply marks the point in the input stream where every request written before
the query, such as a raw-mode change, had taken effect: input before the reply
may be in the old form, input after it is in the new one. `0xFF` is neither a
guest keycode nor a cooked input byte, and the second byte of a raw record is
`0` or `1`, so the reply cannot be confused with input or straddle a record.

`set_keyboard_raw(enabled)` in the crt (`root/crt/keyboard.c`) is the usual
way to use both: it writes the mode request and the sync request, then reads
and discards input up to and including the reply. It returns `0` once the
switch has taken effect, or `-1` when no terminal answers (`STDIN` or
`STDOUT` is not a pipe, or no reply within one second).

### Cursor and scrolling behavior

- A newline moves the cursor to column `0` of the next visible row.
- A newline on the last visible row scrolls the screen upward by one text row.
- Scrolling is implemented by advancing the tile vertical-scroll register one
  tile row at a time and treating the tile framebuffer as a circular backing
  store.
- Writing a printable character into the last column of a non-bottom row wraps
  immediately to the first column of the next row.
- Writing a printable character into the bottom-right cell uses delayed wrap:
  the character remains visible in the last cell, and the actual wrap/scroll
  happens when the next printable character arrives.
- A newline clears any pending delayed wrap before advancing. This prevents
  double-scroll behavior after writing the bottom-right cell.
- Carriage return and backspace also clear any pending delayed wrap before
  moving the cursor.
- Tab output uses ordinary space characters, so it follows the same wrapping and
  scrolling rules as any other printable text.
- `CSI A`, `B`, `C`, `D`, `H`, and `f` cancel any pending delayed wrap before
  moving the cursor.
- `CSI s` and `CSI u` save and restore both the visible cursor position and the
  delayed-wrap state.

### Cursor appearance

- The cursor is drawn by temporarily replacing the tile under the cursor with
  tile `0x7F`.
- When visible, the cursor blinks every `10` jiffies according to
  `get_current_jiffies()`.
- `CSI ?25l` hides the cursor.
- `CSI ?25h` shows the cursor.

### Supported escape sequences

Only CSI sequences of the form `ESC [` are recognized. The terminal currently
supports the following final bytes:

| Sequence | Meaning | Notes |
| --- | --- | --- |
| `ESC [ A` | Move cursor up by `1` row | `ESC [ n A` uses decimal `n` |
| `ESC [ B` | Move cursor down by `1` row | `ESC [ n B` uses decimal `n` |
| `ESC [ C` | Move cursor right by `1` column | `ESC [ n C` uses decimal `n` |
| `ESC [ D` | Move cursor left by `1` column | `ESC [ n D` uses decimal `n` |
| `ESC [ H` | Move cursor to row `1`, column `1` | Home position |
| `ESC [ row ; col H` | Move cursor to absolute position | Rows and columns are `1`-based and clamped to the visible screen |
| `ESC [ f` | Same as `H` | |
| `ESC [ row ; col f` | Same as `row ; col H` | |
| `ESC [ s` | Save cursor state | Saves row, column, and delayed-wrap state |
| `ESC [ u` | Restore cursor state | Restores row, column, and delayed-wrap state |
| `ESC [ J` | Erase from cursor to end of screen | Same as `ESC [ 0 J` |
| `ESC [ 1 J` | Erase from start of screen to cursor | |
| `ESC [ 2 J` | Erase entire screen | |
| `ESC [ 67 J` | Restore terminal display ownership | Dioptase-private recovery command described below |
| `ESC [ K` | Erase from cursor to end of line | Same as `ESC [ 0 K` |
| `ESC [ 1 K` | Erase from start of line to cursor | |
| `ESC [ 2 K` | Erase entire line | |
| `ESC [ L` | Insert `1` line at the cursor row | Same as `ESC [ 1 L` |
| `ESC [ n L` | Insert `n` lines at the cursor row | Lower rows move down; count is clamped to the visible screen |
| `ESC [ M` | Delete `1` line at the cursor row | Same as `ESC [ 1 M` |
| `ESC [ n M` | Delete `n` lines at the cursor row | Lower rows move up; count is clamped to the visible screen |
| `ESC [ m` | Reset text color | Same as `ESC [ 0 m` |
| `ESC [ n m` | Apply one SGR color code | Supported values are listed below |
| `ESC [ a ; b m` | Apply up to two SGR codes in order | Example: `ESC [ 0 ; 32 m` resets then sets green |
| `ESC [ ? 25 l` | Hide cursor | |
| `ESC [ ? 25 h` | Show cursor | |
| `ESC [ ? 68 h` | Enter raw keyboard mode | Dioptase-private; see "Raw keyboard mode" |
| `ESC [ ? 68 l` | Leave raw keyboard mode | Dioptase-private |
| `ESC [ 68 n` | Keyboard sync: write two `0xFF` bytes to the input pipe | Dioptase-private; see "Keyboard sync" |

### Foreground display recovery

The shell writes the private `ESC [ 67 J` command after an external foreground
child exits if the kernel reports that the child used direct display traps.
Because the command and the next prompt use the same output pipe, the terminal
processes recovery before rendering that prompt.

Recovery restores the terminal's established startup state:

- reload the built-in text tiles
- set tile scale, pixel scale, and both tile scroll registers to `0`
- hide every sprite by moving it to `HIDDEN_SPRITE_COORD`
- clear the user-visible part of the pixel framebuffer (pixels `0..75775`,
  mapped by the terminal with `get_pixel_fb()`) to black, because text tiles
  are transparent where their pixels are `0xFXXX` and a program's last frame
  would otherwise show through
- turn raw keyboard mode off
- reset circular-scroll state and clear the visible framebuffer
- home and show the cursor
- reset saved-cursor, delayed-wrap, blink, and text-color state

The shell does not write VGA registers directly. This keeps the hardware state
and the terminal's renderer state under the same owner even when a foreground
program is terminated before it can clean up.

Foreground programs that use direct display traps should configure only the
state they need while running. They do not need to reload terminal tiles, reset
scroll or scale registers, hide sprites, clear the framebuffer, or restore the
terminal cursor before exiting; the terminal performs that recovery after both
normal exit and signal termination.

### Supported SGR color codes

The terminal currently supports these SGR parameters:

| Code | Meaning | Tile color byte |
| --- | --- | --- |
| `0` | Reset to default color | `0xFF` |
| `30` | Black | `0x00` |
| `31` | Red | `0xE9` |
| `32` | Green | `0x3D` |
| `33` | Yellow | `0xFC` |
| `34` | Blue | `0x2F` |
| `35` | Magenta | `0xA2` |
| `36` | Cyan | `0x7B` |
| `37` | White | `0xFF` |
| `38` | Light gray | `0x92` |
| `39` | Dark gray | `0x49` |
| `40` | Orange | `0xF0` |
| `41` | Bright pink | `0xF3` |
| `42` | Bright green | `0x3E` |
| `43` | Bright yellow | `0xFD` |
| `44` | Bright blue | `0x5F` |
| `45` | Purple | `0xA3` |
| `46` | Indigo | `0x83` |
| `47` | Light orange | `0xF9` |
| `48` | Light yellow | `0xFE` |
| `49` | Gold | `0xB0` |
| `50` | Light blue | `0xBB` |
| `51` | Light purple | `0xD7` |

Codes `38` through `51` are Dioptase-specific foreground color extensions. They
do not follow ANSI `38;...` extended-color syntax, and `40` through `47` are
not treated as background colors. In the shell, you can run `colors` to see these.

Unsupported SGR features such as ANSI background colors, bold, underline, and
inverse video are ignored.

### Parser limits and unsupported behavior

- Only CSI sequences beginning with `ESC [` are recognized.
- The parser supports at most `2` arguments per sequence.
- Each numeric argument may contain at most `2` decimal digits.
- `?` is only meaningful as the first argument for `CSI ?25h`, `CSI ?25l`,
  `CSI ?68h`, and `CSI ?68l`.
- Rows and columns for `H` and `f` are `1`-based and clamped into the visible
  `80x60` screen.
- Counts for `A`, `B`, `C`, `D`, `L`, and `M` are also clamped to the visible
  screen bounds.
- Unsupported final bytes terminate escape parsing and are otherwise ignored.
- Overlong or otherwise malformed CSI sequences are not fully consumed. Once the
  parser exceeds its argument count or argument length limits, the escape parse
  aborts and later bytes are processed as ordinary text.
- Example: `ESC [ 100 C` is not supported because the argument has three digits.
  The sequence aborts and the trailing `C` may be rendered as a normal
  character.

### Implementation notes

- The terminal reads from `STDIN` in batches of up to `128` bytes.
- The terminal drains all currently queued keyboard events once per main-loop
  iteration before rendering pending output.
- Before rendering a batch, it erases the cursor from the framebuffer. After the
  batch is processed, it redraws the cursor immediately if the cursor is
  visible.
- The current implementation is intentionally small and does not yet try to be a
  full ANSI or VT-compatible terminal emulator.
