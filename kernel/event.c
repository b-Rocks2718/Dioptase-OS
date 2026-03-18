#include "event.h"
#include "heap.h"

void event_init(struct Event* event){
  blocking_lock_init(&event->lock);
  cond_var_init(&event->cv);
  event->generation = 0;
}

// wait until event is signaled, then return
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

// signal the event, waking all waiters. If there are no waiters, this has no effect
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
