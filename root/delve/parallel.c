#include "delve.h"
#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "../crt/fcntl.h"

/*
 * Parallel rendering: the 3D view is split into three strips of 8-pixel
 * column groups (12, 14, and 14 of the 40). Two forked worker processes each
 * render one strip while the game process renders the third (the narrowest,
 * since it also runs the game), so a frame uses three of Dioptase's four
 * cores (the synthesizer process has the fourth). When the game process has
 * no overlays to draw into the back buffer, every process also copies its own
 * finished strip to the pixel framebuffer, so the copy is split three ways
 * too; otherwise the game process draws its overlays over the whole buffer
 * and copies it alone.
 *
 * Shared memory: Dioptase-OS has no anonymous shared mappings, but shared
 * file-backed mappings of one file alias the same page-cache pages in every
 * process (docs/vmem.md, "Shared File-Backed Mappings"), and fork() keeps
 * them shared. parallel_start() creates a scratch file in the current
 * directory, maps it MMAP_SHARED, and unlinks it at once; the inode lives
 * until the last mapping goes away, and since the kernel only writes dirty
 * cache pages back on that final release, frames never cause disk traffic.
 * The mapping holds:
 *
 *   [0, 122880)       the back buffer g_view (every process draws its strip)
 *   [122880, 123904)  g_seen, the automap's explored cells (rays mark them)
 *   [124928, ...)     struct FrameShare: what a strip needs to render
 *
 * Protocol, per frame, driven by the game process:
 *   1. copy the camera and world render state into FrameShare;
 *   2. increment frame_seq;
 *   3. render its own strip (and copy it to the framebuffer if `blit`);
 *   4. spin until every worker's done_seq equals frame_seq;
 *   5. without `blit`: draw overlays over the complete g_view and copy it.
 * A worker spins on frame_seq (yielding every few hundred polls, and dozing
 * with sleep(1) once frames stop coming), copies FrameShare into its own
 * globals (so the ordinary renderer runs unchanged), renders, and publishes
 * done_seq (after copying its strip out, when `blit` is set: the framebuffer
 * mapping, made before the fork, is shared like the file mapping). Waiting
 * by spinning on shared memory matters: blocking pipe
 * reads were tried first and made parallel frames slower than solo ones,
 * because a woken worker can wait up to a timer tick before an idle core
 * picks it up. Ordering: the architecture's memory model is sequentially
 * consistent (AGENTS.md), so a worker that sees the new frame_seq also sees
 * the FrameShare written before it, and the game process that sees a
 * worker's done_seq also sees that worker's pixels. Workers only read
 * FrameShare and only write their own strip, their done_seq, and g_seen
 * bytes (setting a byte to 1 is idempotent, so concurrent marks are safe).
 *
 * Lifetime: the game process refreshes a heartbeat word every loop and sets
 * `quit` on a normal exit; a worker exits when it sees `quit` or when the
 * heartbeat goes stale (the game crashed or was killed). If a worker stops
 * answering, the game process gives up on parallel rendering and renders
 * every strip itself from then on.
 *
 * Measured on the emulator with four cores, parallel frames were usually
 * ~25-30% faster, but not always: a worker the scheduler places on the game
 * process's core serializes the frame. With fewer cores spinning workers
 * would hurt. So the first game measures both modes (autotune below) and
 * drops back to rendering alone if parallel is clearly slower; '[' flips the
 * mode by hand.
 */

#define WORKERS 2
#define SHARED_BYTES 131072
#define SHARED_SEEN_OFFSET 122880
#define SHARED_FRAME_OFFSET 124928
#define WAIT_TIMEOUT_FRAMES 90     /* VGA frames before a worker is presumed dead */

/* Render state published for the workers each frame. Every member is a
 * multiple of four bytes so copy_words() can move it. */
struct FrameShare {
  int frame_seq;             /* bumped by the game process to start a frame */
  int done_seq[2];           /* per worker (WORKERS): last frame finished */
  int quit;                  /* set by the game process on a normal exit */
  unsigned heartbeat;        /* VGA frame counter, refreshed every game loop */
  struct Camera cam;
  struct ViewExtras extras;
  int blit;                  /* each process copies its strip to the framebuffer */
  int light_offset;
  int floor_flat;
  int ceiling_flat;
  int bill_count;
  int light_count;
  unsigned char map[MAP_CELLS];
  struct Door doors[MAX_DOORS];
  struct Billboard bills[MAX_BILLBOARDS];
  struct Light lights[MAX_LIGHTS];
  struct Particle particles[MAX_PARTICLES];
};

static struct FrameShare* share;
static int worker_count;
static int worker_pid[WORKERS];

/* Column-group strips: workers take 0 and 1, the game process the last
 * (WORKERS + 1 entries; array bounds must be literals). The staff is drawn
 * on the right, so the game process takes the left. */
static int strip_g0[3] = {12, 26, 0};
static int strip_g1[3] = {26, 40, 12};

/* copy_words() for a possibly empty count (the asm loop needs count > 0). */
static void copy_n(void* dst, void* src, int words){
  if (words > 0) copy_words(dst, src, words);
}

/* Game process: publish this frame's render state. */
static void publish(struct Camera* cam, struct ViewExtras* extras, bool blit){
  share->cam = *cam;
  share->extras = *extras;
  share->blit = blit;
  share->light_offset = get_light_offset();
  share->floor_flat = g_floor_flat;
  share->ceiling_flat = g_ceiling_flat;
  share->bill_count = g_bill_count;
  share->light_count = g_light_count;
  copy_n(share->map, g_map, MAP_CELLS / 4);
  copy_n(share->doors, g_doors, g_door_count * (sizeof(struct Door) / 4));
  copy_n(share->bills, g_bills, g_bill_count * (sizeof(struct Billboard) / 4));
  copy_n(share->lights, g_lights, g_light_count * (sizeof(struct Light) / 4));
  copy_n(share->particles, g_particles, MAX_PARTICLES * (sizeof(struct Particle) / 4));
}

/* Worker: load the published state into this process's globals. */
static void adopt(void){
  set_light_offset(share->light_offset);
  g_floor_flat = share->floor_flat;
  g_ceiling_flat = share->ceiling_flat;
  g_bill_count = share->bill_count;
  g_light_count = share->light_count;
  copy_n(g_map, share->map, MAP_CELLS / 4);
  copy_n(g_doors, share->doors, MAX_DOORS * (sizeof(struct Door) / 4));
  copy_n(g_bills, share->bills, share->bill_count * (sizeof(struct Billboard) / 4));
  copy_n(g_lights, share->lights, share->light_count * (sizeof(struct Light) / 4));
  copy_n(g_particles, share->particles, MAX_PARTICLES * (sizeof(struct Particle) / 4));
}

/* Worker process main loop: render strip `index` each time frame_seq moves.
 * Waiting spins on the shared counter and makes a syscall (a clock read, then
 * a yield or a doze) only every WORKER_POLL_SPINS polls: a syscall costs
 * about 10,000 instructions on Dioptase-OS, and a context switch empties the
 * TLB, so polling the clock every few hundred spins made idle workers spend
 * most of their time in the kernel. If the heartbeat goes stale for
 * WORKER_DEAD_FRAMES the game process is gone, so exit. */
#define WORKER_POLL_SPINS 4096
#define WORKER_DEAD_FRAMES 180     /* heartbeat silence meaning the game is gone */
#define WORKER_SPIN_FRAMES 6       /* spin this long after a frame, then doze */
static void worker_main(int index){
  request_priority(DIOPTASE_PRIORITY_HIGH);
  int last = share->frame_seq;
  unsigned last_frame_time = get_vga_frame_counter();
  int spins = 0;
  while (true){
    volatile int* seq = &share->frame_seq;
    if (*seq == last){
      if (share->quit) exit(0);
      spins += 1;
      if ((spins & (WORKER_POLL_SPINS - 1)) == 0){
        unsigned now = get_vga_frame_counter();
        volatile unsigned* beat = &share->heartbeat;
        if (now - *beat > WORKER_DEAD_FRAMES) exit(0);
        /* frames stopped coming (solo mode): stop burning the core */
        if (now - last_frame_time > WORKER_SPIN_FRAMES) sleep(1);
        else yield();
      }
      continue;
    }
    last = *seq;
    spins = 0;
    adopt();
    struct Camera cam = share->cam;
    struct ViewExtras extras = share->extras;
    render_view_strip(&cam, &extras, strip_g0[index], strip_g1[index]);
    if (share->blit) blit_strip(g_pixel_fb, g_view, strip_g0[index], strip_g1[index]);
    share->done_seq[index] = last;
    last_frame_time = get_vga_frame_counter();
  }
}

/* Set up the shared mapping and fork the workers. On any failure the game
 * keeps rendering alone. Must run after render_init(), whose tables the
 * workers inherit, and after the display is mapped. */
void parallel_start(void){
  if (SHARED_FRAME_OFFSET + sizeof(struct FrameShare) > SHARED_BYTES) return;
  int fd = open(".delve-frame");
  if (fd < 0) return;
  void* mem = mmap(SHARED_BYTES, fd, 0, MMAP_SHARED | MMAP_READ | MMAP_WRITE);
  close(fd);
  unlink(".delve-frame");
  if ((int)mem == -1) return;
  char* base = (char*)mem;
  share = (struct FrameShare*)(base + SHARED_FRAME_OFFSET);
  g_view = (unsigned short*)base;
  copy_n(base + SHARED_SEEN_OFFSET, g_seen, MAP_CELLS / 4);
  g_seen = (unsigned char*)(base + SHARED_SEEN_OFFSET);
  share->heartbeat = get_vga_frame_counter();
  for (int i = 0; i < WORKERS; ++i){
    int pid = fork();
    if (pid == 0){
      worker_main(i);
      exit(0);
    }
    if (pid < 0) break;
    worker_pid[worker_count] = pid;
    worker_count += 1;
  }
}

/* Stop using the workers (after a failure or on exit): they see `quit`. */
static void abandon_workers(void){
  if (share != NULL) share->quit = 1;
  worker_count = 0;
}

/* Render alone even though workers are up: chosen by the autotuner below,
 * or flipped by hand with '['. */
static bool solo;

/* Autotune: whether parallel rendering helps depends on how many cores the
 * machine has and on scheduling. Parallel is the default (it usually wins
 * with the emulator's default four cores), but the first real game measures:
 * it alternates TUNE_WINDOW-frame windows rendering alone and in parallel,
 * TUNE_PAIRS times (interleaving cancels out scene changes), counting
 * rendered frames per mode, and falls back to rendering alone only if
 * parallel came out more than 10% slower. */
#define TUNE_WINDOW 90
#define TUNE_PAIRS 2
static int tune_window = 2 * TUNE_PAIRS;   /* windows finished; idle until begun */
static unsigned tune_start;
static int tune_frames[2];     /* rendered frames: [0] alone, [1] parallel */

/* Start measuring (once per run of the game; later calls do nothing). */
void parallel_autotune_begin(void){
  static bool begun;
  if (begun || worker_count == 0) return;
  begun = true;
  tune_window = 0;
  tune_start = get_vga_frame_counter();
  tune_frames[0] = 0;
  tune_frames[1] = 0;
  solo = true;
}

static void autotune(void){
  if (tune_window >= 2 * TUNE_PAIRS) return;
  unsigned now = get_vga_frame_counter();
  tune_frames[solo ? 0 : 1] += 1;
  if (now - tune_start < TUNE_WINDOW) return;
  tune_window += 1;
  tune_start = now;
  if (tune_window < 2 * TUNE_PAIRS){
    solo = !solo;
  } else {
    solo = tune_frames[1] * 10 < tune_frames[0] * 9;
  }
}

void parallel_toggle(void){
  solo = !solo;
  tune_window = 2 * TUNE_PAIRS;
}

/* Render the whole view seen by `cam`, in parallel when workers are up.
 * Unless `game_overlays` is set (the caller will draw over g_view and copy it
 * to the framebuffer itself), the finished view is also copied to the
 * framebuffer, each process copying the strip it rendered. */
void parallel_render(struct Camera* cam, struct ViewExtras* extras, bool game_overlays){
  if (share != NULL) share->heartbeat = get_vga_frame_counter();
  if (worker_count > 0) autotune();
  if (worker_count == 0 || solo){
    render_view(cam, extras);
    if (!game_overlays) blit_strip(g_pixel_fb, g_view, 0, VIEW_GROUPS);
    return;
  }
  publish(cam, extras, !game_overlays);
  int seq = share->frame_seq + 1;
  share->frame_seq = seq;
  render_view_strip(cam, extras, strip_g0[WORKERS], strip_g1[WORKERS]);
  if (!game_overlays) blit_strip(g_pixel_fb, g_view, strip_g0[WORKERS], strip_g1[WORKERS]);
  unsigned start = get_vga_frame_counter();
  int spins = 0;
  while (true){
    volatile int* done = share->done_seq;
    bool all = true;
    for (int i = 0; i < worker_count; ++i){
      if (done[i] != seq) all = false;
    }
    if (all) break;
    spins += 1;
    /* check the clock and give the core away now and then (see
     * worker_main for why not more often) */
    if ((spins & 1023) == 0){
      if (get_vga_frame_counter() - start > WAIT_TIMEOUT_FRAMES){
        /* a worker died or hung: finish this frame (and all later ones) alone */
        abandon_workers();
        render_view(cam, extras);
        if (!game_overlays) blit_strip(g_pixel_fb, g_view, 0, VIEW_GROUPS);
        return;
      }
      yield();
    }
  }
}

/* Shut the workers down and reap them (normal exit). */
void parallel_stop(void){
  int count = worker_count;
  abandon_workers();
  for (int i = 0; i < count; ++i){
    wait_child(worker_pid[i]);
  }
}

/* Number of render workers in use (shown by the frame-rate overlay). */
int parallel_workers(void){
  return solo ? 0 : worker_count;
}
