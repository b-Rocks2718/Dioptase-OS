#ifndef SYS_H
#define SYS_H

#include "constants.h"
#include "atomic.h"
#include "blocking_ringbuf.h"
#include "blocking_lock.h"

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
  TRAP_GETDENTS = 33,
  TRAP_GETCWD = 34,
  TRAP_READLINK = 35,
  TRAP_MOVE_VSCROLL = 36,
  TRAP_MOVE_HSCROLL = 37,
  TRAP_FD_BYTES_AVAILABLE = 38,
  TRAP_TRUNCATE = 39,
  TRAP_MKDIR = 40,
  TRAP_RMDIR = 41,
  TRAP_UNLINK = 42,
  TRAP_SET_SPRITE_SCALE = 43,
  TRAP_SET_SPRITE_COORDS = 44,
  TRAP_LOAD_TEXT_TILES_COLORED = 45,
  TRAP_GET_SPRITEMAP = 46,
  TRAP_SIGNAL_CHILD = 47,
  TRAP_REQUEST_PRIORITY = 49,
  TRAP_SET_FOREGROUND_CHILD = 50,
  TRAP_SIGNAL_FOREGROUND = 51,
  TRAP_REGISTER_HANDLER = 52,
  TRAP_SIGRETURN = 53,
  TRAP_MASK_SIGNAL = 54,
  TRAP_UNMASK_SIGNAL = 55,
  // Code 48 belonged to the removed synthetic-audio trap. Preserve that
  // retired ABI slot; new public traps extend the documented tail instead of
  // making an old code name a different operation.
  TRAP_OPEN_EXISTING = 56,
};

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// The trap accepts exactly the sharing bit plus the three user-visible
// protection bits from root/crt/sys/mman.h. Bit 1 is intentionally absent;
// silently masking an unknown flag would turn a malformed request into a
// different valid mapping.
#define USER_MMAP_ALLOWED_FLAGS 0x1D
#define USER_MMAP_ANON_FD (-1)

// A new user image receives one 16 KiB ordinary stack and one 4 KiB signal
// stack. ELF validation also uses these shared constants to keep the exact
// top-down reservation needed by run_user_program() free of PT_LOAD mappings.
#define INITIAL_USER_STACK_SIZE 0x4000
#define INITIAL_SIGNAL_STACK_SIZE 0x1000

#define MAX_FILE_DESCRIPTORS 100
#define MAX_SEM_DESCRIPTORS 100
#define MAX_CHILD_DESCRIPTORS 100

#define FILE_DESCRIPTORS_START 0
#define SEM_DESCRIPTORS_START 100
#define CHILD_DESCRIPTORS_START 200

// Signals [0, MAX_MASKABLE_SIGNAL) can register handlers and be masked.
#define SIGNAL_HELLO 0
#define SIGNAL_TERMINATE 1

#define MAX_MASKABLE_SIGNAL 16

// can register handlers for, but cannot mask
#define SIGNAL_SEG 16
#define SIGNAL_ILL 17
#define SIGNAL_ALGN 18

// cannot mask or register a handler for
#define SIGNAL_KILL 31

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
  struct BlockingLock offset_lock;
  enum FileDescriptorType type;
  int refcount;
};

struct SemDescriptor {
  struct Semaphore* sem;
  int refcount;
};

struct ChildDescriptor {
  // A non-NULL child_tcb observed while holding state_lock names a live TCB.
  // Every exit path clears it under this lock before that TCB reaches reaper.
  // display_claimed is also protected by state_lock and remains valid after
  // child_tcb is cleared so the foreground owner can recover the display.
  struct CLHLock state_lock;
  struct TCB* child_tcb;
  struct Promise* child_promise;
  bool display_claimed;
  int refcount;
};

// Drop one descriptor reference.  The caller must not hold state_lock because
// the final release destroys it.
void child_descriptor_release(struct ChildDescriptor* descriptor);

struct Pipe {
  struct BlockingRingBuf buf;

  /*
   * These counts describe heap-allocated FileDescriptor endpoint objects,
   * not descriptor-table slots. dup() and fork() share an existing object and
   * increment FileDescriptor.refcount; only that object's final release drops
   * one count here. The initial pipe owns exactly one read object and one
   * write object.
   *
   * Sequentially-consistent atomic RMWs order last-side close publication
   * before the final endpoint_objects release. Therefore the thread that
   * observes endpoint_objects == 1 may destroy `buf`: any operation that is
   * still blocked/running necessarily retains an endpoint descriptor object.
   */
  int endpoint_objects;
  int read_endpoint_objects;
  int write_endpoint_objects;
};

// set up IVT with trap handler entry point
void trap_init(void);

// clean up any resources used by the trap handler
void trap_destroy(void);

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
// Copy the fork-entry descriptor snapshot. parent_only_child_desc names the
// handle allocated by this fork for its parent and is deliberately excluded.
void copy_descriptors(struct TCB* src, struct TCB* dst,
  int parent_only_child_desc);

// deallocate descriptor and free its resources
void deallocate_descriptor(struct TCB* tcb, enum DescriptorType type, int index);

extern void trap_handler_(void);

// copy n bytes from either user -> kernel or kernel -> user
extern int copy_user(void* dest, void* src, unsigned n, struct TCB* cur_tcb);

#endif // SYS_H
