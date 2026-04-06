#include "machine.h"
#include "print.h"
#include "config.h"
#include "atomic.h"
#include "heap.h"
#include "interrupts.h"
#include "pit.h"
#include "threads.h"
#include "debug.h"
#include "per_core.h"
#include "sd_driver.h"
#include "ps2.h"
#include "physmem.h"
#include "vmem.h"

unsigned HEAP_START = 0x100000;
unsigned HEAP_SIZE =  0x700000;

extern void kernel_main(void);

int start_barrier = 0;

// BIOS switches to init.s, which wakes up all cores and then calls kernel_entry on each core.
// Core 0 performs global initialization, creates the first runnable kernel_main
// thread while no other core can contend for heap locks, then all cores
// bootstrap their idle-thread scheduler context and enter event_loop().
void kernel_entry(void){

  int me = get_core_id();
  int num_cores = CONFIG.num_cores;

  static int awake_cores = 0;
  __atomic_fetch_add(&awake_cores, 1);

  if (me == 0){
    vga_text_init();

    say("| Core %d starting up...\n", &me);

    say("| Hello from Dioptase Kernel!\n", NULL);

    say("| Num cores: %d\n", &num_cores);

    say("| Mem size: 128MiB\n", NULL);

    say("| Initializing physmem allocator...\n", NULL);
    physmem_init();

    say("| Initializing virtual memory...\n", NULL);
    vmem_global_init();

    say("| Initializing heap...\n", NULL);
    heap_init((void*)HEAP_START, HEAP_SIZE);

    say("| Registering spurious interrupt handlers...\n", NULL);
    register_spurious_handlers();

    say("| Initializing PIT...\n", NULL);
    pit_init(3000); // trigger interrupts at 3,000Hz 
    // when running on emulator, this will actually be a much lower frequency

    say("| Initializing threads...\n", NULL);
    threads_init();

    say("| Initializing sd driver...\n", NULL);
    sd_init();

    say("| Initializing PS/2 driver...\n", NULL);
    ps2_init();

    // do this before waking other cores so that the heap doesnt block
    say("| Creating kernel_main thread...\n", NULL);
    struct Fun* kernel_main_fun = malloc(sizeof(struct Fun));
    kernel_main_fun->arg = NULL;
    kernel_main_fun->func = (void (*)(void*))kernel_main;
    thread(kernel_main_fun);

    // create barrier for all cores to sync on
    start_barrier = num_cores;

    // initialize other cores
    say("| Waking up other cores...\n", NULL);
    for (int i = 1; i < num_cores; ++i) {
      wakeup_core(i);
    }
  } else {
    say("| Core %d starting up...\n", &me);
  }

  vmem_core_init();

  say("| Core %d creating idle thread context...\n", &me);
  bootstrap();

  say("| Core %d enabling interrupts...\n", &me);
  interrupts_restore(DEFAULT_INTERRUPT_MASK);
  
  // wait for all cores to be awake and set up
  say("| Core %d waiting at start barrier...\n", &me);
  spin_barrier_sync(&start_barrier);

  event_loop();

  panic("event loop returned"); 
}
