#include "elf.h"
#include "string.h"
#include "vmem.h"
#include "physmem.h"
#include "debug.h"
#include "sys.h"

#define ELF_MAGIC_0 0x7F
#define ELF_MAGIC_1 'E'
#define ELF_MAGIC_2 'L'
#define ELF_MAGIC_3 'F'

// Every Dioptase instruction is one 32-bit word according to docs/ISA.md.
#define DIOPTASE_INSTRUCTION_SIZE 4

// Return whether an ELF header fits entirely within the supplied image.
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

// Copy one header instead of dereferencing the image directly. ELF program
// header tables need not have native C alignment, while the Dioptase ISA rounds
// unaligned word loads down and would therefore decode the wrong bytes.
static void elf_read_program_header(void* elf_image,
    struct ElfHeader* header, unsigned index, struct ElfProgramHeader* ph){
  unsigned offset = header->e_phoff + index * header->e_phentsize;
  memcpy(ph, (unsigned char*)elf_image + offset,
    sizeof(struct ElfProgramHeader));
}

/*
 * Convert a nonempty PT_LOAD byte range into the exact exclusive range used by
 * mmap_at(). Validation must duplicate mmap_at()'s representability contract:
 * VMEs store their exclusive end in 32 bits, so the otherwise-addressable page
 * beginning at 0xFFFFF000 cannot be represented as one VME.
 */
static bool elf_load_memory_range(struct ElfProgramHeader* ph,
    unsigned* rounded_end){
  unsigned rounded_size = 0;

  if (ph->p_memsz == 0){
    return false;
  }

  if ((ph->p_vaddr % FRAME_SIZE) != 0){
    return false;
  }

  if (ph->p_vaddr < USER_VMEM_START || ph->p_vaddr > USER_VMEM_END){
    return false;
  }

  if (ph->p_memsz > UINT_MAX - (FRAME_SIZE - 1)){
    return false;
  }

  rounded_size = (ph->p_memsz + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  if (rounded_size == 0 || ph->p_vaddr > UINT_MAX - rounded_size){
    return false;
  }

  *rounded_end = ph->p_vaddr + rounded_size;
  return true;
}

// Return whether two half-open virtual-address ranges overlap.
static bool elf_ranges_overlap(unsigned first_start, unsigned first_end,
    unsigned second_start, unsigned second_end){
  return first_start < second_end && second_start < first_end;
}

/*
 * run_user_program() reserves its ordinary and signal stacks consecutively
 * from the highest representable user VME end. Keeping this whole interval
 * free guarantees both later mmap_stack() calls have a fitting gap; otherwise
 * a structurally valid ELF could make successful exec reach mmap_stack()'s
 * kernel-bug panic path after the old address space has already been removed.
 */
static bool elf_load_overlaps_initial_stacks(unsigned load_start,
    unsigned load_end){
  unsigned stack_end = USER_VMEM_END - (FRAME_SIZE - 1);
  unsigned stack_bytes = INITIAL_USER_STACK_SIZE + INITIAL_SIGNAL_STACK_SIZE;
  unsigned stack_start = stack_end - stack_bytes;

  return elf_ranges_overlap(load_start, load_end, stack_start, stack_end);
}

// Validate ELF headers and PT_LOAD ranges before mapping the image.
bool elf_validate_image(void* elf_image, unsigned image_size){
  struct ElfHeader header;
  unsigned ph_table_bytes = 0;
  bool entry_is_executable = false;

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

  if (header.e_ident[EI_VERSION] != EV_CURRENT ||
      header.e_ident[EI_OSABI] != ELFOSABI_SYSV ||
      header.e_ident[EI_ABIVERSION] != ELF_ABIVERSION_CURRENT){
    return false;
  }

  if (header.e_type != ET_EXEC){
    return false;
  }

  if (header.e_version != EV_CURRENT){
    return false;
  }

  if (header.e_ehsize != sizeof(struct ElfHeader)){
    return false;
  }

  if (header.e_machine != EM_DIOPTASE){
    return false;
  }

  // Program headers are user-controlled. Besides bounding the later loader's
  // VME allocations, this ceiling bounds the pairwise rounded-overlap loop
  // below. The in-tree producer emits three; ELF_MAX_PROGRAM_HEADERS leaves
  // substantial room for future segment kinds without allowing quadratic work
  // over the full 16-bit e_phnum domain.
  if (header.e_phnum == 0 ||
      header.e_phnum > ELF_MAX_PROGRAM_HEADERS ||
      header.e_phentsize != sizeof(struct ElfProgramHeader)){
    return false;
  }

  ph_table_bytes = (unsigned)header.e_phnum * (unsigned)header.e_phentsize;
  if (header.e_phnum != 0 &&
      ph_table_bytes / (unsigned)header.e_phentsize != (unsigned)header.e_phnum){
    return false;
  }

  // The program-header table is loader metadata, not part of the ELF header
  // itself. Requiring it to begin after e_ehsize also prevents a crafted table
  // from reinterpreting the identity bytes as mapping commands.
  if (header.e_phoff < header.e_ehsize ||
      !elf_header_bytes_in_range(image_size, header.e_phoff, ph_table_bytes)){
    return false;
  }

  if ((header.e_entry % DIOPTASE_INSTRUCTION_SIZE) != 0){
    return false;
  }

  for (unsigned i = 0; i < (unsigned)header.e_phnum; ++i){
    struct ElfProgramHeader ph;
    unsigned rounded_end = 0;
    elf_read_program_header(elf_image, &header, i, &ph);

    // Section metadata and other optional program-header kinds do not describe
    // mappings in this loader. Their payload fields are deliberately ignored,
    // and elf_load() applies the identical type filter.
    if (ph.p_type != PT_LOAD){
      continue;
    }

    if (ph.p_memsz < ph.p_filesz){
      return false;
    }

    if (!elf_header_bytes_in_range(image_size, ph.p_offset, ph.p_filesz)){
      return false;
    }

    if ((ph.p_flags & ~PF_KNOWN_MASK) != 0){
      return false;
    }

    // The repository's ELF producer describes every load with one VM page as
    // its alignment unit. mmap_at() independently requires p_vaddr itself to
    // be page aligned; accepting a contradictory p_align would describe a
    // format the Dioptase loader does not implement.
    if (ph.p_align != FRAME_SIZE ||
        (ph.p_vaddr % FRAME_SIZE) != 0 ||
        ph.p_vaddr < USER_VMEM_START){
      return false;
    }

    // The assembler intentionally emits a zero-sized rodata PT_LOAD when a
    // program has no rodata. It consumes no file or virtual-memory range and
    // may share p_vaddr with the following data segment, so it is valid but
    // excluded from range, overlap, and entry-point checks.
    if (ph.p_memsz == 0){
      continue;
    }

    if (!elf_load_memory_range(&ph, &rounded_end)){
      return false;
    }

    if (elf_load_overlaps_initial_stacks(ph.p_vaddr, rounded_end)){
      return false;
    }

    // mmap_at() treats page-rounded VMEs as the ownership unit. Compare the
    // rounded ranges rather than the logical byte ranges so two individually
    // valid headers can never reach mmap_at()'s overlap panic.
    for (unsigned j = 0; j < i; ++j){
      struct ElfProgramHeader prior;
      unsigned prior_rounded_end = 0;
      elf_read_program_header(elf_image, &header, j, &prior);

      if (prior.p_type != PT_LOAD || prior.p_memsz == 0){
        continue;
      }

      // Every earlier nonempty load header passed this helper on its own loop
      // iteration, so failure here would contradict the sequential validation
      // invariant. Return false defensively rather than trusting that state.
      if (!elf_load_memory_range(&prior, &prior_rounded_end)){
        return false;
      }

      if (elf_ranges_overlap(ph.p_vaddr, rounded_end,
          prior.p_vaddr, prior_rounded_end)){
        return false;
      }
    }

    // The architectural PC is four-byte aligned. Require all four bytes of
    // the first instruction to lie inside the logical executable segment, not
    // merely inside zero-filled page-rounding space.
    if ((ph.p_flags & PF_X) != 0 &&
        ph.p_memsz >= DIOPTASE_INSTRUCTION_SIZE &&
        header.e_entry >= ph.p_vaddr &&
        header.e_entry <= ph.p_vaddr + ph.p_memsz - DIOPTASE_INSTRUCTION_SIZE){
      entry_is_executable = true;
    }
  }

  return entry_is_executable;
}

/*
 * Install one validated, nonempty PT_LOAD into the current TCB. The temporary
 * writable permission is kernel-only construction state: after every file byte
 * is copied, vme_change_perms() installs exactly the validated ELF permissions
 * before run_user_program() can enter user mode.
 *
 * Returns false if fixed-address mapping fails. The caller must tear down the
 * in-construction address space; partially installed earlier segments are not
 * individually rolled back here.
 */
static bool program_header_load(void* elf_image, struct ElfProgramHeader* ph){
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

  if (vme == NULL){
    return false;
  }

  memcpy((void*)vaddr, (unsigned char*)elf_image + offset, filesz);

  vme_change_perms(vme, mmap_flags);
  return true;
}

/*
 * Load a previously validated image into the current TCB's user address space.
 * On failure returns false and may leave a partial VME list that the caller must
 * destroy with the whole address space.
 */
bool elf_load(void* elf_image, unsigned* entry_out){
  struct ElfHeader header;
  memcpy(&header, elf_image, sizeof(struct ElfHeader));

  for (unsigned i = 0; i < (unsigned)header.e_phnum; i++){
    struct ElfProgramHeader ph;
    elf_read_program_header(elf_image, &header, i, &ph);

    if (ph.p_type != PT_LOAD || ph.p_memsz == 0){
      continue;
    }

    if (!program_header_load(elf_image, &ph)){
      return false;
    }
  }

  *entry_out = header.e_entry;
  return true;
}
