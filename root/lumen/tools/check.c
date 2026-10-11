/*
 * Native checks for the unchanged guest game, renderer, and synthesizer.
 * Run under AddressSanitizer and UndefinedBehaviorSanitizer to check raster
 * clipping at every floor cell, all camera angles, and near-wall positions.
 * Image comparisons exercise actual occlusion and deterministic redraws;
 * full campaigns check generated-world playability through ordinary actions.
 * This host-only harness uses standard C library facilities, never guest CRT.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../game.h"
#include "../render.h"
#include "../music.h"

/* Uneven lengths cross audio control, score, echo, and caller chunk boundaries. */
#define AUDIO_TEST_SAMPLES 25003
#define CAMPAIGN_DECISION_LIMIT 2400

/* Accumulate useful coverage totals without coupling checks to golden images. */
struct Coverage {
    unsigned frames;
    unsigned floor_cells;
    unsigned campaigns;
    unsigned max_decisions;
    int minimum_hull;
};

/* Stop with the operation and case needed to reproduce a failed check. */
static void fail(const char* operation, unsigned seed, int detail){
    fprintf(stderr, "lumen check: %s failed: seed=%u detail=%d\n", operation, seed, detail);
    exit(EXIT_FAILURE);
}

/* Hash displayed values independently of host byte order or short layout. */
static uint32_t pixels_hash(const unsigned short* pixels, size_t count){
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < count; i++){
        hash ^= pixels[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

/* Require opaque packed 12-bit colors at every pixel in both output planes. */
static void check_pixel_range(unsigned seed){
    for (int i = 0; i < VIEW_W * VIEW_H; i++){
        if (view_pixels[i] > 0xFFF) fail("view color range", seed, i);
    }
    for (int i = 0; i < HUD_W * HUD_H; i++){
        if (hud_pixels[i] > 0xFFF) fail("HUD color range", seed, i);
    }
}

/* Poison both planes so incomplete redraws cannot inherit plausible old pixels. */
static void poison_pixels(void){
    for (int i = 0; i < VIEW_W * VIEW_H; i++) view_pixels[i] = 0xFFFF;
    for (int i = 0; i < HUD_W * HUD_H; i++) hud_pixels[i] = 0xFFFF;
}

/* Repeat identical inputs after poisoning storage; drawing must not mutate rules. */
static void check_frame(struct Game* game, int camera_x, int camera_y, int angle,
                        unsigned frame, int screen, struct Coverage* coverage){
    struct Game original;
    memcpy(&original, game, sizeof(original));
    poison_pixels();
    render_frame(game, camera_x, camera_y, angle, frame, screen);
    check_pixel_range(game->seed);
    uint32_t view_hash = pixels_hash(view_pixels, VIEW_W * VIEW_H);
    uint32_t hud_hash = pixels_hash(hud_pixels, HUD_W * HUD_H);
    poison_pixels();
    render_frame(game, camera_x, camera_y, angle, frame, screen);
    check_pixel_range(game->seed);
    if (view_hash != pixels_hash(view_pixels, VIEW_W * VIEW_H) ||
        hud_hash != pixels_hash(hud_pixels, HUD_W * HUD_H)){
        fprintf(stderr, "camera=(%d,%d) angle=%d frame=%u mode=%d screen=%d\n",
                camera_x, camera_y, angle, frame, game->mode, screen);
        fail("deterministic complete redraw", game->seed, game->sector);
    }
    if (memcmp(game, &original, sizeof(original)) != 0){
        fail("renderer changed gameplay state", original.seed, game->mode);
    }
    coverage->frames += 2;
}

/* Render every traversable cell; four views include both sides of nearby walls. */
static void check_generated_views(unsigned seed, struct Coverage* coverage){
    struct Game game;
    game_start(&game, seed);
    for (int sector = 1; sector <= SECTOR_COUNT; sector++){
        if (game_validate(&game)) fail("generated render scene", seed, sector);
        for (int y = 1; y < WORLD_H - 1; y++){
            for (int x = 1; x < WORLD_W - 1; x++){
                int cell = game_cell(&game, x, y);
                if (cell != CELL_FLOOR && cell != CELL_EXIT) continue;
                coverage->floor_cells++;
                for (int direction = 0; direction < 4; direction++){
                    check_frame(&game, x * 256 + 128, y * 256 + 128,
                                direction * 64, (unsigned)(x + y), SCREEN_GAME, coverage);
                }
            }
        }
        if (sector < SECTOR_COUNT){
            game.mode = MODE_UPGRADE;
            game_upgrade(&game, 1);
        }
    }
}

/* Sweep the complete turn at a chamber corner, including one-unit wall gaps. */
static void check_camera_edges(struct Coverage* coverage){
    struct Game game;
    int offsets[5][2] = {{128, 128}, {1, 1}, {255, 1}, {1, 255}, {255, 255}};
    game_start(&game, 42);
    int x = 1;
    int y = 1;
    int found = 0;
    for (int row = 1; row < WORLD_H - 1 && !found; row++){
        for (int column = 1; column < WORLD_W - 1; column++){
            if (game_cell(&game, column, row) != CELL_FLOOR) continue;
            if (game_cell(&game, column - 1, row) == CELL_WALL &&
                game_cell(&game, column, row - 1) == CELL_WALL){
                x = column;
                y = row;
                found = 1;
                break;
            }
        }
    }
    if (!found) fail("near-wall camera fixture", game.seed, 0);
    for (int position = 0; position < 5; position++){
        for (int angle = 0; angle < 256; angle++){
            check_frame(&game, x * 256 + offsets[position][0],
                        y * 256 + offsets[position][1], angle,
                        (unsigned)angle, SCREEN_GAME, coverage);
        }
    }
}

/* Exercise every state/overlay combination, long HUD strings, and effect edges. */
static void check_overlays(struct Coverage* coverage){
    struct Game game;
    game_start(&game, 17);
    for (int i = 0; i < WORLD_CELLS; i++) game.seen[i] = 1;
    for (int i = 0; i < 95; i++){
        game.message[i] = i % 7 ? 'W' : ' ';
        game.detail[i] = i % 5 ? 'M' : ' ';
    }
    game.message[95] = 0;
    game.detail[95] = 0;
    game.max_hp = 54;
    game.score = INT_MAX;
    for (int mode = MODE_TITLE; mode <= MODE_WON; mode++){
        game.mode = mode;
        for (int screen = SCREEN_GAME; screen <= SCREEN_PAUSE; screen++){
            for (int variant = 0; variant < 3; variant++){
                game.color = variant;
                game.hp = variant == 0 ? 0 : (variant == 1 ? 1 : game.max_hp);
                game.energy = variant == 0 ? 0 : game.max_energy;
                game.relays = variant;
                game.shot = variant == 1 ? 4 : 0;
                game.flash = variant == 2 ? 5 : 0;
                check_frame(&game, game.x * 256 + 128, game.y * 256 + 128,
                            variant * 85, (unsigned)(variant * 30), screen, coverage);
            }
        }
    }
}

/* Clear a controlled room so visible differences come from the tested object. */
static void make_render_room(struct Game* game){
    game_start(game, 9);
    game->entity_count = 0;
    game->x = 3;
    game->y = 9;
    game->dir = 1;
    game->exit_x = 1;
    game->exit_y = 1;
    game->shot = 0;
    game->flash = 0;
    for (int y = 1; y < WORLD_H - 1; y++){
        for (int x = 1; x < WORLD_W - 1; x++) game->cells[y * WORLD_W + x] = CELL_FLOOR;
    }
    game->cells[game->exit_y * WORLD_W + game->exit_x] = CELL_EXIT;
}

/* Count image changes in the central world view, excluding HUD and weapon. */
static int center_changes(const unsigned short* before){
    int changes = 0;
    for (int y = 16; y < 68; y++){
        for (int x = 58; x < 101; x++){
            if (before[y * VIEW_W + x] != view_pixels[y * VIEW_W + x]) changes++;
        }
    }
    return changes;
}

/* A walkable lift must be visible, change when unlocked, and respect a wall. */
static void check_lift_visibility(struct Coverage* coverage){
    struct Game game;
    unsigned short before[VIEW_W * VIEW_H];
    make_render_room(&game);
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    memcpy(before, view_pixels, sizeof(before));
    game.cells[game.exit_y * WORLD_W + game.exit_x] = CELL_FLOOR;
    game.exit_x = 6;
    game.exit_y = 9;
    game.cells[game.exit_y * WORLD_W + game.exit_x] = CELL_EXIT;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    if (center_changes(before) < 20) fail("visible locked lift", game.seed, 0);
    memcpy(before, view_pixels, sizeof(before));
    game.relays = 3;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    if (center_changes(before) < 20) fail("unlocked lift visual state", game.seed, 0);
    for (int y = 1; y < WORLD_H - 1; y++) game.cells[y * WORLD_W + 5] = CELL_WALL;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    memcpy(before, view_pixels, sizeof(before));
    game.exit_x = 1;
    game.exit_y = 1;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    if (memcmp(before, view_pixels, sizeof(before)) != 0){
        fail("lift occlusion behind solid wall", game.seed, 0);
    }
}

/* Hidden sentry warnings must be occluded together with the sentry itself. */
static void check_sentry_occlusion(struct Coverage* coverage){
    struct Game game;
    unsigned short before[VIEW_W * VIEW_H];
    make_render_room(&game);
    for (int y = 1; y < WORLD_H - 1; y++) game.cells[y * WORLD_W + 5] = CELL_WALL;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    memcpy(before, view_pixels, sizeof(before));
    game.entity_count = 1;
    game.entities[0].type = ENTITY_DRONE;
    game.entities[0].x = 7;
    game.entities[0].y = 9;
    game.entities[0].color = 0;
    game.entities[0].health = 4;
    game.entities[0].active = 1;
    game.entities[0].alert = 2;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    if (memcmp(before, view_pixels, sizeof(before)) != 0){
        fail("sentry warning leaked through wall", game.seed, 0);
    }
    game.cells[9 * WORLD_W + 5] = CELL_FLOOR;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    memcpy(before, view_pixels, sizeof(before));
    game.entities[0].active = 0;
    check_frame(&game, 3 * 256 + 128, 9 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
    if (center_changes(before) < 20) fail("sentry visible through doorway", game.seed, 0);
}

/* Fill the documented entity capacity and include the additional lift marker. */
static void check_entity_capacity(struct Coverage* coverage){
    struct Game game;
    make_render_room(&game);
    game.entity_count = MAX_ENTITIES;
    game.exit_x = 18;
    game.exit_y = 9;
    for (int i = 0; i < MAX_ENTITIES; i++){
        struct Entity* entity = &game.entities[i];
        entity->type = 1 + i % 5;
        entity->x = 4 + i % 10;
        entity->y = 4 + i / 10;
        entity->color = i % 3;
        entity->health = 4;
        entity->active = 1;
        entity->alert = i % 3;
    }
    for (int angle = 0; angle < 256; angle++){
        check_frame(&game, 3 * 256 + 255, 6 * 256 + 1,
                    angle, (unsigned)angle, SCREEN_GAME, coverage);
    }
    game.entities[0].active = 0;
    game.entities[0].health = 0;
    game.entities[1].active = 0;
    game.entities[1].health = 0;
    check_frame(&game, 3 * 256 + 128, 4 * 256 + 128, 64, 0, SCREEN_GAME, coverage);
}

/* PCM output must be independent of arbitrary caller chunk boundaries. */
static void check_audio_chunks(void){
    short reference[AUDIO_TEST_SAMPLES];
    short chunked[AUDIO_TEST_SAMPLES];
    for (int sector = 0; sector < SECTOR_COUNT; sector++){
        music_reset();
        music_effect(SOUND_HURT);
        music_render(reference, AUDIO_TEST_SAMPLES, sector);
        music_reset();
        music_effect(SOUND_HURT);
        int offset = 0;
        while (offset < AUDIO_TEST_SAMPLES){
            int count = 1 + offset % 389;
            if (count > AUDIO_TEST_SAMPLES - offset) count = AUDIO_TEST_SAMPLES - offset;
            short untouched = 1234;
            music_render(&untouched, 0, sector);
            music_render(&untouched, -7, sector);
            if (untouched != 1234) fail("nonpositive PCM count wrote data", 0, sector);
            music_render(chunked + offset, count, sector);
            offset += count;
        }
        if (memcmp(reference, chunked, sizeof(reference)) != 0){
            fail("PCM chunk independence", 0, sector);
        }
        int audible = 0;
        for (int i = 0; i < AUDIO_TEST_SAMPLES; i++){
            if (reference[i] < -16000 || reference[i] > 16000){
                fail("PCM amplitude bound", 0, i);
            }
            if (reference[i] < -100 || reference[i] > 100) audible = 1;
        }
        if (!audible) fail("audible score", 0, sector);
    }
    for (int side = 0; side < 2; side++){
        music_reset();
        music_render(reference, AUDIO_TEST_SAMPLES, side == 0 ? 0 : 2);
        music_reset();
        music_render(chunked, AUDIO_TEST_SAMPLES, side == 0 ? -99 : 99);
        if (memcmp(reference, chunked, sizeof(reference)) != 0){
            fail("PCM sector clamp", 0, side);
        }
    }
}

/* Prove varied generated runs reach victory without modifying player resources. */
static void check_campaigns(int count, struct Coverage* coverage){
    struct Game game;
    unsigned seed = 0x2718abcdu;
    for (int run = 0; run < count; run++){
        seed = seed * 1664525u + 1013904223u;
        game_start(&game, seed);
        int decision = 0;
        while (game.mode != MODE_WON && game.mode != MODE_DEAD &&
               decision < CAMPAIGN_DECISION_LIMIT){
            game_autoplay(&game);
            int valid = game_validate(&game);
            if (valid) fail("campaign state invariant", seed, valid);
            decision++;
        }
        if (game.mode != MODE_WON) fail("campaign victory", seed, decision);
        if (game.sector != SECTOR_COUNT || game.relays != 3){
            fail("campaign completed all objectives", seed, game.sector);
        }
        if ((unsigned)decision > coverage->max_decisions) coverage->max_decisions = (unsigned)decision;
        if (game.hp < coverage->minimum_hull) coverage->minimum_hull = game.hp;
        coverage->campaigns++;
    }
}

/* Run the routine suite, with broader seeded campaigns when explicitly selected. */
int main(int argc, char** argv){
    int exhaustive = argc == 2 && strcmp(argv[1], "--exhaustive") == 0;
    if (argc > 2 || (argc == 2 && !exhaustive)){
        fprintf(stderr, "usage: %s [--exhaustive]\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct Coverage coverage = {0, 0, 0, 0, INT_MAX};
    int result = game_selftest();
    if (result) fail("game selftest", 0, result);
    result = render_selftest();
    if (result) fail("renderer selftest", 0, result);
    result = music_selftest();
    if (result) fail("music selftest", 0, result);
    render_init();
    check_generated_views(42, &coverage);
    if (exhaustive){
        check_generated_views(0, &coverage);
        check_generated_views(3937812002u, &coverage);
    }
    check_camera_edges(&coverage);
    check_overlays(&coverage);
    check_lift_visibility(&coverage);
    check_sentry_occlusion(&coverage);
    check_entity_capacity(&coverage);
    check_audio_chunks();
    check_campaigns(exhaustive ? 1000 : 128, &coverage);
    printf("lumen check: %u frames, %u floor cells, %u campaigns passed\n",
           coverage.frames, coverage.floor_cells, coverage.campaigns);
    printf("campaign maximum %u decisions; minimum final hull %d\n",
           coverage.max_decisions, coverage.minimum_hull);
    return EXIT_SUCCESS;
}
