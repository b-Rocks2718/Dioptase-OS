/*
 * Dioptase ELF validation and loading regression test.
 *
 * This test constructs loader images directly so malformed input cannot be
 * normalized by the assembler before it reaches the kernel. It verifies:
 * - the assembler's empty rodata PT_LOAD is accepted and skipped by elf_load()
 * - non-PT_LOAD headers are ignored by validation and loading
 * - ELF identity/header/table and load-file bounds are rejected when malformed
 * - the implementation-defined program-header count limit accepts its exact
 *   boundary and rejects the next count before quadratic overlap work
 * - every nonempty load range is aligned, representable, in user memory, and
 *   non-overlapping after the VM layer's page rounding
 * - the entry PC is four-byte aligned and its full instruction lies in a
 *   nonempty executable load segment
 *
 * The valid fixture mirrors the assembler's text/empty-rodata/data layout. Its
 * empty rodata header deliberately has the same file offset and virtual address
 * as the following data header, which is safe only when zero-length headers do
 * not reserve VMEs.
 */

#include "../kernel/elf.h"
#include "../kernel/vmem.h"
#include "../kernel/physmem.h"
#include "../kernel/sys.h"
#include "../kernel/string.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/heap.h"

// The assembler starts text at the documented user boundary and places this
// fixture's data at the next 4 KiB page.
#define ELF_TEST_TEXT_VADDR 0x80000000
#define ELF_TEST_DATA_VADDR 0x80001000
#define ELF_TEST_NON_LOAD_TYPE 0x70000000
#define ELF_TEST_TEXT_WORD 0x00000000
#define ELF_TEST_DATA_WORD 0x12345678
#define ELF_TEST_PROGRAM_HEADER_COUNT 3
#define ELF_TEST_PAYLOAD_OFFSET (sizeof(struct ElfHeader) + 3 * sizeof(struct ElfProgramHeader))

struct ElfTestImage {
  struct ElfHeader header;
  struct ElfProgramHeader ph[ELF_TEST_PROGRAM_HEADER_COUNT];
  unsigned text_word;
  unsigned data_word;
};

// Reserve one header beyond the accepted limit so the rejection test has a
// complete in-range table. Without the explicit count limit, both fixtures
// would therefore be structurally valid rather than failing a file-bounds check.
struct ElfHeaderLimitImage {
  struct ElfHeader header;
  struct ElfProgramHeader ph[ELF_MAX_PROGRAM_HEADERS];
  struct ElfProgramHeader above_limit_ph;
  unsigned text_word;
};

static void elf_test_require(bool condition, char* message){
  if (!condition){
    panic(message);
  }
}

static void elf_test_set_load(struct ElfProgramHeader* ph, unsigned offset,
    unsigned vaddr, unsigned filesz, unsigned memsz, unsigned flags){
  ph->p_type = PT_LOAD;
  ph->p_offset = offset;
  ph->p_vaddr = vaddr;
  ph->p_paddr = 0;
  ph->p_filesz = filesz;
  ph->p_memsz = memsz;
  ph->p_flags = flags;
  ph->p_align = FRAME_SIZE;
}

static void elf_test_init_valid(struct ElfTestImage* image){
  memset(image, 0, sizeof(struct ElfTestImage));

  image->header.e_ident[0] = 0x7F;
  image->header.e_ident[1] = 'E';
  image->header.e_ident[2] = 'L';
  image->header.e_ident[3] = 'F';
  image->header.e_ident[EI_CLASS] = ELFCLASS32;
  image->header.e_ident[EI_DATA] = ELFDATA2LSB;
  image->header.e_ident[EI_VERSION] = EV_CURRENT;
  image->header.e_ident[EI_OSABI] = ELFOSABI_SYSV;
  image->header.e_ident[EI_ABIVERSION] = ELF_ABIVERSION_CURRENT;
  image->header.e_type = ET_EXEC;
  image->header.e_machine = EM_DIOPTASE;
  image->header.e_version = EV_CURRENT;
  image->header.e_entry = ELF_TEST_TEXT_VADDR;
  image->header.e_phoff = sizeof(struct ElfHeader);
  image->header.e_ehsize = sizeof(struct ElfHeader);
  image->header.e_phentsize = sizeof(struct ElfProgramHeader);
  image->header.e_phnum = ELF_TEST_PROGRAM_HEADER_COUNT;

  elf_test_set_load(&image->ph[0], ELF_TEST_PAYLOAD_OFFSET,
    ELF_TEST_TEXT_VADDR, sizeof(unsigned), sizeof(unsigned), PF_R | PF_X);

  // The empty rodata header and data header intentionally share their start.
  elf_test_set_load(&image->ph[1],
    ELF_TEST_PAYLOAD_OFFSET + sizeof(unsigned), ELF_TEST_DATA_VADDR,
    0, 0, PF_R);

  elf_test_set_load(&image->ph[2],
    ELF_TEST_PAYLOAD_OFFSET + sizeof(unsigned), ELF_TEST_DATA_VADDR,
    sizeof(unsigned), sizeof(unsigned), PF_R | PF_W);

  image->text_word = ELF_TEST_TEXT_WORD;
  image->data_word = ELF_TEST_DATA_WORD;
}

static void elf_test_init_header_limit(struct ElfHeaderLimitImage* image,
    unsigned header_count){
  memset(image, 0, sizeof(struct ElfHeaderLimitImage));

  image->header.e_ident[0] = 0x7F;
  image->header.e_ident[1] = 'E';
  image->header.e_ident[2] = 'L';
  image->header.e_ident[3] = 'F';
  image->header.e_ident[EI_CLASS] = ELFCLASS32;
  image->header.e_ident[EI_DATA] = ELFDATA2LSB;
  image->header.e_ident[EI_VERSION] = EV_CURRENT;
  image->header.e_ident[EI_OSABI] = ELFOSABI_SYSV;
  image->header.e_ident[EI_ABIVERSION] = ELF_ABIVERSION_CURRENT;
  image->header.e_type = ET_EXEC;
  image->header.e_machine = EM_DIOPTASE;
  image->header.e_version = EV_CURRENT;
  image->header.e_entry = ELF_TEST_TEXT_VADDR;
  image->header.e_phoff = sizeof(struct ElfHeader);
  image->header.e_ehsize = sizeof(struct ElfHeader);
  image->header.e_phentsize = sizeof(struct ElfProgramHeader);
  image->header.e_phnum = header_count;

  // One executable load is sufficient for a valid entry point. All remaining
  // zeroed entries are non-PT_LOAD metadata and must be ignored.
  elf_test_set_load(&image->ph[0],
    sizeof(struct ElfHeaderLimitImage) - sizeof(unsigned),
    ELF_TEST_TEXT_VADDR, sizeof(unsigned), sizeof(unsigned), PF_R | PF_X);
  image->text_word = ELF_TEST_TEXT_WORD;
}

static void elf_test_expect_invalid(struct ElfTestImage* image, char* message){
  elf_test_require(!elf_validate_image(image, sizeof(struct ElfTestImage)),
    message);
}

static void elf_test_load_and_verify(struct ElfTestImage* image){
  elf_test_require(elf_validate_image(image, sizeof(struct ElfTestImage)),
    "elf validation test: valid empty-segment fixture was rejected.\n");

  unsigned entry = 0;
  elf_test_require(elf_load(image, &entry),
    "elf validation test: loader rejected a validated empty-segment fixture.\n");
  elf_test_require(entry == ELF_TEST_TEXT_VADDR,
    "elf validation test: loader returned the wrong entry address.\n");
  elf_test_require(*(unsigned*)ELF_TEST_TEXT_VADDR == ELF_TEST_TEXT_WORD,
    "elf validation test: loader copied the wrong text word.\n");
  elf_test_require(*(unsigned*)ELF_TEST_DATA_VADDR == ELF_TEST_DATA_WORD,
    "elf validation test: loader copied the wrong data word.\n");

  munmap((void*)ELF_TEST_TEXT_VADDR);
  munmap((void*)ELF_TEST_DATA_VADDR);
}

static void elf_test_empty_and_non_load_headers(struct ElfTestImage* image){
  elf_test_init_valid(image);
  elf_test_load_and_verify(image);

  // A non-load header's mapping fields are irrelevant and must never reach
  // mmap_at(), even when they would be invalid for a load header.
  elf_test_init_valid(image);
  image->ph[1].p_type = ELF_TEST_NON_LOAD_TYPE;
  image->ph[1].p_offset = UINT_MAX;
  image->ph[1].p_vaddr = 1;
  image->ph[1].p_filesz = UINT_MAX;
  image->ph[1].p_memsz = 0;
  image->ph[1].p_flags = UINT_MAX;
  elf_test_load_and_verify(image);
}

static void elf_test_header_validation(struct ElfTestImage* image){
  elf_test_init_valid(image);
  image->header.e_ident[0] = 0;
  elf_test_expect_invalid(image,
    "elf validation test: bad ELF magic was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_ident[EI_CLASS] = ELFCLASS64;
  elf_test_expect_invalid(image,
    "elf validation test: non-ELF32 image was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_ident[EI_DATA] = ELFDATA2MSB;
  elf_test_expect_invalid(image,
    "elf validation test: non-little-endian image was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_ident[EI_VERSION] = 0;
  elf_test_expect_invalid(image,
    "elf validation test: stale identity version was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_ident[EI_OSABI] = 1;
  elf_test_expect_invalid(image,
    "elf validation test: unsupported OS ABI was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_ident[EI_ABIVERSION] = 1;
  elf_test_expect_invalid(image,
    "elf validation test: unsupported ABI version was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_type = 1;
  elf_test_expect_invalid(image,
    "elf validation test: non-executable ELF type was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_machine = 0;
  elf_test_expect_invalid(image,
    "elf validation test: wrong machine identifier was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_version = 0;
  elf_test_expect_invalid(image,
    "elf validation test: stale ELF header version was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_ehsize = sizeof(struct ElfHeader) - 1;
  elf_test_expect_invalid(image,
    "elf validation test: wrong ELF header size was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_phentsize = sizeof(struct ElfProgramHeader) - 1;
  elf_test_expect_invalid(image,
    "elf validation test: wrong program-header size was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_phnum = 0;
  elf_test_expect_invalid(image,
    "elf validation test: image without program headers was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_phoff = sizeof(struct ElfHeader) - sizeof(unsigned);
  elf_test_expect_invalid(image,
    "elf validation test: program table overlapping the ELF header was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_phnum = 0xFFFF;
  elf_test_expect_invalid(image,
    "elf validation test: oversized program-header table was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_phoff = UINT_MAX - sizeof(struct ElfProgramHeader);
  elf_test_expect_invalid(image,
    "elf validation test: wrapping program-header offset was accepted.\n");
}

static void elf_test_program_header_limit(void){
  // Keep this fixture off the kernel stack: 64 program headers already exceed
  // the assembler's signed 12-bit frame immediate when allocated as a local.
  struct ElfHeaderLimitImage* image = malloc(sizeof(struct ElfHeaderLimitImage));
  elf_test_require(image != NULL,
    "elf validation test: failed to allocate program-header limit fixture.\n");

  elf_test_init_header_limit(image, ELF_MAX_PROGRAM_HEADERS);
  elf_test_require(elf_validate_image(image,
      sizeof(struct ElfHeaderLimitImage)),
    "elf validation test: exact program-header count limit was rejected.\n");

  elf_test_init_header_limit(image, ELF_MAX_PROGRAM_HEADERS + 1);
  elf_test_require(!elf_validate_image(image,
      sizeof(struct ElfHeaderLimitImage)),
    "elf validation test: program-header count above the limit was accepted.\n");

  free(image);
}

static void elf_test_load_range_validation(struct ElfTestImage* image){
  elf_test_init_valid(image);
  image->ph[0].p_filesz = sizeof(unsigned) + 1;
  image->ph[0].p_memsz = sizeof(unsigned);
  elf_test_expect_invalid(image,
    "elf validation test: PT_LOAD filesz greater than memsz was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_offset = sizeof(struct ElfTestImage) - 1;
  elf_test_expect_invalid(image,
    "elf validation test: PT_LOAD payload outside the file was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_flags |= 0x8;
  elf_test_expect_invalid(image,
    "elf validation test: unknown PT_LOAD permission bit was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_align = 1;
  elf_test_expect_invalid(image,
    "elf validation test: unsupported PT_LOAD alignment was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_vaddr = ELF_TEST_TEXT_VADDR + sizeof(unsigned);
  image->header.e_entry = image->ph[0].p_vaddr;
  elf_test_expect_invalid(image,
    "elf validation test: non-page-aligned PT_LOAD address was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_vaddr = USER_VMEM_START - FRAME_SIZE;
  image->header.e_entry = image->ph[0].p_vaddr;
  elf_test_expect_invalid(image,
    "elf validation test: kernel-range PT_LOAD address was accepted.\n");

  elf_test_init_valid(image);
  image->ph[2].p_vaddr = UINT_MAX - (FRAME_SIZE - 1);
  elf_test_expect_invalid(image,
    "elf validation test: unrepresentable top user page was accepted.\n");

  elf_test_init_valid(image);
  image->ph[2].p_vaddr =
    UINT_MAX - (FRAME_SIZE - 1) -
    INITIAL_USER_STACK_SIZE - INITIAL_SIGNAL_STACK_SIZE;
  elf_test_expect_invalid(image,
    "elf validation test: PT_LOAD consuming the initial stack reservation was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_memsz = UINT_MAX;
  elf_test_expect_invalid(image,
    "elf validation test: wrapping PT_LOAD size was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_memsz = FRAME_SIZE + 1;
  elf_test_expect_invalid(image,
    "elf validation test: page-rounded overlapping PT_LOADs were accepted.\n");
}

static void elf_test_entry_validation(struct ElfTestImage* image){
  elf_test_init_valid(image);
  image->header.e_entry = ELF_TEST_TEXT_VADDR + 2;
  elf_test_expect_invalid(image,
    "elf validation test: misaligned entry PC was accepted.\n");

  elf_test_init_valid(image);
  image->header.e_entry = ELF_TEST_TEXT_VADDR + FRAME_SIZE;
  elf_test_expect_invalid(image,
    "elf validation test: entry outside executable memory was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_memsz = sizeof(unsigned) + 1;
  image->header.e_entry = ELF_TEST_TEXT_VADDR + sizeof(unsigned);
  elf_test_expect_invalid(image,
    "elf validation test: partial entry instruction was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_flags = PF_R;
  elf_test_expect_invalid(image,
    "elf validation test: entry in non-executable memory was accepted.\n");

  elf_test_init_valid(image);
  image->ph[0].p_flags = PF_R;
  image->ph[1].p_flags = PF_R | PF_X;
  image->header.e_entry = ELF_TEST_DATA_VADDR;
  elf_test_expect_invalid(image,
    "elf validation test: entry in an empty executable segment was accepted.\n");
}

int kernel_main(void){
  struct ElfTestImage image;

  say("***elf validation test start\n", NULL);

  elf_test_empty_and_non_load_headers(&image);
  say("***elf empty and non-load handling ok\n", NULL);

  elf_test_header_validation(&image);
  say("***elf header validation ok\n", NULL);

  elf_test_program_header_limit();
  say("***elf program-header count limit ok\n", NULL);

  elf_test_load_range_validation(&image);
  say("***elf load range validation ok\n", NULL);

  elf_test_entry_validation(&image);
  say("***elf entry validation ok\n", NULL);

  say("***elf validation test complete\n", NULL);
  return 0;
}
