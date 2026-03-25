#ifndef SHARED_H
#define SHARED_H

#include "atomic.h"

// port of Gheith kernel implementation

// refcount control block shared by StrongPtr and WeakPtr
struct RefCount {
  void* ptr; // owned object; NULL after the last strong reference destroys it
  unsigned strong_count; // number of strong owners keeping ptr alive
  unsigned weak_count; // number of weak observers keeping the control block alive
  void (*destructor)(void*); // called when strong_count reaches 0
  struct SpinLock lock; // serializes count updates and weak-to-strong promotion
};

// owning reference to a refcounted object
struct StrongPtr {
  struct RefCount* refcount;
};

// non-owning reference to a refcounted object
struct WeakPtr {
  struct RefCount* refcount;
};

// initialize a control block with one strong reference
void refcount_init(struct RefCount* refcount, void* ptr, void (*destructor)(void*));

// increment the strong reference count if the object is still alive
struct RefCount* refcount_add_strong(struct RefCount* refcount);

// decrement the strong reference count and destroy the object at 0
struct RefCount* refcount_drop_strong(struct RefCount* refcount);

// increment the weak reference count if the object is still alive
struct RefCount* refcount_add_weak(struct RefCount* refcount);

// decrement the weak reference count and free the control block if needed
struct RefCount* refcount_drop_weak(struct RefCount* refcount);

// promote a weak reference to strong if the object is still alive
struct RefCount* refcount_promote_strong(struct RefCount* refcount);


// initialize a StrongPtr from a raw pointer
void strongptr_init(struct StrongPtr* sptr, void* ptr, void (*destructor)(void*));

// return another StrongPtr to the same object
struct StrongPtr strongptr_clone(struct StrongPtr* sptr);

// replace dest with src while updating reference counts
void strongptr_assign(struct StrongPtr* dest, struct StrongPtr* src);

// report whether this StrongPtr currently owns a live object
bool strongptr_not_null(struct StrongPtr* sptr);

// report whether two StrongPtrs share the same control block
bool strongptr_compare(struct StrongPtr* sptr1, struct StrongPtr* sptr2);

// return the owned object; asserts if the StrongPtr is empty
void* strongptr_deref(struct StrongPtr* sptr);

// drop one StrongPtr reference
void strongptr_drop(struct StrongPtr* sptr);


// initialize a WeakPtr from a StrongPtr
void weakptr_init(struct WeakPtr* wptr, struct StrongPtr* sptr);

// return another WeakPtr to the same control block
struct WeakPtr weakptr_clone(struct WeakPtr* wptr);

// attempt to promote a WeakPtr to StrongPtr
struct StrongPtr weakptr_promote(struct WeakPtr* wptr);

// drop one WeakPtr reference
void weakptr_drop(struct WeakPtr* wptr);

#endif // SHARED_H
