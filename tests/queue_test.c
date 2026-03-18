/*
 * Queue data structure test summary:
 * - Verifies FIFO queue and spin queue operations, including remove_all and the
 *   stale-next clearing required when reusing detached nodes.
 * - Verifies sleep queue ordering, equal-deadline stability, and readiness
 *   checks against current_jiffies.
 * - Verifies generic queue, generic spin queue, and ring buffer operations,
 *   including wrap-around and full detection.
 */

#include "../kernel/queue.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/pit.h"
#include "../kernel/interrupts.h"

struct TestElement {
  struct GenericQueueElement link;
  int id;
};

static void fail_ptr(char* msg, void* got, void* expected) {
  int args[2] = { (int)got, (int)expected };
  say("***queue FAIL got=0x%X expected=0x%X\n", args);
  panic(msg);
}

static void fail_uint(char* msg, unsigned got, unsigned expected) {
  int args[2] = { (int)got, (int)expected };
  say("***queue FAIL got=%u expected=%u\n", args);
  panic(msg);
}

static void fail_bool(char* msg, bool got, bool expected) {
  int args[2] = { got, expected };
  say("***queue FAIL got=%d expected=%d\n", args);
  panic(msg);
}

static void init_tcb(struct TCB* tcb, unsigned wakeup_jiffies, struct TCB* stale_next) {
  tcb->wakeup_jiffies = wakeup_jiffies;
  tcb->next = stale_next;
}

static void init_element(struct TestElement* element, int id, struct TestElement* stale_next) {
  element->id = id;
  if (stale_next == NULL) {
    element->link.next = NULL;
  } else {
    element->link.next = &stale_next->link;
  }
}

static void expect_tcb(struct TCB* got, struct TCB* expected, char* msg) {
  if (got != expected) {
    fail_ptr(msg, got, expected);
  }
}

static void expect_ptr(void* got, void* expected, char* msg) {
  if (got != expected) {
    fail_ptr(msg, got, expected);
  }
}

static void expect_element(struct GenericQueueElement* got, struct GenericQueueElement* expected, char* msg) {
  if (got != expected) {
    fail_ptr(msg, got, expected);
  }
}

static void expect_uint(unsigned got, unsigned expected, char* msg) {
  if (got != expected) {
    fail_uint(msg, got, expected);
  }
}

static void expect_bool(bool got, bool expected, char* msg) {
  if (got != expected) {
    fail_bool(msg, got, expected);
  }
}

static void add_sleep_node(struct SleepQueue* queue, struct TCB* node) {
  int args[2] = { (int)queue, (int)node };
  sleep_queue_add(args);
}

static struct TCB* remove_sleep_node_at(struct SleepQueue* queue, unsigned jiffies) {
  unsigned was = interrupts_disable();
  current_jiffies = jiffies;
  struct TCB* node = sleep_queue_remove(queue);
  interrupts_restore(was);
  return node;
}

static void test_queue(void) {
  struct Queue queue;
  struct TCB a;
  struct TCB b;
  struct TCB c;
  struct TCB extra;

  queue_init(&queue);
  expect_uint(queue_size(&queue), 0, "queue test: queue_init size mismatch\n");
  expect_tcb(queue_peek(&queue), NULL, "queue test: empty queue peek mismatch\n");
  expect_tcb(queue_remove(&queue), NULL, "queue test: empty queue remove mismatch\n");
  expect_tcb(queue_remove_all(&queue), NULL, "queue test: empty queue remove_all mismatch\n");

  init_tcb(&extra, 0, NULL);
  init_tcb(&a, 0, NULL);
  init_tcb(&b, 0, &extra);
  init_tcb(&c, 0, NULL);

  queue_add(&queue, &a);
  queue_add(&queue, &b);
  queue_add(&queue, &c);

  expect_uint(queue_size(&queue), 3, "queue test: queue size after add mismatch\n");
  expect_tcb(queue_peek(&queue), &a, "queue test: queue peek mismatch\n");

  expect_tcb(queue_remove(&queue), &a, "queue test: first FIFO remove mismatch\n");
  expect_tcb(a.next, NULL, "queue test: removed head kept stale next pointer\n");
  expect_tcb(queue_remove(&queue), &b, "queue test: second FIFO remove mismatch\n");
  expect_tcb(b.next, NULL, "queue test: removed middle node kept stale next pointer\n");
  expect_tcb(queue_remove(&queue), &c, "queue test: third FIFO remove mismatch\n");
  expect_tcb(queue_remove(&queue), NULL, "queue test: queue should be empty after removes\n");
  expect_uint(queue_size(&queue), 0, "queue test: queue size after removes mismatch\n");
  expect_tcb(queue_peek(&queue), NULL, "queue test: queue peek after removes mismatch\n");

  init_tcb(&a, 0, NULL);
  init_tcb(&b, 0, &extra);
  init_tcb(&c, 0, &extra);
  queue_add(&queue, &a);
  queue_add(&queue, &b);
  queue_add(&queue, &c);

  struct TCB* head = queue_remove_all(&queue);
  struct TCB* second = head != NULL ? head->next : NULL;
  struct TCB* third = second != NULL ? second->next : NULL;
  expect_tcb(head, &a, "queue test: remove_all head mismatch\n");
  expect_tcb(second, &b, "queue test: remove_all second node mismatch\n");
  expect_tcb(third, &c, "queue test: remove_all third node mismatch\n");
  expect_tcb(c.next, NULL, "queue test: remove_all tail kept stale next pointer\n");
  expect_uint(queue_size(&queue), 0, "queue test: queue size after remove_all mismatch\n");
  expect_tcb(queue_peek(&queue), NULL, "queue test: queue peek after remove_all mismatch\n");
  expect_tcb(queue_remove(&queue), NULL, "queue test: remove after remove_all mismatch\n");
}

static void test_spin_queue(void) {
  struct SpinQueue queue;
  struct TCB a;
  struct TCB b;
  struct TCB c;
  struct TCB extra;

  spin_queue_init(&queue);
  expect_uint(spin_queue_size(&queue), 0, "queue test: spin_queue_init size mismatch\n");
  expect_tcb(spin_queue_peek(&queue), NULL, "queue test: empty spin queue peek mismatch\n");
  expect_tcb(spin_queue_remove(&queue), NULL, "queue test: empty spin queue remove mismatch\n");
  expect_tcb(spin_queue_remove_all(&queue), NULL, "queue test: empty spin queue remove_all mismatch\n");

  init_tcb(&extra, 0, NULL);
  init_tcb(&a, 0, NULL);
  init_tcb(&b, 0, &extra);
  init_tcb(&c, 0, NULL);

  spin_queue_add(&queue, &a);
  spin_queue_add(&queue, &b);
  spin_queue_add(&queue, &c);

  expect_uint(spin_queue_size(&queue), 3, "queue test: spin queue size after add mismatch\n");
  expect_tcb(spin_queue_peek(&queue), &a, "queue test: spin queue peek mismatch\n");

  expect_tcb(spin_queue_remove(&queue), &a, "queue test: first spin FIFO remove mismatch\n");
  expect_tcb(a.next, NULL, "queue test: removed spin head kept stale next pointer\n");
  expect_tcb(spin_queue_remove(&queue), &b, "queue test: second spin FIFO remove mismatch\n");
  expect_tcb(b.next, NULL, "queue test: removed spin middle kept stale next pointer\n");
  expect_tcb(spin_queue_remove(&queue), &c, "queue test: third spin FIFO remove mismatch\n");
  expect_tcb(spin_queue_remove(&queue), NULL, "queue test: spin queue should be empty after removes\n");
  expect_uint(spin_queue_size(&queue), 0, "queue test: spin queue size after removes mismatch\n");
  expect_tcb(spin_queue_peek(&queue), NULL, "queue test: spin queue peek after removes mismatch\n");

  init_tcb(&a, 0, NULL);
  init_tcb(&b, 0, &extra);
  init_tcb(&c, 0, &extra);
  spin_queue_add(&queue, &a);
  spin_queue_add(&queue, &b);
  spin_queue_add(&queue, &c);

  struct TCB* head = spin_queue_remove_all(&queue);
  struct TCB* second = head != NULL ? head->next : NULL;
  struct TCB* third = second != NULL ? second->next : NULL;
  expect_tcb(head, &a, "queue test: spin remove_all head mismatch\n");
  expect_tcb(second, &b, "queue test: spin remove_all second node mismatch\n");
  expect_tcb(third, &c, "queue test: spin remove_all third node mismatch\n");
  expect_tcb(c.next, NULL, "queue test: spin remove_all tail kept stale next pointer\n");
  expect_uint(spin_queue_size(&queue), 0, "queue test: spin queue size after remove_all mismatch\n");
  expect_tcb(spin_queue_peek(&queue), NULL, "queue test: spin queue peek after remove_all mismatch\n");
  expect_tcb(spin_queue_remove(&queue), NULL, "queue test: spin remove after remove_all mismatch\n");
}

static void test_sleep_queue(void) {
  struct SleepQueue queue;
  struct TCB early_1;
  struct TCB early_2;
  struct TCB mid;
  struct TCB late;
  struct TCB extra;

  sleep_queue_init(&queue);
  expect_uint(sleep_queue_size(&queue), 0, "queue test: sleep_queue_init size mismatch\n");
  expect_tcb(remove_sleep_node_at(&queue, 0), NULL, "queue test: empty sleep queue remove mismatch\n");

  init_tcb(&extra, 0, NULL);
  init_tcb(&late, 30, &extra);
  init_tcb(&mid, 20, &extra);
  init_tcb(&early_1, 15, NULL);
  init_tcb(&early_2, 15, &extra);

  add_sleep_node(&queue, &late);
  add_sleep_node(&queue, &mid);
  add_sleep_node(&queue, &early_1);
  add_sleep_node(&queue, &early_2);

  expect_uint(sleep_queue_size(&queue), 4, "queue test: sleep queue size after add mismatch\n");

  expect_tcb(remove_sleep_node_at(&queue, 14), NULL,
             "queue test: sleep queue released before wakeup_jiffies\n");
  expect_tcb(remove_sleep_node_at(&queue, 15), &early_1,
             "queue test: sleep queue first wakeup mismatch\n");
  expect_tcb(remove_sleep_node_at(&queue, 15), &early_2,
             "queue test: sleep queue equal deadline ordering mismatch\n");
  expect_tcb(early_2.next, NULL, "queue test: sleep queue node kept stale next pointer\n");
  expect_tcb(remove_sleep_node_at(&queue, 15), NULL,
             "queue test: sleep queue removed a future node too early\n");
  expect_uint(sleep_queue_size(&queue), 2, "queue test: sleep queue size after early removes mismatch\n");

  expect_tcb(remove_sleep_node_at(&queue, 20), &mid,
             "queue test: sleep queue middle wakeup mismatch\n");
  expect_tcb(mid.next, NULL, "queue test: sleep queue middle node kept stale next pointer\n");
  expect_tcb(remove_sleep_node_at(&queue, 29), NULL,
             "queue test: sleep queue released late node before deadline\n");
  expect_tcb(remove_sleep_node_at(&queue, 30), &late,
             "queue test: sleep queue late wakeup mismatch\n");
  expect_tcb(late.next, NULL, "queue test: sleep queue tail kept stale next pointer\n");
  expect_tcb(remove_sleep_node_at(&queue, 30), NULL,
             "queue test: sleep queue should be empty after all wakes\n");
  expect_uint(sleep_queue_size(&queue), 0, "queue test: sleep queue size after removals mismatch\n");
}

static void test_generic_queue(void) {
  struct GenericQueue queue;
  struct TestElement a;
  struct TestElement b;
  struct TestElement c;
  struct TestElement extra;

  generic_queue_init(&queue);
  expect_uint(generic_queue_size(&queue), 0, "queue test: generic_queue_init size mismatch\n");
  expect_element(generic_queue_remove(&queue), NULL, "queue test: empty generic queue remove mismatch\n");
  expect_element(generic_queue_remove_all(&queue), NULL,
                 "queue test: empty generic queue remove_all mismatch\n");

  init_element(&extra, 99, NULL);
  init_element(&a, 1, NULL);
  init_element(&b, 2, &extra);
  init_element(&c, 3, NULL);

  generic_queue_add(&queue, &a.link);
  generic_queue_add(&queue, &b.link);
  generic_queue_add(&queue, &c.link);

  expect_uint(generic_queue_size(&queue), 3, "queue test: generic queue size after add mismatch\n");
  expect_element(generic_queue_remove(&queue), &a.link,
                 "queue test: first generic FIFO remove mismatch\n");
  expect_element(a.link.next, NULL, "queue test: removed generic head kept stale next pointer\n");
  expect_element(generic_queue_remove(&queue), &b.link,
                 "queue test: second generic FIFO remove mismatch\n");
  expect_element(b.link.next, NULL, "queue test: removed generic middle kept stale next pointer\n");
  expect_element(generic_queue_remove(&queue), &c.link,
                 "queue test: third generic FIFO remove mismatch\n");
  expect_element(generic_queue_remove(&queue), NULL,
                 "queue test: generic queue should be empty after removes\n");
  expect_uint(generic_queue_size(&queue), 0, "queue test: generic queue size after removes mismatch\n");

  init_element(&a, 1, NULL);
  init_element(&b, 2, &extra);
  init_element(&c, 3, &extra);
  generic_queue_add(&queue, &a.link);
  generic_queue_add(&queue, &b.link);
  generic_queue_add(&queue, &c.link);

  struct GenericQueueElement* head = generic_queue_remove_all(&queue);
  struct GenericQueueElement* second = head != NULL ? head->next : NULL;
  struct GenericQueueElement* third = second != NULL ? second->next : NULL;
  expect_element(head, &a.link, "queue test: generic remove_all head mismatch\n");
  expect_element(second, &b.link, "queue test: generic remove_all second node mismatch\n");
  expect_element(third, &c.link, "queue test: generic remove_all third node mismatch\n");
  expect_element(c.link.next, NULL, "queue test: generic remove_all tail kept stale next pointer\n");
  expect_uint(generic_queue_size(&queue), 0,
              "queue test: generic queue size after remove_all mismatch\n");
  expect_element(generic_queue_remove(&queue), NULL,
                 "queue test: generic remove after remove_all mismatch\n");
}

static void test_generic_spin_queue(void) {
  struct GenericSpinQueue queue;
  struct TestElement a;
  struct TestElement b;
  struct TestElement c;
  struct TestElement extra;

  generic_spin_queue_init(&queue);
  expect_uint(generic_spin_queue_size(&queue), 0,
              "queue test: generic_spin_queue_init size mismatch\n");
  expect_element(generic_spin_queue_remove(&queue), NULL,
                 "queue test: empty generic spin queue remove mismatch\n");
  expect_element(generic_spin_queue_remove_all(&queue), NULL,
                 "queue test: empty generic spin queue remove_all mismatch\n");

  init_element(&extra, 99, NULL);
  init_element(&a, 1, NULL);
  init_element(&b, 2, &extra);
  init_element(&c, 3, NULL);

  generic_spin_queue_add(&queue, &a.link);
  generic_spin_queue_add(&queue, &b.link);
  generic_spin_queue_add(&queue, &c.link);

  expect_uint(generic_spin_queue_size(&queue), 3,
              "queue test: generic spin queue size after add mismatch\n");
  expect_element(generic_spin_queue_remove(&queue), &a.link,
                 "queue test: first generic spin FIFO remove mismatch\n");
  expect_element(a.link.next, NULL,
                 "queue test: removed generic spin head kept stale next pointer\n");
  expect_element(generic_spin_queue_remove(&queue), &b.link,
                 "queue test: second generic spin FIFO remove mismatch\n");
  expect_element(b.link.next, NULL,
                 "queue test: removed generic spin middle kept stale next pointer\n");
  expect_element(generic_spin_queue_remove(&queue), &c.link,
                 "queue test: third generic spin FIFO remove mismatch\n");
  expect_element(generic_spin_queue_remove(&queue), NULL,
                 "queue test: generic spin queue should be empty after removes\n");
  expect_uint(generic_spin_queue_size(&queue), 0,
              "queue test: generic spin queue size after removes mismatch\n");

  init_element(&a, 1, NULL);
  init_element(&b, 2, &extra);
  init_element(&c, 3, &extra);
  generic_spin_queue_add(&queue, &a.link);
  generic_spin_queue_add(&queue, &b.link);
  generic_spin_queue_add(&queue, &c.link);

  struct GenericQueueElement* head = generic_spin_queue_remove_all(&queue);
  struct GenericQueueElement* second = head != NULL ? head->next : NULL;
  struct GenericQueueElement* third = second != NULL ? second->next : NULL;
  expect_element(head, &a.link, "queue test: generic spin remove_all head mismatch\n");
  expect_element(second, &b.link, "queue test: generic spin remove_all second node mismatch\n");
  expect_element(third, &c.link, "queue test: generic spin remove_all third node mismatch\n");
  expect_element(c.link.next, NULL,
                 "queue test: generic spin remove_all tail kept stale next pointer\n");
  expect_uint(generic_spin_queue_size(&queue), 0,
              "queue test: generic spin queue size after remove_all mismatch\n");
  expect_element(generic_spin_queue_remove(&queue), NULL,
                 "queue test: generic spin remove after remove_all mismatch\n");
}

static void test_ringbuf(void) {
  struct RingBuf ringbuf;
  int a = 1;
  int b = 2;
  int c = 3;
  int d = 4;
  int e = 5;

  ringbuf_init(&ringbuf, 4);
  expect_uint(ringbuf_size(&ringbuf), 0, "queue test: ringbuf_init size mismatch\n");
  expect_ptr(ringbuf_remove_front(&ringbuf), NULL, "queue test: empty ringbuf remove_front mismatch\n");
  expect_ptr(ringbuf_remove_back(&ringbuf), NULL, "queue test: empty ringbuf remove_back mismatch\n");

  expect_bool(ringbuf_add_front(&ringbuf, &a), true, "queue test: ringbuf add_front a failed\n");
  expect_bool(ringbuf_add_front(&ringbuf, &b), true, "queue test: ringbuf add_front b failed\n");
  expect_bool(ringbuf_add_back(&ringbuf, &c), true, "queue test: ringbuf add_back c failed\n");
  expect_uint(ringbuf_size(&ringbuf), 3, "queue test: ringbuf size after fill mismatch\n");
  expect_bool(ringbuf_add_front(&ringbuf, &d), false,
              "queue test: ringbuf add_front should fail when full\n");
  expect_bool(ringbuf_add_back(&ringbuf, &e), false,
              "queue test: ringbuf add_back should fail when full\n");

  expect_ptr(ringbuf_remove_front(&ringbuf), &b, "queue test: ringbuf remove_front mismatch\n");
  expect_bool(ringbuf_add_back(&ringbuf, &d), true,
              "queue test: ringbuf add_back after wrap-around failed\n");
  expect_ptr(ringbuf_remove_back(&ringbuf), &d, "queue test: ringbuf wrapped remove_back mismatch\n");
  expect_ptr(ringbuf_remove_back(&ringbuf), &c, "queue test: ringbuf second remove_back mismatch\n");
  expect_ptr(ringbuf_remove_front(&ringbuf), &a, "queue test: ringbuf final remove_front mismatch\n");
  expect_ptr(ringbuf_remove_front(&ringbuf), NULL, "queue test: ringbuf should be empty\n");
  expect_uint(ringbuf_size(&ringbuf), 0, "queue test: ringbuf size after drain mismatch\n");

  ringbuf_destroy(&ringbuf);
  expect_ptr(ringbuf.buf, NULL, "queue test: ringbuf_destroy did not clear buf pointer\n");
  expect_uint(ringbuf.capacity, 0, "queue test: ringbuf_destroy did not clear capacity\n");
  expect_uint(ringbuf.head, 0, "queue test: ringbuf_destroy did not clear head\n");
  expect_uint(ringbuf.tail, 0, "queue test: ringbuf_destroy did not clear tail\n");

  struct RingBuf* heap_ringbuf = malloc(sizeof(struct RingBuf));
  assert(heap_ringbuf != NULL, "queue test: heap ringbuf allocation failed.\n");
  ringbuf_init(heap_ringbuf, 2);
  expect_bool(ringbuf_add_front(heap_ringbuf, &a), true,
              "queue test: heap ringbuf add_front failed\n");
  expect_uint(ringbuf_size(heap_ringbuf), 1, "queue test: heap ringbuf size mismatch\n");
  ringbuf_free(heap_ringbuf);
}

void kernel_main(void) {
  say("***queue test start\n", NULL);

  test_queue();
  say("***queue fifo ok\n", NULL);

  test_spin_queue();
  say("***spin_queue ok\n", NULL);

  test_sleep_queue();
  say("***sleep_queue ok\n", NULL);

  test_generic_queue();
  say("***generic_queue ok\n", NULL);

  test_generic_spin_queue();
  say("***generic_spin_queue ok\n", NULL);

  test_ringbuf();
  say("***ringbuf ok\n", NULL);

  say("***queue test complete\n", NULL);
}
