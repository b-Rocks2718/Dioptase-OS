# User C runtime

The bundled programs under `root/` use a small C runtime tailored to Dioptase.
It is not a complete ISO C or POSIX libc. Raw descriptor I/O follows the
syscall contracts in [syscalls.md](syscalls.md); the interfaces below document
the additional CRT behavior that is not specified by the architecture.

## Formatted output

`printf(fmt, args)` and `fdprintf(fd, fmt, args)` are intentionally
non-variadic. `args` points to consecutive 32-bit slots in conversion order.
`%.*s` consumes the precision slot followed by the string-pointer slot. A null
argument-array pointer is valid only when the format consumes no arguments.

The supported conversions are `%d`, `%u`, `%x`, `%X`, `%s`, `%.*s`, `%c`, and
`%%`. Decimal minimum width and leading-zero padding are supported. Unknown
conversions preserve the existing behavior of emitting `%` literally and then
processing the unrecognized character as ordinary text.

One leading `l` or `z` is accepted as a compatibility spelling, but it does not
change the argument-array layout: the conversion still consumes one 32-bit
slot. This is correct for the CRT's 32-bit `size_t`; it is not a formatter for
the ABI's 64-bit `long`. Full-width `long` formatting remains unsupported.

On complete success, `printf` and `fdprintf` return the exact number of emitted
bytes. Format syntax such as `%`, width digits, precision syntax, and length
modifiers is not itself counted. They return `-1` if a `write()` fails, makes
zero progress, violates its at-most-requested-byte invariant, or if the final
count/field width cannot be represented by `int`. A positive prefix committed
before a later failure remains visible; formatted output is not transactional.

`fdputs()` and this CRT's `puts()` use the same count-or-`-1` convention.
Unlike ISO C `puts`, the Dioptase function does not append a newline. The
standalone `print_signed`, `print_unsigned`, and `print_hex` helpers also return
their complete byte count or `-1`. `putchar` returns the emitted unsigned byte
value or `-1`.

The fd-backed `fputs()` shim propagates the `fdputs()` result, including `-1`.
`fwrite()` retains its separate item-count contract: it returns the number of
whole items written before failure or loss of progress, which can be less than
the requested item count and is zero when no complete item was written.
`fread()` has the corresponding whole-items-read contract. Both functions
return zero without dereferencing the buffer or performing descriptor I/O when
`size` or `count` is zero, or when `size * count` is not representable by the
CRT's 32-bit `size_t`; a rejected request therefore does not change the stream
position or underlying file contents.
