/*
 * User-mode verification of RETAKE's shipping save codec and real file traps.
 * A tiny immutable room replaces the art pack only; the codec uses the normal
 * CRT, guest compiler, filesystem, checksums and validation unchanged. A
 * truncated newest slot must fall back to the prior checkpoint, and rejected
 * loads must not overwrite live progress. No concurrent writers participate.
 */
#include "../../../root/crt/sys.h"
#include "../../../root/crt/print.h"
#include "../../../root/retake/retake.h"
#include "../../../root/retake/save.h"

static unsigned char collision[1200];
unsigned char* g_pak;
static struct PakRoom fixture;
static struct Progress progress;
static struct Progress loaded;
static int failures;

/* The fixture has one 320x240 room with a clear checkpoint above its floor. */
int pak_room_count(void) { return 1; }

/* Save validation only asks for the fixture's bounded room index. */
struct PakRoom* pak_room(int index) {
  if (index != 0) failures++;
  return &fixture;
}

/* Guest diagnostics name the failing recovery behavior without needing printf. */
static void expect(int ok, char* why) {
  if (!ok) {
    failures++;
    puts("***FAIL retake save: ");
    puts(why);
    puts("\n");
  }
}

/* Run real slot creation, replacement, corruption recovery and validation. */
int main(void) {
  g_pak = collision;
  fixture.w = 40;
  fixture.h = 30;
  fixture.col_off = 0;
  fixture.entity_count = 0;
  fixture.entities_off = 0;
  for (int x = 0; x < 40; x++) collision[800 + x] = COL_SOLID;
  progress.max_health = 5;
  progress.scrip = 17;
  progress.take_slots = 1;
  progress.take_frames = TAKE_BASE_FRAMES;
  progress.save_room = 0;
  progress.save_x = 40;
  progress.save_y = 148;
  expect(progress_save(&progress) == 0, "first slot creation and readback succeed");
  expect(progress_load(&loaded) == 0 && loaded.scrip == 17 && loaded.save_y == 148,
         "the first checkpoint round-trips through real file syscalls");
  progress.scrip = 23;
  expect(progress_save(&progress) == 0, "second slot leaves the first intact");
  expect(progress_load(&loaded) == 0 && loaded.scrip == 23, "the newer slot is selected");
  int fd = open_existing("/retake/save1.dat");
  expect(fd >= 0, "the second slot exists after successful save");
  if (fd >= 0) {
    expect(truncate(fd, 17) == 0, "the fixture can truncate the newer slot");
    close(fd);
  }
  expect(progress_load(&loaded) == 0 && loaded.scrip == 17,
         "a truncated newest slot recovers the earlier checkpoint");
  progress.save_x = -1;
  expect(progress_save(&progress) == -1, "an invalid checkpoint cannot replace saved progress");
  fd = open_existing("/retake/save0.dat");
  if (fd >= 0) { truncate(fd, 0); close(fd); }
  loaded.scrip = 91;
  expect(progress_load(&loaded) == -1 && loaded.scrip == 91,
         "two invalid slots leave live output untouched");
  if (!failures) puts("***PASS retake save round-trip, corrupted-slot recovery and validation\n");
  return failures != 0;
}
