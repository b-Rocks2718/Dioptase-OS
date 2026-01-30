#include "kernel/machine.h"
#include "kernel/print.h"
#include "kernel/heap.h"
#include "kernel/constants.h"
#include "kernel/atomic.h"

#define NULL 0

struct IntList {
  struct IntList* next;
  int val;
};

struct IntList* make_int_list(int x){
  struct IntList* node = malloc(sizeof(struct IntList));

  node->next = NULL;
  node->val = x;

  return node;
}

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

void print_int_list(struct IntList* seq){
  spin_lock_get(&print_lock);
  puts("***");
  while (seq) {
    struct IntList* next = seq->next;
    print_signed(seq->val);
    putchar(',');
    seq = next;
  }
  spin_lock_release(&print_lock);
}

void free_int_list(struct IntList* seq){
  while (seq) {
    int args[2] = {seq->val, (int)seq};
    struct IntList* next = seq->next;
    free(seq);
    seq = next;
  }
}

unsigned next_collatz(unsigned x){
  return (x & 1) ? (3 * x + 1) : (x / 2);
}

struct IntList* collatz_seq(unsigned x){
  struct IntList* head = 0;
  struct IntList* tail = 0;
  
  while (x != 1){
    struct IntList* node = make_int_list(x);

    int_list_append(&head, &tail, node);

    x = next_collatz(x);
  }

  struct IntList* node = make_int_list(x);
  int_list_append(&head, &tail, node);

  return head;
}

int kernel_main(void) {
  struct IntList* seq = collatz_seq(67);

  say("***Collatz sequence:\n", NULL);
  print_int_list(seq);
  putchar('\n');

  free_int_list(seq);

  return 67;
}
