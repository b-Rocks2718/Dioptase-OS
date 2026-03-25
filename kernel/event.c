#include "event.h"
#include "heap.h"

// initialize lock, condition variable, and generation counter
void event_init(struct Event* event){
  blocking_lock_init(&event->lock);
  cond_var_init(&event->cv);
  event->generation = 0;
}

// wait for generation to advance so only signals after this call count
void event_wait(struct Event* event){
  blocking_lock_acquire(&event->lock);
  unsigned gen = event->generation;
  while (gen == event->generation) {
    // prevent spurious wakeups by checking generation in a loop
    // implicitly assumes we don't get 2 billion signals before we have time to wake up
    cond_var_wait(&event->cv, &event->lock);
  }
  blocking_lock_release(&event->lock);
}

// advance the generation and wake all current waiters
void event_signal(struct Event* event){
  blocking_lock_acquire(&event->lock);
  event->generation++;
  cond_var_broadcast(&event->cv, &event->lock);
  blocking_lock_release(&event->lock);
}

void event_destroy(struct Event* event){
  cond_var_destroy(&event->cv);
  blocking_lock_destroy(&event->lock);
}

void event_free(struct Event* event){
  event_destroy(event);
  free(event);
}
