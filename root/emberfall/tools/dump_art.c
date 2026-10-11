/* Host art dumper: builds render.c's procedural sprites with stubbed MMIO
 * and writes them as PPM for visual review. Not shipped to the guest. */
#include <assert.h>
#include <stdio.h>

static short stub_pixel[320 * 236];
static short stub_tfb[80 * 60];
static short stub_tmap[256 * 64];
static short stub_sprites[16 * 1024];

volatile short* get_pixel_fb(void) { return stub_pixel; }
volatile short* get_tile_fb(void) { return stub_tfb; }
volatile short* get_tilemap(void) { return stub_tmap; }
volatile short* get_spritemap(void) { return stub_sprites; }
void set_tile_scale(unsigned scale) { (void)scale; }
void set_vscroll(unsigned value) { (void)value; }
void set_hscroll(unsigned value) { (void)value; }
void load_text_tiles(void) {}
int set_pixel_scale(unsigned scale) { (void)scale; return 0; }
int set_sprite_scale(unsigned n, unsigned s) {
  (void)n;
  (void)s;
  return 0;
}
int set_sprite_coords(unsigned n, unsigned x, unsigned y) {
  (void)n;
  (void)x;
  (void)y;
  return 0;
}

#include "../render.c"

static void dump_sheet(char* path) {
  /* 8 warden facings in a row, each 12x12, on magenta. */
  static unsigned char px[12][8 * 12][3];
  int f = 0;
  int x = 0;
  int y = 0;
  FILE* fp = 0;
  while (f < 8) {
    y = 0;
    while (y < 12) {
      x = 0;
      while (x < 12) {
        short c = art_player[f][y * 12 + x];
        unsigned char* p = px[y][f * 12 + x];
        if (c == TRANSPARENT_PX) {
          p[0] = 255;
          p[1] = 0;
          p[2] = 255;
        } else {
          p[0] = (unsigned char)((c & 0xF) * 16);
          p[1] = (unsigned char)(((c >> 4) & 0xF) * 16);
          p[2] = (unsigned char)(((c >> 8) & 0xF) * 16);
        }
        x++;
      }
      y++;
    }
    f++;
  }
  fp = fopen(path, "wb");
  if (!fp) return;
  fprintf(fp, "P6\n96 12\n255\n");
  y = 0;
  while (y < 12) {
    x = 0;
    while (x < 96) {
      fwrite(px[y][x], 1, 3, fp);
      x++;
    }
    y++;
  }
  fclose(fp);
}

static void dump_icons(char* path) {
  /* 12 upgrade icons in a row, each 12x12, on magenta. */
  static unsigned char px[12][12 * 12][3];
  int f = 0;
  int x = 0;
  int y = 0;
  FILE* fp = 0;
  while (f < 12) {
    y = 0;
    while (y < 12) {
      x = 0;
      while (x < 12) {
        short c = art_icon[f][y * 12 + x];
        unsigned char* p = px[y][f * 12 + x];
        if (c == TRANSPARENT_PX) {
          p[0] = 255;
          p[1] = 0;
          p[2] = 255;
        } else {
          p[0] = (unsigned char)((c & 0xF) * 16);
          p[1] = (unsigned char)(((c >> 4) & 0xF) * 16);
          p[2] = (unsigned char)(((c >> 8) & 0xF) * 16);
        }
        x++;
      }
      y++;
    }
    f++;
  }
  fp = fopen(path, "wb");
  if (!fp) return;
  fprintf(fp, "P6\n144 12\n255\n");
  y = 0;
  while (y < 12) {
    x = 0;
    while (x < 144) {
      fwrite(px[y][x], 1, 3, fp);
      x++;
    }
    y++;
  }
  fclose(fp);
}

static void check_art(void) {
  int fx = 0;
  int fy = 0;
  int seen = 0;
  int f = 0;
  /* Every move direction maps to a distinct facing. */
  fy = -1;
  while (fy <= 1) {
    fx = -1;
    while (fx <= 1) {
      if (fx != 0 || fy != 0) {
        int fi = facing_index(fx, fy);
        assert(fi >= 0 && fi < 8);
        assert((seen & (1 << fi)) == 0);
        seen |= 1 << fi;
      }
      fx++;
    }
    fy++;
  }
  /* Rotation preserves the face-center pixel the e2e tracks. */
  while (f < 8) {
    assert(art_player[f][6 * 12 + 6] == (short)0xF9B);
    f++;
  }
  /* Every upgrade icon has visible (non-disc, non-clear) pixels. */
  f = 0;
  while (f < 12) {
    int motif = 0;
    int i = 0;
    while (i < 144) {
      short c = art_icon[f][i];
      if (c != TRANSPARENT_PX && c != (short)0x222) motif++;
      i++;
    }
    assert(motif > 0);
    f++;
  }
}

int main(void) {
  render_init();
  check_art();
  dump_sheet("/tmp/facings.ppm");
  dump_icons("/tmp/icons.ppm");
  printf("art dumped\n");
  return 0;
}
