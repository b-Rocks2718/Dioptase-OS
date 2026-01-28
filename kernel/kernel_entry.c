#include "machine.h"
#include "print.h"
#include "config.h"
#include "atomic.h"
#include "heap.h"
#include "interrupts.h"
#include "pit.h"

unsigned HEAP_START = 0x200000;
unsigned HEAP_SIZE = 0x500000;

extern void kernel_main(void);

void kernel_entry(void){
  int me = get_core_id();

  get_spinlock(&print_lock);

  puts("| Core ");
  print_num(me);
  puts(" awake\n");

  release_spinlock(&print_lock);

  // get number of cores from config
  int num_cores = CONFIG.num_cores;

  static int awake_cores = 0;
  __atomic_fetch_add(&awake_cores, 1);

  if (me == 0){
    puts("| Hello from Dioptase Kernel!\n");

    puts("| Num cores: ");
    print_num(num_cores);
    putchar('\n');

    puts("| Mem size: ");
    print_num(128);
    puts("MiB\n");

    puts("| Initializing heap...\n");
    heap_init((void*)HEAP_START, HEAP_SIZE);

    register_spurious_handlers(); // register spurious interrupt handlers

    puts("| Initializing PIT...\n");
    pit_init(1000); // trigger interrupts at 1000Hz

    // initialize other cores
    puts("| Waking up other cores...\n");
    for (int i = 1; i < num_cores; ++i) {
      wakeup_core(i);
    }
  }

  get_spinlock(&print_lock);
  puts("| Core "); 
  print_num(me);
  puts(" enabling interrupts...\n");
  release_spinlock(&print_lock);

  clear_isr(); // clear interrupt status register
  restore_interrupts(0x80000001); // only PIT interrupt enabled for now

  // have the final core run kernel_main
  if (me == num_cores - 1){
    get_spinlock(&print_lock);
    puts("| Entering kernel_main...\n");
    release_spinlock(&print_lock);

    kernel_main();
    
    get_spinlock(&print_lock);
    puts("| kernel_main finished in ");
    print_num(jiffies);
    puts(" jiffies\n");
    puts("| Halting...\n");
    release_spinlock(&print_lock);
    
    shutdown();
  } else {
    while (1);
  }
}