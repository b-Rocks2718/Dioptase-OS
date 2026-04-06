#ifndef VMEM_H
#define VMEM_H

// Initialize virtual memory structures
// Called once by the first core to set up global structures
void vmem_global_init(void);

// Per-core virtual memory initialization
void vmem_core_init(void);

#endif // VMEM_H