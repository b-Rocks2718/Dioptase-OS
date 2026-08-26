#ifndef PROMISE_H
#define PROMISE_H

#include "semaphore.h"

// Promise blocks getters until it has been set at least once
struct Promise {
  struct Semaphore sem;
  void* value;
};

// initialize an unset promise
void promise_init(struct Promise* promise);

// store a value and wake waiting getters
void promise_set(struct Promise* promise, void* value);

// return the current value, blocking until the promise has been set
void* promise_get(struct Promise* promise);

// destroy the promise after all getters/setters have returned
void promise_destroy(struct Promise* promise);

// destroy the promise and free its memory
void promise_free(struct Promise* promise);

#endif // PROMISE_H
