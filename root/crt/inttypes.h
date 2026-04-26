#ifndef INTTYPES_H
#define INTTYPES_H

#include "stdint.h"

/*
 * These format macros follow the bootstrap integer aliases in stdint.h.
 * Until the userland compiler regains `long`, `%d` and `%u` are the matching
 * print formats for the temporary 32-bit stand-ins.
 */
#define PRId64 "d"
#define PRIu64 "u"

#endif // INTTYPES_H
