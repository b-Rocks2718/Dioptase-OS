#ifndef EVENT_H
#define EVENT_H

#include "constants.h"
#include "cond_var.h"
#include "blocking_lock.h"

struct Event {
  struct BlockingLock lock;
  struct CondVar cv;
  unsigned generation; // used to prevent spurious wakeups, incremented each time event is signaled
};

void event_init(struct Event* event);

// wait until event is signaled, then return
void event_wait(struct Event* event);

// signal the event, waking all waiters. If there are no waiters, this has no effect
void event_signal(struct Event* event);

void event_destroy(struct Event* event);

void event_free(struct Event* event);

#endif // EVENT_H