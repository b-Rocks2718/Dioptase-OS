#ifndef PS2_H
#define PS2_H

#include "blocking_queue.h"

#define PS2_WAKE_INTERVAL 1000

extern struct BlockingQueue ps2_queue;
extern struct TCB* ps2_worker_thread;

// Initialize the PS/2 driver
void ps2_init(void);

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void);

// read a key from the PS/2 keyboard
// If no key is pressed, block until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
short waitkey(void);

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short getkey_raw(void);

// read a key from the PS/2 keyboard
// If no key is pressed, spin until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short waitkey_raw(void);

// ps2 interrupt handler, defined in ps2.s
extern void ps2_handler_(void);

// mark the PS/2 interrupt as handled
extern void mark_ps2_handled(void);

#endif // PS2_H