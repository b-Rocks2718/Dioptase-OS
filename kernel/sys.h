#ifndef SYS_H
#define SYS_H

#include "constants.h"
#include "atomic.h"
#include "blocking_ringbuf.h"

enum TrapCode {
  TRAP_EXIT = 0,
  TRAP_TEST_SYSCALL = 1,
  TRAP_GET_CURRENT_JIFFIES = 2,
  TRAP_GET_KEY = 3,
  TRAP_SET_TILE_SCALE = 4,
  TRAP_SET_VSCROLL = 5,
  TRAP_SET_HSCROLL = 6,
  TRAP_LOAD_TEXT_TILES = 7,
  TRAP_CLEAR_SCREEN = 8,
  TRAP_GET_TILEMAP = 9,
  TRAP_GET_TILE_FB = 10,
  TRAP_GET_VGA_STATUS = 11,
  TRAP_GET_VGA_FRAME_COUNTER = 12,
  TRAP_SLEEP = 13,
  TRAP_OPEN = 14,
  TRAP_READ = 15,
  TRAP_WRITE = 16,
  TRAP_CLOSE = 17,
  TRAP_SEM_OPEN = 18,
  TRAP_SEM_UP = 19,
  TRAP_SEM_DOWN = 20,
  TRAP_SEM_CLOSE = 21,
  TRAP_MMAP = 22,
  TRAP_FORK = 23,
  TRAP_EXEC = 24,
  TRAP_PLAY_AUDIO = 25,
  TRAP_SET_TEXT_COLOR = 26,
  TRAP_WAIT_CHILD = 27,
  TRAP_CHDIR = 28,
  TRAP_PIPE = 29,
  TRAP_DUP = 30,
  TRAP_SEEK = 31,
  TRAP_YIELD = 32,



  TRAP_MOVE_VSCROLL = 36,
  TRAP_MOVE_HSCROLL = 37,
  TRAP_FD_BYTES_AVAILABLE = 38,
};

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define USER_MMAP_FLAGS_MASK 0x1F

#define MAX_FILE_DESCRIPTORS 100
#define MAX_SEM_DESCRIPTORS 100
#define MAX_CHILD_DESCRIPTORS 100

#define FILE_DESCRIPTORS_START 0
#define SEM_DESCRIPTORS_START 100
#define CHILD_DESCRIPTORS_START 200

struct TCB;
struct Node;
struct Semaphore;
struct Promise;

enum DescriptorType {
  DESCRIPTOR_FILE,
  DESCRIPTOR_SEM,
  DESCRIPTOR_CHILD,
};

enum FileDescriptorType {
  FILE_DESCRIPTOR_STDIN = 0,
  FILE_DESCRIPTOR_STDOUT = 1,
  FILE_DESCRIPTOR_STDERR = 2,
  FILE_DESCRIPTOR_PIPE_READ = 3,
  FILE_DESCRIPTOR_PIPE_WRITE = 4,
  FILE_DESCRIPTOR_NORMAL = 5,
};

struct FileDescriptor {
  struct Node* file; // pipe descriptors cast this to a (struct Pipe*)
  int offset;
  struct SpinLock offset_lock;
  enum FileDescriptorType type;
  int refcount;
};

struct SemDescriptor {
  struct Semaphore* sem;
  int refcount;
};

struct ChildDescriptor {
  struct Promise* child;
  int refcount;
};

struct Pipe {
  struct BlockingRingBuf buf;
  unsigned refcount;
};

// set up IVT with trap handler entry point
void trap_init(void);

// Enter user mode through rfe
// can pass in r1, r2 for use as either a return value or argc and argv
unsigned jump_to_user(unsigned entry, unsigned stack, unsigned r1, unsigned r2);

// run a user program given a node representing its ELF file
// consumes the node, so the caller cannot use it after calling this function
int run_user_program(struct Node* prog_node, int argc, char** argv);

// initialize descriptor tables for one TCB.
// If init_stdio is true, install stdin/stdout/stderr in slots 0..2.
// Kernel-only daemon threads that never enter the trap ABI can pass false so
// their descriptor tables stay empty and do not allocate unused stdio state.
void init_descriptors(struct TCB* tcb, bool init_stdio);

// find an unused descriptor of the given type in the TCB and return its index,
// or -1 if none are available
int allocate_descriptor(struct TCB* tcb, enum DescriptorType type, bool fill);

// copy all descriptors from one TCB to another, incrementing refcounts
void copy_descriptors(struct TCB* src, struct TCB* dst);

// deallocate descriptor and free its resources
void deallocate_descriptor(struct TCB* tcb, enum DescriptorType type, int index);

extern void trap_handler_(void);

// copy n bytes from either user -> kernel or kernel -> user
extern int copy_user(void* dest, void* src, unsigned n, struct TCB* cur_tcb);

#endif // SYS_H
