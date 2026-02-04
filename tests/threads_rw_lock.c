// Reader-writer lock test.
// Purpose: validate reader concurrency and writer preference.
/*
 * Test overview:
 * - Spawn initial readers that acquire the lock and wait.
 * - Start a writer that must block while readers hold the lock.
 * - Enqueue late readers after the writer is waiting.
 * - Release initial readers and verify the writer acquires before late readers.
 * - Verify readers overlap (shared access) and writers are exclusive.
 */

#include "../kernel/rw_lock.h"
#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_INITIAL_READERS 2
#define NUM_LATE_READERS 4

static struct RwLock rwlock;
static struct Semaphore release_sem;
static struct SpinLock stats_lock;

static int initial_ready = 0;
static int active_readers = 0;
static int max_readers = 0;
static int writer_started = 0;
static int writer_inside = 0;
static int writer_done = 0;
static int writer_seq = 0;
static int late_done = 0;
static int event_seq = 0;
static int late_seq[NUM_LATE_READERS];

// Purpose: update reader counters under a stats lock.
// Inputs: delta is +1 on entry, -1 on exit.
// Preconditions: kernel mode.
// Postconditions: active_readers updated; max_readers reflects peak.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void update_reader_stats(int delta) {
  spin_lock_get(&stats_lock);
  active_readers += delta;
  if (active_readers > max_readers) {
    max_readers = active_readers;
  }
  spin_lock_release(&stats_lock);
}

// Purpose: read the current number of active readers under the stats lock.
// Inputs: none.
// Outputs: active reader count.
// Preconditions: kernel mode.
// Postconditions: none.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static int get_active_readers(void) {
  spin_lock_get(&stats_lock);
  int count = active_readers;
  spin_lock_release(&stats_lock);
  return count;
}

// Purpose: initial readers hold the lock until released by the main thread.
// Inputs: arg is unused.
// Preconditions: rwlock and release_sem initialized.
// Postconditions: increments initial_ready and holds a read slot until release.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void initial_reader(void* arg) {
  (void)arg;
  rw_lock_acquire_read(&rwlock);

  update_reader_stats(1);
  __atomic_fetch_add(&initial_ready, 1);

  sem_down(&release_sem);

  update_reader_stats(-1);
  rw_lock_release_read(&rwlock);
}

// Purpose: writer must acquire after initial readers and before late readers.
// Inputs: arg is unused.
// Preconditions: rwlock initialized.
// Postconditions: sets writer_seq when lock acquired; releases write lock.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void writer_thread(void* arg) {
  (void)arg;
  __atomic_store_n(&writer_started, 1);

  rw_lock_acquire_write(&rwlock);

  int prior = __atomic_exchange_n(&writer_inside, 1);
  if (prior != 0) {
    say("***rw_lock FAIL concurrent writers detected\n", NULL);
    panic("rw_lock test: concurrent writers\n");
  }

  if (get_active_readers() != 0) {
    say("***rw_lock FAIL writer saw active readers\n", NULL);
    panic("rw_lock test: writer acquired with active readers\n");
  }

  writer_seq = __atomic_fetch_add(&event_seq, 1) + 1;

  __atomic_store_n(&writer_inside, 0);
  rw_lock_release_write(&rwlock);
  __atomic_store_n(&writer_done, 1);
}

// Purpose: late readers must not acquire before the writer.
// Inputs: arg points to reader id.
// Preconditions: rwlock initialized; writer enqueued.
// Postconditions: records acquisition order in late_seq.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void late_reader(void* arg) {
  int id = *(int*)arg;

  rw_lock_acquire_read(&rwlock);

  update_reader_stats(1);

  if (__atomic_load_n(&writer_inside) != 0) {
    say("***rw_lock FAIL reader overlapped writer\n", NULL);
    panic("rw_lock test: reader acquired during writer\n");
  }

  late_seq[id] = __atomic_fetch_add(&event_seq, 1) + 1;

  update_reader_stats(-1);
  rw_lock_release_read(&rwlock);

  __atomic_fetch_add(&late_done, 1);
}

// Purpose: validate reader concurrency and writer preference semantics.
// Inputs: none.
// Outputs: prints pass/fail status; panics on failure.
// Preconditions: kernel mode; scheduler initialized; PIT running.
// Postconditions: late readers acquire after writer; max_readers >= 2.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
void kernel_main(void) {
  say("***rw_lock test start\n", NULL);

  rw_lock_init(&rwlock);
  sem_init(&release_sem, 0);
  spin_lock_init(&stats_lock);

  for (int i = 0; i < NUM_INITIAL_READERS; i++) {
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "rw_lock test: Fun allocation failed.\n");
    fun->func = initial_reader;
    fun->arg = NULL;
    thread(fun);
  }

  while (__atomic_load_n(&initial_ready) != NUM_INITIAL_READERS) {
    yield();
  }

  struct Fun* writer_fun = malloc(sizeof(struct Fun));
  assert(writer_fun != NULL, "rw_lock test: writer Fun allocation failed.\n");
  writer_fun->func = writer_thread;
  writer_fun->arg = NULL;
  thread(writer_fun);

  while (__atomic_load_n(&writer_started) == 0) {
    yield();
  }

  // Wait for the writer to enqueue before starting late readers.
  while (true) {
    spin_lock_get(&rwlock.lock);
    int waiting = rwlock.waiting_writers.size;
    spin_lock_release(&rwlock.lock);
    if (waiting > 0) {
      break;
    }
    yield();
  }

  for (int i = 0; i < NUM_LATE_READERS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "rw_lock test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "rw_lock test: late reader Fun allocation failed.\n");
    fun->func = late_reader;
    fun->arg = id;
    thread(fun);
  }

  for (int i = 0; i < NUM_INITIAL_READERS; i++) {
    sem_up(&release_sem);
  }

  while (__atomic_load_n(&writer_done) == 0) {
    yield();
  }

  while (__atomic_load_n(&late_done) != NUM_LATE_READERS) {
    yield();
  }

  if (max_readers < NUM_INITIAL_READERS) {
    int args[2] = { max_readers, NUM_INITIAL_READERS };
    say("***rw_lock FAIL max_readers=%d expected>=%d\n", args);
    panic("rw_lock test: readers did not overlap\n");
  }

  if (writer_seq == 0) {
    say("***rw_lock FAIL writer did not acquire\n", NULL);
    panic("rw_lock test: writer never acquired\n");
  }

  for (int i = 0; i < NUM_LATE_READERS; i++) {
    if (late_seq[i] <= writer_seq) {
      int args[3] = { i, late_seq[i], writer_seq };
      say("***rw_lock FAIL late_reader=%d seq=%d writer_seq=%d\n", args);
      panic("rw_lock test: writer preference violated\n");
    }
  }

  say("***rw_lock ok\n", NULL);
  say("***rw_lock test complete\n", NULL);
}
