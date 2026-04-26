#ifndef ELF_H
#define ELF_H

#include "instruction_array.h"

struct ProgramDescriptor {
  unsigned entry_point;
  struct InstructionArrayList* sections;
  unsigned bss_size;
};

struct ElfHeader {
  unsigned char e_ident[16];
  unsigned short e_type;
  unsigned short e_machine;
  unsigned int e_version;
  unsigned int e_entry;
  unsigned int e_phoff;
  unsigned int e_shoff;
  unsigned int e_flags;
  unsigned short e_ehsize;
  unsigned short e_phentsize;
  unsigned short e_phnum;
  unsigned short e_shentsize;
  unsigned short e_shnum;
  unsigned short e_shstrndx;
};

struct ElfProgramHeader {
  unsigned int p_type;
  unsigned int p_offset;
  unsigned int p_vaddr;
  unsigned int p_paddr;
  unsigned int p_filesz;
  unsigned int p_memsz;
  unsigned int p_flags;
  unsigned int p_align;
};

void destroy_program_descriptor(struct ProgramDescriptor* program);

struct ElfHeader create_elf_header(struct ProgramDescriptor* program);

struct ElfProgramHeader* create_PHT(struct ProgramDescriptor* program);

struct ElfProgramHeader create_text_program_header(unsigned int offset, unsigned int vaddr, unsigned int filesz);

struct ElfProgramHeader create_rodata_program_header(unsigned int offset, unsigned int vaddr, unsigned int filesz);

struct ElfProgramHeader create_data_program_header(unsigned int offset, unsigned int vaddr, unsigned int filesz, unsigned int memsz);

void fprint_elf_header(int file, struct ElfHeader* header);

void fprint_pht(int file, struct ElfProgramHeader* pht);

// Purpose: Write the ELF header as raw little-endian bytes.
// Inputs: ptr is the binary output; header describes the ELF header fields.
// Outputs: Writes the ELF header bytes to ptr.
void fwrite_elf_header(int file, struct ElfHeader* header);

// Purpose: Write the program header table as raw little-endian bytes.
// Inputs: ptr is the binary output; pht points to 3 program header entries.
// Outputs: Writes the program header table bytes to ptr.
void fwrite_pht(int file, struct ElfProgramHeader* pht);

#endif  // ELF_H
