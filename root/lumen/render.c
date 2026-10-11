/* Integer software raycaster. All pixels are ordinary user RAM until main.c
 * publishes them to the documented tilemap and sprite MMIO mappings. Drawing
 * is single-threaded. Camera coordinates are Q8; direction vectors are Q10.
 * Bounded DDA traverses the map front to back, then billboards use a per-column
 * depth buffer. No floating point or hardware arithmetic extension is used. */
#include "render.h"

#define TEXTURE_SIZE 16
#define SHADE_COUNT 8
#define TEXTURE_COUNT 6
#define HORIZON 43
#define PROJECTION 106
#define CLEAR_PIXEL 0xF000

unsigned short view_pixels[15360];
unsigned short hud_pixels[10240];
unsigned render_hud_revision;
static unsigned short textures[12288];
static int depth_buffer[VIEW_W];
static int draw_order[41]; /* MAX_ENTITIES plus the lift marker. */
static int draw_depth[41]; /* MAX_ENTITIES plus the lift marker. */
static unsigned short object_pixels[6912]; /* Six object kinds, three colors, 16 by 24. */
static const unsigned short colors[3] = {0xFE3, 0x3BF, 0xF5E};
static unsigned short accents[24]; /* Three deck colors at eight distances. */

/* A floor scanline has direction-dependent Q12 offsets and texture increments. */
struct PlaneProjection {
    int x;
    int y;
    int step_x;
    int step_y;
    unsigned short* texture;
};

/* Each wall ray reuses its direction and DDA distances while the player moves. */
struct WallProjection {
    int x;
    int y;
    int delta_x;
    int delta_y;
};

static struct PlaneProjection plane_projection[VIEW_H];
static struct WallProjection wall_projection[VIEW_W];
static int projection_angle = -1;

/* Only visible HUD inputs participate in invalidation; one user thread owns
 * both this snapshot and the retained image, with no concurrent readers. */
struct HudState {
    int hp;
    int max_hp;
    int energy;
    int relays;
    int color;
    char message[96];
    char detail[96];
};

static struct HudState hud_state;
static int hud_valid;

/* Original compact 5x7 glyphs, packed as five-bit rows from top to bottom. */
static const unsigned char font[69][7] = {
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14},
 {0,4,4,0,4,4,0},{0,0,0,31,0,0,0},{0,0,0,0,0,4,4},
 {0,4,4,31,4,4,0},{1,2,2,4,8,8,16},{4,4,4,4,4,0,4},
 {0,8,4,2,4,8,0},
 {0,0,14,1,15,17,15},{16,16,30,17,17,17,30},{0,0,14,16,16,17,14},
 {1,1,15,17,17,17,15},{0,0,14,17,31,16,14},{6,8,8,28,8,8,8},
 {0,0,15,17,15,1,14},{16,16,30,17,17,17,17},{4,0,12,4,4,4,14},
 {2,0,6,2,2,18,12},{16,16,18,20,24,20,18},{12,4,4,4,4,4,14},
 {0,0,26,21,21,21,21},{0,0,30,17,17,17,17},{0,0,14,17,17,17,14},
 {0,0,30,17,30,16,16},{0,0,15,17,15,1,1},{0,0,22,25,16,16,16},
 {0,0,15,16,14,1,30},{8,8,28,8,8,9,6},{0,0,17,17,17,19,13},
 {0,0,17,17,17,10,4},{0,0,17,17,21,21,10},{0,0,17,10,4,10,17},
 {0,0,17,17,15,1,14},{0,0,31,2,4,8,31}
};

/* Clamp raster edges before touching the row-major target. */
static void rect(unsigned short* out, int width, int height,
                 int x, int y, int w, int h, int color) {
    int right = x + w;
    int bottom = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (x >= right || y >= bottom) return;
    for (int row = y; row < bottom; row++) {
        unsigned short* target = out + row * width + x;
        for (int col = x; col < right; col++) *target++ = color;
    }
}

/* Translate supported UI characters; unsupported punctuation becomes space. */
static int glyph(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 43;
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return c - '0' + 26;
    if (c == ':') return 36;
    if (c == '-') return 37;
    if (c == '.') return 38;
    if (c == '+') return 39;
    if (c == '/') return 40;
    if (c == '!') return 41;
    if (c == '>') return 42;
    return -1;
}

/* Draw clipped text at integer scale, allowing the same font in both planes. */
static void text(unsigned short* out, int width, int height,
                 int x, int y, char* str, int color, int scale) {
    while (*str && x < width) {
        int index = glyph(*str++);
        if (index >= 0) {
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (font[index][row] & (16 >> col)) {
                        rect(out, width, height, x + col * scale, y + row * scale,
                             scale, scale, color);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

/* Shade each VGA color channel independently to avoid cross-channel carries. */
static int shade(int color, int level) {
    int r = (color & 15) * level / 8;
    int g = ((color >> 4) & 15) * level / 8;
    int b = ((color >> 8) & 15) * level / 8;
    return r | (g << 4) | (b << 8);
}

/* Paint a small transparent billboard: relay, drone, repair, cell, or archive. */
static int object_pixel(int type, int color, int x, int y) {
    int c = colors[color];
    int ax = x - 7;
    if (ax < 0) ax = -ax;
    if (type == 0) {
        if (y >= 21 && x >= 2 && x <= 13) return y == 21 ? 0x778 : 0x334;
        if ((x == 1 || x == 14) && y >= 4 && y <= 21) return (y & 3) == 0 ? c : 0x445;
        if (y >= 2 && y <= 19) {
            int radius = y < 10 ? (y - 1) / 2 : (20 - y) / 2;
            if (ax <= radius) return x < 7 ? c : shade(c, 5);
            if (ax == radius + 1 && y > 4 && y < 16) return shade(c, 2);
        }
    } else if (type == 1) {
        if (y >= 8 && y <= 13 && (x <= 2 || x >= 13)) return y == 9 ? c : 0x445;
        if (y >= 5 && y <= 16 && ax <= 5) {
            if (y == 5 || y == 16 || ax == 5) return 0x223;
            if (y == 9 || y == 10) return ax < 3 ? c : 0x112;
            return y < 9 ? 0x99A : 0x556;
        }
        if (y >= 17 && y <= 20 && ax < 3) return y == 17 ? c : shade(c, 3);
        if ((x == 4 || x == 11) && y >= 2 && y <= 4) return 0x667;
    } else if (type == 2) {
        if (x >= 3 && x <= 12 && y >= 11 && y <= 21) {
            if (x == 3 || x == 12 || y == 11 || y == 21) return 0xAAA;
            if ((x >= 7 && x <= 8) || (y >= 15 && y <= 16)) return 0x5E6;
            return 0x242;
        }
    } else if (type == 3) {
        if (x >= 5 && x <= 10 && y >= 9 && y <= 21) {
            if (y == 9 || y == 21) return 0xBBB;
            return ((y / 3) & 1) ? 0xFB6 : 0x753;
        }
    } else if (type == 4) {
        if (x >= 2 && x <= 13 && y >= 5 && y <= 18) {
            if (y == 5 || y == 18 || x == 2 || x == 13) return 0x667;
            return (y & 2) && x < 11 ? 0xDB5 : 0x321;
        }
        if (x >= 6 && x <= 9 && y >= 19) return 0x556;
    }
    if (type == 5) {
        if (y >= 1 && y <= 23 && (x <= 1 || x >= 14)) return c;
        if (y <= 2 || y >= 22) return c;
        if (y >= 6 && y <= 14 && ax <= (y < 10 ? y - 6 : 2)) return c;
        if (y >= 16 && y <= 18 && x >= 4 && x <= 11) return 0xAAA;
    }
    return CLEAR_PIXEL;
}

/* Build immutable textured metal, glass, bulkheads, floor, ceiling, and exit. */
void render_init(void) {
    projection_angle = -1;
    render_invalidate_hud();
    for (int color = 0; color < 3; color++) {
        for (int level = 0; level < SHADE_COUNT; level++) {
            accents[color * SHADE_COUNT + level] = shade(colors[color], 8 - level);
        }
    }
    for (int kind = 0; kind < TEXTURE_COUNT; kind++) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int c = 0x456;
                if (kind == 0) {
                    c = 0x456 + (((x * 13 + y * 7) & 3) == 0 ? 0x111 : 0);
                    if (y == 0 || y == 15 || x == 0) c = 0x223;
                    if (y == 3 && x > 2 && x < 14) c = 0xCB4;
                    if ((x == 2 || x == 13) && (y == 2 || y == 13)) c = 0x99A;
                    if (y > 8 && y < 13 && x > 3 && x < 12 && !(y & 1)) c = 0x234;
                } else if (kind == 1) {
                    c = 0x632;
                    if (x == 0 || y == 0 || x == 15 || y == 15) c = 0x778;
                    else if (x == y || x == y + 1 || x == y - 6) c = 0xA64;
                    else if ((x & 3) == 0 || (y & 3) == 0) c = 0x743;
                } else if (kind == 2) {
                    c = 0x556;
                    if (x == 7 || x == 8) c = 0x112;
                    if (y < 2 || y > 13) c = ((x + y) & 4) ? 0x3BF : 0x222;
                    if (y > 5 && y < 10 && x > 10 && x < 14) c = 0x5BF;
                } else if (kind == 3) {
                    c = 0x223;
                    if (x == 0 || y == 0) c = 0x445;
                    if (x == 1 || y == 1) c = 0x112;
                    if (x > 12 && y > 12) c = 0x754;
                    if (((x * 7 + y * 11) & 31) == 0) c = 0x334;
                } else if (kind == 4) {
                    c = 0x211;
                    if (x == 0 || y == 0) c = 0x433;
                    if ((x == 7 || x == 8) && y > 3 && y < 13) c = 0xA75;
                } else {
                    c = 0x433;
                    if (x == 0 || x == 15 || y == 0 || y == 15) c = 0xBC5;
                    if ((x + y / 2) % 7 == 0) c = 0xDE7;
                }
                for (int level = 0; level < SHADE_COUNT; level++) {
                    textures[(kind * SHADE_COUNT + level) * 256 + y * 16 + x] = shade(c, 8 - level);
                }
            }
        }
    }
    for (int kind = 0; kind < 6; kind++) {
        for (int color = 0; color < 3; color++) {
            for (int y = 0; y < 24; y++) {
                for (int x = 0; x < 16; x++) {
                    object_pixels[(kind * 3 + color) * 384 + y * 16 + x] = object_pixel(kind, color, x, y);
                }
            }
        }
    }
}

/* Invalidation is consumed by the next frame; revision changes after drawing. */
void render_invalidate_hud(void) {
    hud_valid = 0;
}

/* Cache direction-only divisions until the camera turns. Camera translation
 * and live map cells remain uncached, so motion and opened doors are immediate.
 * Expressions retain the original signed-division rounding at each step. */
static void prepare_projection(int angle, int vx, int vy) {
    angle &= 255;
    if (angle == projection_angle) return;
    projection_angle = angle;
    for (int y = 0; y < VIEW_H; y++) {
        int offset = y - HORIZON;
        int kind = offset > 0 ? 3 : 4;
        if (offset < 0) offset = -offset;
        if (offset == 0) offset = 1;
        int distance = PROJECTION * 128 / offset;
        int level = distance / 400;
        if (level > 7) level = 7;
        int side_x = -vy * distance / 64;
        int side_y = vx * distance / 64;
        struct PlaneProjection* row = &plane_projection[y];
        row->x = vx * distance / 64 - side_x * 80 / PROJECTION;
        row->y = vy * distance / 64 - side_y * 80 / PROJECTION;
        row->step_x = side_x / PROJECTION;
        row->step_y = side_y / PROJECTION;
        row->texture = textures + (kind * SHADE_COUNT + level) * 256;
    }
    for (int x = 0; x < VIEW_W; x++) {
        struct WallProjection* ray = &wall_projection[x];
        ray->x = vx - vy * (x - 80) / PROJECTION;
        ray->y = vy + vx * (x - 80) / PROJECTION;
        int abs_x = ray->x < 0 ? -ray->x : ray->x;
        int abs_y = ray->y < 0 ? -ray->y : ray->y;
        ray->delta_x = abs_x ? 262144 / abs_x : 1000000;
        ray->delta_y = abs_y ? 262144 / abs_y : 1000000;
    }
}

/* Interpolate floor/ceiling in Q12 across each scanline. Extra fractional bits
 * keep distant textures stable while avoiding per-pixel division. */
static void planes(int px, int py) {
    int camera_x = px * 16;
    int camera_y = py * 16;
    for (int y = 0; y < VIEW_H; y++) {
        struct PlaneProjection* row = &plane_projection[y];
        int world_x = camera_x + row->x;
        int world_y = camera_y + row->y;
        int step_x = row->step_x;
        int step_y = row->step_y;
        unsigned short* target = view_pixels + y * VIEW_W;
        unsigned short* texture = row->texture;
        for (int x = 0; x < VIEW_W; x++) {
            int tx = ((unsigned)world_x >> 8) & 15;
            int ty = ((unsigned)world_y >> 8) & 15;
            *target++ = texture[ty * 16 + tx];
            world_x += step_x;
            world_y += step_y;
        }
    }
}

/* Cast a bounded grid ray per column, retaining perpendicular depth for sprites. */
static void walls(struct Game* g, int px, int py) {
    unsigned short* deck_accents = accents + ((g->sector - 1) % 3) * SHADE_COUNT;
    for (int x = 0; x < VIEW_W; x++) {
        struct WallProjection* ray = &wall_projection[x];
        int ray_x = ray->x;
        int ray_y = ray->y;
        int delta_x = ray->delta_x;
        int delta_y = ray->delta_y;
        int map_x = px / 256;
        int map_y = py / 256;
        int step_x = ray_x < 0 ? -1 : 1;
        int step_y = ray_y < 0 ? -1 : 1;
        int side_x = (ray_x < 0 ? (px & 255) : 256 - (px & 255)) * delta_x / 256;
        int side_y = (ray_y < 0 ? (py & 255) : 256 - (py & 255)) * delta_y / 256;
        int distance = 8192;
        int side = 0;
        int cell = CELL_WALL;
        for (int step = 0; step < WORLD_W + WORLD_H; step++) {
            int x_first = side_x < side_y;
            if (side_x == side_y) {
                /* Integer DDA ties can be rounded; only an exact geometric
                 * corner needs both side walls checked, like the beam ray.
                 * Give the x wall priority; the normal y step tests the other. */
                int gap_x = ray_x < 0 ? px - map_x * 256 : (map_x + 1) * 256 - px;
                int gap_y = ray_y < 0 ? py - map_y * 256 : (map_y + 1) * 256 - py;
                int abs_x = ray_x < 0 ? -ray_x : ray_x;
                int abs_y = ray_y < 0 ? -ray_y : ray_y;
                if (gap_x * abs_y == gap_y * abs_x) {
                    int corner = game_cell(g, map_x + step_x, map_y);
                    if (corner == CELL_WALL || corner == CELL_GLASS || corner == CELL_DOOR) x_first = 1;
                }
            }
            if (x_first) {
                distance = side_x;
                side_x += delta_x;
                map_x += step_x;
                side = 0;
            } else {
                distance = side_y;
                side_y += delta_y;
                map_y += step_y;
                side = 1;
            }
            cell = game_cell(g, map_x, map_y);
            if (cell == CELL_WALL || cell == CELL_GLASS || cell == CELL_DOOR) break;
        }
        if (distance < 24) distance = 24;
        depth_buffer[x] = distance;
        int span = PROJECTION * 256 / distance;
        if (span > 1130) span = 1130;
        int top = HORIZON - span / 2;
        int bottom = top + span;
        int coordinate = side ? px + ray_x * distance / 1024 : py + ray_y * distance / 1024;
        int tex_x = ((unsigned)coordinate >> 4) & 15;
        int kind = cell == CELL_GLASS ? 1 : (cell == CELL_DOOR ? 2 : 0);
        int level = distance / 512 + side;
        if (level > 7) level = 7;
        int first = top < 0 ? 0 : top;
        int last = bottom > VIEW_H ? VIEW_H : bottom;
        int tex_step = 4096 / (span ? span : 1);
        int tex_pos = (first - top) * tex_step;
        unsigned short* target = view_pixels + first * VIEW_W;
        unsigned short* texture = textures + (kind * SHADE_COUNT + level) * 256;
        int accent = deck_accents[level];
        for (int y = first; y < last; y++) {
            int tex_y = tex_pos >> 8;
            if (tex_y > 15) tex_y = 15;
            int pixel = texture[tex_y * 16 + tex_x];
            /* Accent bands give each deck its own luminous circuit color. */
            if (kind == 0 && tex_y == 3 && tex_x > 2 && tex_x < 14) pixel = accent;
            target[x] = pixel;
            target += VIEW_W;
            tex_pos += tex_step;
        }
    }
}

/* Project objects far to near and clip their columns against wall depth. */
static void objects(struct Game* g, int px, int py, int vx, int vy, unsigned frame) {
    int count = 0;
    /* A virtual billboard marks the walkable lift without adding a blocker. */
    struct Entity lift;
    lift.type = 6;
    lift.x = g->exit_x;
    lift.y = g->exit_y;
    lift.color = g->relays == 3 ? 0 : 1;
    lift.active = 1;
    lift.health = 1;
    lift.alert = 0;
    for (int i = 0; i <= g->entity_count; i++) {
        struct Entity* e = i == g->entity_count ? &lift : &g->entities[i];
        if (!e->active && e->type != ENTITY_RELAY) continue;
        int dx = e->x * 256 + 128 - px;
        int dy = e->y * 256 + 128 - py;
        int depth = (dx * vx + dy * vy) / 1024;
        if (depth < 48) continue;
        int slot = count;
        while (slot > 0 && draw_depth[slot - 1] < depth) {
            draw_depth[slot] = draw_depth[slot - 1];
            draw_order[slot] = draw_order[slot - 1];
            slot--;
        }
        draw_depth[slot] = depth;
        draw_order[slot] = i;
        count++;
    }
    for (int i = 0; i < count; i++) {
        struct Entity* e = draw_order[i] == g->entity_count ? &lift : &g->entities[draw_order[i]];
        int depth = draw_depth[i];
        int dx = e->x * 256 + 128 - px;
        int dy = e->y * 256 + 128 - py;
        int side = (-dx * vy + dy * vx) / 1024;
        int center = 80 + side * PROJECTION / depth;
        int height = PROJECTION * 210 / depth;
        int width = height * 2 / 3;
        if (width < 1 || height < 1) continue;
        int bottom = HORIZON + PROJECTION * 128 / depth;
        if (e->type == ENTITY_DRONE) bottom -= 3 + (int)((frame / 8 + i) & 1);
        int top = bottom - height;
        int left = center - width / 2;
        int start = left < 0 ? 0 : left;
        int end = left + width > VIEW_W ? VIEW_W : left + width;
        int first = top < 0 ? 0 : top;
        int last = bottom > VIEW_H ? VIEW_H : bottom;
        int kind = e->type - 1;
        if (kind < 0 || kind > 5) continue;
        int color = e->color % 3;
        if (color < 0) color = 0;
        int step = 6144 / height;
        int tx = (start - left) * 16 / width;
        int tx_remainder = (start - left) * 16 - tx * width;
        int tx_step = 16 / width;
        int tx_fraction = 16 - tx_step * width;
        unsigned short* texture = object_pixels + (kind * 3 + color) * 384;
        for (int x = start; x < end; x++) {
            /* Quotient/remainder stepping exactly matches (x-left)*16/width
             * while removing software division from every sprite column. */
            int sample_x = tx;
            tx += tx_step;
            tx_remainder += tx_fraction;
            if (tx_remainder >= width) { tx++; tx_remainder -= width; }
            if (depth >= depth_buffer[x]) continue;
            int pos = (first - top) * step;
            unsigned short* target = view_pixels + first * VIEW_W;
            for (int y = first; y < last; y++) {
                int ty = pos >> 8;
                if (ty > 23) ty = 23;
                int pixel = texture[ty * 16 + sample_x];
                if (pixel != CLEAR_PIXEL) {
                    if (e->type == ENTITY_RELAY && !e->active && pixel == colors[color]) pixel = 0xFFF;
                    if (e->type == ENTITY_DRONE && e->alert >= 2 && ty < 6) pixel = 0x33F;
                    target[x] = pixel;
                }
                target += VIEW_W;
                pos += step;
            }
        }
        if (e->type == ENTITY_DRONE && e->alert >= 1 && center >= 2 && center < 158 && top > 9) {
            /* Alert pixels use the same wall depth test as their sentry. */
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    int marker_x = center - 2 + col;
                    int marker_y = top - 9 + row;
                    if (marker_y < VIEW_H && depth < depth_buffer[marker_x] &&
                        (font[41][row] & (16 >> col))) {
                        view_pixels[marker_y * VIEW_W + marker_x] = e->alert >= 2 ? 0x33F : 0x3BF;
                    }
                }
            }
        }
    }
}

/* Plot a clipped line for energy beams, weapon edges, and minimap direction. */
static void line(int x0, int y0, int x1, int y1, int color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int error = dx - dy;
    for (;;) {
        if (x0 >= 0 && x0 < VIEW_W && y0 >= 0 && y0 < VIEW_H) view_pixels[y0 * VIEW_W + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int doubled = error * 2;
        if (doubled > -dy) { error -= dy; x0 += sx; }
        if (doubled < dx) { error += dx; y0 += sy; }
    }
}

/* Draw the prism emitter, its color selector, and a sparse centered reticle. */
static void weapon(struct Game* g, unsigned frame) {
    int c = colors[g->color];
    int recoil = g->shot ? 3 : 0;
    for (int y = 71 + recoil; y < VIEW_H; y++) {
        int half = 5 + (y - 71) / 2;
        rect(view_pixels, VIEW_W, VIEW_H, 106 - half, y, half * 2, 1, 0x223);
        rect(view_pixels, VIEW_W, VIEW_H, 108 - half, y, half * 2 - 4, 1, y & 1 ? 0x667 : 0x556);
    }
    rect(view_pixels, VIEW_W, VIEW_H, 103, 70 + recoil, 8, 17, 0x112);
    rect(view_pixels, VIEW_W, VIEW_H, 105, 70 + recoil, 4, 13, c);
    rect(view_pixels, VIEW_W, VIEW_H, 113, 84 + recoil, 5, 3, c);
    rect(view_pixels, VIEW_W, VIEW_H, 74, HORIZON, 4, 1, 0xAAA);
    rect(view_pixels, VIEW_W, VIEW_H, 83, HORIZON, 4, 1, 0xAAA);
    rect(view_pixels, VIEW_W, VIEW_H, 80, HORIZON - 5, 1, 3, 0xAAA);
    rect(view_pixels, VIEW_W, VIEW_H, 80, HORIZON + 3, 1, 3, 0xAAA);
    if (g->shot) {
        line(106, 71, 80, HORIZON, c);
        line(107, 71, 81, HORIZON, 0xFFF);
        int size = 2 + (int)(frame & 3);
        line(80 - size, HORIZON, 80 + size, HORIZON, c);
        line(80, HORIZON - size, 80, HORIZON + size, c);
    }
    if (g->flash) {
        rect(view_pixels, VIEW_W, VIEW_H, 0, 0, VIEW_W, 2, 0x33B);
        rect(view_pixels, VIEW_W, VIEW_H, 0, VIEW_H - 2, VIEW_W, 2, 0x33B);
        rect(view_pixels, VIEW_W, VIEW_H, 0, 0, 2, VIEW_H, 0x33B);
        rect(view_pixels, VIEW_W, VIEW_H, VIEW_W - 2, 0, 2, VIEW_H, 0x33B);
    }
}

/* Append an unsigned decimal without CRT formatting or dynamic allocation. */
static void number(char* out, int value) {
    char digits[12];
    int n = 0;
    if (value < 0) value = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value && n < 11);
    int j = 0;
    while (n) out[j++] = digits[--n];
    out[j] = 0;
}

/* Compare bounded radio strings without reading beyond their terminators. */
static int same_radio(char* left, char* right) {
    for (int i = 0; i < 96; i++) {
        if (left[i] != right[i]) return 0;
        if (!left[i]) return 1;
    }
    return 1;
}

/* Snapshot a radio line without inheriting unused bytes after its terminator. */
static void copy_radio(char* target, char* source) {
    for (int i = 0; i < 95; i++) {
        target[i] = source[i];
        if (!source[i]) return;
    }
    target[95] = 0;
}

/* Keep the objective beside both radio lines, redrawing only on input changes.
 * The output buffer remains renderer-owned between calls; its revision lets
 * main skip display publication entirely when the retained HUD is unchanged. */
static void hud(struct Game* g) {
    if (hud_valid && hud_state.hp == g->hp && hud_state.max_hp == g->max_hp &&
        hud_state.energy == g->energy && hud_state.relays == g->relays &&
        hud_state.color == g->color && same_radio(hud_state.message, g->message) &&
        same_radio(hud_state.detail, g->detail)) return;
    hud_state.hp = g->hp;
    hud_state.max_hp = g->max_hp;
    hud_state.energy = g->energy;
    hud_state.relays = g->relays;
    hud_state.color = g->color;
    copy_radio(hud_state.message, g->message);
    copy_radio(hud_state.detail, g->detail);
    rect(hud_pixels, HUD_W, HUD_H, 0, 0, HUD_W, HUD_H, 0x211);
    rect(hud_pixels, HUD_W, HUD_H, 0, 0, HUD_W, 1, 0x754);
    char n[12];
    text(hud_pixels, HUD_W, HUD_H, 3, 1, "HULL", 0xBBA, 1);
    number(n, g->hp);
    text(hud_pixels, HUD_W, HUD_H, 33, 1, n, g->hp * 3 < g->max_hp ? 0x55F : 0xBF7, 1);
    rect(hud_pixels, HUD_W, HUD_H, 58, 2, 33, 5, 0x433);
    rect(hud_pixels, HUD_W, HUD_H, 59, 3, g->hp * 31 / (g->max_hp ? g->max_hp : 1), 3, 0xAD4);
    text(hud_pixels, HUD_W, HUD_H, 97, 1, "CHARGE", 0xBBA, 1);
    number(n, g->energy);
    text(hud_pixels, HUD_W, HUD_H, 139, 1, n, 0xFC6, 1);
    text(hud_pixels, HUD_W, HUD_H, 162, 1, "RELAYS", 0xBBA, 1);
    number(n, g->relays);
    text(hud_pixels, HUD_W, HUD_H, 204, 1, n, 0xFFF, 1);
    text(hud_pixels, HUD_W, HUD_H, 210, 1, "/3", 0x887, 1);
    for (int i = 0; i < 3; i++) {
        rect(hud_pixels, HUD_W, HUD_H, 239 + i * 26, 1, 21, 7, g->color == i ? colors[i] : 0x544);
        number(n, i + 1);
        text(hud_pixels, HUD_W, HUD_H, 247 + i * 26, 1, n, g->color == i ? 0x111 : colors[i], 1);
    }
    char* objective = g->relays < 3 ? "RELAYS: MATCH 1/2/3 + SPACE   M SCAN  H HELP" :
                      "RELAYS LIT: GREEN LIFT + E   M SCAN  H HELP";
    text(hud_pixels, HUD_W, HUD_H, 3, 9, objective, 0xBF7, 1);
    text(hud_pixels, HUD_W, HUD_H, 3, 17, g->message, 0xDDB, 1);
    text(hud_pixels, HUD_W, HUD_H, 3, 25, g->detail, 0xA97, 1);
    hud_valid = 1;
    render_hud_revision++;
}

/* Dim the already-rendered scene beneath a sharply bordered instrument panel. */
static void panel(int x, int y, int w, int h) {
    for (int row = y; row < y + h && row < VIEW_H; row++) {
        unsigned short* target = view_pixels + row * VIEW_W + x;
        for (int col = x; col < x + w && col < VIEW_W; col++) {
            int p = *target;
            *target++ = (p & 0xCCC) >> 2;
        }
    }
    rect(view_pixels, VIEW_W, VIEW_H, x, y, w, 1, 0x875);
    rect(view_pixels, VIEW_W, VIEW_H, x, y + h - 1, w, 1, 0x543);
    rect(view_pixels, VIEW_W, VIEW_H, x, y, 1, h, 0x543);
    rect(view_pixels, VIEW_W, VIEW_H, x + w - 1, y, 1, h, 0x543);
}

/* Show explored cells with objective colors and explicit map legend. */
static void map_screen(struct Game* g) {
    panel(4, 4, 152, 88);
    text(view_pixels, VIEW_W, VIEW_H, 10, 9, "STATION SCAN", 0xFE7, 1);
    for (int y = 0; y < WORLD_H; y++) {
        for (int x = 0; x < WORLD_W; x++) {
            int index = y * WORLD_W + x;
            int c = 0x211;
            if (g->seen[index]) {
                int cell = g->cells[index];
                c = cell == CELL_WALL || cell == CELL_GLASS ? 0x655 : 0x433;
                if (cell == CELL_DOOR) c = 0x3AF;
                if (cell == CELL_EXIT) c = 0xBF5;
            }
            rect(view_pixels, VIEW_W, VIEW_H, 12 + x * 3, 22 + y * 3, 2, 2, c);
        }
    }
    for (int i = 0; i < g->entity_count; i++) {
        struct Entity* e = &g->entities[i];
        if (!g->seen[e->y * WORLD_W + e->x] || (!e->active && e->type != ENTITY_RELAY)) continue;
        int c = e->type == ENTITY_RELAY ? colors[e->color % 3] : (e->type == ENTITY_DRONE ? 0x44F : 0xEEE);
        rect(view_pixels, VIEW_W, VIEW_H, 12 + e->x * 3, 22 + e->y * 3, 2, 2, c);
    }
    int x = 13 + g->x * 3;
    int y = 23 + g->y * 3;
    line(x, y, x + game_sine(g->angle) * 4 / 1024,
         y - game_sine(g->angle + 64) * 4 / 1024, 0xFFF);
    rect(view_pixels, VIEW_W, VIEW_H, x - 1, y - 1, 2, 2, 0xFFF);
    text(view_pixels, VIEW_W, VIEW_H, 83, 24, "YOU", 0xFFF, 1);
    text(view_pixels, VIEW_W, VIEW_H, 83, 35, "SENTRY", 0x44F, 1);
    text(view_pixels, VIEW_W, VIEW_H, 83, 46, "RELAY", 0xFE3, 1);
    text(view_pixels, VIEW_W, VIEW_H, 83, 57, "LIFT", 0xBF5, 1);
    text(view_pixels, VIEW_W, VIEW_H, 83, 72, "M CLOSE", 0xAAA, 1);
}

/* Present concise in-game controls; looking, scanning, and color changes are free. */
static void help_screen(void) {
    panel(4, 4, 152, 88);
    text(view_pixels, VIEW_W, VIEW_H, 12, 10, "FIELD MANUAL", 0xFE7, 1);
    text(view_pixels, VIEW_W, VIEW_H, 12, 24, "W S   MOVE", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 12, 34, "A D   AIM FREELY", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 12, 44, "Z C   SIDESTEP", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 12, 54, "SPACE FIRE  E USE", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 12, 64, "1 2 3 BEAM  X WAIT", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 12, 78, "H CLOSE  M MAP  Q QUIT", 0xA97, 1);
}

/* Teach the mission before play begins; reading and looking never spend turns. */
static void briefing_screen(void) {
    panel(4, 3, 152, 91);
    text(view_pixels, VIEW_W, VIEW_H, 14, 8, "RESTORE THE STATION", 0xFE7, 1);
    text(view_pixels, VIEW_W, VIEW_H, 14, 21, "FIND 3 RELAYS PER DECK", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 14, 31, "MATCH 1/2/3 + SPACE", 0xFE7, 1);
    text(view_pixels, VIEW_W, VIEW_H, 14, 41, "THEN GREEN LIFT: E", 0xBF7, 1);
    text(view_pixels, VIEW_W, VIEW_H, 14, 54, "W/S MOVE  A/D AIM", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 14, 64, "M SCAN  H HELP", 0xDDD, 1);
    text(view_pixels, VIEW_W, VIEW_H, 14, 74, "LOOKING COSTS NO TURN", 0xA97, 1);
    text(view_pixels, VIEW_W, VIEW_H, 38, 84, "ENTER TO BEGIN", 0xFFF, 1);
}

/* Compose animated 3D, state-specific overlays, and the independent HUD strip. */
void render_frame(struct Game* g, int px, int py, int angle, unsigned frame, int screen) {
    int vx = game_sine(angle);
    int vy = -game_sine(angle + 64);
    prepare_projection(angle, vx, vy);
    planes(px, py);
    walls(g, px, py);
    objects(g, px, py, vx, vy, frame);
    if (g->mode == MODE_PLAY) {
        weapon(g, frame);
        char deck[11] = "DECK 1 / 3";
        deck[5] = '0' + g->sector;
        text(view_pixels, VIEW_W, VIEW_H, 3, 3, deck, 0xCCC, 1);
        char* directions[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        text(view_pixels, VIEW_W, VIEW_H, 145, 3, directions[((angle + 16) & 255) / 32], 0xFE7, 1);
        if (screen == SCREEN_MAP) map_screen(g);
        if (screen == SCREEN_HELP) help_screen();
        if (screen == SCREEN_BRIEFING) briefing_screen();
        if (screen == SCREEN_PAUSE) {
            panel(20, 28, 120, 38);
            text(view_pixels, VIEW_W, VIEW_H, 43, 35, "SUSPENDED", 0xFE7, 1);
            text(view_pixels, VIEW_W, VIEW_H, 34, 51, "P TO CONTINUE", 0xAAA, 1);
        }
    } else if (g->mode == MODE_TITLE) {
        panel(7, 5, 146, 87);
        text(view_pixels, VIEW_W, VIEW_H, 13, 12, "A DIOPTASE EXPEDITION", 0xA97, 1);
        text(view_pixels, VIEW_W, VIEW_H, 36, 28, "LUMEN", 0xFE7, 3);
        text(view_pixels, VIEW_W, VIEW_H, 26, 54, "THE GLASS STATION", 0xCCC, 1);
        text(view_pixels, VIEW_W, VIEW_H, 47, 66, "gpt-6-astra", 0xA97, 1);
        if ((frame / 30 & 1) == 0) text(view_pixels, VIEW_W, VIEW_H, 32, 81, "ENTER TO DESCEND", 0xFFF, 1);
    } else if (g->mode == MODE_UPGRADE) {
        panel(4, 5, 152, 86);
        text(view_pixels, VIEW_W, VIEW_H, 15, 12, "RELAY LINK RESTORED", 0xBF7, 1);
        text(view_pixels, VIEW_W, VIEW_H, 15, 29, "CHOOSE AN UPGRADE", 0xDDD, 1);
        text(view_pixels, VIEW_W, VIEW_H, 15, 44, "1  REINFORCED HULL", 0xBF7, 1);
        text(view_pixels, VIEW_W, VIEW_H, 15, 57, "2  AMPLIFIED BEAM", 0xF8F, 1);
        text(view_pixels, VIEW_W, VIEW_H, 15, 70, "3  DEEP CAPACITOR", 0xFC7, 1);
    } else {
        panel(8, 14, 144, 69);
        text(view_pixels, VIEW_W, VIEW_H, g->mode == MODE_WON ? 23 : 29, 24,
             g->mode == MODE_WON ? "STATION REAWAKENED" : "SIGNAL LOST", g->mode == MODE_WON ? 0xBF7 : 0x66F, 1);
        char n[12];
        number(n, g->score);
        text(view_pixels, VIEW_W, VIEW_H, 29, 42, "SCORE", 0xAAA, 1);
        text(view_pixels, VIEW_W, VIEW_H, 77, 42, n, 0xFFF, 1);
        text(view_pixels, VIEW_W, VIEW_H, 23, 63, "R RETRY  N NEW RUN", 0xDDB, 1);
    }
    hud(g);
}

/* Check bounded pixel values in the gameplay overlays at a seeded spawn. */
int render_selftest(void) {
    struct Game g;
    game_start(&g, 42);
    render_init();
    unsigned checksum = 0;
    for (int screen = 0; screen <= SCREEN_BRIEFING; screen++) {
        render_frame(&g, g.x * 256 + 128, g.y * 256 + 128, g.angle, 0, screen);
        for (int i = 0; i < VIEW_W * VIEW_H; i++) {
            if (view_pixels[i] > 0xFFF) return 1;
            checksum += view_pixels[i];
        }
        for (int i = 0; i < HUD_W * HUD_H; i++) {
            if (hud_pixels[i] > 0xFFF) return 2;
        }
    }
    if (checksum == 0) return 3;
    return 0;
}
