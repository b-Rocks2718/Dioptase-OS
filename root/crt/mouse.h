#ifndef MOUSE_H
#define MOUSE_H

/*
 * Decode PS/2 mouse event words returned by getmouse(). Layout from
 * docs/mem_map.md "PS/2 mouse input stream":
 *   bit 0 left, bit 1 right, bit 2 middle (1 = held at the time of the event)
 *   bit 3 always 1, so a real event is never 0
 *   byte 1 DX, byte 2 DY, byte 3 WHEEL, each signed 8-bit
 * Motion is in 640x480 screen pixels with +DY moving down; +WHEEL is a scroll
 * toward the user. Button bits are state, not edges: compare with the previous
 * event's buttons to detect presses and releases.
 */

#define MOUSE_BUTTON_LEFT   0x1
#define MOUSE_BUTTON_RIGHT  0x2
#define MOUSE_BUTTON_MIDDLE 0x4
#define MOUSE_BUTTON_MASK   0x7

#define MOUSE_BUTTONS(ev) ((ev) & MOUSE_BUTTON_MASK)
// Shift the field into the top byte, then arithmetic-shift it back down to
// sign-extend.
#define MOUSE_DX(ev)    (((int)(ev) << 16) >> 24)
#define MOUSE_DY(ev)    (((int)(ev) << 8) >> 24)
#define MOUSE_WHEEL(ev) ((int)(ev) >> 24)

#endif // MOUSE_H
