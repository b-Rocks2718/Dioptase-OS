#ifndef STDDEF_H
#define STDDEF_H

// The bootstrap compiler still rejects typedef, so keep these conventional
// names as macro aliases until the C front end grows full typedef support.
#define NULL 0
#define size_t unsigned

#endif // STDDEF_H
