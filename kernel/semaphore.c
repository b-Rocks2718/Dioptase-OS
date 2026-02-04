#include "semaphore.h"
#include "heap.h"
#include "per_core.h"
#include "threads.h"
#include "interrupts.h"

// port of Gheith kernel

void sem_init(struct Semaphore* sem, int initial_count){
  spin_lock_init(&sem->lock);
  sem->count = initial_count;
  queue_init(&sem->wait_queue);
}

static void sem_add(void* arg){
  int** args = (int**)arg;
  struct Semaphore* sem = (struct Semaphore*)args[0];
  struct TCB* tcb = (struct TCB*)args[1];

  spin_lock_acquire(&sem->lock);

  if (sem->count > 0){
    sem->count--;
    spin_lock_release(&sem->lock);
    spin_queue_add(&ready_queue, tcb);
  } else {
    queue_add(&sem->wait_queue, tcb);
    spin_lock_release(&sem->lock);
  }
}

void sem_down(struct Semaphore* sem){
  spin_lock_acquire(&sem->lock);

  if (sem->count > 0){
    sem->count--;
    spin_lock_release(&sem->lock);
    return;
  }
  
  spin_lock_release(&sem->lock);

  int was = disable_interrupts();

  struct TCB* current_tcb = get_current_tcb();
  
  int* args[2] = { (int*)sem, (int*)current_tcb };
  block(was, (void (*)(void *))sem_add, (void*)(args));
}

void sem_up(struct Semaphore* sem){
  spin_lock_acquire(&sem->lock);

  struct TCB* wakeup = queue_remove(&sem->wait_queue);
  if (wakeup == NULL){
    sem->count++;
  }
  spin_lock_release(&sem->lock);

  if (wakeup != NULL){
    spin_queue_add(&ready_queue, wakeup);
  }
}

void sem_destroy(struct Semaphore* sem) {
  spin_lock_acquire(&sem->lock);

  struct TCB* dead = queue_remove_all(&sem->wait_queue);

  spin_lock_release(&sem->lock);

  while (dead != NULL) {
    struct TCB* next = dead->next;
    // Detach from the semaphore wait-list before enqueueing on reaper_queue.
    // reaper_queue insertion expects a single node and will relink next itself.
    dead->next = NULL;
    spin_queue_add(&reaper_queue, dead);
    dead = next;
  }
}

void sem_free(struct Semaphore* sem) {
  sem_destroy(sem);
  free(sem);
}
