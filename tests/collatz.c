/*
 * Collatz sequence test.
 *
 * Validates:
 * - the basic list-building helpers can build, print, and free a simple heap
 *   owned integer sequence
 * - the interactive and non-interactive test paths both generate the expected
 *   Collatz walk from a starting value
 *
 * How:
 * - convert either the typed VGA input or a fixed fallback value into a start
 *   integer
 * - build the Collatz sequence as a linked list of heap-allocated nodes
 * - print the resulting list, then free every node
 */

#include "../kernel/machine.h"
#include "../kernel/print.h"
#include "../kernel/heap.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/threads.h"
#include "../kernel/pit.h"
#include "../kernel/config.h"
#include "../kernel/ps2.h"
#include "../kernel/debug.h"

struct IntList {
  struct IntList* next;
  int val;
};

// Allocate one linked-list node for one sequence value.
struct IntList* make_int_list(int x){
  struct IntList* node = malloc(sizeof(struct IntList));

  node->next = NULL;
  node->val = x;

  return node;
}

// Append one node to the tail of the integer list.
void int_list_append(struct IntList** head, struct IntList** tail, struct IntList* node){
  if (*head == NULL){
    *head = node;
    *tail = node;
    return;
  } else {
    (*tail)->next = node;
    *tail = node;
  }
}

// Print the full integer list while holding the shared print lock.
void print_int_list(struct IntList* seq){
  preempt_spin_lock_acquire(&print_lock);
  int old_color = text_color;
  text_color = 0x1F;
  puts("***");
  while (seq) {
    struct IntList* next = seq->next;
    print_signed(seq->val);
    putchar(',');
    seq = next;
  }
  putchar('\n');
  text_color = old_color;
  preempt_spin_lock_release(&print_lock);
}

// Free every node in the integer list.
void free_int_list(struct IntList* seq){
  while (seq) {
    struct IntList* next = seq->next;
    free(seq);
    seq = next;
  }
}

// Compute the next Collatz value for one positive integer.
unsigned next_collatz(unsigned x){
  return (x & 1) ? (3 * x + 1) : (x / 2);
}

// Build the full Collatz sequence from x down to 1.
struct IntList* collatz_seq(unsigned x){
  struct IntList* head = 0;
  struct IntList* tail = 0;
  
  while (x != 1){
    // Append each intermediate value before advancing to the next one.
    struct IntList* node = make_int_list(x);

    int_list_append(&head, &tail, node);

    x = next_collatz(x);
  }

  struct IntList* node = make_int_list(x);
  int_list_append(&head, &tail, node);

  return head;
}

// Read one start value, print its Collatz sequence, and free the list.
int kernel_main(void) {
  if (CONFIG.use_vga){
    say_color("***Enter a number: ", NULL, 0x1C);

    int key = 0;
    int num = 0;
    while (true){
      key = waitkey();
      if ((key & 0xFF) == '\r') break;

      if ((key & 0xFF00) == 0) {
        // Only accept printable digit keys in the VGA input path.
        if (!isnum(key)) panic("key was not a number\n");
        num = num * 10 + (key - '0');
        putchar_color(key, 0x1F);
      }
    }

    // Drain any leftover buffered key events before printing the result.
    while (getkey() != 0);

    putchar('\n');

    say_color("***Collatz sequence:\n", NULL, 0x1C);
    
    struct IntList* seq = collatz_seq(num);
   
    print_int_list(seq);

    say_color("***Collatz sequence done\n", NULL, 0x1C);
   
    free_int_list(seq);
  } else {
    // In non-VGA runs, use one deterministic input so the test stays automatic.
    struct IntList* seq = collatz_seq(67);

    say("***Collatz sequence:\n", NULL);
    print_int_list(seq);
    say("***Collatz sequence done\n", NULL);

    free_int_list(seq);
  }

  return 67;
}
