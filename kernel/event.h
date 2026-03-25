#ifndef EVENT_H
#define EVENT_H

#include "constants.h"
#include "cond_var.h"
#include "blocking_lock.h"

// broadcast-style event, wakes all waiters when signaled
struct Event {
  struct BlockingLock lock;
  struct CondVar cv;
  unsigned generation; // used to prevent spurious wakeups, incremented each time event is signaled
};

// initialize an unsignaled event
void event_init(struct Event* event);

// wait for the next signal after this call starts waiting
void event_wait(struct Event* event);

// signal the event, waking all current waiters
void event_signal(struct Event* event);

// destroy the event and reap any waiters
void event_destroy(struct Event* event);

// destroy the event and free its memory
void event_free(struct Event* event);

#endif // EVENT_H
