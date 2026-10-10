/*
 * Portal-style end credits for Still Alive.
 *
 * Streams /still_alive/still_alive.wav to the audio device (root/crt/wav.h)
 * and draws three panels directly into the VGA tile framebuffer:
 * lyrics typed out on the left, credits scrolling in the top right, and ASCII
 * art in the bottom right. script.txt is the timeline: lyric, art, and clear
 * events in seconds from the start of playback. art.txt holds the images and
 * credits.txt the scrolling text.
 *
 * The show's clock is the audio itself: the number of samples the device has
 * played, converted to 60 Hz frames. Whatever slows playback (an emulator
 * slower than real time, host audio backpressure, a late refill), the text
 * follows the music. After the song ends the clock continues on the VGA frame
 * counter (60 Hz, docs/mem_map.md) for the closing screen. The device ring
 * holds about 0.33 s of audio, so the main loop refills it every frame.
 *
 * Press q at any point to exit (Ctrl-C also works). When the song ends the
 * closing screen stays up until then.
 *
 * Display handoff follows snake and dino: hide the terminal cursor and clear
 * the screen through stdout, wait for the terminal to process that, then own
 * the tile framebuffer until exit. The shell restores the terminal display
 * after the program exits.
 */

#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/stdbool.h"
#include "../crt/stddef.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"
#include "../crt/vga.h"
#include "../crt/wav.h"

#define SCRIPT_PATH "/still_alive/script.txt"
#define ART_PATH "/still_alive/art.txt"
#define CREDITS_PATH "/still_alive/credits.txt"
#define MUSIC_PATH "/still_alive/still_alive.wav"

// The VGA frame counter register increments once per 60 Hz frame
// (docs/mem_map.md, VGA frame count register).
#define FRAMES_PER_SECOND 60
#define CENTISECONDS_PER_SECOND 100

// Tile colors are 8-bit RRRGGGBB; amber approximates the Portal terminal.
#define TEXT_COLOR 0xF4

// Cursor blink half-period, matching the terminal's 0.25 s blink.
#define CURSOR_BLINK_FRAMES 15
#define CURSOR_CHAR '_'

// Frames to let the terminal process the hide-cursor/clear sequence before
// this program starts writing the framebuffer itself.
#define TERMINAL_HANDOFF_FRAMES 10
// Jiffies to wait for the first VGA frame before concluding there is no VGA.
#define VGA_PROBE_JIFFIES 3000
// Key that exits the program at any point (Ctrl-C also works).
#define QUIT_KEY 'q'
// Credits finish typing this long before the end event.
#define CREDITS_LEAD_OUT_FRAMES (4 * FRAMES_PER_SECOND)
// Lyric typing speed bounds, in frames per character.
#define MIN_TYPING_FRAMES 2
#define MAX_TYPING_FRAMES 6
#define DEFAULT_TYPING_FRAMES 4

// Screen layout on the 80x60 tile grid: a full-height lyric box on the left,
// a credits box top right, and an unboxed art area below it. Bounds are
// inclusive.
#define LYRIC_BOX_LEFT 0
#define LYRIC_BOX_RIGHT 47
#define LYRIC_BOX_TOP 0
#define LYRIC_BOX_BOTTOM 59
#define LYRIC_TEXT_LEFT 2
#define LYRIC_TEXT_RIGHT 45
#define LYRIC_TEXT_TOP 1
#define LYRIC_TEXT_BOTTOM 58

#define CREDITS_BOX_LEFT 48
#define CREDITS_BOX_RIGHT 79
#define CREDITS_BOX_TOP 0
#define CREDITS_BOX_BOTTOM 29
#define CREDITS_TEXT_LEFT 50
#define CREDITS_TEXT_RIGHT 77
#define CREDITS_TEXT_TOP 1
#define CREDITS_TEXT_BOTTOM 28

#define ART_LEFT 48
#define ART_RIGHT 79
#define ART_TOP 31
#define ART_BOTTOM 59

// Timeline commands from script.txt.
enum EventKind {
  EVENT_LINE,    // ">TEXT": start a new lyric row, then type TEXT
  EVENT_APPEND,  // "+TEXT": keep typing TEXT on the current row
  EVENT_CLEAR,   // "clear": empty the lyric panel
  EVENT_ART,     // "art NAME": show an art.txt image
  EVENT_END,     // "end": stop the show
};

// One timeline event. arg points into the script buffer.
struct Event {
  unsigned time_cs;     // centiseconds from the start of playback
  enum EventKind kind;
  char* arg;            // lyric text or art name
  int art_index;        // resolved image for EVENT_ART
};

// One art.txt image; rows point into the art buffer.
struct ArtImage {
  char* name;
  char** rows;
  unsigned num_rows;
  unsigned width;
};

// A file split into NUL-terminated lines inside one heap buffer.
struct TextFile {
  char* text;
  char** lines;
  unsigned num_lines;
};

volatile short* TILE_FB = NULL;

struct TextFile script_file;
struct Event* events = NULL;
unsigned num_events = 0;

struct TextFile art_file;
struct ArtImage* art_images = NULL;
unsigned num_art_images = 0;

char* credits_text = NULL;
unsigned credits_len = 0;

// Lyric panel state. lyric_row_open means a row has been started, so the
// next ">" moves down; it is false right after a clear.
unsigned lyric_row = LYRIC_TEXT_TOP;
unsigned lyric_col = LYRIC_TEXT_LEFT;
bool lyric_row_open = false;

// The lyric segment currently being typed out one character at a time.
char* typing_text = NULL;
unsigned typing_len = 0;
unsigned typing_done = 0;
unsigned typing_start = 0;
unsigned typing_interval = DEFAULT_TYPING_FRAMES;

unsigned credits_row = CREDITS_TEXT_TOP;
unsigned credits_col = CREDITS_TEXT_LEFT;
unsigned credits_emitted = 0;

int art_current = -1;
unsigned art_rows_drawn = 0;

// Each cursor is drawn in the blank cell after its panel's text, so hiding it
// only has to blank that one cell again.
bool lyric_cursor_drawn = false;
unsigned lyric_cursor_row = 0;
unsigned lyric_cursor_col = 0;
bool credits_cursor_drawn = false;
unsigned credits_cursor_row = 0;
unsigned credits_cursor_col = 0;

// ---------------------------------------------------------------------------
// Files

// Read a whole file into a NUL-terminated heap buffer. Returns NULL if the file
// cannot be opened or read.
static char* read_whole_file(char* path, unsigned* size_out){
  int fd = open_existing(path);
  if (fd < 0){
    return NULL;
  }

  unsigned capacity = 4096;
  unsigned size = 0;
  char* buf = malloc(capacity + 1);
  while (true){
    if (size == capacity){
      capacity *= 2;
      buf = realloc(buf, capacity + 1);
    }
    int n = read(fd, buf + size, capacity - size);
    if (n < 0){
      close(fd);
      free(buf);
      return NULL;
    }
    if (n == 0){
      break;
    }
    size += n;
  }
  close(fd);

  buf[size] = '\0';
  *size_out = size;
  return buf;
}

// Split text into lines in place: '\n' becomes NUL and a trailing '\r' is
// dropped, so every line is its own C string.
static void split_lines(char* text, struct TextFile* out){
  unsigned count = 1;
  for (char* p = text; *p != '\0'; ++p){
    if (*p == '\n'){
      count++;
    }
  }

  out->text = text;
  out->lines = malloc(sizeof(char*) * count);
  out->num_lines = 0;

  char* line = text;
  while (true){
    char* end = line;
    while (*end != '\0' && *end != '\n'){
      end++;
    }
    bool last = *end == '\0';
    *end = '\0';
    if (end > line && end[-1] == '\r'){
      end[-1] = '\0';
    }
    // A final newline does not start another (empty) line.
    if (!(last && *line == '\0' && out->num_lines > 0)){
      out->lines[out->num_lines++] = line;
    }
    if (last){
      break;
    }
    line = end + 1;
  }
}

// Load a file and split it into lines, printing a diagnostic on failure.
static bool load_text_file(char* path, struct TextFile* out){
  unsigned size;
  char* text = read_whole_file(path, &size);
  if (text == NULL){
    void* args[1];
    args[0] = path;
    printf("still_alive: could not read %s\n", args);
    return false;
  }
  split_lines(text, out);
  return true;
}

// Print "still_alive: <file> line N: <message>" for a parse error.
static void report_parse_error(char* path, unsigned line_index, char* message){
  void* args[3];
  args[0] = path;
  args[1] = (void*)(line_index + 1);
  args[2] = message;
  printf("still_alive: %s line %u: %s\n", args);
}

// Whether a line holds no content (empty or only spaces).
static bool line_is_blank(char* line){
  while (*line == ' ' || *line == '\t'){
    line++;
  }
  return *line == '\0';
}

// Parse art.txt into images. Lines before the first "@@ NAME" header are
// comments; within an image every line is art, and trailing blank lines are
// dropped.
static bool parse_art(void){
  if (!load_text_file(ART_PATH, &art_file)){
    return false;
  }

  num_art_images = 0;
  for (unsigned i = 0; i < art_file.num_lines; ++i){
    if (strncmp(art_file.lines[i], "@@ ", 3) == 0){
      num_art_images++;
    }
  }
  art_images = malloc(sizeof(struct ArtImage) * (num_art_images + 1));

  int current = -1;
  for (unsigned i = 0; i < art_file.num_lines; ++i){
    char* line = art_file.lines[i];
    if (strncmp(line, "@@ ", 3) == 0){
      current++;
      art_images[current].name = line + 3;
      art_images[current].rows = &art_file.lines[i + 1];
      art_images[current].num_rows = 0;
      art_images[current].width = 0;
      continue;
    }
    if (current < 0){
      continue;
    }
    struct ArtImage* image = &art_images[current];
    image->num_rows++;
    unsigned width = strlen(line);
    if (width > image->width){
      image->width = width;
    }
  }

  for (unsigned i = 0; i < num_art_images; ++i){
    struct ArtImage* image = &art_images[i];
    while (image->num_rows > 0 && line_is_blank(image->rows[image->num_rows - 1])){
      image->num_rows--;
    }
    if (image->width > ART_RIGHT - ART_LEFT + 1 ||
        image->num_rows > ART_BOTTOM - ART_TOP + 1){
      void* args[3];
      args[0] = image->name;
      args[1] = (void*)(ART_RIGHT - ART_LEFT + 1);
      args[2] = (void*)(ART_BOTTOM - ART_TOP + 1);
      printf("still_alive: art.txt image '%s' is larger than the %u x %u picture panel\n", args);
      return false;
    }
  }
  return true;
}

// Find an art image by name; -1 if there is none.
static int find_art(char* name){
  for (unsigned i = 0; i < num_art_images; ++i){
    if (streq(art_images[i].name, name)){
      return i;
    }
  }
  return -1;
}

// Parse "<digits>[.<1-2 digits>]" seconds into centiseconds, advancing *cursor.
static bool parse_time_cs(char** cursor, unsigned* time_out){
  char* p = *cursor;
  if (*p < '0' || *p > '9'){
    return false;
  }
  unsigned seconds = 0;
  while (*p >= '0' && *p <= '9'){
    seconds = seconds * 10 + (*p - '0');
    p++;
  }
  unsigned fraction = 0;
  if (*p == '.'){
    p++;
    unsigned digits = 0;
    while (*p >= '0' && *p <= '9'){
      if (digits < 2){
        fraction = fraction * 10 + (*p - '0');
      }
      digits++;
      p++;
    }
    if (digits == 1){
      fraction *= 10;
    }
  }
  *time_out = seconds * CENTISECONDS_PER_SECOND + fraction;
  *cursor = p;
  return true;
}

// Parse script.txt into the event timeline and check it against art.txt.
static bool parse_script(void){
  if (!load_text_file(SCRIPT_PATH, &script_file)){
    return false;
  }

  events = malloc(sizeof(struct Event) * (script_file.num_lines + 1));
  num_events = 0;
  bool saw_end = false;

  for (unsigned i = 0; i < script_file.num_lines; ++i){
    char* p = script_file.lines[i];
    while (*p == ' ' || *p == '\t'){
      p++;
    }
    if (*p == '\0' || *p == '#'){
      continue;
    }

    struct Event* event = &events[num_events];
    if (!parse_time_cs(&p, &event->time_cs)){
      report_parse_error(SCRIPT_PATH, i, "expected a time in seconds, like 12.34");
      return false;
    }
    if (*p != ' ' && *p != '\t'){
      report_parse_error(SCRIPT_PATH, i, "expected a space between the time and the command");
      return false;
    }
    while (*p == ' ' || *p == '\t'){
      p++;
    }

    event->art_index = -1;
    if (*p == '>'){
      event->kind = EVENT_LINE;
      event->arg = p + 1;
    } else if (*p == '+'){
      event->kind = EVENT_APPEND;
      event->arg = p + 1;
    } else if (streq(p, "clear")){
      event->kind = EVENT_CLEAR;
      event->arg = p;
    } else if (streq(p, "end")){
      event->kind = EVENT_END;
      event->arg = p;
      saw_end = true;
    } else if (strncmp(p, "art ", 4) == 0){
      event->kind = EVENT_ART;
      event->arg = p + 4;
      event->art_index = find_art(event->arg);
      if (event->art_index < 0){
        report_parse_error(SCRIPT_PATH, i, "art names an image that art.txt does not define");
        return false;
      }
    } else {
      report_parse_error(SCRIPT_PATH, i, "expected >TEXT, +TEXT, clear, art NAME, or end");
      return false;
    }

    if (num_events > 0 && event->time_cs < events[num_events - 1].time_cs){
      report_parse_error(SCRIPT_PATH, i, "event is earlier than the event before it; keep events in time order");
      return false;
    }
    num_events++;
  }

  if (!saw_end){
    void* args[1];
    args[0] = SCRIPT_PATH;
    printf("still_alive: %s has no 'end' event, so the show would never stop\n", args);
    return false;
  }
  return true;
}

// Load credits.txt as one string typed out over the course of the song.
static bool load_credits(void){
  credits_text = read_whole_file(CREDITS_PATH, &credits_len);
  if (credits_text == NULL){
    void* args[1];
    args[0] = CREDITS_PATH;
    printf("still_alive: could not read %s\n", args);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Drawing

// Write one character cell of the tile framebuffer.
static void put_cell(unsigned row, unsigned col, char c, unsigned color){
  TILE_FB[row * TILE_ROW_WIDTH + col] = (short)((color << 8) | (c & 0xFF));
}

// Blank an inclusive rectangle of cells.
static void clear_rect(unsigned top, unsigned left, unsigned bottom, unsigned right){
  for (unsigned row = top; row <= bottom; ++row){
    for (unsigned col = left; col <= right; ++col){
      TILE_FB[row * TILE_ROW_WIDTH + col] = 0;
    }
  }
}

// Draw a Portal-style box: dashes along the top and bottom, bars down the
// sides, and open corners.
static void draw_box(unsigned top, unsigned left, unsigned bottom, unsigned right){
  for (unsigned col = left + 1; col < right; ++col){
    put_cell(top, col, '-', TEXT_COLOR);
    put_cell(bottom, col, '-', TEXT_COLOR);
  }
  for (unsigned row = top + 1; row < bottom; ++row){
    put_cell(row, left, '|', TEXT_COLOR);
    put_cell(row, right, '|', TEXT_COLOR);
  }
}

// Write a string at a position, clipped at the right column.
static void put_string(unsigned row, unsigned col, unsigned right, char* text, unsigned color){
  for (unsigned i = 0; text[i] != '\0' && col <= right; ++i, ++col){
    put_cell(row, col, text[i], color);
  }
}

// Erase both blinking cursors so text updates never collide with them.
static void hide_cursors(void){
  if (lyric_cursor_drawn){
    put_cell(lyric_cursor_row, lyric_cursor_col, ' ', TEXT_COLOR);
    lyric_cursor_drawn = false;
  }
  if (credits_cursor_drawn){
    put_cell(credits_cursor_row, credits_cursor_col, ' ', TEXT_COLOR);
    credits_cursor_drawn = false;
  }
}

// Draw each panel's cursor after its text during the "on" half of the blink.
static void show_cursors(unsigned elapsed){
  if ((elapsed / CURSOR_BLINK_FRAMES) % 2 != 0){
    return;
  }
  if (lyric_row_open && lyric_col <= LYRIC_TEXT_RIGHT){
    lyric_cursor_row = lyric_row;
    lyric_cursor_col = lyric_col;
    put_cell(lyric_cursor_row, lyric_cursor_col, CURSOR_CHAR, TEXT_COLOR);
    lyric_cursor_drawn = true;
  }
  if (credits_col <= CREDITS_TEXT_RIGHT){
    credits_cursor_row = credits_row;
    credits_cursor_col = credits_col;
    put_cell(credits_cursor_row, credits_cursor_col, CURSOR_CHAR, TEXT_COLOR);
    credits_cursor_drawn = true;
  }
}

// ---------------------------------------------------------------------------
// Lyric panel

// Empty the lyric panel; the next lyric starts on its top row.
static void lyric_clear(void){
  clear_rect(LYRIC_TEXT_TOP, LYRIC_TEXT_LEFT, LYRIC_TEXT_BOTTOM, LYRIC_TEXT_RIGHT);
  lyric_row = LYRIC_TEXT_TOP;
  lyric_col = LYRIC_TEXT_LEFT;
  lyric_row_open = false;
}

// Move to the start of a new lyric row. A full panel starts over at the top
// rather than overwriting the border.
static void lyric_newline(void){
  if (lyric_row_open){
    lyric_row++;
  }
  if (lyric_row > LYRIC_TEXT_BOTTOM){
    lyric_clear();
  }
  lyric_col = LYRIC_TEXT_LEFT;
  lyric_row_open = true;
}

// Type one lyric character, wrapping at the panel edge.
static void lyric_put_char(char c){
  if (lyric_col > LYRIC_TEXT_RIGHT){
    lyric_newline();
  }
  put_cell(lyric_row, lyric_col, c, TEXT_COLOR);
  lyric_col++;
}

// Instantly type whatever is left of the current lyric segment.
static void finish_typing(void){
  while (typing_done < typing_len){
    lyric_put_char(typing_text[typing_done++]);
  }
}

// Frames until the next event that moves the lyric panel on, or 0 if none.
static unsigned frames_until_next_lyric_event(unsigned index){
  for (unsigned i = index + 1; i < num_events; ++i){
    if (events[i].kind != EVENT_ART){
      unsigned gap_cs = events[i].time_cs - events[index].time_cs;
      return gap_cs * FRAMES_PER_SECOND / CENTISECONDS_PER_SECOND;
    }
  }
  return 0;
}

// Begin typing a lyric segment, spreading it over most of the time before
// the next lyric event so the text keeps pace with the singing.
static void start_typing(char* text, unsigned index, unsigned now){
  finish_typing();
  typing_text = text;
  typing_len = strlen(text);
  typing_done = 0;
  typing_start = now;
  typing_interval = DEFAULT_TYPING_FRAMES;

  unsigned gap = frames_until_next_lyric_event(index);
  if (gap > 0 && typing_len > 0){
    typing_interval = (gap * 3 / 4) / typing_len;
    if (typing_interval < MIN_TYPING_FRAMES){
      typing_interval = MIN_TYPING_FRAMES;
    }
    if (typing_interval > MAX_TYPING_FRAMES){
      typing_interval = MAX_TYPING_FRAMES;
    }
  }
}

// Type the characters of the current segment that are due by `now`.
static void advance_typing(unsigned now){
  unsigned due = (now - typing_start) / typing_interval + 1;
  if (due > typing_len){
    due = typing_len;
  }
  while (typing_done < due){
    lyric_put_char(typing_text[typing_done++]);
  }
}

// ---------------------------------------------------------------------------
// Credits panel

// Advance the credits cursor to a new row, scrolling the panel once full.
static void credits_newline(void){
  credits_col = CREDITS_TEXT_LEFT;
  if (credits_row < CREDITS_TEXT_BOTTOM){
    credits_row++;
    return;
  }
  for (unsigned row = CREDITS_TEXT_TOP; row < CREDITS_TEXT_BOTTOM; ++row){
    for (unsigned col = CREDITS_TEXT_LEFT; col <= CREDITS_TEXT_RIGHT; ++col){
      TILE_FB[row * TILE_ROW_WIDTH + col] = TILE_FB[(row + 1) * TILE_ROW_WIDTH + col];
    }
  }
  clear_rect(CREDITS_TEXT_BOTTOM, CREDITS_TEXT_LEFT, CREDITS_TEXT_BOTTOM, CREDITS_TEXT_RIGHT);
}

// Type one credits character, wrapping at the panel edge.
static void credits_put_char(char c){
  if (c == '\r'){
    return;
  }
  if (c == '\n'){
    credits_newline();
    return;
  }
  if (credits_col > CREDITS_TEXT_RIGHT){
    credits_newline();
  }
  put_cell(credits_row, credits_col, c, TEXT_COLOR);
  credits_col++;
}

// Type credits evenly so the last character lands `CREDITS_LEAD_OUT_FRAMES`
// before the end of the show.
static void advance_credits(unsigned now, unsigned end_frame){
  unsigned finish = end_frame > CREDITS_LEAD_OUT_FRAMES ? end_frame - CREDITS_LEAD_OUT_FRAMES : 1;
  unsigned due = credits_len;
  if (now < finish){
    due = now * credits_len / finish;
  }
  while (credits_emitted < due){
    credits_put_char(credits_text[credits_emitted++]);
  }
}

// ---------------------------------------------------------------------------
// Art panel

// Replace the picture; it is revealed one row per frame by advance_art().
static void show_art(int index){
  clear_rect(ART_TOP, ART_LEFT, ART_BOTTOM, ART_RIGHT);
  art_current = index;
  art_rows_drawn = 0;
}

// Draw the next row of the current picture, centered in the art area.
static void advance_art(void){
  if (art_current < 0){
    return;
  }
  struct ArtImage* image = &art_images[art_current];
  if (art_rows_drawn >= image->num_rows){
    return;
  }
  unsigned left = ART_LEFT + (ART_RIGHT - ART_LEFT + 1 - image->width) / 2;
  unsigned top = ART_TOP + (ART_BOTTOM - ART_TOP + 1 - image->num_rows) / 2;
  put_string(top + art_rows_drawn, left, ART_RIGHT, image->rows[art_rows_drawn], TEXT_COLOR);
  art_rows_drawn++;
}

// ---------------------------------------------------------------------------
// Display and audio setup

// Block until the VGA frame counter moves past `frame`.
static unsigned wait_next_frame(unsigned frame){
  unsigned now = get_vga_frame_counter();
  while (now == frame){
    sleep(1);
    now = get_vga_frame_counter();
  }
  return now;
}

// Confirm the VGA device is producing frames; without it the frame-counter
// clock would never advance.
static bool vga_frames_advancing(void){
  unsigned start = get_vga_frame_counter();
  for (unsigned i = 0; i < VGA_PROBE_JIFFIES; ++i){
    if (get_vga_frame_counter() != start){
      return true;
    }
    sleep(1);
  }
  return false;
}

// Take the tile framebuffer over from the terminal and draw the panel frames.
static void take_over_display(void){
  // Hide the terminal cursor and clear through the terminal first; its
  // framebuffer writes must be finished before ours begin.
  puts("\x1b[?25l\x1b[2J");
  unsigned frame = get_vga_frame_counter();
  for (unsigned i = 0; i < TERMINAL_HANDOFF_FRAMES; ++i){
    frame = wait_next_frame(frame);
  }

  TILE_FB = get_tile_fb();
  load_text_tiles();
  set_tile_scale(0);
  set_hscroll(0);
  set_vscroll(0);
  clear_rect(0, 0, TILE_COL_HEIGHT - 1, TILE_ROW_WIDTH - 1);

  draw_box(LYRIC_BOX_TOP, LYRIC_BOX_LEFT, LYRIC_BOX_BOTTOM, LYRIC_BOX_RIGHT);
  draw_box(CREDITS_BOX_TOP, CREDITS_BOX_LEFT, CREDITS_BOX_BOTTOM, CREDITS_BOX_RIGHT);
}

// The song being played. Once it stops (finished or failed) the show clock
// continues on the VGA frame counter from the last audio position.
struct WavStream music;
bool music_done = false;
unsigned music_done_position = 0;
unsigned music_done_vga = 0;

// Open the song and start playback. Runs before the display takeover so a
// diagnostic still reaches the terminal.
static bool start_music(void){
  struct WavError error;
  if (wav_stream_open(&music, MUSIC_PATH, &error) != WAV_OK){
    wav_print_error("still_alive", MUSIC_PATH, &error);
    return false;
  }
  return true;
}

// Keep the audio ring fed and return the show time in frames since playback
// started. Must be called at least every few frames while the song plays.
static unsigned show_clock(void){
  if (music_done){
    return music_done_position + (get_vga_frame_counter() - music_done_vga);
  }

  unsigned frames = wav_stream_position(&music) * FRAMES_PER_SECOND / WAV_SAMPLE_RATE;
  if (wav_stream_pump(&music) < 0 || wav_stream_finished(&music)){
    wav_stream_close(&music);
    music_done = true;
    music_done_position = frames;
    music_done_vga = get_vga_frame_counter();
  }
  return frames;
}

// Stop the song if the show ends before it does.
static void stop_music(void){
  if (!music_done){
    wav_stream_close(&music);
    music_done = true;
  }
}

// ---------------------------------------------------------------------------
// Show

// Apply one timeline event at frame `now`.
static void apply_event(unsigned index, unsigned now){
  struct Event* event = &events[index];
  switch (event->kind){
    case EVENT_LINE:
      finish_typing();
      lyric_newline();
      start_typing(event->arg, index, now);
      break;
    case EVENT_APPEND:
      finish_typing();
      if (!lyric_row_open){
        lyric_newline();
      }
      start_typing(event->arg, index, now);
      break;
    case EVENT_CLEAR:
      finish_typing();
      lyric_clear();
      break;
    case EVENT_ART:
      show_art(event->art_index);
      break;
    case EVENT_END:
      finish_typing();
      break;
  }
}

// Frame offset of an event from the start of playback.
static unsigned event_frame(unsigned index){
  return events[index].time_cs * FRAMES_PER_SECOND / CENTISECONDS_PER_SECOND;
}

// Drain pending keyboard input and return whether the quit key was pressed.
// Draining also keeps keys pressed during the show from reaching the shell
// afterward.
static bool quit_pressed(void){
  char keys[16];
  bool quit = false;
  int available = fd_bytes_available(STDIN);
  while (available > 0){
    int n = read(STDIN, keys, available < 16 ? available : 16);
    if (n <= 0){
      break;
    }
    for (int i = 0; i < n; ++i){
      if (keys[i] == QUIT_KEY || keys[i] == QUIT_KEY - 'a' + 'A'){
        quit = true;
      }
    }
    available -= n;
  }
  return quit;
}

// Run the timeline against the audio clock, then hold the closing screen
// until the quit key. The quit key also ends the show early.
static void run_show(void){
  unsigned end_frame = 0;
  for (unsigned i = 0; i < num_events; ++i){
    if (events[i].kind == EVENT_END){
      end_frame = event_frame(i);
      break;
    }
  }

  unsigned next_event = 0;
  unsigned frame = get_vga_frame_counter();
  while (true){
    unsigned now = show_clock();
    hide_cursors();

    bool ended = false;
    while (next_event < num_events && event_frame(next_event) <= now){
      apply_event(next_event, now);
      if (events[next_event].kind == EVENT_END){
        ended = true;
      }
      next_event++;
    }
    if (ended){
      break;
    }

    if (quit_pressed()){
      stop_music();
      return;
    }
    advance_typing(now);
    advance_credits(now, end_frame);
    advance_art();
    show_cursors(now);
    frame = wait_next_frame(frame);
  }

  // Let the closing screen settle: everything typed, picture complete. It
  // stays up (and any remaining music keeps playing) until the quit key.
  advance_credits(end_frame, end_frame);
  while (art_current >= 0 && art_rows_drawn < art_images[art_current].num_rows){
    advance_art();
  }
  while (!quit_pressed()){
    hide_cursors();
    show_cursors(show_clock());
    frame = wait_next_frame(frame);
  }
  stop_music();
}

// Play the Still Alive credits.
int main(void){
  // Load everything before touching the display so errors reach the terminal.
  if (!parse_art() || !parse_script() || !load_credits()){
    return 1;
  }
  if (!vga_frames_advancing()){
    puts("still_alive: the VGA frame counter is not advancing; this program needs a VGA display (EMU_VGA=yes)\n");
    return 1;
  }

  // Run at high priority: this process now feeds the audio ring, and a refill
  // delayed past the ring's ~0.33 s of buffered audio is an audible gap.
  request_priority(DIOPTASE_PRIORITY_HIGH);

  if (!start_music()){
    return 1;
  }
  // The handoff waits ~10 frames without refilling, well inside the ~0.33 s
  // the first refill queued. The show clock is the audio position, so the
  // first loop iteration simply catches up on those frames.
  take_over_display();

  run_show();
  return 0;
}
