#ifndef ELF_H
#define ELF_H

#include "constants.h"

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8

#define ELFCLASS32 1
#define ELFCLASS64 2

#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define EV_CURRENT 1

#define ELFOSABI_SYSV 0
#define ELF_ABIVERSION_CURRENT 0

#define ET_EXEC 2

#define EM_DIOPTASE 0xD105

// Describe the fixed-width ELF image header consumed by the kernel loader.
struct ElfHeader {
  unsigned char e_ident[16];
  unsigned short e_type;
  unsigned short e_machine;
  unsigned e_version;
  unsigned e_entry;
  unsigned e_phoff;
  unsigned e_shoff;
  unsigned e_flags;
  unsigned short e_ehsize;
  unsigned short e_phentsize;
  unsigned short e_phnum;
  unsigned short e_shentsize;
  unsigned short e_shnum;
  unsigned short e_shstrndx;
};

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
// Written as the resolved mask because the in-tree compiler does not recursively
// expand identifiers introduced by another object-like macro.
#define PF_KNOWN_MASK 0x7

#define PT_LOAD 1

/*
 * The in-tree assembler emits exactly three program headers (text, rodata,
 * and data). Keep a generous implementation-defined ceiling while bounding
 * both the validator's pairwise overlap work and the loader's VME allocations
 * for a user-controlled executable.
 */
#define ELF_MAX_PROGRAM_HEADERS 64

// Describe one ELF segment and its file-to-memory mapping requirements.
struct ElfProgramHeader {
  unsigned p_type;
  unsigned p_offset;
  unsigned p_vaddr;
  unsigned p_paddr;
  unsigned p_filesz;
  unsigned p_memsz;
  unsigned p_flags;
  unsigned p_align;
};

/*
 * Validate the complete loader-visible structure of one Dioptase ELF image.
 * This is a read-only operation and does not reserve any VMEs. A true result
 * guarantees that every nonempty PT_LOAD header is safe to pass to mmap_at()
 * in an otherwise non-overlapping user address space.
 */
bool elf_validate_image(void* elf_image, unsigned image_size);

/*
 * Load a previously validated image into the current TCB's user address space.
 *
 * Preconditions:
 * - execution is in kernel mode and ordinary thread context, where VM faults
 *   and heap/physical-page allocation are permitted;
 * - elf_validate_image() returned true for these exact, still-stable bytes;
 * - no existing VME overlaps a nonempty PT_LOAD range.
 *
 * On success every nonempty PT_LOAD is mapped with its requested known
 * permissions, zero-length and non-PT_LOAD headers are ignored, and
 * *entry_out receives the validated user entry address. On failure returns
 * false and may leave a partial VME list; the caller must destroy that
 * in-construction address space rather than entering user mode.
 */
bool elf_load(void* elf_image, unsigned* entry_out);

#endif // ELF_H
