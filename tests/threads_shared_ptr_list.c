// Shared pointer list test.
// Purpose: validate strong/weak list links and destructor order visibility.

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

// Purpose: destructor for Node that drops its links and logs.
// Inputs: ptr points to a Node.
// Preconditions: ptr is non-NULL.
// Postconditions: next strong and prev weak references dropped; node freed.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
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

// Purpose: initialize a Node with empty links.
// Inputs: node points to storage; id is the node id.
// Preconditions: node is non-NULL.
// Postconditions: next/prev links are empty.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void node_init(struct Node* node, int id) {
  node->id = id;
  node->next.refcount = NULL;
  node->prev.refcount = NULL;
}

void kernel_main(void) {
  say("***shared_ptr_list test start\n", NULL);

  {
    struct Node* head_node = malloc(sizeof(struct Node));
    node_init(head_node, 0);

    __attribute__((cleanup(strongptr_drop))) struct StrongPtr head;
    strongptr_init(&head, head_node, node_destructor);

    __attribute__((cleanup(strongptr_drop))) struct StrongPtr tail = strongptr_clone(&head);

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
  } // head dropped automatically here

  assert(destroyed == NODE_COUNT, "Not all nodes were destroyed\n");

  say("***shared_ptr_list ok\n", NULL);
  say("***shared_ptr_list test complete\n", NULL);
}
