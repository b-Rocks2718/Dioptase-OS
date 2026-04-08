#include "ps2.h"
#include "threads.h"
#include "machine.h"
#include "interrupts.h"
#include "print.h"
#include "debug.h"
#include "heap.h"
#include "per_core.h"
#include "scheduler.h"

struct KeyElement {
  struct GenericQueueElement link;
  short key;
};

struct BlockingQueue ps2_queue;
struct TCB* ps2_worker_thread;

// PS/2 MMIO address for keyboard input
static short* ps2_in = (short*)0x7FE5800;

static void ps2_worker_block(void* arg){
  struct TCB* tcb = (struct TCB*)arg;
  __atomic_store_n((int*)&ps2_worker_thread, (int)tcb);
}

bool keys_pending = false;

// ps2 worker thread to fill ps2 queue
static void ps2_worker(void){
  while (true){
    // check each core's keybuf for new keys, and add them to the blocking queue
    // this is fine without locks because these are SPSC queues
    // each core is the single producer, this thread is the single consumer
    __atomic_store_n(&keys_pending, false);
    for (int i = 0; i < MAX_CORES; ++i){
      short key = 0;
      while ((key = keybuf_remove(&per_core_data[i].keybuf)) != 0){
        struct KeyElement* element = malloc(sizeof(struct KeyElement));
        element->key = key;
        blocking_queue_add(&ps2_queue, (struct GenericQueueElement*)element);
      }
    }

    if (!__atomic_load_n(&keys_pending)) {
      // there is a small race where if an interrupt happens here we drop the key
      // this is probably super rare because ps2 interrupts are rare
      // to be safe, the pit handler occasionally wakes the ps2 worker 
      // so it can catch dropped keys

      // no keys pending, go back to sleep
      int was = interrupts_disable();
      struct TCB* me = get_current_tcb();
      block(was, ps2_worker_block, me, true);
    }
  }
  panic("PS/2 worker thread exited unexpectedly");
}

// Initialize the PS/2 driver
void ps2_init(void){
  blocking_queue_init(&ps2_queue);

  for (int i = 0; i < MAX_CORES; ++i){
    keybuf_init(&per_core_data[i].keybuf);
  }

  ps2_worker_thread = NULL;

  // init ps2 worker thread
  struct Fun* ps2_worker_fun = leak(sizeof(struct Fun));
  ps2_worker_fun->func = (void (*)(void *))ps2_worker;
  ps2_worker_fun->arg = NULL;

  setup_thread(ps2_worker_fun, HIGH_PRIORITY, ANY_CORE);

  register_handler((void*)ps2_handler_, (void*)0x3C4);
}

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void){
  struct KeyElement* element = (struct KeyElement*)blocking_queue_try_remove(&ps2_queue);
  if (element == NULL) {
    return 0;
  }

  short key = element->key;

  free(element);

  return key;
}

// read a key from the PS/2 keyboard
// If no key is pressed, block until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
short waitkey(void){
  struct KeyElement* element = (struct KeyElement*)blocking_queue_remove(&ps2_queue);
  short key = element->key;

  free(element);

  return key;
}

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short getkey_raw(void){
  return *ps2_in;
}

// read a key from the PS/2 keyboard
// If no key is pressed, spin until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short waitkey_raw(void){
  short key = 0;
  while ((key = *ps2_in) == 0) {
    // spin until a key is pressed

    // sleep to save power
    pause();
  }
  return key;
}

void ps2_handler(void){
  mark_ps2_handled();

  struct PerCore* pc = get_per_core();
  struct KeyBuf* kb = &pc->keybuf;

  // add key to per core queue, worker thread will remove it
  // and add it to a blocking queue
  keybuf_add(kb, *ps2_in);

  // if core queue somehow fills, we lose the key
  // ps2 events should be infrequent enough that this does not happen

  // decide if we need to wake ps2 worker thread
  // atomic exchange prevents a double wakeup
  struct TCB* worker = (struct TCB*)__atomic_exchange_n((int*)&ps2_worker_thread, NULL);

  if (worker != NULL) {
    scheduler_wake_thread_from_interrupt(worker);
  } else {
    __atomic_store_n(&keys_pending, true);
  }
}
