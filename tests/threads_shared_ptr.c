/*
 * Shared pointer test.
 *
 * Validates:
 * - strong references keep the payload alive until the last one drops
 * - weak promotion fails after the payload has been destroyed
 * - the payload destructor runs exactly once
 *
 * How:
 * - create one strong root, clone it, and create one weak pointer
 * - drop the strong references one at a time
 * - check the destructor count before and after the final drop
 * - verify weakptr_promote() returns NULL after destruction
 */

#include "../kernel/shared.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

struct Payload {
  int value;
};

static int destroyed = 0;

// Drop a scoped StrongPtr automatically at scope exit.
static void strongptr_cleanup(struct StrongPtr* sptr) {
  if (sptr != NULL && sptr->refcount != NULL) {
    strongptr_drop(sptr);
  }
}

// Drop a scoped WeakPtr automatically at scope exit.
static void weakptr_cleanup(struct WeakPtr* wptr) {
  if (wptr != NULL && wptr->refcount != NULL) {
    weakptr_drop(wptr);
  }
}

// Free the payload and record that its destructor ran.
static void payload_destructor(void* ptr) {
  struct Payload* payload = (struct Payload*)ptr;
  free(payload);
  __atomic_fetch_add(&destroyed, 1);
}

// Walk one strong/weak lifetime sequence and verify the expected transitions.
void kernel_main(void) {
  say("***shared_ptr test start\n", NULL);

  struct Payload* payload = malloc(sizeof(struct Payload));
  assert(payload != NULL, "shared_ptr test: payload allocation failed.\n");
  payload->value = 0xABCD;

  struct StrongPtr sptr __attribute__((cleanup(strongptr_cleanup)));
  strongptr_init(&sptr, payload, payload_destructor);
  assert(strongptr_not_null(&sptr), "shared_ptr test: strongptr is NULL after init.\n");

  if (*(int*)strongptr_deref(&sptr) != 0xABCD) {
    int args[2] = { *(int*)strongptr_deref(&sptr), 0xABCD };
    say("***shared_ptr FAIL value=%d expected=%d\n", args);
    panic("shared_ptr test: deref value mismatch\n");
  }

  struct StrongPtr sptr2 __attribute__((cleanup(strongptr_cleanup))) = strongptr_clone(&sptr);

  struct WeakPtr wptr __attribute__((cleanup(weakptr_cleanup)));
  weakptr_init(&wptr, &sptr);

  // The first drop should leave the payload alive because one strong ref remains.
  strongptr_drop(&sptr);
  if (__atomic_load_n(&destroyed) != 0) {
    int args[2] = { __atomic_load_n(&destroyed), 0 };
    say("***shared_ptr FAIL destroyed=%d expected=%d\n", args);
    panic("shared_ptr test: destructor ran too early\n");
  }

  // The second drop should run the destructor exactly once.
  strongptr_drop(&sptr2);
  if (__atomic_load_n(&destroyed) != 1) {
    int args[2] = { __atomic_load_n(&destroyed), 1 };
    say("***shared_ptr FAIL destroyed=%d expected=%d\n", args);
    panic("shared_ptr test: destructor did not run\n");
  }

  // After destruction, weak promotion must fail.
  struct StrongPtr promoted = weakptr_promote(&wptr);
  if (strongptr_not_null(&promoted)) {
    say("***shared_ptr FAIL weak promoted after destruction\n", NULL);
    panic("shared_ptr test: weakptr promoted after destruction\n");
  }

  say("***shared_ptr ok\n", NULL);
  say("***shared_ptr test complete\n", NULL);
}
