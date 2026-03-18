#ifndef GATE_H
#define GATE_H

#include "constants.h"
#include "cond_var.h"
#include "blocking_lock.h"

struct Gate {
  bool signaled;
  struct BlockingLock lock;
  struct CondVar cv;
};

void gate_init(struct Gate* gate);

// wait until gate is signaled, then return. If gate is already signaled, returns immediately
void gate_wait(struct Gate *gate);

// signal the gate, waking all waiters. If the gate is already signaled, this has no effect
void gate_signal(struct Gate *gate);

// reset the gate to the non-signaled state. If the gate is already non-signaled, this has no effect
void gate_reset(struct Gate *gate);

void gate_destroy(struct Gate* gate);

void gate_free(struct Gate* gate);

#endif // GATE_H
