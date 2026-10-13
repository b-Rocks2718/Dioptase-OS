/*
 * Exercise the actual renderer and synthesizer under the Dioptase compiler and
 * full emulator. Device doubles make clipping, failed mappings, ring backpressure,
 * and refill amounts deterministic without relying on VGA or audio wall time.
 * Including the implementation gives access to private drawing primitives while
 * retaining the exact production algorithms. The fixed frame workload also serves
 * as an instruction-count benchmark with the emulator's --profile option.
 */
#define get_tile_fb test_get_tile_fb
#define get_tilemap test_get_tilemap
#define set_tile_scale test_set_tile_scale
#define set_hscroll test_set_hscroll
#define set_vscroll test_set_vscroll
#define get_pixel_fb test_get_pixel_fb
#define set_pixel_scale test_set_pixel_scale
#define audio_open test_audio_open
#define audio_close test_audio_close
#define audio_buffered test_audio_buffered
#define audio_write test_audio_write
#include "../../../root/microrealm/render.c"
#include "../../../root/microrealm/audio.c"
#include "../../../root/microrealm/font.c"
#undef get_tile_fb
#undef get_tilemap
#undef set_tile_scale
#undef set_hscroll
#undef set_vscroll
#undef get_pixel_fb
#undef set_pixel_scale
#undef audio_open
#undef audio_close
#undef audio_buffered
#undef audio_write
#include "../../../root/crt/print.h"

/* 320 * 236 writable pixels plus two guards; bcc array bounds require literals. */
static short pixels[75522];
static short expected[75520];
/* Full documented tile regions, initially opaque as after terminal CSI 2J. */
static short tile_entries[TILE_FB_ENTRIES];
static short tile_pixels[16384];
static int tile_fb_failed;
static int tilemap_failed;
static unsigned tile_scale = 1;
static unsigned horizontal_scroll = 8;
static unsigned vertical_scroll = 8;
static struct GameWorld scene;
static int mapping_failed;
static int queued_bytes;
static int write_limit = 1000;
static int accepted_bytes;
static int write_calls;
static short expected_pcm[AUDIO_BUF_SAMPLES];
static short captured_pcm[AUDIO_BUF_SAMPLES];
static int capture_samples;
static int capture_enabled;

/* Return an owned test buffer or the syscall's documented mapping failure. */
volatile short* test_get_pixel_fb(void) {
  return mapping_failed ? (volatile short*)-1 : &pixels[1];
}

/* Supply the covering tile framebuffer or its documented failed mapping. */
volatile short* test_get_tile_fb(void) {
  return tile_fb_failed ? (volatile short*)-1 : tile_entries;
}

/* Opaque zero-initialized graphics reproduce the terminal's black tile. */
volatile short* test_get_tilemap(void) {
  return tilemap_failed ? (volatile short*)-1 : tile_pixels;
}

/* Capture the overlay scale established during the graphics handoff. */
void test_set_tile_scale(unsigned scale) { tile_scale = scale; }

/* Capture horizontal scrolling inherited from a prior foreground program. */
void test_set_hscroll(unsigned value) { horizontal_scroll = value; }

/* Capture the terminal's circular row offset at display handoff. */
void test_set_vscroll(unsigned value) { vertical_scroll = value; }

/* Scaling does not alter the backing pixel values checked by this test. */
int test_set_pixel_scale(unsigned scale) { return 0; }

/* Start a deterministic empty ring owned by the test process. */
int test_audio_open(void) { queued_bytes = 0; return 0; }

/* Discard the test ring when its owner releases the device. */
int test_audio_close(void) { queued_bytes = 0; return 0; }

/* Expose queued bytes without advancing a host-dependent playback clock. */
int test_audio_buffered(void) { return queued_bytes; }

/* Accept a bounded even prefix, including zero, to model a nonblocking ring. */
int test_audio_write(void* pcm, unsigned bytes) {
  int count = (bytes < (unsigned)write_limit) ? (int)bytes : write_limit;
  if (capture_enabled) {
    short* samples = (short*)pcm;
    for (int i = 0; i < (count >> 1) && capture_samples < AUDIO_BUF_SAMPLES; i++) {
      captured_pcm[capture_samples++] = samples[i];
    }
  }
  queued_bytes += count;
  accepted_bytes += count;
  write_calls++;
  return count;
}

/* Supply map lookup independently of the economy and entity simulation. */
struct MapCell* world_get_cell(struct GameWorld* w, int tx, int ty) {
  if (tx < 0 || tx >= MAP_SIZE || ty < 0 || ty >= MAP_SIZE) return NULL;
  return &w->grid[ty * MAP_SIZE + tx];
}

/* Placement previews use a known valid footprint in the synthetic scene. */
bool world_can_place(struct GameWorld* w, int type, int tx, int ty) {
  return tx >= 2 && ty >= 2 && tx < MAP_SIZE - 2 && ty < MAP_SIZE - 2;
}

/* Reset only the writable pixels, leaving boundary sentinels intact. */
static void clear_pixels(void) {
  for (int i = 1; i <= SCREEN_W * MAX_RENDER_Y; i++) pixels[i] = 0;
}

/* Compare decimal rendering against literal text, including the signed limits. */
static bool check_number(int value, char* text) {
  clear_pixels();
  draw_string(4, 5, text, PAL_WHITE);
  for (int i = 0; i < SCREEN_W * MAX_RENDER_Y; i++) expected[i] = pixels[i + 1];
  clear_pixels();
  draw_number(4, 5, value, PAL_WHITE);
  for (int i = 0; i < SCREEN_W * MAX_RENDER_Y; i++) {
    if (expected[i] != pixels[i + 1]) return false;
  }
  return true;
}

/* Render a reproducible mixture of terrain, resources, units and camera edges. */
static unsigned render_workload(void) {
  for (int i = 0; i < TOTAL_CELLS; i++) {
    scene.grid[i].terrain = i % 5;
    scene.grid[i].resource = (i % 7 == 0) ? RES_TREE : RES_NONE;
  }
  scene.day_number = 3;
  scene.score = 3500;
  scene.food = 90;
  scene.wood = 140;
  scene.stone = 80;
  scene.population = 3;
  scene.max_population = 10;
  for (int i = 0; i < 8; i++) {
    scene.units[i].active = 1;
    scene.units[i].type = i + 1;
    scene.units[i].x = (80 + i * 24) << FIXED_SHIFT;
    scene.units[i].y = (80 + i * 8) << FIXED_SHIFT;
    scene.units[i].hp = 10;
    scene.units[i].max_hp = 20;
  }
  unsigned hash = 0;
  for (int frame = 0; frame < 8; frame++) {
    scene.cam_x = frame * 3;
    scene.cam_y = frame * 5;
    render_frame(&scene, 160, 120, 1, 30, 40);
    for (int i = 0; i < SCREEN_W * MAX_RENDER_Y; i++) {
      hash = ((hash << 5) | (hash >> 27)) ^ (unsigned short)pixels[i + 1];
    }
  }
  return hash;
}

/* Run the fixed scene benchmark, followed by edge cases in normal test builds. */
int main(void) {
  pixels[0] = 1234;
  pixels[SCREEN_W * MAX_RENDER_Y + 1] = 2345;
  if (!render_init()) {
    puts("***FAIL microrealm valid display mappings were rejected\n");
    return 1;
  }
  for (int i = 0; i < TILE_FB_ENTRIES; i++) {
    int tile = tile_entries[i] & 255;
    for (int pixel = 0; pixel < TILEMAP_PIXELS_PER_TILE; pixel++) {
      if (((unsigned short)tile_pixels[tile * TILEMAP_PIXELS_PER_TILE + pixel] & 0xF000) != 0xF000) {
        puts("***FAIL microrealm opaque tile overlay hides the pixel game\n");
        return 1;
      }
    }
  }
  if (tile_scale != 0 || horizontal_scroll != 0 || vertical_scroll != 0) {
    puts("***FAIL microrealm display setup retained terminal scale or scrolling\n");
    return 1;
  }
  unsigned hash = render_workload();
  if (hash != 0x10517ae7) {
    puts("***FAIL microrealm fixed scene pixels differ from pre-optimization frames\n");
    print_hex(hash, false);
    puts(" observed frame hash\n");
    return 1;
  }
  if (pixels[0] != 1234 || pixels[SCREEN_W * MAX_RENDER_Y + 1] != 2345) {
    puts("***FAIL microrealm renderer wrote outside its framebuffer rows\n");
    return 1;
  }
#ifndef MEDIA_BENCHMARK
  if (!check_number(0, "0") || !check_number(99999, "99999") ||
      !check_number(100000, "100000") || !check_number(-123456, "-123456") ||
      !check_number(2147483647, "2147483647") ||
      !check_number(-2147483647 - 1, "-2147483648")) {
    puts("***FAIL microrealm decimal glyphs differ from expected literal digits\n");
    return 1;
  }
  clear_pixels();
  draw_char(0, 0, 'A', COLOR_TRANSPARENT);
  for (int i = 1; i <= SCREEN_W * MAX_RENDER_Y; i++) {
    if (pixels[i] != 0) {
      puts("***FAIL microrealm transparent glyph changed framebuffer pixels\n");
      return 1;
    }
  }
  fill_rect(-5, -5, 10, 10, PAL_WHITE);
  fill_rect(SCREEN_W - 2, MAX_RENDER_Y - 2, 10, 10, PAL_WHITE);
  draw_char(-2, -3, 'A', PAL_WHITE);
  draw_char(SCREEN_W - 2, MAX_RENDER_Y - 2, 'A', PAL_WHITE);
  if (pixels[0] != 1234 || pixels[SCREEN_W * MAX_RENDER_Y + 1] != 2345) {
    puts("***FAIL microrealm clipped primitives overwrote boundary guards\n");
    return 1;
  }
  mapping_failed = 1;
  if (render_init()) {
    puts("***FAIL microrealm accepted a failed pixel mapping\n");
    return 1;
  }
  render_title_screen(&scene);
  render_frame(&scene, 160, 120, 0, 0, 0);
  mapping_failed = 0;
  tile_fb_failed = 1;
  if (render_init()) {
    puts("***FAIL microrealm accepted a failed tile framebuffer mapping\n");
    return 1;
  }
  tile_fb_failed = 0;
  tilemap_failed = 1;
  if (render_init()) {
    puts("***FAIL microrealm accepted a failed tilemap mapping\n");
    return 1;
  }
  tilemap_failed = 0;
  if (!render_init()) {
    puts("***FAIL microrealm could not restore valid display mappings\n");
    return 1;
  }
#endif
  audio_init();
  scene.sound_enabled = 1;
  audio_play_sfx(SFX_BELL_DUSK);
  audio_service(&scene);
#ifndef MEDIA_BENCHMARK
  if (queued_bytes != 6000 || accepted_bytes != 6000 || write_calls != 6) {
    puts("***FAIL microrealm audio refill must queue 6000 bytes from an empty ring\n");
    return 1;
  }
  int previous_phase = music_phase_accum;
  audio_service(&scene);
  if (write_calls != 6 || music_phase_accum != previous_phase) {
    puts("***FAIL microrealm full audio queue advanced synthesizer state\n");
    return 1;
  }
  queued_bytes = 0;
  write_limit = 0;
  audio_service(&scene);
  previous_phase = music_phase_accum;
  int previous_calls = write_calls;
  audio_service(&scene);
  if (write_calls != previous_calls + 1 || music_phase_accum != previous_phase) {
    puts("***FAIL microrealm blocked audio write lost pending PCM or failed to return\n");
    return 1;
  }
  for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) expected_pcm[i] = audio_mix_buf[i];
  capture_enabled = 1;
  write_limit = 200;
  audio_service(&scene);
  if (queued_bytes != 6000 || music_phase_accum == previous_phase) {
    puts("***FAIL microrealm partial audio writes failed to refill the ring\n");
    return 1;
  }
  for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) {
    if (captured_pcm[i] != expected_pcm[i]) {
      puts("***FAIL microrealm partial writes reordered or discarded pending samples\n");
      return 1;
    }
  }
#endif
  audio_shutdown();
  puts("***PASS microrealm media guest\n");
  return 0;
}
