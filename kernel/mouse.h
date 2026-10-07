#ifndef MOUSE_H
#define MOUSE_H

#include "constants.h"

/*
 * PS/2 mouse driver. Events are the raw 32-bit words defined by
 * docs/mem_map.md "PS/2 mouse input stream": buttons in bits [2:0], an
 * always-set VALID bit 3, and signed 8-bit DX, DY (+down), and WHEEL
 * (+toward the user) in bytes 1..3. A valid event is therefore never 0.
 */

// Initialize the mouse driver. Runs once on core 0 in kernel mode after
// bootstrap() and before secondary cores enable interrupts.
void mouse_init(void);

// Destroy mouse queue synchronization after interrupts and every
// worker/reader is stopped. Static event-pool elements are drained but never
// freed.
void mouse_destroy(void);

// Return the aggregate number of mouse events dropped because either a
// per-core ISR buffer or the fixed worker-to-reader pool was full. The 32-bit
// count is reset by mouse_init() and wraps modulo 2^32.
unsigned mouse_dropped_event_count(void);

// Return the oldest queued mouse event word, or 0 if none is pending.
// Non-blocking; callable from any kernel thread context.
int getmouse(void);

// mouse interrupt handler, defined in mouse.s
extern void mouse_handler_(void);

// mark the mouse interrupt as handled (eoi on the mouse ISR bit)
extern void mark_mouse_handled(void);

#endif // MOUSE_H
