/* Host-only preview and sanitizer entry point for the exact guest algorithms.
 * Writes an RGB PPM using the tile/sprite placement documented by the app;
 * it does not emulate instruction execution or replace the guest self-test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../game.h"
#include "../render.h"
#include "../music.h"

/* Expand one packed VGA pixel to the same 24-bit color used by the emulator. */
static void pixel(FILE* file, unsigned short color) {
    fputc((color & 15) * 17, file);
    fputc(((color >> 4) & 15) * 17, file);
    fputc(((color >> 8) & 15) * 17, file);
}

/* Run all portable checks and capture a selected scene of a repeatable expedition. */
int main(int argc, char** argv) {
    int result = game_selftest();
    if (!result) result = render_selftest();
    if (!result) result = music_selftest();
    if (result) { fprintf(stderr, "lumen host selftest failed: %d\n", result); return 1; }
    if (argc < 2) { puts("lumen host selftests passed"); return 0; }
    struct Game g;
    game_start(&g, 20261010u);
    int screen = SCREEN_GAME;
    int frames = argc > 3 ? atoi(argv[3]) : 0;
    for (int i = 0; i < frames && g.mode != MODE_WON && g.mode != MODE_DEAD; i++) game_autoplay(&g);
    if (argc > 2 && strcmp(argv[2], "title") == 0) g.mode = MODE_TITLE;
    if (argc > 2 && strcmp(argv[2], "map") == 0) screen = SCREEN_MAP;
    if (argc > 2 && strcmp(argv[2], "help") == 0) screen = SCREEN_HELP;
    if (argc > 2 && strcmp(argv[2], "briefing") == 0) screen = SCREEN_BRIEFING;
    if (argc > 2 && strcmp(argv[2], "oblique") == 0) g.angle = 80;
    if (argc > 2 && strcmp(argv[2], "lift") == 0) {
        g.x = g.exit_x;
        g.y = g.exit_y - 1;
        g.angle = 128;
        g.relays = 3;
    }
    render_frame(&g, g.x * 256 + 128, g.y * 256 + 128, g.angle, 0, screen);
    FILE* file = fopen(argv[1], "wb");
    if (!file) { perror(argv[1]); return 1; }
    fprintf(file, "P6\n640 480\n255\n");
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 640; x++) {
            unsigned short color = 0x211;
            if (y < 384) color = view_pixels[(y / 4) * VIEW_W + x / 4];
            if (y >= 400 && y < 464) color = hud_pixels[((y - 400) / 2) * HUD_W + x / 2];
            pixel(file, color);
        }
    }
    if (fclose(file)) { perror("lumen preview write"); return 1; }
    printf("lumen preview: sector%d cell%d,%d hp%d mode%d -> %s\n",g.sector,g.x,g.y,g.hp,g.mode,argv[1]);
    return 0;
}
