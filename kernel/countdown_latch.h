struct CountDownLatch {
  unsigned count;
};

// initialize the barrier with the given count of threads
void countdownlatch_init(struct CountDownLatch* latch, unsigned count);

// block until the count reaches 0
void countdownlatch_sync(struct CountDownLatch* latch);

// Non blocking; increment or decrement the count. If the count reaches 0, all waiting threads will be woken.
void countdownlatch_down(struct CountDownLatch* latch);
void countdownlatch_up(struct CountDownLatch* latch);

// Destroying the latch while threads are still waiting on it causes undefined behavior