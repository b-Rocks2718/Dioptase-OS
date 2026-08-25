#ifndef GATE_H
#define GATE_H

#include "constants.h"
#include "cond_var.h"
#include "blocking_lock.h"

// latch-style event that stays signaled until reset
struct Gate {
  bool signaled; // true once signaled, until gate_reset()
  struct BlockingLock lock;
  struct CondVar cv;
};

// initialize a non-signaled gate
void gate_init(struct Gate* gate);

// wait until gate is signaled, then return. If gate is already signaled, returns immediately
void gate_wait(struct Gate *gate);

// signal the gate, waking all waiters. If the gate is already signaled, this has no effect
void gate_signal(struct Gate *gate);

// reset the gate to the non-signaled state. If the gate is already non-signaled, this has no effect
void gate_reset(struct Gate *gate);

// Free synchronization resources after all waiters/signallers have returned,
// but do not free the gate itself.
void gate_destroy(struct Gate* gate);

// Destroy a quiescent gate and free it.
void gate_free(struct Gate* gate);

#endif // GATE_H
