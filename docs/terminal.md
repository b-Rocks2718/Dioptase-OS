## Terminal

### Display model

- The terminal renders to the tile framebuffer returned by `get_tile_fb()`.
- The visible screen is `80` columns by `60` rows.
- Each character cell is one `8x8` tile.
- The low byte of each tile framebuffer entry is the tile index.
- The high byte of each tile framebuffer entry is the tile color.
- The terminal loads the built-in text tileset with `load_text_tiles()`.
- The terminal sets tile scale to `0` and initializes tile vertical scroll to `0`.

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
- When visible, the cursor blinks every `100` jiffies according to
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

### Supported SGR color codes

The terminal currently supports these SGR parameters:

| Code | Meaning | Tile color byte |
| --- | --- | --- |
| `0` | Reset to default color | `0xFF` |
| `30` | Black | `0x00` |
| `31` | Red | `0xE0` |
| `32` | Green | `0x1C` |
| `33` | Yellow | `0xFC` |
| `34` | Blue | `0x03` |
| `35` | Magenta | `0xE3` |
| `36` | Cyan | `0x1F` |
| `37` | White | `0xFF` |

Unsupported SGR features such as background colors, bold, underline, and
inverse video are ignored.

### Parser limits and unsupported behavior

- Only CSI sequences beginning with `ESC [` are recognized.
- The parser supports at most `2` arguments per sequence.
- Each numeric argument may contain at most `2` decimal digits.
- `?` is only meaningful as the first argument for `CSI ?25h` and `CSI ?25l`.
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
- Before rendering a batch, it erases the cursor from the framebuffer. After the
  batch is processed, it redraws the cursor immediately if the cursor is
  visible.
- The current implementation is intentionally small and does not yet try to be a
  full ANSI or VT-compatible terminal emulator.
