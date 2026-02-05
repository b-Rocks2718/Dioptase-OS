#include "shared.h"
#include "atomic.h"
#include "machine.h"
#include "debug.h"
#include "constants.h"
#include "heap.h"

// port of Gheith kernel implementation

void refcount_init(struct RefCount* refcount, void* ptr, void (*destructor)(void*)){
  refcount->ptr = ptr;
  refcount->strong_count = 1;
  refcount->weak_count = 0;
  refcount->destructor = destructor;
  spin_lock_init(&refcount->lock);
}

struct RefCount* refcount_add_strong(struct RefCount* refcount){
  if (refcount != NULL){
    spin_lock_acquire(&refcount->lock);
    if (refcount->ptr != NULL) {
      refcount->strong_count++;
    }
    spin_lock_release(&refcount->lock);
  }
  return refcount;
}

struct RefCount* refcount_drop_strong(struct RefCount* refcount){
  if (refcount != NULL) {
    spin_lock_acquire(&refcount->lock);
    assert(refcount->strong_count > 0, "refcount strong_count underflow");
    assert(refcount->ptr != NULL, "refcount ptr is NULL in drop_strong");
    refcount->strong_count--;
   
    bool delete_ptr = false;
    bool delete_obj = false;

    if (refcount->strong_count == 0) {
      delete_obj = true;
      if (refcount->weak_count == 0) {
        delete_ptr = true;
      }
    }
    spin_lock_release(&refcount->lock);

    assert((!delete_ptr) || delete_obj, "delete_ptr without delete_obj");

    if (delete_obj) {
      assert(refcount->ptr != NULL, "refcount ptr is NULL when deleting object");
      refcount->destructor(refcount->ptr);
      refcount->ptr = NULL;
    }

    if (delete_ptr) {
      // free the refcount structure itself
      free(refcount);
      refcount = NULL;
      return NULL;
    }
  }

  return refcount;
}

struct RefCount* refcount_add_weak(struct RefCount* refcount){
  if (refcount != NULL) {
    spin_lock_acquire(&refcount->lock);
    if (refcount->ptr != NULL) {
      refcount->weak_count++;
    }
    spin_lock_release(&refcount->lock);
  }
  return refcount;
}

struct RefCount* refcount_drop_weak(struct RefCount* refcount){
  if (refcount != NULL) {
    spin_lock_acquire(&refcount->lock);
    assert(refcount->weak_count > 0, "refcount weak_count underflow");
    refcount->weak_count--;

    bool delete_ptr = (refcount->weak_count == 0 && refcount->strong_count == 0);

    spin_lock_release(&refcount->lock);

    if (delete_ptr) {
      // free the refcount structure itself
      free(refcount);
      refcount = NULL;
      return NULL;
    }
  }
  return refcount;
}

struct RefCount* refcount_promote_strong(struct RefCount* refcount){
  if (refcount != NULL) {
    spin_lock_acquire(&refcount->lock);
    assert(refcount->weak_count > 0, "refcount weak_count underflow in promote_strong");
    if (refcount->strong_count > 0) {
      refcount->strong_count++;
      spin_lock_release(&refcount->lock);
      return refcount;
    } else {
      spin_lock_release(&refcount->lock);
      return NULL;
    }
  }
  return refcount;
}


void strongptr_init(struct StrongPtr* sptr, void* ptr, void (*destructor)(void*)){

}

struct StrongPtr strongptr_clone(struct StrongPtr* sptr){

}

bool strongptr_not_null(struct StrongPtr* sptr){

}

bool strongptr_compare(struct StrongPtr* sptr1, struct StrongPtr* sptr2){

}

void* strongptr_deref(struct StrongPtr* sptr){

}

void strongptr_drop(struct StrongPtr* sptr){

}


void weakptr_init(struct WeakPtr* wptr, struct StrongPtr* sptr){

}

struct WeakPtr weakptr_clone(struct WeakPtr* wptr){

}

struct StrongPtr weakptr_promote(struct WeakPtr* wptr){

}

void weakptr_drop(struct WeakPtr* wptr){

}
