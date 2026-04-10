#ifndef ELF_H
#define ELF_H

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8

#define ELFCLASS32 1
#define ELFCLASS64 2

#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

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

void elf_read(void* elf_image);

unsigned elf_load(void* elf_image);

#endif // ELF_H