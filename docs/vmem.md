## Virtual Memory

Uses a 2 level tree consisting of page dir and page tables, similar to x86

a thread's pid is a pointer to its page dir for now. this gets stored in `cr1`

PDE entry: `Bits 31-12 of PTE addr | Flags (12 bits)`

Flags are `V (bit 5) | G | U | X | W | R (bit 0)`  
- `V` = valid
- `G` = global
- `U` = user
- `X` = executable
- `W` = writable
- `R` = readable

PTE entry: `Bits 31-12 of page addr | Flags (12 bits)`  
flags are same as pde flags

to lookup an address, the top 10 bits form an index into the page directory. the next 10
bits form an index into the page table

### TLB stuff
The TLB has 16 entries.

A TLB entry looks like this: `PID (32 bits) | VPN (20 bits) || PPN (15 bits) | Flags (12 bits)`
The part to the left of the `||` is the key, the part on the right is the value.

For now, only the bottom 5 bits of the flags are used. They are `G U X W R` (with `R` being in bit 0)
G - global (if set, any PID can match this entry)
U - user (if set, this entry becomes valid in user mode)
X - executable
W - writable
R - readable

the TLB internally keeps a valid bit for each entry

`tlbr rA, rB` will use the PID and `(rB & 0xFFFFF000)` as a key and put the value in `rA`
`tlbw rA, rB` will use the PID and `(rB & 0xFFFFF000)` as a key and store `(rA & 0x7FFFFFF)` as the value in the TLB


### mmap stuff
private anonymous - easy
private file-backed - does not update backing file
shared file-backed - all instances of mmapping that file are shared
shared anonymous - all children of thread share
