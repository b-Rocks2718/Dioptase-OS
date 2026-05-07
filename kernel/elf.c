#include "elf.h"
#include "string.h"
#include "heap.h"
#include "print.h"
#include "vmem.h"

#define ELF_MAGIC_0 0x7F
#define ELF_MAGIC_1 'E'
#define ELF_MAGIC_2 'L'
#define ELF_MAGIC_3 'F'

static bool elf_header_bytes_in_range(unsigned image_size, unsigned offset,
    unsigned size){
  if (offset > image_size){
    return false;
  }

  if (size > image_size - offset){
    return false;
  }

  return true;
}

bool elf_validate_image(void* elf_image, unsigned image_size){
  struct ElfHeader header;
  unsigned ph_table_bytes = 0;

  if (elf_image == NULL){
    return false;
  }

  if (image_size < sizeof(struct ElfHeader)){
    return false;
  }

  memcpy(&header, elf_image, sizeof(struct ElfHeader));

  if (header.e_ident[0] != ELF_MAGIC_0 ||
      header.e_ident[1] != ELF_MAGIC_1 ||
      header.e_ident[2] != ELF_MAGIC_2 ||
      header.e_ident[3] != ELF_MAGIC_3){
    return false;
  }

  if (header.e_ident[EI_CLASS] != ELFCLASS32){
    return false;
  }

  if (header.e_ident[EI_DATA] != ELFDATA2LSB){
    return false;
  }

  if (header.e_ehsize != sizeof(struct ElfHeader)){
    return false;
  }

  if (header.e_machine != EM_DIOPTASE){
    return false;
  }

  if (header.e_phnum == 0 || header.e_phentsize != sizeof(struct ElfProgramHeader)){
    return false;
  }

  ph_table_bytes = (unsigned)header.e_phnum * (unsigned)header.e_phentsize;
  if (header.e_phnum != 0 &&
      ph_table_bytes / (unsigned)header.e_phentsize != (unsigned)header.e_phnum){
    return false;
  }

  if (!elf_header_bytes_in_range(image_size, header.e_phoff, ph_table_bytes)){
    return false;
  }

  for (unsigned i = 0; i < (unsigned)header.e_phnum; ++i){
    struct ElfProgramHeader ph;
    unsigned ph_offset = header.e_phoff + i * header.e_phentsize;
    memcpy(&ph, (unsigned char*)elf_image + ph_offset, sizeof(struct ElfProgramHeader));

    if (ph.p_memsz < ph.p_filesz){
      return false;
    }

    if (ph.p_filesz != 0 &&
        !elf_header_bytes_in_range(image_size, ph.p_offset, ph.p_filesz)){
      return false;
    }
  }

  return true;
}
 
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
