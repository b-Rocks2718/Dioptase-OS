#ifndef PS2_H
#define PS2_H

// Initialize the PS/2 driver
void ps2_init(void);

// Destroy PS/2 queue synchronization after interrupts and every worker/reader
// is stopped. Static event-pool elements are drained but never freed.
void ps2_destroy(void);

// Return the aggregate number of nonzero key events dropped because either a
// per-core ISR buffer or the fixed worker-to-reader pool was full. The 32-bit
// count is reset by ps2_init() and wraps modulo 2^32.
unsigned ps2_dropped_event_count(void);

// read a key from the PS/2 keyboard
// return the guest keycode event, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void);

// read a key from the PS/2 keyboard
// If no key is pressed, block until one is pressed and return it
// return the guest keycode event, clears the key from the buffer
short waitkey(void);

// read a key from the PS/2 keyboard
// return the guest keycode event, or 0 if no key is pressed
// clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short getkey_raw(void);

// read a key from the PS/2 keyboard
// If no key is pressed, spin until one is pressed and return it
// return the guest keycode event, clears the key from the buffer
// reads directly from MMIO, bypassing the queue of keypresses
// only should be used when threading is not set up (boot/shutdown)
short waitkey_raw(void);

// ps2 interrupt handler, defined in ps2.s
extern void ps2_handler_(void);

// mark the PS/2 interrupt as handled
extern void mark_ps2_handled(void);

#endif // PS2_H
