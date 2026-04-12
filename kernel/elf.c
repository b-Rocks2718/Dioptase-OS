#include "elf.h"
#include "string.h"
#include "heap.h"
#include "print.h"
#include "vmem.h"
 
// mmap a program segment described by a program header into memory
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

// load an ELF image in the layout described by its program headers
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
