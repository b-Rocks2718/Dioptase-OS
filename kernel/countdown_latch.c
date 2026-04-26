#include "countdown_latch.h"
#include "atomic.h"

void countdownlatch_init(struct CountDownLatch* latch, unsigned count) {
  latch->count = count;
}

void countdownlatch_sync(struct CountDownLatch* latch) {
  while (latch->count > 0) {
    yield();
  }
}

void countdownlatch_down(struct CountDownLatch* latch) {
  __atomic_fetch_add(&latch->count, -1);
}

void countdownlatch_up(struct CountDownLatch* latch) {
  __atomic_fetch_add(&latch->count, 1);
}
