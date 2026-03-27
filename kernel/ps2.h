#ifndef PS2_H
#define PS2_H

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void);

// read a key from the PS/2 keyboard
// If no key is pressed, block until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
short waitkey(void);

// read a key from the PS/2 keyboard
// If no key is pressed, spin until one is pressed and return it
// return the ASCII code of the key, clears the key from the buffer
short waitkey_spin(void);

#endif // PS2_H