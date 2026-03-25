#ifndef PS2_H
#define PS2_H

// read a key from the PS/2 keyboard
// return the ASCII code of the key, or 0 if no key is pressed
// clears the key from the buffer
short getkey(void);

#endif // PS2_H