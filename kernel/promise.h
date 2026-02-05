#ifndef PROMISE_H
#define PROMISE_H

#include "semaphore.h"

// port of Gheith kernel implementation

struct Promise {
  struct Semaphore sem;
  void* value;
};

void promise_init(struct Promise* promise);

void promise_set(struct Promise* promise, void* value);

void* promise_get(struct Promise* promise);

void promise_destroy(struct Promise* promise);

void promise_free(struct Promise* promise);

#endif // PROMISE_H