#include "promise.h"
#include "heap.h"

// port of Gheith kernel implementation

// initialize the promise in the unset state
void promise_init(struct Promise* promise){
  sem_init(&promise->sem, 0);
  promise->value = NULL;
}

// publish a value and open the promise
void promise_set(struct Promise* promise, void* value){
  promise->value = value;
  sem_up(&promise->sem);
}

// once the promise is open, re-open it after reading so later getters also pass
void* promise_get(struct Promise* promise){
  sem_down(&promise->sem);
  void* tmp = promise->value;
  sem_up(&promise->sem);
  return tmp;
}

void promise_destroy(struct Promise* promise){
  sem_destroy(&promise->sem);
}

void promise_free(struct Promise* promise){
  promise_destroy(promise);
  free(promise);
}
