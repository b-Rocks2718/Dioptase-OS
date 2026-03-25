/*
 * Shared pointer list test.
 *
 * Validates:
 * - a list built from strong next links and weak prev links tears down cleanly
 * - each node destructor can safely drop its outgoing references
 * - every node destructor runs exactly once
 *
 * How:
 * - build a NODE_COUNT-long list
 * - keep only the head/tail strong references in local scope
 * - let the scope end so the list tears down through chained destructors
 * - verify the destructor count reaches NODE_COUNT
 */

#include "../kernel/shared.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NODE_COUNT 40

struct Node {
  int id;
  struct StrongPtr next;
  struct WeakPtr prev;
};

static int destroyed = 0;

// Drop one node's links, log it, and count the destruction.
static void node_destructor(void* ptr) {
  struct Node* node = (struct Node*)ptr;

  if (node->next.refcount != NULL) {
    strongptr_drop(&node->next);
  }

  if (node->prev.refcount != NULL) {
    weakptr_drop(&node->prev);
  }

  int args[1] = { node->id };
  say("***shared_ptr_list free id=%d\n", args);
  free(node);
  __atomic_fetch_add(&destroyed, 1);
}

// Initialize one node before it is linked into the list.
static void node_init(struct Node* node, int id) {
  node->id = id;
  node->next.refcount = NULL;
  node->prev.refcount = NULL;
}

// Build the list, drop the roots, and verify every node gets destroyed.
void kernel_main(void) {
  say("***shared_ptr_list test start\n", NULL);

  {
    struct Node* head_node = malloc(sizeof(struct Node));
    node_init(head_node, 0);

    __attribute__((cleanup(strongptr_drop))) struct StrongPtr head;
    strongptr_init(&head, head_node, node_destructor);

    __attribute__((cleanup(strongptr_drop))) struct StrongPtr tail = strongptr_clone(&head);

    // Chain each new node onto the tail with strong-next / weak-prev links.
    for (int i = 1; i < NODE_COUNT; ++i) {
      struct Node* n = malloc(sizeof(struct Node));
      node_init(n, i);

      __attribute__((cleanup(strongptr_drop))) struct StrongPtr s_ptr;
      strongptr_init(&s_ptr, n, node_destructor);

      ((struct Node*)strongptr_deref(&tail))->next = strongptr_clone(&s_ptr);
      weakptr_init(&n->prev, &tail);
      
      strongptr_assign(&tail, &s_ptr);
    }

    strongptr_drop(&tail);
  } // Head drops here and should tear down the whole list.

  assert(destroyed == NODE_COUNT, "Not all nodes were destroyed\n");

  say("***shared_ptr_list ok\n", NULL);
  say("***shared_ptr_list test complete\n", NULL);
}
