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
  spin_lock_acquire(&print_lock);
  puts("***");
  while (seq) {
    struct IntList* next = seq->next;
    print_signed(seq->val);
    putchar(',');
    seq = next;
  }
  putchar('\n');
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
  if (CONFIG.use_vga){
    int old_color = __atomic_exchange_n(&text_color, 0x1C);
    say("***Enter a number: ", NULL);
    __atomic_store_n(&text_color, 0x1F);

    int key = 0;
    int num = 0;
    while (true){
      key = getkey();
      if ((key & 0xFF) == '\r') break;

      if (key != 0 && ((key & 0xFF00) == 0)) {
        if (!isnum(key)) panic("key was not a number\n");
        num = num * 10 + (key - '0');
        putchar(key);
      }
    }

    // empty buffer
    while (getkey() != 0);

    putchar('\n');

    __atomic_store_n(&text_color, 0x1C);
    say("***Collatz sequence:\n", NULL);
    __atomic_store_n(&text_color, 0xFF);

    struct IntList* seq = collatz_seq(num);
    
    __atomic_store_n(&text_color, 0x1F);
    print_int_list(seq);
    __atomic_store_n(&text_color, 0x1C);

    say("***Collatz sequence done\n", NULL);
    __atomic_store_n(&text_color, old_color);
  
    free_int_list(seq);
  } else {
    // just use 67
    struct IntList* seq = collatz_seq(67);

    say("***Collatz sequence:\n", NULL);
    print_int_list(seq);
    say("***Collatz sequence done\n", NULL);

    free_int_list(seq);
  }

  return 67;
}
