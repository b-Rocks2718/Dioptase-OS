#include "elf.h"
#include "string.h"
#include "heap.h"
#include "print.h"
#include "vmem.h"

void program_header_read(void* elf_image, unsigned phoff){
  struct ElfProgramHeader* ph = (struct ElfProgramHeader*)((unsigned char*)elf_image + phoff);
  say("  Program Header:\n", NULL);
  say("    Type: 0x%x\n", &ph->p_type);
  say("    Offset: 0x%x\n", &ph->p_offset);
  say("    Virtual Address: 0x%x\n", &ph->p_vaddr);
  say("    Physical Address: 0x%x\n", &ph->p_paddr);
  say("    File Size: 0x%x\n", &ph->p_filesz);
  say("    Memory Size: 0x%x\n", &ph->p_memsz);

  char* yes = "yes";
  char* no = "no";
  say("    Flags:\n", NULL);
  say("      Read: %s\n", (ph->p_flags & PF_R) ? &yes : &no);
  say("      Write: %s\n", (ph->p_flags & PF_W) ? &yes : &no);
  say("      Execute: %s\n", (ph->p_flags & PF_X) ? &yes : &no);

  say("    Alignment: 0x%x\n", &ph->p_align);
}

void elf_read(void* elf_image){
  struct ElfHeader* header = malloc(sizeof(struct ElfHeader));
  memcpy(header, elf_image, sizeof(struct ElfHeader));

  say("ELF Header:\n", NULL);

  {
    int magic[4] = {header->e_ident[0], header->e_ident[1], header->e_ident[2], header->e_ident[3]};
    say("  Magic: 0x%X '%c' '%c' '%c'\n", magic);

    int class = header->e_ident[EI_CLASS];
    char* thirty_two_bit = "32-bit";
    char* sixty_four_bit = "64-bit";
    say("  Class: %s\n", (class == ELFCLASS32) ? &thirty_two_bit : &sixty_four_bit);

    int data = header->e_ident[EI_DATA];
    char* big_endian = "big-endian";
    char* little_endian = "little-endian";
    say("  Data: %s\n", (data == ELFDATA2LSB) ? &little_endian : &big_endian);

    int version = header->e_ident[EI_VERSION];
    if (version != 1){
      say("  Version: %d\n", &version);
    } else {
      say("  Version: current\n", NULL);
    }

    int osabi = header->e_ident[EI_OSABI];
    say("  OSABI: %d\n", &osabi);

    int abiversion = header->e_ident[EI_ABIVERSION];
    say("  ABIVERSION: %d\n", &abiversion);
  }

  int type = header->e_type;
  if (type == 2){
    say("  Type: executable\n", NULL);
  } else {
    say("  Type: %d\n", &type);
  }

  unsigned machine = header->e_machine;
  if (machine == 0xd105){
    say("  Machine: Dioptase\n", NULL);
  } else {
    say("  Machine: 0x%x\n", &machine);
  }

  unsigned version = header->e_version;
  say("  Version: %d\n", &version);

  unsigned entry = header->e_entry;
  say("  Entry point: 0x%x\n", &entry);

  unsigned phoff = header->e_phoff;
  say("  Program header offset: 0x%x\n", &phoff);

  unsigned shoff = header->e_shoff;
  say("  Section header offset: 0x%x\n", &shoff);

  unsigned flags = header->e_flags;
  say("  Flags: 0x%x\n", &flags);

  unsigned ehsize = header->e_ehsize;
  say("  ELF header size: %d\n", &ehsize);

  unsigned phentsize = header->e_phentsize;
  say("  Program header entry size: %d\n", &phentsize);

  unsigned phnum = header->e_phnum;
  say("  Program header entry count: %d\n", &phnum);

  unsigned shentsize = header->e_shentsize;
  say("  Section header entry size: %d\n", &shentsize);

  unsigned shnum = header->e_shnum;
  say("  Section header entry count: %d\n", &shnum);

  unsigned shstrndx = header->e_shstrndx;
  say("  Section header string table index: %d\n", &shstrndx);

  for (unsigned i = 0; i < phnum; i++){
    program_header_read(elf_image, phoff + i * phentsize);
  }

  free(header);
}

void program_header_load(void* elf_image, unsigned phoff){
  struct ElfProgramHeader* ph = (struct ElfProgramHeader*)((unsigned char*)elf_image + phoff);
  unsigned vaddr = ph->p_vaddr;
  unsigned memsz = ph->p_memsz;
  unsigned filesz = ph->p_filesz;
  unsigned offset = ph->p_offset;
  unsigned flags = ph->p_flags;

  unsigned mmap_flags = MMAP_USER;
  if (flags & PF_R){
    mmap_flags |= MMAP_READ;
  }
  if (flags & PF_W){
    mmap_flags |= MMAP_WRITE;
  }
  if (flags & PF_X){
    mmap_flags |= MMAP_EXEC;
  }

  struct VME* vme = mmap_at(memsz, NULL, offset, MMAP_READ | MMAP_WRITE | MMAP_USER, vaddr);
  
  for (int i = 0; i < filesz; i++){
    ((char*)vaddr)[i] = ((char*)elf_image)[offset + i];
  }

  vme_change_perms(vme, mmap_flags);
}

unsigned elf_load(void* elf_image){
  struct ElfHeader* header = malloc(sizeof(struct ElfHeader));
  memcpy(header, elf_image, sizeof(struct ElfHeader));

  unsigned entry = header->e_entry;
  for (int i = 0; i < header->e_phnum; i++){
    program_header_load(elf_image, header->e_phoff + i * header->e_phentsize);
  }

  free(header);

  return entry;
}
