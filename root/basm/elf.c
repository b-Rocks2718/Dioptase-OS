#include "elf.h"

#include "../crt/assert.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/stddef.h"
#include "../crt/stdint.h"
#include "../crt/string.h"
#include "../crt/unistd.h"

static void write_u16_le(uint8_t* buf, size_t* offset, uint16_t value){
  buf[*offset] = (uint8_t)value;
  buf[*offset + 1] = (uint8_t)(value >> 8);
  *offset += 2;
}

static void write_u32_le(uint8_t* buf, size_t* offset, uint32_t value){
  buf[*offset] = (uint8_t)value;
  buf[*offset + 1] = (uint8_t)(value >> 8);
  buf[*offset + 2] = (uint8_t)(value >> 16);
  buf[*offset + 3] = (uint8_t)(value >> 24);
  *offset += 4;
}

static void write_all(int file, uint8_t* bytes, size_t len){
  size_t offset = 0;
  while (offset < len){
    int wrote = write(file, bytes + offset, len - offset);
    assert(wrote >= 0, "elf write failed");
    offset += (size_t)wrote;
  }
}

static void fprint_word_bytes(int file, uint8_t* bytes, size_t len){
  assert(len % 4 == 0, "ELF word dump requires 4-byte alignment");
  for (size_t i = 0; i < len; i += 4){
    uint32_t word = (uint32_t)bytes[i]
      | ((uint32_t)bytes[i + 1] << 8)
      | ((uint32_t)bytes[i + 2] << 16)
      | ((uint32_t)bytes[i + 3] << 24);
    int args[1];
    args[0] = word;
    fdprintf(file, "%08X\n", args);
  }
}

// Purpose: Write a byte buffer directly to a file.
// Inputs: ptr is the output file; bytes points to the payload; len is the byte count.
// Outputs: Writes len bytes to ptr.
// Invariants/Assumptions: ptr is open for binary output.
static void fwrite_bytes(int file, uint8_t* bytes, size_t len){
  write_all(file, bytes, len);
}

void destroy_program_descriptor(struct ProgramDescriptor* program){
  destroy_instruction_array_list(program->sections);
  free(program);
}

struct ElfHeader create_elf_header(struct ProgramDescriptor* program){
  struct ElfHeader header;

  // ELF magic number
  header.e_ident[0] = 0x7f;
  header.e_ident[1] = 'E';
  header.e_ident[2] = 'L';
  header.e_ident[3] = 'F';

  // class: 32 bit
  header.e_ident[4] = 1;

  // data: little endian
  header.e_ident[5] = 1;

  // version
  header.e_ident[6] = 1;

  // OS ABI: System V
  header.e_ident[7] = 0;

  // ABI version
  header.e_ident[8] = 0;

  // padding
  for (int i = 9; i < 16; ++i) header.e_ident[i] = 0;

  header.e_type = 2; // executable file
  header.e_machine = 0xD105; // Dioptase machine type
  header.e_version = 1;
  header.e_entry = program->entry_point;
  header.e_phoff = sizeof(struct ElfHeader); // program header table offset
  header.e_shoff = 0; // no section headers
  header.e_flags = 0;
  header.e_ehsize = sizeof(struct ElfHeader);
  header.e_phentsize = sizeof(struct ElfProgramHeader);
  header.e_phnum = 3; // text, rodata, data
  header.e_shentsize = 0; // no section headers
  header.e_shnum = 0; // no section headers
  header.e_shstrndx = 0; // no section headers

  return header;
}

struct ElfProgramHeader* create_PHT(struct ProgramDescriptor* program){
  struct ElfProgramHeader* pht = malloc(3 * sizeof(struct ElfProgramHeader));

  uint32_t offset = sizeof(struct ElfHeader) + 3 * sizeof(struct ElfProgramHeader);
  uint32_t vaddr = 0x80000000; // load at address 0x80000000
  struct InstructionArray* cur_section = program->sections->head;

  // text segment
  size_t text_size = cur_section->size * 4;
  pht[0] = create_text_program_header(offset, vaddr, text_size);
  offset += text_size;

  // round vaddr to next page
  vaddr += text_size;
  if (vaddr % 0x1000 != 0) vaddr += 0x1000 - (vaddr % 0x1000);
  cur_section = cur_section->next;

  // rodata segment
  size_t rodata_size = cur_section->size * 4;
  pht[1] = create_rodata_program_header(offset, vaddr, rodata_size);
  offset += rodata_size;

  vaddr += rodata_size;
  if (vaddr % 0x1000 != 0) vaddr += 0x1000 - (vaddr % 0x1000);
  cur_section = cur_section->next;

  // data segment
  size_t data_size = cur_section->size * 4;
  pht[2] = create_data_program_header(offset, vaddr, data_size, data_size + program->bss_size);

  return pht;
}

struct ElfProgramHeader create_text_program_header(uint32_t offset, uint32_t vaddr, uint32_t filesz){
  struct ElfProgramHeader ph;

  ph.p_type = 1; // loadable segment
  ph.p_offset = offset;
  ph.p_vaddr = vaddr;
  ph.p_paddr = 0; // physical address not used
  ph.p_filesz = filesz;
  ph.p_memsz = filesz;
  ph.p_flags = 5; // read + execute
  ph.p_align = 0x1000; // page aligned

  return ph;
}

struct ElfProgramHeader create_rodata_program_header(uint32_t offset, uint32_t vaddr, uint32_t filesz){
  struct ElfProgramHeader ph;

  ph.p_type = 1; // loadable segment
  ph.p_offset = offset;
  ph.p_vaddr = vaddr;
  ph.p_paddr = 0; // physical address not used
  ph.p_filesz = filesz;
  ph.p_memsz = filesz;
  ph.p_flags = 4; // read only
  ph.p_align = 0x1000; // page aligned

  return ph;
}

struct ElfProgramHeader create_data_program_header(uint32_t offset, uint32_t vaddr, uint32_t filesz, uint32_t memsz){
  struct ElfProgramHeader ph;

  ph.p_type = 1; // loadable segment
  ph.p_offset = offset;
  ph.p_vaddr = vaddr;
  ph.p_paddr = 0; // physical address not used
  ph.p_filesz = filesz;
  ph.p_memsz = memsz;
  ph.p_flags = 6; // read + write
  ph.p_align = 0x1000; // page aligned

  return ph;
}

void fprint_elf_header(int file, struct ElfHeader* header){
  uint8_t bytes[52];
  size_t offset = 0;

  memcpy(bytes, header->e_ident, sizeof(header->e_ident));
  offset += sizeof(header->e_ident);

  write_u16_le(bytes, &offset, header->e_type);
  write_u16_le(bytes, &offset, header->e_machine);
  write_u32_le(bytes, &offset, header->e_version);
  write_u32_le(bytes, &offset, header->e_entry);
  write_u32_le(bytes, &offset, header->e_phoff);
  write_u32_le(bytes, &offset, header->e_shoff);
  write_u32_le(bytes, &offset, header->e_flags);
  write_u16_le(bytes, &offset, header->e_ehsize);
  write_u16_le(bytes, &offset, header->e_phentsize);
  write_u16_le(bytes, &offset, header->e_phnum);
  write_u16_le(bytes, &offset, header->e_shentsize);
  write_u16_le(bytes, &offset, header->e_shnum);
  write_u16_le(bytes, &offset, header->e_shstrndx);

  assert(offset == sizeof(bytes), "ELF header size mismatch while formatting");
  fprint_word_bytes(file, bytes, sizeof(bytes));
}

void fprint_pht(int file, struct ElfProgramHeader* pht){
  for (int i = 0; i < 3; ++i){
    uint8_t bytes[32];
    size_t offset = 0;

    write_u32_le(bytes, &offset, pht[i].p_type);
    write_u32_le(bytes, &offset, pht[i].p_offset);
    write_u32_le(bytes, &offset, pht[i].p_vaddr);
    write_u32_le(bytes, &offset, pht[i].p_paddr);
    write_u32_le(bytes, &offset, pht[i].p_filesz);
    write_u32_le(bytes, &offset, pht[i].p_memsz);
    write_u32_le(bytes, &offset, pht[i].p_flags);
    write_u32_le(bytes, &offset, pht[i].p_align);

    assert(offset == sizeof(bytes), "PHT entry size mismatch while formatting");
    fprint_word_bytes(file, bytes, sizeof(bytes));
  }
}

void fwrite_elf_header(int file, struct ElfHeader* header){
  uint8_t bytes[52];
  size_t offset = 0;

  memcpy(bytes, header->e_ident, sizeof(header->e_ident));
  offset += sizeof(header->e_ident);

  write_u16_le(bytes, &offset, header->e_type);
  write_u16_le(bytes, &offset, header->e_machine);
  write_u32_le(bytes, &offset, header->e_version);
  write_u32_le(bytes, &offset, header->e_entry);
  write_u32_le(bytes, &offset, header->e_phoff);
  write_u32_le(bytes, &offset, header->e_shoff);
  write_u32_le(bytes, &offset, header->e_flags);
  write_u16_le(bytes, &offset, header->e_ehsize);
  write_u16_le(bytes, &offset, header->e_phentsize);
  write_u16_le(bytes, &offset, header->e_phnum);
  write_u16_le(bytes, &offset, header->e_shentsize);
  write_u16_le(bytes, &offset, header->e_shnum);
  write_u16_le(bytes, &offset, header->e_shstrndx);

  assert(offset == sizeof(bytes), "ELF header size mismatch while writing");
  fwrite_bytes(file, bytes, sizeof(bytes));
}

void fwrite_pht(int file, struct ElfProgramHeader* pht){
  for (int i = 0; i < 3; ++i){
    uint8_t bytes[32];
    size_t offset = 0;

    write_u32_le(bytes, &offset, pht[i].p_type);
    write_u32_le(bytes, &offset, pht[i].p_offset);
    write_u32_le(bytes, &offset, pht[i].p_vaddr);
    write_u32_le(bytes, &offset, pht[i].p_paddr);
    write_u32_le(bytes, &offset, pht[i].p_filesz);
    write_u32_le(bytes, &offset, pht[i].p_memsz);
    write_u32_le(bytes, &offset, pht[i].p_flags);
    write_u32_le(bytes, &offset, pht[i].p_align);

    assert(offset == sizeof(bytes), "PHT entry size mismatch while writing");
    fwrite_bytes(file, bytes, sizeof(bytes));
  }
}
