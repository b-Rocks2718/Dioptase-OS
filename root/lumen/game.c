/*
 * LUMEN expedition rules. This file is pure, deterministic user-mode game
 * logic: it does not access devices, clocks, shared memory, or OS services.
 * Positions are map cells, directions run clockwise from north, and beam
 * colors are cyan, amber, and magenta. Enemy decisions happen only on turns
 * spent moving, firing, opening a door, reading a log, or waiting.
 */
#include "game.h"

#define START_HEALTH 30
#define START_ENERGY 14
#define SHOT_COST 2
#define RELAYS_REQUIRED 3
#define ENEMY_SIGHT 7
#define MEDKIT_HEAL 12
#define CELL_CHARGE 10

/* Cardinal offsets share the direction numbering used by the renderer. */
static int step_x[4] = {0, 1, 0, -1};
static int step_y[4] = {-1, 0, 1, 0};

/* Copy one short HUD sentence without depending on a host C library. */
static void sentence(char* target, const char* source){
    int i = 0;
    while (source[i] && i < 95){
        target[i] = source[i];
        i++;
    }
    target[i] = 0;
}

/* Publish the result of an action together with a useful next-step hint. */
static void announce(struct Game* game, const char* message, const char* detail){
    sentence(game->message, message);
    sentence(game->detail, detail);
}

/* Advance a nonzero xorshift state; unsigned arithmetic supplies wraparound. */
static unsigned random_word(struct Game* game){
    unsigned value = game->rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    game->rng = value;
    return value;
}

/* Return an integer distance without signed-minimum inputs in map space. */
static int magnitude(int value){
    return value < 0 ? -value : value;
}

/* Outside-map reads are walls, so rays and movement need no edge exceptions. */
int game_cell(struct Game* game, int x, int y){
    if (x < 0 || x >= WORLD_W || y < 0 || y >= WORLD_H) return CELL_WALL;
    return game->cells[y * WORLD_W + x];
}

/* Find a live object; completed relays remain in the array for rendering. */
int game_entity(struct Game* game, int x, int y){
    for (int i = 0; i < game->entity_count; i++){
        struct Entity* entity = &game->entities[i];
        if (entity->active && entity->x == x && entity->y == y) return i;
    }
    return -1;
}

/* Placement also considers inactive objects so scenery never shares cells. */
static int occupied(struct Game* game, int x, int y){
    for (int i = 0; i < game->entity_count; i++){
        if (game->entities[i].x == x && game->entities[i].y == y) return 1;
    }
    return 0;
}

/* Relays are physical pylons even after activation; pickups can be crossed. */
static int actor_blocked(struct Game* game, int x, int y){
    for (int i = 0; i < game->entity_count; i++){
        struct Entity* entity = &game->entities[i];
        if (entity->x != x || entity->y != y) continue;
        if (entity->type == ENTITY_RELAY) return 1;
        if (entity->type == ENTITY_DRONE && entity->active) return 1;
    }
    return 0;
}

/* Floor and exit pads admit movement; closed doors require interaction. */
static int open_cell(struct Game* game, int x, int y){
    int cell = game_cell(game, x, y);
    return cell == CELL_FLOOR || cell == CELL_EXIT;
}

/* Carve only interior cells, preserving a solid boundary around every map. */
static void carve(struct Game* game, int x, int y){
    if (x > 0 && x < WORLD_W - 1 && y > 0 && y < WORLD_H - 1){
        game->cells[y * WORLD_W + x] = CELL_FLOOR;
    }
}

/* Join rooms through a randomly bent orthogonal passage without isolation. */
static void connect_rooms(struct Game* game, int ax, int ay, int bx, int by){
    if (ay == by){
        int bend = ay + (int)(random_word(game) % 3) - 1;
        for (int y = ay - 1; y <= ay + 1; y++){
            if ((y >= ay && y <= bend) || (y <= ay && y >= bend)){
                carve(game, ax, y);
                carve(game, bx, y);
            }
        }
        for (int x = ax; x <= bx; x++) carve(game, x, bend);
    } else {
        int bend = ax + (int)(random_word(game) % 3) - 1;
        for (int x = ax - 1; x <= ax + 1; x++){
            if ((x >= ax && x <= bend) || (x <= ax && x >= bend)){
                carve(game, x, ay);
                carve(game, x, by);
            }
        }
        for (int y = ay; y <= by; y++) carve(game, bend, y);
    }
}

/* Initialize every entity field; later turns may safely inspect any type. */
static void add_entity(struct Game* game, int type, int x, int y, int color){
    if (game->entity_count >= MAX_ENTITIES) return;
    struct Entity* entity = &game->entities[game->entity_count++];
    entity->type = type;
    entity->x = x;
    entity->y = y;
    entity->color = color;
    entity->health = type == ENTITY_DRONE ? 2 + game->sector * 2 : 1;
    entity->active = 1;
    entity->alert = 0;
}

/* Scatter objects on distinct floors, with a deterministic fallback search. */
static void scatter(struct Game* game, int type, int color, int minimum_distance){
    int start = (int)(random_word(game) % WORLD_CELLS);
    for (int i = 0; i < WORLD_CELLS; i++){
        int index = (start + i) % WORLD_CELLS;
        int x = index % WORLD_W;
        int y = index / WORLD_W;
        if (game_cell(game, x, y) != CELL_FLOOR || occupied(game, x, y)) continue;
        if (magnitude(x - 3) + magnitude(y - 3) < minimum_distance) continue;
        if (x == game->exit_x && y == game->exit_y) continue;
        add_entity(game, type, x, y, color);
        return;
    }
}

/* Reveal nearby cells through short rays, including the wall ending a ray. */
static void reveal(struct Game* game){
    for (int target_y = game->y - 5; target_y <= game->y + 5; target_y++){
        for (int target_x = game->x - 5; target_x <= game->x + 5; target_x++){
            int x = game->x;
            int y = game->y;
            int dx = magnitude(target_x - x);
            int dy = -magnitude(target_y - y);
            int sx = x < target_x ? 1 : -1;
            int sy = y < target_y ? 1 : -1;
            int error = dx + dy;
            for (int length = 0; length < 7; length++){
                if (x < 0 || x >= WORLD_W || y < 0 || y >= WORLD_H) break;
                game->seen[y * WORLD_W + x] = 1;
                if (!open_cell(game, x, y) || (x == target_x && y == target_y)) break;
                int twice_error = error * 2;
                if (twice_error >= dy){ error += dy; x += sx; }
                if (twice_error <= dx){ error += dx; y += sy; }
            }
        }
    }
}

/* Build nine connected chambers and every grid edge, providing escape loops. */
static void build_sector(struct Game* game){
    game->entity_count = 0;
    game->relays = 0;
    game->x = 3;
    game->y = 3;
    game->dir = 1;
    game->exit_x = 17;
    game->exit_y = 15;
    game->sound = SOUND_NONE;
    game->shot = 0;
    game->flash = 0;
    for (int i = 0; i < WORLD_CELLS; i++){
        game->cells[i] = CELL_WALL;
        game->seen[i] = 0;
    }
    for (int row = 0; row < 3; row++){
        for (int column = 0; column < 3; column++){
            int cx = 3 + column * 7;
            int cy = 3 + row * 6;
            int half_width = 1 + (int)(random_word(game) % 2);
            int half_height = 1 + (int)(random_word(game) % 2);
            /* Objective chambers need room to walk around their pylons. */
            if ((column == 1 && row == 0) || (column == 0 && row == 2) ||
                (column == 2 && row == 1)){
                half_width = 2;
                half_height = 2;
            }
            for (int y = cy - half_height; y <= cy + half_height; y++){
                for (int x = cx - half_width; x <= cx + half_width; x++) carve(game, x, y);
            }
            if (column < 2) connect_rooms(game, cx, cy, cx + 7, cy);
            if (row < 2) connect_rooms(game, cx, cy, cx, cy + 6);
        }
    }
    /* A clear entry vista introduces the first cyan relay immediately. */
    for (int x = 3; x <= 10; x++) carve(game, x, 3);
    add_entity(game, ENTITY_RELAY, 10, 3, 0);
    add_entity(game, ENTITY_RELAY, 3, 15, 1);
    add_entity(game, ENTITY_RELAY, 17, 9, 2);
    game->cells[game->exit_y * WORLD_W + game->exit_x] = CELL_EXIT;
    /* Doors add deliberate turn costs; none closes the introductory vista. */
    for (int i = 0; i < 4; i++){
        int x = 6 + (i % 2) * 7;
        int y = 8 + (i / 2) * 6;
        for (int n = 0; n < 3; n++){
            if (game_cell(game, x, y + n) == CELL_FLOOR){
                game->cells[(y + n) * WORLD_W + x] = CELL_DOOR;
                break;
            }
        }
    }
    /* Glass only replaces walls, preserving the connected floor graph. */
    for (int i = 0; i < 20; i++){
        int x = 1 + (int)(random_word(game) % (WORLD_W - 2));
        int y = 1 + (int)(random_word(game) % (WORLD_H - 2));
        if (game_cell(game, x, y) == CELL_WALL &&
            (open_cell(game, x - 1, y) || open_cell(game, x + 1, y))){
            game->cells[y * WORLD_W + x] = CELL_GLASS;
        }
    }
    for (int i = 0; i < 3 + game->sector * 2; i++){
        scatter(game, ENTITY_DRONE, (i + game->sector - 1) % 3, 9);
    }
    for (int i = 0; i < 3; i++) scatter(game, ENTITY_MEDKIT, 0, 4);
    for (int i = 0; i < 4; i++) scatter(game, ENTITY_CELL, 0, 4);
    for (int i = 0; i < 2; i++) scatter(game, ENTITY_LOG, i, 5);
    if (game->sector == 1){
        announce(game, "WAKE UP, LIGHTKEEPER.", "MATCH THREE RELAYS. FIND THE LIFT. KEEP MOVING.");
    } else if (game->sector == 2){
        announce(game, "SECTOR 02 / THE CHOIR VAULT", "THE STATION IS LISTENING. BRING ITS LIGHT BACK.");
    } else {
        announce(game, "SECTOR 03 / THE LAST SUN", "THREE MORE RELAYS. THEN WE CAN SEE HOME.");
    }
    reveal(game);
}

/* Start with fully initialized storage, including deterministic test padding. */
void game_start(struct Game* game, unsigned seed){
    unsigned char* bytes = (unsigned char*)game;
    for (unsigned i = 0; i < sizeof(struct Game); i++) bytes[i] = 0;
    game->seed = seed;
    game->rng = seed ? seed : 0x6d2b79f5u;
    game->sector = 1;
    game->mode = MODE_PLAY;
    game->hp = START_HEALTH;
    game->max_hp = START_HEALTH;
    game->energy = START_ENERGY;
    game->max_energy = START_ENERGY;
    game->power = 2;
    game->regen = 2;
    build_sector(game);
}

/* Cardinal beam paths stop at solid cells and physical relay pylons. */
static int clear_shot(struct Game* game, struct Entity* drone){
    int dx = game->x - drone->x;
    int dy = game->y - drone->y;
    if (dx != 0 && dy != 0) return 0;
    if (magnitude(dx) + magnitude(dy) > ENEMY_SIGHT) return 0;
    int sx = dx == 0 ? 0 : (dx > 0 ? 1 : -1);
    int sy = dy == 0 ? 0 : (dy > 0 ? 1 : -1);
    int x = drone->x + sx;
    int y = drone->y + sy;
    while (x != game->x || y != game->y){
        if (!open_cell(game, x, y) || actor_blocked(game, x, y)) return 0;
        x += sx;
        y += sy;
    }
    return 1;
}

/* Breadth-first distances let sentries navigate loops without walking walls. */
static void distances(struct Game* game, int* distance){
    int queue[WORLD_CELLS];
    int head = 0;
    int tail = 0;
    for (int i = 0; i < WORLD_CELLS; i++) distance[i] = -1;
    int initial = game->y * WORLD_W + game->x;
    distance[initial] = 0;
    queue[tail++] = initial;
    while (head < tail){
        int index = queue[head++];
        int x = index % WORLD_W;
        int y = index / WORLD_W;
        for (int direction = 0; direction < 4; direction++){
            int nx = x + step_x[direction];
            int ny = y + step_y[direction];
            if (!open_cell(game, nx, ny)) continue;
            int next = ny * WORLD_W + nx;
            if (distance[next] != -1) continue;
            distance[next] = distance[index] + 1;
            queue[tail++] = next;
        }
    }
}

/* Resolve sentries after the player, allowing two visible lock stages to dodge. */
static void enemies_turn(struct Game* game){
    int distance[WORLD_CELLS];
    int danger = 0;
    int hurt = 0;
    distances(game, distance);
    for (int i = 0; i < game->entity_count; i++){
        struct Entity* drone = &game->entities[i];
        if (drone->type != ENTITY_DRONE || !drone->active) continue;
        if (clear_shot(game, drone)){
            if (drone->alert == 2){
                game->hp -= 2 + game->sector;
                hurt++;
                drone->alert = 0;
            } else {
                drone->alert++;
                if (drone->alert > danger) danger = drone->alert;
            }
            continue;
        }
        /* Breaking the beam line cancels its lock immediately. */
        drone->alert = 0;
        int current = distance[drone->y * WORLD_W + drone->x];
        if (current <= 1 || current > ENEMY_SIGHT || current < 0) continue;
        /* Alternating steps leave time to reposition and inspect the map. */
        if ((game->turns + (unsigned)i) % 2 != 0) continue;
        for (int direction = 0; direction < 4; direction++){
            int nx = drone->x + step_x[direction];
            int ny = drone->y + step_y[direction];
            if (!open_cell(game, nx, ny) || actor_blocked(game, nx, ny)) continue;
            int next = distance[ny * WORLD_W + nx];
            if (next > 0 && next < current){
                drone->x = nx;
                drone->y = ny;
                break;
            }
        }
    }
    if (hurt){
        game->sound = SOUND_HURT;
        game->flash = 5;
        sentence(game->detail, "HULL HIT! BREAK THE SENTRY'S LINE OF FIRE.");
    } else if (danger == 2){
        sentence(game->detail, "LOCK CONFIRMED! STEP ASIDE OR FIRE TO DESTROY.");
    } else if (danger == 1){
        sentence(game->detail, "SENTRY ACQUIRING. MATCH ITS COLOR OR TAKE COVER.");
    }
    if (game->hp <= 0){
        game->hp = 0;
        game->mode = MODE_DEAD;
        announce(game, "YOUR LIGHT GOES QUIET.", "R: ANOTHER SIGNAL. Q: RETURN TO THE TERMINAL.");
    }
}

/* Walking over supplies collects only resources that can currently help. */
static void collect(struct Game* game){
    int index = game_entity(game, game->x, game->y);
    if (index < 0) return;
    struct Entity* entity = &game->entities[index];
    if (entity->type == ENTITY_MEDKIT){
        if (game->hp == game->max_hp){
            announce(game, "MEDICAL CACHE / HULL ALREADY FULL", "THIS CACHE WILL WAIT HERE UNTIL YOU NEED IT.");
            return;
        }
        game->hp += MEDKIT_HEAL;
        if (game->hp > game->max_hp) game->hp = game->max_hp;
        announce(game, "MEDICAL NANITES / HULL RESTORED", "TWELVE HULL POINTS. EVERY LITTLE LIGHT HELPS.");
    } else if (entity->type == ENTITY_CELL){
        if (game->energy == game->max_energy){
            announce(game, "POWER CELL / CAPACITOR ALREADY FULL", "THIS CELL WILL WAIT HERE UNTIL YOU NEED IT.");
            return;
        }
        game->energy += CELL_CHARGE;
        if (game->energy > game->max_energy) game->energy = game->max_energy;
        announce(game, "POWER CELL / TEN CHARGES RECOVERED", "SPACE FIRES. WAIT TO RECHARGE WITHOUT A CELL.");
    } else if (entity->type == ENTITY_LOG){
        announce(game, "A VOICE STILL LIVES IN THIS TERMINAL.", "PRESS E TO READ THE CREW'S LAST TRANSMISSION.");
        return;
    } else return;
    entity->active = 0;
    game->score += 10;
    game->sound = SOUND_PICKUP;
}

/* Match a relay's spectrum to activate it once; other beams are harmless. */
static void shoot_relay(struct Game* game, struct Entity* relay){
    if (relay->color != game->color){
        announce(game, "RELAY REJECTED / SPECTRUM MISMATCH", "1 CYAN   2 AMBER   3 MAGENTA. MATCH ITS GLOW.");
        return;
    }
    relay->active = 0;
    relay->health = 0;
    game->relays++;
    game->score += 100;
    game->sound = SOUND_RELAY;
    if (game->relays == RELAYS_REQUIRED){
        announce(game, "THREE LIGHTS ONLINE / LIFT UNLOCKED", "FIND THE LIFT IN THE SOUTHEAST. E TO ASCEND.");
    } else {
        announce(game, "RELAY ONLINE / THE DARKNESS RETREATS", "FIND THE REMAINING COLORS. CHECK YOUR MAP.");
    }
}

/* Resolve a straight beam against the nearest solid object in front. */
static void fire(struct Game* game){
    game->energy -= SHOT_COST;
    game->shot = 4;
    game->sound = SOUND_SHOT;
    announce(game, "PRISM BEAM / NO TARGET", "LINE UP A RELAY OR SENTRY IN THE CROSSHAIR.");
    int x = game->x;
    int y = game->y;
    for (int length = 0; length < WORLD_W + WORLD_H; length++){
        x += step_x[game->dir];
        y += step_y[game->dir];
        int cell = game_cell(game, x, y);
        if (cell == CELL_WALL || cell == CELL_GLASS || cell == CELL_DOOR){
            if (cell == CELL_DOOR) announce(game, "BULKHEAD BLOCKS THE BEAM", "STAND NEXT TO IT AND PRESS E TO OPEN IT.");
            break;
        }
        for (int i = 0; i < game->entity_count; i++){
            struct Entity* entity = &game->entities[i];
            if (entity->x != x || entity->y != y) continue;
            if (entity->type == ENTITY_RELAY){
                if (entity->active) shoot_relay(game, entity);
                else announce(game, "THIS RELAY IS ALREADY SHINING.", "SEEK ANOTHER RELAY OR THE SOUTHEAST LIFT.");
                return;
            }
            if (entity->type != ENTITY_DRONE || !entity->active) continue;
            int matched = entity->color == game->color;
            entity->health -= matched ? game->power * 2 : 1;
            game->sound = SOUND_HIT;
            if (entity->health <= 0){
                entity->health = 0;
                entity->active = 0;
                entity->alert = 0;
                game->kills++;
                game->score += 40;
                announce(game, "SENTRY SHATTERED / SIGNAL LOST", "MATCHING COLORS CUT THROUGH SENTRY SHIELDS.");
            } else if (matched){
                announce(game, "SPECTRUM MATCH / SHIELD FRACTURED", "ANOTHER MATCHING SHOT WILL FINISH THE JOB.");
            } else {
                announce(game, "WRONG SPECTRUM / SHIELD DEFLECTED", "ONLY A GLANCING HIT. 1 CYAN  2 AMBER  3 MAGENTA.");
            }
            return;
        }
    }
}

/* Collect a crew fragment and enough charge to make exploration rewarding. */
static void read_log(struct Game* game, struct Entity* entity){
    entity->active = 0;
    game->secrets++;
    game->score += 75;
    game->energy += 4;
    if (game->energy > game->max_energy) game->energy = game->max_energy;
    game->sound = SOUND_PICKUP;
    if (game->sector == 1 && entity->color == 0){
        announce(game, "LOG / DR. SOL: WE TAUGHT THE STATION TO DREAM.", "IT DREAMED OF A SUN. THEN IT TURNED US OFF.");
    } else if (game->sector == 1){
        announce(game, "LOG / ENGINEERING: COLOR IS THE KEY.", "THE SENTRIES WEAR THE FREQUENCY THEY FEAR.");
    } else if (game->sector == 2 && entity->color == 0){
        announce(game, "LOG / NIA: THE LIGHTKEEPER IS STILL ASLEEP.", "LEAVE A PATH. LEAVE A LITTLE POWER. LEAVE HOPE.");
    } else if (game->sector == 2){
        announce(game, "LOG / DR. SOL: IT WAS NEVER AN EMPTY STATION.", "THE CHOIR IS US. THE LIGHTS REMEMBER EVERYONE.");
    } else if (entity->color == 0){
        announce(game, "LOG / NIA: IF YOU REACH THE LAST SUN...", "DON'T SHUT IT DOWN. OPEN THE SHUTTERS.");
    } else {
        announce(game, "LOG / HOME: YOUR SIGNAL REACHED US.", "WE CAN SEE YOU. COME BACK INTO THE LIGHT.");
    }
}

/* Finish a sector only at its unlocked lift; upgrades precede regeneration. */
static void use_exit(struct Game* game){
    if (game->relays < RELAYS_REQUIRED){
        announce(game, "LIFT OFFLINE / THREE RELAYS REQUIRED", "RESTORE CYAN, AMBER, AND MAGENTA RELAYS FIRST.");
        return;
    }
    game->score += 250 + game->hp * 2;
    game->sound = SOUND_EXIT;
    if (game->sector == SECTOR_COUNT){
        game->mode = MODE_WON;
        game->score += 500;
        announce(game, "SHUTTERS OPEN / YOU BROUGHT THE SUN HOME.", "THE CHOIR IS SINGING. EVERY LIGHT HAS A NAME.");
    } else {
        game->mode = MODE_UPGRADE;
        announce(game, "LIFT ASCENDING / CHOOSE YOUR NEXT LIGHT", "1 HULL WEAVE   2 PRISM LENS   3 CAPACITOR");
    }
}

/* Interactions without a useful target are free and explain the requirement. */
static int interact(struct Game* game){
    int x = game->x + step_x[game->dir];
    int y = game->y + step_y[game->dir];
    if (game_cell(game, x, y) == CELL_DOOR){
        game->cells[y * WORLD_W + x] = CELL_FLOOR;
        game->sound = SOUND_STEP;
        announce(game, "BULKHEAD OPEN / ANOTHER WAY THROUGH", "DOORS BLOCK SENTRY BEAMS UNTIL THEY ARE OPEN.");
        return 1;
    }
    for (int i = 0; i < game->entity_count; i++){
        struct Entity* entity = &game->entities[i];
        if (!entity->active || entity->type != ENTITY_LOG) continue;
        if ((entity->x == x && entity->y == y) ||
            (entity->x == game->x && entity->y == game->y)){
            read_log(game, entity);
            return 1;
        }
    }
    if ((game->x == game->exit_x && game->y == game->exit_y) ||
        (x == game->exit_x && y == game->exit_y)){
        use_exit(game);
        return 0;
    }
    announce(game, "NOTHING TO INTERACT WITH HERE.", "E OPENS DOORS, READS LOGS, AND USES THE LIFT.");
    return 0;
}

/* Apply one discrete command; turning and failed actions never wake enemies. */
int game_act(struct Game* game, int action){
    if (game->mode != MODE_PLAY) return 0;
    game->sound = SOUND_NONE;
    game->shot = 0;
    game->flash = 0;
    if (action == ACT_LEFT || action == ACT_RIGHT){
        game->dir = (game->dir + (action == ACT_LEFT ? 3 : 1)) % 4;
        reveal(game);
        return 0;
    }
    int spent = 0;
    if (action == ACT_FORWARD || action == ACT_BACK ||
        action == ACT_STRAFE_LEFT || action == ACT_STRAFE_RIGHT){
        int direction = game->dir;
        if (action == ACT_BACK) direction = (direction + 2) % 4;
        if (action == ACT_STRAFE_LEFT) direction = (direction + 3) % 4;
        if (action == ACT_STRAFE_RIGHT) direction = (direction + 1) % 4;
        int x = game->x + step_x[direction];
        int y = game->y + step_y[direction];
        if (!open_cell(game, x, y)){
            if (game_cell(game, x, y) == CELL_DOOR){
                announce(game, "CLOSED BULKHEAD", "FACE THE DOOR AND PRESS E TO OPEN IT.");
            } else announce(game, "SOLID WALL / FIND ANOTHER PATH", "TURN WITH A / D. STRAFE WITH Z / C.");
            return 0;
        }
        if (actor_blocked(game, x, y)){
            announce(game, "THE WAY IS OCCUPIED.", "RELAYS NEED MATCHING LIGHT. SENTRIES NEED LESS.");
            return 0;
        }
        game->x = x;
        game->y = y;
        game->sound = SOUND_STEP;
        announce(game, "FOOTSTEPS IN THE QUIET.", "THREE RELAYS OPEN THE LIFT. E READS CREW LOGS.");
        collect(game);
        spent = 1;
    } else if (action == ACT_FIRE){
        if (game->energy < SHOT_COST){
            announce(game, "CAPACITOR EMPTY / BEAM NEEDS TWO CHARGES", "WAIT TO RECHARGE, OR FIND A POWER CELL.");
            return 0;
        }
        fire(game);
        spent = 1;
    } else if (action == ACT_WAIT){
        game->energy += game->regen;
        if (game->energy > game->max_energy) game->energy = game->max_energy;
        announce(game, "BREATHE / CAPACITOR RECHARGING", "WAITING COSTS A TURN. SENTRY LOCKS ADVANCE.");
        collect(game);
        spent = 1;
    } else if (action == ACT_USE){
        spent = interact(game);
    }
    if (spent && game->mode == MODE_PLAY){
        game->turns++;
        enemies_turn(game);
    }
    reveal(game);
    return spent;
}

/* Apply exactly one valid lift upgrade, repair supplies, and build the sector. */
void game_upgrade(struct Game* game, int choice){
    if (game->mode != MODE_UPGRADE || choice < 1 || choice > 3) return;
    if (choice == 1){
        game->max_hp += 12;
        game->hp += 12;
    } else if (choice == 2){
        game->power++;
    } else {
        game->max_energy += 6;
        game->regen++;
    }
    game->hp += 8;
    if (game->hp > game->max_hp) game->hp = game->max_hp;
    game->energy = game->max_energy;
    game->sector++;
    game->mode = MODE_PLAY;
    build_sector(game);
}

/* Turn one quarter-step toward an intended command without spending a turn. */
static int face_direction(struct Game* game, int direction){
    if (game->dir == direction) return 1;
    int clockwise = (direction - game->dir + 4) % 4;
    game_act(game, clockwise == 3 ? ACT_LEFT : ACT_RIGHT);
    return 0;
}

/* Select the closest visible target, giving sentries priority over relays. */
static int demo_target(struct Game* game, int* direction){
    int best = -1;
    int best_value = WORLD_CELLS;
    for (int dir = 0; dir < 4; dir++){
        int x = game->x;
        int y = game->y;
        for (int distance = 1; distance < WORLD_W + WORLD_H; distance++){
            x += step_x[dir];
            y += step_y[dir];
            if (!open_cell(game, x, y)) break;
            int blocked = 0;
            for (int i = 0; i < game->entity_count; i++){
                struct Entity* entity = &game->entities[i];
                if (entity->x != x || entity->y != y) continue;
                if (entity->type == ENTITY_RELAY ||
                    (entity->type == ENTITY_DRONE && entity->active)){
                    blocked = 1;
                    int value = distance + (entity->type == ENTITY_RELAY ? 40 : 0);
                    if (entity->active && value < best_value){
                        best = i;
                        best_value = value;
                        *direction = dir;
                    }
                }
            }
            if (blocked) break;
        }
    }
    return best;
}

/* Pick one useful reachable cell from the BFS tree, favoring needed supplies. */
static int demo_destination(struct Game* game, int* distance){
    int best = -1;
    int best_cost = WORLD_CELLS * 4;
    if (game->relays == RELAYS_REQUIRED){
        int exit = game->exit_y * WORLD_W + game->exit_x;
        if (distance[exit] >= 0){
            best = exit;
            best_cost = distance[exit] + 20;
        }
    }
    for (int i = 0; i < game->entity_count; i++){
        struct Entity* entity = &game->entities[i];
        if (!entity->active) continue;
        int priority = 0;
        if (entity->type == ENTITY_LOG) priority = 10;
        if (entity->type == ENTITY_CELL){
            if (game->energy >= game->max_energy - 4) continue;
            priority = game->energy < 4 ? -8 : 8;
        }
        if (entity->type == ENTITY_MEDKIT){
            if (game->hp > game->max_hp - 8) continue;
            priority = game->hp < 14 ? -12 : 6;
        }
        for (int direction = 0; direction < 4; direction++){
            int x = entity->x + step_x[direction];
            int y = entity->y + step_y[direction];
            if (!open_cell(game, x, y) && game_cell(game, x, y) != CELL_DOOR) continue;
            int index = y * WORLD_W + x;
            if (distance[index] < 0) continue;
            /* Supplies and logs can be occupied directly, unlike pylons. */
            if (entity->type == ENTITY_CELL || entity->type == ENTITY_MEDKIT ||
                entity->type == ENTITY_LOG){
                index = entity->y * WORLD_W + entity->x;
                if (distance[index] < 0) continue;
            }
            int cost = distance[index] + priority;
            if (cost < best_cost){
                best_cost = cost;
                best = index;
            }
        }
    }
    return best;
}

/* Execute one demo decision through normal rules, including damage and costs. */
void game_autoplay(struct Game* game){
    if (game->mode == MODE_UPGRADE){
        game_upgrade(game, 2);
        return;
    }
    if (game->mode != MODE_PLAY) return;
    int direction = game->dir;
    int target = demo_target(game, &direction);
    if (target >= 0){
        if (!face_direction(game, direction)) return;
        game->color = game->entities[target].color;
        game_act(game, game->energy >= SHOT_COST ? ACT_FIRE : ACT_WAIT);
        return;
    }
    int current = game->y * WORLD_W + game->x;
    int object = game_entity(game, game->x, game->y);
    if (object >= 0 && game->entities[object].type == ENTITY_LOG){
        game_act(game, ACT_USE);
        return;
    }
    if (game->relays == RELAYS_REQUIRED && game->x == game->exit_x && game->y == game->exit_y){
        game_act(game, ACT_USE);
        return;
    }
    int distance[WORLD_CELLS];
    int predecessor[WORLD_CELLS];
    int queue[WORLD_CELLS];
    int head = 0;
    int tail = 0;
    for (int i = 0; i < WORLD_CELLS; i++){
        distance[i] = -1;
        predecessor[i] = -1;
    }
    distance[current] = 0;
    queue[tail++] = current;
    while (head < tail){
        int index = queue[head++];
        int x = index % WORLD_W;
        int y = index / WORLD_W;
        for (int dir = 0; dir < 4; dir++){
            int nx = x + step_x[dir];
            int ny = y + step_y[dir];
            if ((!open_cell(game, nx, ny) && game_cell(game, nx, ny) != CELL_DOOR) ||
                actor_blocked(game, nx, ny)) continue;
            int next = ny * WORLD_W + nx;
            if (distance[next] >= 0) continue;
            distance[next] = distance[index] + 1;
            predecessor[next] = index;
            queue[tail++] = next;
        }
    }
    int destination = demo_destination(game, distance);
    if (destination < 0 || destination == current){
        game_act(game, ACT_WAIT);
        return;
    }
    while (predecessor[destination] != current){
        destination = predecessor[destination];
        if (destination < 0) return;
    }
    int nx = destination % WORLD_W;
    int ny = destination / WORLD_W;
    direction = ny < game->y ? 0 : (nx > game->x ? 1 : (ny > game->y ? 2 : 3));
    if (!face_direction(game, direction)) return;
    game_act(game, game_cell(game, nx, ny) == CELL_DOOR ? ACT_USE : ACT_FORWARD);
}

/* Verify every walkable cell and objective is reachable with doors opened. */
int game_validate(struct Game* game){
    unsigned char visited[WORLD_CELLS];
    int queue[WORLD_CELLS];
    int head = 0;
    int tail = 0;
    int exits = 0;
    int relays = 0;
    int active_relays = 0;
    int colors = 0;
    if (game->entity_count < 0 || game->entity_count > MAX_ENTITIES) return 1;
    if (game->sector < 1 || game->sector > SECTOR_COUNT) return 2;
    if (game->dir < 0 || game->dir > 3 || !open_cell(game, game->x, game->y)) return 3;
    if (game->hp < 0 || game->hp > game->max_hp || game->max_hp <= 0) return 4;
    if (game->energy < 0 || game->energy > game->max_energy) return 5;
    for (int y = 0; y < WORLD_H; y++){
        for (int x = 0; x < WORLD_W; x++){
            int index = y * WORLD_W + x;
            int cell = game->cells[index];
            visited[index] = 0;
            if (cell < CELL_FLOOR || cell > CELL_EXIT) return 6;
            if ((x == 0 || y == 0 || x == WORLD_W - 1 || y == WORLD_H - 1) &&
                cell != CELL_WALL) return 7;
            if (cell == CELL_EXIT) exits++;
        }
    }
    if (exits != 1 || game_cell(game, game->exit_x, game->exit_y) != CELL_EXIT) return 8;
    int start = game->y * WORLD_W + game->x;
    queue[tail++] = start;
    visited[start] = 1;
    while (head < tail){
        int index = queue[head++];
        int x = index % WORLD_W;
        int y = index / WORLD_W;
        for (int direction = 0; direction < 4; direction++){
            int nx = x + step_x[direction];
            int ny = y + step_y[direction];
            int cell = game_cell(game, nx, ny);
            if (cell != CELL_FLOOR && cell != CELL_DOOR && cell != CELL_EXIT) continue;
            int next = ny * WORLD_W + nx;
            if (!visited[next]){ visited[next] = 1; queue[tail++] = next; }
        }
    }
    for (int i = 0; i < WORLD_CELLS; i++){
        if ((game->cells[i] == CELL_FLOOR || game->cells[i] == CELL_DOOR ||
            game->cells[i] == CELL_EXIT) && !visited[i]) return 9;
    }
    for (int i = 0; i < game->entity_count; i++){
        struct Entity* entity = &game->entities[i];
        if (entity->type < ENTITY_RELAY || entity->type > ENTITY_LOG) return 10;
        if (!open_cell(game, entity->x, entity->y)) return 11;
        if (entity->color < 0 || entity->color > 2) return 12;
        if (entity->alert < 0 || entity->alert > 2) return 13;
        if (entity->active != 0 && entity->active != 1) return 14;
        if (entity->active && entity->health <= 0) return 17;
        if (entity->active && entity->type == ENTITY_DRONE &&
            entity->x == game->x && entity->y == game->y) return 18;
        for (int j = i + 1; j < game->entity_count; j++){
            struct Entity* other = &game->entities[j];
            /* Sentries may cross supplies, but two live solids cannot overlap. */
            if (entity->x == other->x && entity->y == other->y &&
                entity->active && other->active){
                int crossing_supply =
                    (entity->type == ENTITY_DRONE && other->type >= ENTITY_MEDKIT) ||
                    (other->type == ENTITY_DRONE && entity->type >= ENTITY_MEDKIT);
                if (!crossing_supply) return 15;
            }
        }
        if (entity->type == ENTITY_RELAY){
            relays++;
            active_relays += entity->active;
            colors |= 1 << entity->color;
        }
    }
    if (relays != RELAYS_REQUIRED || colors != 7 ||
        active_relays + game->relays != RELAYS_REQUIRED) return 16;
    return 0;
}

/* Create a controlled arena so action tests do not depend on map generation. */
static void test_arena(struct Game* game){
    game_start(game, 1);
    game->entity_count = 0;
    for (int y = 1; y < WORLD_H - 1; y++){
        for (int x = 1; x < WORLD_W - 1; x++) game->cells[y * WORLD_W + x] = CELL_FLOOR;
    }
    game->cells[game->exit_y * WORLD_W + game->exit_x] = CELL_EXIT;
    game->x = 3;
    game->y = 3;
    game->dir = 1;
}

/* Check seeded maps and core action contracts; each failure has a stable ID. */
int game_selftest(void){
    struct Game game;
    struct Game twin;
    for (unsigned seed = 0; seed < 128; seed++){
        game_start(&game, seed);
        game_start(&twin, seed);
        unsigned char* first = (unsigned char*)&game;
        unsigned char* second = (unsigned char*)&twin;
        for (unsigned byte = 0; byte < sizeof(struct Game); byte++){
            if (first[byte] != second[byte]) return 100;
        }
        for (int sector = 1; sector <= SECTOR_COUNT; sector++){
            if (game_validate(&game)) return 101;
            if (game.entity_count != 15 + sector * 2) return 102;
            if (sector < SECTOR_COUNT){
                game.mode = MODE_UPGRADE;
                game_upgrade(&game, sector);
            }
        }
    }
    test_arena(&game);
    game_act(&game, ACT_LEFT);
    if (game.dir != 0 || game.turns != 0) return 110;
    game_act(&game, ACT_RIGHT);
    game.cells[3 * WORLD_W + 4] = CELL_DOOR;
    if (game_act(&game, ACT_FORWARD) || game.x != 3 || game.turns) return 111;
    if (!game_act(&game, ACT_USE) || game_cell(&game, 4, 3) != CELL_FLOOR) return 112;
    game_act(&game, ACT_FORWARD);
    if (game.x != 4 || game.y != 3 || game.turns != 2) return 113;
    game_act(&game, ACT_STRAFE_RIGHT);
    if (game.x != 4 || game.y != 4 || game.dir != 1) return 114;
    test_arena(&game);
    add_entity(&game, ENTITY_RELAY, 5, 3, 1);
    game_act(&game, ACT_FIRE);
    if (game.relays || !game.entities[0].active || game.energy != 12) return 120;
    game.color = 1;
    game_act(&game, ACT_FIRE);
    if (game.relays != 1 || game.entities[0].active || game.sound != SOUND_RELAY) return 121;
    game_act(&game, ACT_FIRE);
    if (game.relays != 1 || game.score != 100) return 122;
    test_arena(&game);
    add_entity(&game, ENTITY_DRONE, 6, 3, 2);
    game_act(&game, ACT_FIRE);
    if (game.entities[0].health != 3 || game.entities[0].alert != 1 || game.hp != START_HEALTH) return 130;
    game_act(&game, ACT_WAIT);
    if (game.entities[0].alert != 2 || game.hp != START_HEALTH) return 131;
    game_act(&game, ACT_STRAFE_RIGHT);
    if (game.entities[0].alert != 0 || game.hp != START_HEALTH) return 132;
    test_arena(&game);
    add_entity(&game, ENTITY_DRONE, 6, 3, 0);
    game_act(&game, ACT_FIRE);
    if (game.entities[0].active || game.kills != 1 || game.hp != START_HEALTH) return 133;
    test_arena(&game);
    add_entity(&game, ENTITY_DRONE, 6, 3, 0);
    game_act(&game, ACT_WAIT);
    game_act(&game, ACT_WAIT);
    game_act(&game, ACT_WAIT);
    if (game.hp != START_HEALTH - 3 || game.flash != 5) return 134;
    game.hp = 1;
    game.entities[0].alert = 2;
    game_act(&game, ACT_WAIT);
    if (game.mode != MODE_DEAD || game.hp != 0) return 135;
    unsigned stopped_turn = game.turns;
    game_act(&game, ACT_FORWARD);
    if (game.turns != stopped_turn) return 136;
    test_arena(&game);
    game.energy = 1;
    if (game_act(&game, ACT_FIRE) || game.energy != 1 || game.turns != 0) return 140;
    game_act(&game, ACT_WAIT);
    if (game.energy != 3 || game.turns != 1) return 141;
    game.energy = game.max_energy - 1;
    game_act(&game, ACT_WAIT);
    if (game.energy != game.max_energy) return 142;
    test_arena(&game);
    add_entity(&game, ENTITY_MEDKIT, 4, 3, 0);
    game.hp = 9;
    game_act(&game, ACT_FORWARD);
    if (game.hp != 21 || game.entities[0].active) return 150;
    add_entity(&game, ENTITY_CELL, 5, 3, 0);
    game.energy = 1;
    game_act(&game, ACT_FORWARD);
    if (game.energy != 11 || game.entities[1].active) return 151;
    add_entity(&game, ENTITY_LOG, 6, 3, 0);
    game_act(&game, ACT_USE);
    if (game.secrets != 1 || game.entities[2].active || game.energy != game.max_energy) return 152;
    game.x = game.exit_x;
    game.y = game.exit_y - 1;
    game.dir = 2;
    add_entity(&game, ENTITY_LOG, game.x, game.y, 1);
    game_act(&game, ACT_USE);
    if (game.secrets != 2 || game.entities[3].active || game.mode != MODE_PLAY) return 153;
    test_arena(&game);
    game.x = game.exit_x;
    game.y = game.exit_y;
    game_act(&game, ACT_USE);
    if (game.mode != MODE_PLAY) return 160;
    game.relays = RELAYS_REQUIRED;
    game_act(&game, ACT_USE);
    if (game.mode != MODE_UPGRADE) return 161;
    game_upgrade(&game, 0);
    if (game.mode != MODE_UPGRADE) return 162;
    game.hp = 10;
    game_upgrade(&game, 1);
    if (game.mode != MODE_PLAY || game.sector != 2 || game.max_hp != 42 || game.hp != 30) return 163;
    game.mode = MODE_UPGRADE;
    game_upgrade(&game, 3);
    if (game.sector != 3 || game.max_energy != 20 || game.regen != 3) return 164;
    game.x = game.exit_x;
    game.y = game.exit_y;
    game.relays = RELAYS_REQUIRED;
    game_act(&game, ACT_USE);
    if (game.mode != MODE_WON || game.sound != SOUND_EXIT) return 165;
    game_start(&game, 99);
    game.mode = MODE_UPGRADE;
    game_upgrade(&game, 2);
    if (game.power != 3 || game.sector != 2 || game_validate(&game)) return 166;
    /* Corruptions prove diagnostics reject broken boundaries and objectives. */
    game.cells[0] = CELL_FLOOR;
    if (game_validate(&game) == 0) return 170;
    game.cells[0] = CELL_WALL;
    game.entities[0].color = game.entities[1].color;
    if (game_validate(&game) == 0) return 171;
    game.entities[0].color = 0;
    game.entities[3].x = game.entities[4].x;
    game.entities[3].y = game.entities[4].y;
    if (game_validate(&game) == 0) return 172;
    /* The extra seed puts a log beside the lift: E must read it before lifting. */
    for (unsigned sample = 0; sample < 33; sample++){
        unsigned seed = sample == 32 ? 3937812002u : sample;
        game_start(&game, seed);
        for (int decision = 0; decision < 2400 && game.mode != MODE_WON && game.mode != MODE_DEAD; decision++){
            game_autoplay(&game);
            if (game_validate(&game)) return 180;
        }
        if (game.mode != MODE_WON) return 181;
        if (game.sector != SECTOR_COUNT || game.relays != RELAYS_REQUIRED) return 182;
    }
    return 0;
}
