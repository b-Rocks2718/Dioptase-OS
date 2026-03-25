#ifndef MACHINE_H
#define MACHINE_H

// Jump directly to the loaded kernel entry point
extern void enter_kernel(void* kernel_start_address);

#endif // MACHINE_H
