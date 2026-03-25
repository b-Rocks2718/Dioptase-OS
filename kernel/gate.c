#include "gate.h"
#include "heap.h"

// initialize lock, condition variable, and signaled state
void gate_init(struct Gate* gate) {
  gate->signaled = false;
  blocking_lock_init(&gate->lock);
  cond_var_init(&gate->cv);
}

// wait until gate is signaled, then return. If gate is already signaled, returns immediately.
void gate_wait(struct Gate *gate) {
  blocking_lock_acquire(&gate->lock);
  while (!gate->signaled) {
    cond_var_wait(&gate->cv, &gate->lock);
  }
  blocking_lock_release(&gate->lock);
}

// signal the gate, waking all waiters. If the gate is already signaled, this has no effect
void gate_signal(struct Gate *gate) {
  blocking_lock_acquire(&gate->lock);
  gate->signaled = true;
  cond_var_broadcast(&gate->cv, &gate->lock);
  blocking_lock_release(&gate->lock);
}

// reset the gate to the non-signaled state. If the gate is already non-signaled, this has no effect
void gate_reset(struct Gate *gate) {
  blocking_lock_acquire(&gate->lock);
  gate->signaled = false;
  blocking_lock_release(&gate->lock);
}

// Free the resources associated with the gate, but do not free the gate itself
// waiting threads will be reaped
void gate_destroy(struct Gate* gate) {
  cond_var_destroy(&gate->cv);
}

// Free the resources associated with the gate and the gate itself
// waiting threads will be reaped
void gate_free(struct Gate* gate) {
  gate_destroy(gate);
  free(gate);
}
