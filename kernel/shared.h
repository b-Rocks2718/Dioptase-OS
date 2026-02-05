#ifndef SHARED_H
#define SHARED_H

#include "atomic.h"

// port of Gheith kernel implementation

struct RefCount {
  void* ptr;
  unsigned strong_count;
  unsigned weak_count;
  void (*destructor)(void*);
  struct SpinLock lock;
};

struct StrongPtr {
  struct RefCount* refcount;
};

struct WeakPtr {
  struct RefCount* refcount;
};

void refcount_init(struct RefCount* refcount, void* ptr, void (*destructor)(void*));

struct RefCount* refcount_add_strong(struct RefCount* refcount);

struct RefCount* refcount_drop_strong(struct RefCount* refcount);

struct RefCount* refcount_add_weak(struct RefCount* refcount);

struct RefCount* refcount_drop_weak(struct RefCount* refcount);

struct RefCount* refcount_promote_strong(struct RefCount* refcount);


void strongptr_init(struct StrongPtr* sptr, void* ptr, void (*destructor)(void*));

struct StrongPtr strongptr_clone(struct StrongPtr* sptr);

void strongptr_assign(struct StrongPtr* dest, struct StrongPtr* src);

bool strongptr_not_null(struct StrongPtr* sptr);

bool strongptr_compare(struct StrongPtr* sptr1, struct StrongPtr* sptr2);

void* strongptr_deref(struct StrongPtr* sptr);

void strongptr_drop(struct StrongPtr* sptr);


void weakptr_init(struct WeakPtr* wptr, struct StrongPtr* sptr);

struct WeakPtr weakptr_clone(struct WeakPtr* wptr);

struct StrongPtr weakptr_promote(struct WeakPtr* wptr);

void weakptr_drop(struct WeakPtr* wptr);

#endif // SHARED_H