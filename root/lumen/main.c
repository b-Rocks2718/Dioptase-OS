/* LUMEN runs entirely in one foreground user process. Display and audio access
 * use the CRT trap ABI (r1 code, r2-r8 arguments; caller-saved r1-r19), never
 * physical MMIO addresses or privileged instructions. The compiler owns the
 * ABI's four-byte-aligned stack; no TLS or shared-memory worker is used.
 *
 * Display ownership follows the existing foreground shell contract: hide and
 * drain terminal output before changing tiles, emit no console output while
 * owning graphics, and let the shell restore its font/display after exit.
 * Tile and sprite writes are sequentially consistent but not frame-atomic;
 * there is no documented double-buffer swap, so brief scanout tearing is possible.
 */
#include "game.h"
#include "render.h"
#include "music.h"
#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/string.h"
#include "../crt/ps2.h"
#include "../crt/vga.h"

#define VIEW_TILE_COLUMNS 20
#define VIEW_TILE_ROWS 12
#define BACKGROUND_TILE 240
#define HUD_SPRITES 10
#define KEY_RELEASE 0x100
#define FRAME_INTERVAL 1
/* Durations use 60 Hz scanout ticks, independent of the renderer's throughput. */
#define MOVE_TICKS 8
#define TURN_TICKS 3
#define AUDIO_CHUNK_SAMPLES 1024
#define AUDIO_TARGET_BYTES 12000

static struct Game game;
static volatile short* tilemap;
static volatile short* tilefb;
static volatile short* spritemap;
static unsigned old_hud[5120]; /* Two little-endian pixels per device word. */
static unsigned last_hud_revision;
static short pcm[AUDIO_CHUNK_SAMPLES];
static int pcm_offset;
static int pcm_count;
static int sound_enabled;
static int sound_requested = 1;
static int quit_requested;
static int screen;
static int animation;
static unsigned animation_start;
static int camera_x;
static int camera_y;
static int camera_angle;
static int origin_x;
static int origin_y;
static int origin_angle;
static int target_angle;
static int pending_action;
static int turn_unpresented;
static int autoplay;
static unsigned last_demo_frame;
static unsigned run_seed;

/* A bounded copy keeps all radio messages terminated within their public fields. */
static void message(char* dst, char* src) {
    int i = 0;
    while (src[i] && i < 95) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Read terminal-forwarded presses; direct PS/2 is only for non-pipe stdin. */
static unsigned input(void) {
    int available = fd_bytes_available(STDIN);
    if (available > 0) {
        unsigned char c = 0;
        if (read(STDIN, &c, 1) == 1) return c;
        return 0;
    }
    if (available < 0) return getkey();
    return 0;
}

/* Retain an unwritten PCM suffix until the nonblocking audio device accepts it. */
static void audio_pump(void) {
    if (!sound_enabled) return;
    int buffered = audio_buffered();
    if (buffered < 0) {
        audio_close();
        sound_enabled = 0;
        message(game.detail, "AUDIO BUFFER QUERY FAILED - V TO RETRY SOUND");
        return;
    }
    for (int attempt = 0; attempt < 6 && buffered < AUDIO_TARGET_BYTES; attempt++) {
        if (pcm_offset == pcm_count) {
            music_render(pcm, AUDIO_CHUNK_SAMPLES, game.sector - 1);
            pcm_offset = 0;
            pcm_count = AUDIO_CHUNK_SAMPLES;
        }
        int accepted = audio_write(pcm + pcm_offset, (pcm_count - pcm_offset) * 2);
        if (accepted < 0) {
            audio_close();
            sound_enabled = 0;
            message(game.detail, "AUDIO WRITE FAILED - V TO RETRY SOUND");
            return;
        }
        if (accepted == 0) return;
        pcm_offset += accepted / 2;
        buffered += accepted;
    }
}

/* Claim exclusive playback or keep the expedition playable with sound disabled. */
static void audio_start(void) {
    pcm_offset = 0;
    pcm_count = 0;
    music_reset();
    sound_enabled = sound_requested && audio_open() == 0;
    if (sound_requested && !sound_enabled) message(game.detail, "AUDIO BUSY - PLAYING SILENTLY. V TO RETRY");
    audio_pump();
}

/* Acquire only documented mappings, then repurpose tiles as a software canvas. */
static int display_start(void) {
    tilemap = get_tilemap();
    tilefb = get_tile_fb();
    spritemap = get_spritemap();
    if (tilemap == (volatile short*)-1 || tilefb == (volatile short*)-1 || spritemap == (volatile short*)-1) {
        puts("lumen: VGA mapping failed; tilemap, tile framebuffer, and sprites are required\n");
        return 0;
    }
    puts("\x1b[?25l\x1b[2J");
    /* Existing terminal owns the output pipe. Drain it before touching its font;
     * no cursor redraw follows the hide sequence. Yield lets that user process
     * finish even on a single core. This is not a hardware vblank assumption. */
    for (int i = 0; i < 8; i++) { sleep(3); yield(); }
    set_tile_scale(2);
    set_hscroll(0);
    set_vscroll(0);
    for (int i = 0; i < NUM_SPRITES; i++) set_sprite_coords(i, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
    for (int i = 0; i < 64; i++) tilemap[BACKGROUND_TILE * 64 + i] = 0x211;
    for (int i = 0; i < FB_NUM_TILES; i++) tilefb[i] = BACKGROUND_TILE;
    for (int y = 0; y < VIEW_TILE_ROWS; y++) {
        for (int x = 0; x < VIEW_TILE_COLUMNS; x++) tilefb[y * TILE_ROW_WIDTH + x] = y * VIEW_TILE_COLUMNS + x;
    }
    for (int i = 0; i < HUD_W * HUD_H / 2; i++) old_hud[i] = 0xFFFFFFFFu;
    last_hud_revision = render_hud_revision - 1;
    for (int i = 0; i < HUD_SPRITES; i++) {
        set_sprite_scale(i, 1);
        /* The hardware document leaves coordinate scaling unspecified. This
         * uses the existing emulator/dino convention: scale 1 multiplies
         * coordinates too, giving screen x=64*i and screen y=400. */
        set_sprite_coords(i, i * 32, 200);
    }
    return 1;
}

/* Pack ordinary RAM pixels without violating C aliasing or alignment rules. */
static unsigned pixel_pair(unsigned short* pixels) {
    return (unsigned)pixels[0] | ((unsigned)pixels[1] << 16);
}

/* Publish exactly 240 tiles and 10 sprites. The mapping getters return page-
 * aligned regions; each word store writes two adjacent little-endian 16-bit
 * pixels (mem_map tile/sprite data). Word byte order is unspecified by the
 * ISA text; this uses the current emulator's little-endian word stores and is
 * checked by the guest MMIO readback test. No control register or buffer
 * boundary is crossed. This halves device writes; scanout is still non-atomic. */
static void present(void) {
    volatile unsigned* target = (volatile unsigned*)tilemap;
    unsigned short* tile_row = view_pixels;
    for (int ty = 0; ty < VIEW_TILE_ROWS; ty++) {
        for (int tx = 0; tx < VIEW_TILE_COLUMNS; tx++) {
            unsigned short* source = tile_row + tx * 8;
            for (int y = 0; y < 8; y++) {
                *target++ = pixel_pair(source);
                *target++ = pixel_pair(source + 2);
                *target++ = pixel_pair(source + 4);
                *target++ = pixel_pair(source + 6);
                source += VIEW_W;
            }
        }
        tile_row += VIEW_W * 8;
        /* Feed between tile rows because scanout copies may span host frames. */
        if ((ty & 3) == 3) audio_pump();
    }
    if (last_hud_revision == render_hud_revision) return;
    last_hud_revision = render_hud_revision;
    target = (volatile unsigned*)spritemap;
    unsigned* previous = old_hud;
    for (int sprite = 0; sprite < HUD_SPRITES; sprite++) {
        unsigned short* source = hud_pixels + sprite * 32;
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x += 2) {
                unsigned pair = pixel_pair(source + x);
                if (*previous != pair) {
                    *previous = pair;
                    *target = pair;
                }
                previous++;
                target++;
            }
            source += HUD_W;
        }
    }
}

/* Interpolate along the shortest turn, starting at the last displayed camera.
 * Repeated turn presses may retarget this interpolation; movement is queued
 * until its previous step finishes so tactical actions cannot run unseen. */
static void begin_animation(void) {
    origin_x = camera_x;
    origin_y = camera_y;
    origin_angle = camera_angle;
    target_angle = game.angle;
    while (target_angle - origin_angle > 128) target_angle -= 256;
    while (target_angle - origin_angle < -128) target_angle += 256;
    animation = camera_x == game.x * 256 + 128 && camera_y == game.y * 256 + 128 ? TURN_TICKS : MOVE_TICKS;
    animation_start = get_vga_frame_counter();
}

/* Elapsed scanout ticks, including a wrapped counter, determine camera motion. */
static void advance_camera(unsigned frame) {
    if (!animation) return;
    unsigned elapsed = frame - animation_start;
    if (elapsed >= (unsigned)animation) {
        camera_x = game.x * 256 + 128;
        camera_y = game.y * 256 + 128;
        camera_angle = game.angle;
        animation = 0;
        return;
    }
    camera_x = origin_x + (game.x * 256 + 128 - origin_x) * (int)elapsed / animation;
    camera_y = origin_y + (game.y * 256 + 128 - origin_y) * (int)elapsed / animation;
    camera_angle = origin_angle + (target_angle - origin_angle) * (int)elapsed / animation;
}

/* Snap to a newly entered deck; no interpolation is allowed across map changes. */
static void camera_reset(void) {
    camera_x = game.x * 256 + 128;
    camera_y = game.y * 256 + 128;
    camera_angle = game.angle;
    animation = 0;
    pending_action = 0;
    turn_unpresented = 0;
}

/* Restart from the supplied seed, preserving the user's sound preference. */
static void restart(unsigned seed) {
    if (sound_enabled) audio_close();
    sound_enabled = 0;
    run_seed = seed;
    game_start(&game, seed);
    camera_reset();
    screen = SCREEN_GAME;
    audio_start();
}

/* Commit one tactical action and animate only actual position/heading changes. */
static void player_action(int action) {
    autoplay = 0;
    int old_sector = game.sector;
    unsigned old_turn = game.turns;
    game_act(&game, action);
    if (game.turns != old_turn) turn_unpresented = 1;
    if (game.sector != old_sector) camera_reset();
    else if (camera_x != game.x * 256 + 128 || camera_y != game.y * 256 + 128 ||
             (camera_angle & 255) != game.angle) begin_animation();
    music_effect(game.sound);
}

/* Consume at most one buffered action after its previous visible step ends. */
static void finish_pending_action(void) {
    if (!animation && !turn_unpresented && pending_action && screen == SCREEN_GAME && game.mode == MODE_PLAY) {
        int action = pending_action;
        pending_action = 0;
        player_action(action);
    }
}

/* Translate terminal bytes into discrete, reviewable tactical actions. */
static void keypress(unsigned key) {
    if (key & KEY_RELEASE) return;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (key == 'q') { quit_requested = 1; pending_action = 0; return; }
    if (key == 'v') {
        sound_requested = !sound_enabled;
        if (sound_enabled) { audio_close(); sound_enabled = 0; }
        else audio_start();
        return;
    }
    if (game.mode == MODE_TITLE) {
        if (key == '\n' || key == KEY_ENTER || key == ' ' || key == 'd') {
            game.mode = MODE_PLAY;
            autoplay = key == 'd';
            screen = autoplay ? SCREEN_GAME : SCREEN_BRIEFING;
            message(game.message, "FIND THREE RELAYS. MATCH THEIR BEAM COLORS.");
            message(game.detail, "W S MOVE / A D TURN / SPACE FIRE / H HELP");
            camera_reset();
        }
        return;
    }
    if (game.mode == MODE_DEAD || game.mode == MODE_WON) {
        if (key == 'r' || key == 'n') {
            autoplay = 0;
            restart(key == 'r' ? run_seed : get_current_jiffies() ^ get_vga_frame_counter());
        }
        return;
    }
    if (game.mode == MODE_UPGRADE) {
        if (key >= '1' && key <= '3') {
            game_upgrade(&game, key - '0');
            camera_reset();
            music_effect(SOUND_EXIT);
        }
        return;
    }
    if (screen == SCREEN_BRIEFING) {
        if (key == '\n' || key == KEY_ENTER || key == ' ' || key == 'h') screen = SCREEN_GAME;
        return;
    }
    if (key == 'p') { pending_action = 0; screen = screen == SCREEN_PAUSE ? SCREEN_GAME : SCREEN_PAUSE; return; }
    if (key == 'h') { pending_action = 0; screen = screen == SCREEN_HELP ? SCREEN_GAME : SCREEN_HELP; return; }
    if (key == 'm' || key == '\t') { pending_action = 0; screen = screen == SCREEN_MAP ? SCREEN_GAME : SCREEN_MAP; return; }
    if (screen != SCREEN_GAME) return;
    if (key == 't') { pending_action = 0; autoplay = !autoplay; return; }
    if (key >= '1' && key <= '3') { game.color = key - '1'; return; }
    int action = 0;
    if (key == 'w' || key == KEY_UP) action = ACT_FORWARD;
    if (key == 's' || key == KEY_DOWN) action = ACT_BACK;
    if (key == 'a' || key == KEY_LEFT) action = ACT_LEFT;
    if (key == 'd' || key == KEY_RIGHT) action = ACT_RIGHT;
    if (key == 'z') action = ACT_STRAFE_LEFT;
    if (key == 'c') action = ACT_STRAFE_RIGHT;
    if (key == ' ' || key == 'f') action = ACT_FIRE;
    if (key == 'e' || key == '\n' || key == KEY_ENTER) action = ACT_USE;
    if (key == 'x' || key == '.') action = ACT_WAIT;
    if (action) {
        /* Preserve one pending step instead of dropping a press during motion.
         * Pure turns can be retargeted immediately, keeping keyboard repeat
         * smooth without allowing a backlog of unseen combat turns. */
        int turning = action == ACT_LEFT || action == ACT_RIGHT;
        int moving = camera_x != game.x * 256 + 128 || camera_y != game.y * 256 + 128;
        if ((animation && (moving || !turning)) || (turn_unpresented && !turning)) pending_action = action;
        else player_action(action);
    }
}

/* Complete short descriptor writes; zero progress is a capture failure. */
static int capture_write(int fd, void* buffer, int bytes) {
    unsigned char* data = buffer;
    while (bytes > 0) {
        int count = write(fd, data, bytes);
        if (count <= 0 || count > bytes) return 0;
        data += count;
        bytes -= count;
    }
    return 1;
}

/* Save actual MMIO contents at half the emulator's 640x480 display size.
 * Each 960-byte RGB row fits the documented 1024-byte write syscall cap.
 * This test-only export never depends on VGA frame-counter progress. */
static int capture_display(char* path) {
    int fd = open(path);
    if (fd < 0) return 0;
    if (truncate(fd, 0) < 0 || !capture_write(fd, "P6\n320 240\n255\n", 15)) {
        close(fd);
        return 0;
    }
    unsigned char row[960];
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 320; x++) {
            int color = 0x211;
            if (y < 192) {
                int vx = x / 2;
                int vy = y / 2;
                int tile = (vy / 8) * VIEW_TILE_COLUMNS + vx / 8;
                color = (unsigned short)tilemap[tile * 64 + (vy & 7) * 8 + (vx & 7)];
            }
            if (y >= 200 && y < 232) {
                color = (unsigned short)spritemap[(x / 32) * 1024 + (y - 200) * 32 + (x & 31)];
            }
            row[x * 3] = (color & 15) * 17;
            row[x * 3 + 1] = ((color >> 4) & 15) * 17;
            row[x * 3 + 2] = ((color >> 8) & 15) * 17;
        }
        if (!capture_write(fd, row, 960)) { close(fd); return 0; }
    }
    return close(fd) == 0;
}

/* A finite guest run checks UI, oblique rays, and packed MMIO publication. */
static int capture_scenes(void) {
    char* paths[7] = {"/lumen-title.ppm", "/lumen-game.ppm", "/lumen-map.ppm",
                      "/lumen-action.ppm", "/lumen-lift.ppm", "/lumen-briefing.ppm",
                      "/lumen-oblique.ppm"};
    render_init();
    if (!display_start()) return 1;
    for (int scene = 0; scene < 7; scene++) {
        game_start(&game, 20261010u);
        int which = SCREEN_GAME;
        if (scene == 0) game.mode = MODE_TITLE;
        if (scene == 2) which = SCREEN_MAP;
        if (scene == 3) {
            for (int step = 0; step < 35; step++) game_autoplay(&game);
        }
        if (scene == 4) {
            game.x = game.exit_x;
            game.y = game.exit_y - 1;
            game.angle = 128;
            game.relays = 3;
        }
        if (scene == 5) which = SCREEN_BRIEFING;
        if (scene == 6) game.angle = 80;
        camera_reset();
        render_frame(&game, camera_x, camera_y, camera_angle, 0, which);
        present();
        if (!capture_display(paths[scene])) return 2 + scene;
    }
    return 0;
}

/* Check the real input path without a terminal: briefing is turn-free, repeated
 * turning is retained, one movement press survives animation, and opening an
 * overlay cancels queued movement. Synthetic ticks test counter wrap directly. */
static int input_selftest(void) {
    game_start(&game, 42);
    game.mode = MODE_TITLE;
    camera_reset();
    keypress(KEY_ENTER);
    if (game.mode != MODE_PLAY || screen != SCREEN_BRIEFING || game.turns) return 1;
    keypress('w');
    if (game.turns || animation) return 2;
    keypress(KEY_ENTER);
    if (screen != SCREEN_GAME || game.turns) return 3;
    int start_angle = game.angle;
    keypress('d');
    keypress('d');
    if (game.angle != ((start_angle + 2 * TURN_STEP) & 255) || game.turns) return 4;
    advance_camera(animation_start + TURN_TICKS);
    if (animation || camera_angle != game.angle) return 5;
    game.entity_count = 0;
    game.x = 3;
    game.y = 3;
    game.angle = 64;
    game.cells[3 * WORLD_W + 4] = CELL_FLOOR;
    game.cells[3 * WORLD_W + 5] = CELL_FLOOR;
    camera_reset();
    keypress('w');
    keypress('w');
    if (game.x != 4 || game.turns != 1 || pending_action != ACT_FORWARD) return 6;
    turn_unpresented = 0; /* Model publication of the first movement frame. */
    advance_camera(animation_start + MOVE_TICKS);
    finish_pending_action();
    if (game.x != 5 || game.turns != 2 || pending_action) return 7;
    turn_unpresented = 0;
    keypress('s');
    keypress('m');
    advance_camera(animation_start + MOVE_TICKS);
    keypress('m');
    finish_pending_action();
    if (game.x != 5 || game.turns != 2 || pending_action) return 8;
    game.angle = 8;
    origin_angle = 248;
    target_angle = 264;
    origin_x = camera_x;
    origin_y = camera_y;
    animation = TURN_TICKS;
    animation_start = 0xFFFFFFFEu;
    advance_camera(0);
    if (camera_angle != 258 || !animation) return 9;
    advance_camera(1);
    if (camera_angle != 8 || animation) return 10;
    keypress('f');
    unsigned shot_turn = game.turns;
    keypress('x');
    finish_pending_action();
    if (game.turns != shot_turn || !game.shot || pending_action != ACT_WAIT) return 11;
    keypress('d');
    if (!game.shot || game.turns != shot_turn) return 12;
    turn_unpresented = 0;
    advance_camera(animation_start + TURN_TICKS);
    finish_pending_action();
    if (game.turns != shot_turn + 1 || pending_action) return 13;
    return 0;
}

/* Run actual gameplay, raster, and synthesis code without any device mappings. */
static int selftest(void) {
    puts("lumen: testing seeded maps, turn rules, combat, and progression\n");
    int result = game_selftest();
    if (result) { puts("lumen: gameplay test failed at case "); print_signed(result); puts("\n"); return 1; }
    puts("lumen: testing renderer and synth\n");
    result = render_selftest();
    if (result) { puts("lumen: renderer test failed at case "); print_signed(result); puts("\n"); return 2; }
    result = music_selftest();
    if (result) { puts("lumen: synth test failed at case "); print_signed(result); puts("\n"); return 3; }
    result = input_selftest();
    if (result) { puts("lumen: input test failed at case "); print_signed(result); puts("\n"); return 4; }
    puts("lumen: all self-tests passed\n");
    return 0;
}

/* Measure identical static, translating, and turning views through real MMIO.
 * This diagnostic disables audio and reports elapsed 60 Hz scanout ticks;
 * --headless/--vga is required for timing. It owns the normal display mapping
 * and prints only after the timed section, so terminal output cannot skew it. */
static int benchmark(void) {
    game_start(&game, 20261010u);
    render_init();
    if (!display_start()) return 1;
    unsigned elapsed[3];
    for (int scene = 0; scene < 3; scene++) {
        unsigned start = get_vga_frame_counter();
        for (int frame = 0; frame < 96; frame++) {
            int x = game.x * 256 + 128;
            int angle = 64;
            if (scene == 1) x += (frame & 31) * 4;
            if (scene == 2) angle = (64 + frame * 2) & 255;
            render_frame(&game, x, game.y * 256 + 128, angle, (unsigned)frame, SCREEN_GAME);
            present();
        }
        elapsed[scene] = get_vga_frame_counter() - start;
        if (!elapsed[scene]) {
            puts("lumen: --benchmark requires an advancing VGA clock; use emulator --headless or --vga\n");
            return 1;
        }
    }
    for (int scene = 0; scene < 3; scene++) {
        puts("***BENCH lumen scene "); print_signed(scene);
        puts(": 96 frames in "); print_unsigned(elapsed[scene]);
        puts(" VGA ticks\n");
    }
    return 0;
}

/* Enter the foreground application or its bounded, headless verification mode. */
int main(int argc, char** argv) {
    unsigned seed = 20261010u;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--benchmark") == 0) return benchmark();
        if (strcmp(argv[i], "--selftest") == 0) return selftest();
        if (strcmp(argv[i], "--capture") == 0) return capture_scenes();
        if (strcmp(argv[i], "--silent") == 0) sound_requested = 0;
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            i++;
            seed = 0;
            for (int j = 0; argv[i][j]; j++) {
                if (argv[i][j] < '0' || argv[i][j] > '9') {
                    puts("lumen: --seed expects decimal digits\n"); return 1;
                }
                seed = seed * 10u + (unsigned)(argv[i][j] - '0');
            }
        } else if (strcmp(argv[i], "--demo") == 0) autoplay = 1;
        else if (strcmp(argv[i], "--silent") != 0) {
            puts("usage: lumen [--silent] [--seed NUMBER] [--demo] [--selftest] [--capture] [--benchmark]\n"); return 1;
        }
    }
    run_seed = seed;
    game_start(&game, seed);
    game.mode = autoplay ? MODE_PLAY : MODE_TITLE;
    message(game.message, "THE STATION WENT DARK. YOU ARE ITS LAST SIGNAL.");
    message(game.detail, "ENTER PLAY / D DEMO / Q QUIT / V SOUND");
    render_init();
    camera_reset();
    if (!display_start()) return 1;
    audio_start();
    unsigned last_frame = get_vga_frame_counter() - FRAME_INTERVAL;
    while (!quit_requested) {
        audio_pump();
        unsigned frame = get_vga_frame_counter();
        advance_camera(frame);
        finish_pending_action();
        for (int i = 0; i < 8; i++) {
            unsigned key = input();
            if (!key) break;
            keypress(key);
        }
        frame = get_vga_frame_counter();
        if (frame - last_frame < FRAME_INTERVAL) { sleep(1); continue; }
        last_frame = frame;
        advance_camera(frame);
        if (autoplay && !animation && screen == SCREEN_GAME &&
            (game.mode == MODE_PLAY || game.mode == MODE_UPGRADE) &&
            frame - last_demo_frame >= 45) {
            int old_sector = game.sector;
            game_autoplay(&game);
            last_demo_frame = frame;
            if (game.sector != old_sector) camera_reset();
            else begin_animation();
            music_effect(game.sound);
        }
        int angle = camera_angle;
        if (game.mode == MODE_TITLE) {
            int sway = (frame / 8) % 40;
            if (sway > 20) sway = 40 - sway;
            angle += sway - 10;
        }
        render_frame(&game, camera_x, camera_y, angle, frame, screen);
        present();
        turn_unpresented = 0;
        if (game.shot > 0) game.shot--;
        if (game.flash > 0) game.flash--;
    }
    if (sound_enabled) audio_close();
    for (int i = 0; i < NUM_SPRITES; i++) set_sprite_coords(i, HIDDEN_SPRITE_COORD, HIDDEN_SPRITE_COORD);
    return 0;
}
