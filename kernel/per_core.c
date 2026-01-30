#include "per_core.h"
#include "machine.h"
#include "constants.h"

struct PerCore per_core_data[MAX_CORES];

struct PerCore* get_per_core(void){
  int me = get_core_id();
  return &per_core_data[me];
}

struct TCB* get_current_tcb() {
  return get_per_core()->current_thread;
}

struct TCB* get_idle_tcb() {
  return get_per_core()->idle_thread;
}
