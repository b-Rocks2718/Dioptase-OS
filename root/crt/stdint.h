#ifndef STDINT_H
#define STDINT_H

/*
 * Bootstrap compatibility note:
 * The Dioptase ABI defines 64-bit long values in docs/abi.md, but the current
 * userland bootstrap compiler explicitly rejects `long`. Keep these aliases
 * narrow so root/bcc can still parse while long support remains disabled.
 */
#define uint8_t unsigned char
#define uint16_t unsigned short
#define uint32_t unsigned
#define int32_t int
#define int8_t char
#define int64_t int
#define uint64_t unsigned

#define UINT8_MAX 0xFFu
#define UINT16_MAX 0xFFFFu
#define UINT32_MAX 0xFFFFFFFFu

#endif // STDINT_H
