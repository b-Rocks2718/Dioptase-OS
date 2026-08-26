#include "sys.h"
#include "interrupts.h"
#include "debug.h"
#include "print.h"
#include "ivt.h"
#include "constants.h"
#include "vmem.h"
#include "elf.h"
#include "pit.h"
#include "vga.h"
#include "ps2.h"
#include "threads.h"
#include "per_core.h"
#include "promise.h"
#include "audio.h"
#include "heap.h"
#include "ext.h"
#include "string.h"
#include "scheduler.h"

#define SYSCALL_MAX_PATH_BYTES 1024
#define SYSCALL_MAX_IO_BYTES 1024
#define EXEC_MAX_ARGC 64
#define EXEC_MAX_ARG_BYTES 256
#define USER_INSTRUCTION_BYTES 4

#define PIPE_BUFFER_CAPACITY 1024
#define PIPE_DESCRIPTOR_COUNT 2
#define PIPE_INITIAL_SIDE_ENDPOINT_OBJECTS 1
#define MAX_GETDENTS_BUFFER_SIZE 1024

// Single-terminal foreground control state.
static struct BlockingLock foreground_child_lock;
static struct ChildDescriptor* foreground_child = NULL;

// Return false if any slash-delimited path component exceeds ext2's basename
// limit. Empty components produced by repeated or leading slashes are allowed
// here and retain node_find()/open()'s existing normalization behavior.
//
// This check runs on the bounded kernel copy of a user path before open starts
// creating missing parents. Consequently an overlong later component cannot
// leave earlier directories allocated as a partial side effect.
static bool ext2_path_component_lengths_valid(char* path){
  unsigned component_bytes = 0;

  for (unsigned i = 0; path[i] != '\0'; ++i){
    if (path[i] == '/'){
      component_bytes = 0;
      continue;
    }

    if (component_bytes == EXT2_MAX_NAME_BYTES){
      return false;
    }
    component_bytes += 1;
  }

  return true;
}

static unsigned trap_test_syscall_handler(int arg){
  say("***test_syscall arg = %d\n", &arg);
  return arg + 7;
}

// Validate the user-visible side of a syscall copy before touching memory.
//
// Preconditions:
// - `tcb` is the current thread whose VME list defines the active user address
//   space.
// - `required_flags` contains MMAP_READ for kernel reads from user memory or
//   MMAP_WRITE for kernel writes to user memory, and/or MMAP_EXEC for a user
//   address that the kernel will install as a future program counter.
//
// Postconditions:
// - Returns true only if every byte in [user_ptr, user_ptr + n) is in the user
//   virtual half and covered by user VMEs with the requested permissions.
// - Rejects low kernel/physical aliases before `copy_user` can dereference
//   them in kernel mode.
static bool user_range_ok(struct TCB* tcb, void* user_ptr, unsigned n,
    unsigned required_flags){
  if (n == 0){
    return true;
  }

  unsigned start = (unsigned)user_ptr;
  if (start < USER_VMEM_START){
    // check start is in user space
    return false;
  }

  if ((n - 1) > (USER_VMEM_END - start)){
    // check end is in user space (have to write this way to avoid overflow)
    return false;
  }

  unsigned last = start + n - 1;
  unsigned cur = start;
  while (true){
    // check that the current byte is covered by a user VME

    struct VME* vme = tcb->vme_list;
    while (vme != NULL && vme->end <= cur){
      // find vme covering the current byte
      vme = vme->next;
    }

    if (vme == NULL || vme->start > cur || !(vme->flags & MMAP_USER)){
      // no VME covers the current byte or it is not a user mapping
      return false;
    }

    if ((required_flags & MMAP_READ) && !(vme->flags & MMAP_READ)){
      // required read permission is not present in this VME
      return false;
    }
    if ((required_flags & MMAP_WRITE) && !(vme->flags & MMAP_WRITE)){
      // required write permission is not present in this VME
      return false;
    }
    if ((required_flags & MMAP_EXEC) && !(vme->flags & MMAP_EXEC)){
      // required execute permission is not present in this VME
      return false;
    }

    if ((vme->end - 1) >= last){
      // the current VME covers the last byte in the requested range
      return true;
    }

    cur = vme->end;
  }
}

// validate that a user memory range is safe to copy from
// if so perform the copy, otherwise return -1
static int copy_from_user(void* dest, void* src, unsigned n, struct TCB* tcb){
  if (!user_range_ok(tcb, src, n, MMAP_READ)){
    return -1;
  }

  return copy_user(dest, src, n, tcb);
}

// validate that a user memory range is safe to copy to
// if so perform the copy, otherwise return -1
static int copy_to_user(void* dest, void* src, unsigned n, struct TCB* tcb){
  if (!user_range_ok(tcb, dest, n, MMAP_WRITE)){
    return -1;
  }

  return copy_user(dest, src, n, tcb);
}

static int copy_cstr_from_user(char* dest, char* src, unsigned max,
    struct TCB* tcb){
  if (max == 0){
    return -1;
  }

  for (unsigned i = 0; i < max; i++){
    char c;
    if ((unsigned)src > UINT_MAX - i){
      dest[0] = '\0';
      return -1;
    }

    if (copy_from_user(&c, (void*)((unsigned)src + i), 1, tcb) != 0){
      dest[0] = '\0';
      return -1;
    }

    dest[i] = c;
    if (c == '\0'){
      return 0;
    }
  }

  dest[max - 1] = '\0';
  return -1;
}

// Free a kernel snapshot of exec argv strings
static void free_exec_argv(int argc, char** kargv){
  if (kargv == NULL){
    return;
  }

  for (int i = 0; i < argc; i++){
    free(kargv[i]);
  }
  free(kargv);
}

// Declared early so handle_exec can stage a transactional address-space swap.
static int construct_user_program_from_image(void* image, unsigned image_size,
    int argc, char** argv, unsigned* entry_out, unsigned* initial_sp_out,
    unsigned* user_argv_out);

// Compute how much of the initial user stack exec argv will occupy.
// returns -1 on failure
static int exec_argv_stack_bytes(int argc, char** kargv,
    unsigned* required_bytes){
  if (argc < 0 || argc > EXEC_MAX_ARGC){
    return -1;
  }

  unsigned total = sizeof(unsigned);
  for (int i = 0; i < argc; i++){
    unsigned len = strlen(kargv[i]);
    if (len >= EXEC_MAX_ARG_BYTES){
      return -1;
    }

    len++; // count the NUL terminator

    unsigned aligned_len = (len + 3) & ~3;
    total += aligned_len;
  }

  // C process entry requires argv[argc] == NULL. Account for that sentinel in
  // the same checked stack-capacity calculation as the argument pointers.
  unsigned argv_entries = (unsigned)argc + 1;
  unsigned argv_bytes = argv_entries * sizeof(char*);
  unsigned aligned_argv_bytes = (argv_bytes + 3) & ~3;
  total += aligned_argv_bytes;

  if (total > INITIAL_USER_STACK_SIZE){
    return -1;
  }

  *required_bytes = total;
  return 0;
}

// Snapshot exec argv out of the caller's current user address space before the
// current image is torn down.
// - On success, `*out_kargv` owns a kernel heap snapshot of the argument vector
//   that remains valid after the old address space is destroyed.
// - Returns -1 for any invalid user pointer, oversized argument, or snapshot
//   that would not fit back into the initial user stack of the new image.
static int copy_exec_argv_from_user(char*** out_kargv, int argc, char** argv,
    struct TCB* tcb){
  *out_kargv = NULL;

  if (argc < 0 || argc > EXEC_MAX_ARGC){
    return -1;
  }

  if (argc == 0){
    return 0;
  }

  if (argv == NULL){
    return -1;
  }

  // Keep a kernel-side sentinel as part of the snapshot contract. We do not
  // trust or copy a caller-provided argv[argc]; the kernel constructs NULL.
  unsigned argv_entries = (unsigned)argc + 1;
  char** kargv = malloc(sizeof(char*) * argv_entries);
  memset(kargv, 0, sizeof(char*) * argv_entries);

  unsigned argv_addr = (unsigned)argv;
  for (int i = 0; i < argc; i++){
    unsigned entry_offset = (unsigned)i * sizeof(char*);
    if (argv_addr > UINT_MAX - entry_offset){
      // avoid overflow
      free_exec_argv(argc, kargv);
      return -1;
    }

    // get user pointer for this argv entry
    char* user_arg = NULL;
    if (copy_from_user(&user_arg, (void*)(argv_addr + entry_offset),
        sizeof(char*), tcb) != 0){
      free_exec_argv(argc, kargv);
      return -1;
    }

    // get data at this pointer
    kargv[i] = malloc(EXEC_MAX_ARG_BYTES);
    if (copy_cstr_from_user(kargv[i], user_arg, EXEC_MAX_ARG_BYTES, tcb) != 0){
      free_exec_argv(argc, kargv);
      return -1;
    }
  }

  unsigned required_bytes = 0;
  if (exec_argv_stack_bytes(argc, kargv, &required_bytes) != 0){
    free_exec_argv(argc, kargv);
    return -1;
  }

  *out_kargv = kargv;
  return 0;
}

// Rebuild the exec argument vector on the new user stack.
// - On success, the top of the new user stack contains copies of every argv
//   string followed by a rebuilt argv pointer array with a trailing NULL entry.
// - `*initial_sp` is set to the stack pointer that jump_to_user() should use,
//   below the copied argv block so the new program can grow its stack downward.
// - `*user_argv` is the user-space address of the rebuilt argv array, or 0 when
//   `argc == 0`.
static int build_exec_argv_on_stack(unsigned stack_bottom, unsigned stack_top,
    char** kargv, int argc, unsigned* initial_sp, unsigned* user_argv,
    struct TCB* tcb){
  *initial_sp = stack_top - sizeof(unsigned);
  *user_argv = 0;

  if (argc == 0){
    return 0;
  }

  unsigned required_bytes = 0;
  if (exec_argv_stack_bytes(argc, kargv, &required_bytes) != 0){
    return -1;
  }

  if (stack_top < stack_bottom || (stack_top - stack_bottom) < required_bytes){
    return -1;
  }

  unsigned argv_entries = (unsigned)argc + 1;
  char** user_argv_buf = malloc(sizeof(char*) * argv_entries);
  unsigned cursor = stack_top;
  for (int i = argc - 1; i >= 0; i--){
    // copy each string in argv
    unsigned len = strlen(kargv[i]) + 1;
    unsigned aligned_len = 0;
    aligned_len = (len + 3) & ~3;
    if (cursor - stack_bottom < aligned_len){
      free(user_argv_buf);
      return -1;
    }

    cursor -= len;
    cursor &= ~(sizeof(unsigned) - 1);
    if (copy_to_user((void*)cursor, kargv[i], len, tcb) != 0){
      free(user_argv_buf);
      return -1;
    }

    user_argv_buf[i] = (char*)cursor;
  }
  user_argv_buf[argc] = NULL;

  unsigned argv_bytes = argv_entries * sizeof(char*);
  unsigned aligned_argv_bytes = 0;
  aligned_argv_bytes = (argv_bytes + 3) & ~3;
  if (cursor - stack_bottom < aligned_argv_bytes){
    free(user_argv_buf);
    return -1;
  }

  cursor -= argv_bytes;
  cursor &= ~(sizeof(unsigned) - 1);
  // copy argv itself
  if (copy_to_user((void*)cursor, user_argv_buf, argv_bytes, tcb) != 0){
    free(user_argv_buf);
    return -1;
  }

  *user_argv = cursor;
  *initial_sp = cursor - sizeof(unsigned);
  free(user_argv_buf);
  return 0;
}

int handle_pipe(int* fds){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // pipe() publishes both descriptors as one user-visible result. Reject an
  // incomplete destination before allocating endpoint objects or pipe state;
  // copy_to_user() is still checked below as a defensive final delivery step.
  if (!user_range_ok(tcb, fds, sizeof(int) * PIPE_DESCRIPTOR_COUNT,
      MMAP_WRITE)){
    return -1;
  }
 
  int read_end = allocate_descriptor(tcb, DESCRIPTOR_FILE, true);
  if (read_end < 0){
    return -1;
  }

  int write_end = allocate_descriptor(tcb, DESCRIPTOR_FILE, true);
  if (write_end < 0){
    deallocate_descriptor(tcb, DESCRIPTOR_FILE, read_end);
    return -1;
  }

  struct Pipe* pipe = malloc(sizeof(struct Pipe));

  pipe->read_endpoint_objects = PIPE_INITIAL_SIDE_ENDPOINT_OBJECTS;
  pipe->write_endpoint_objects = PIPE_INITIAL_SIDE_ENDPOINT_OBJECTS;
  pipe->endpoint_objects =
    pipe->read_endpoint_objects + pipe->write_endpoint_objects;
  blocking_ringbuf_init(&pipe->buf, PIPE_BUFFER_CAPACITY);

  tcb->file_descriptors[read_end]->file = (struct Node*)pipe;
  tcb->file_descriptors[read_end]->offset = 0;
  tcb->file_descriptors[read_end]->type = FILE_DESCRIPTOR_PIPE_READ;
  tcb->file_descriptors[read_end]->refcount = 1;

  tcb->file_descriptors[write_end]->file = (struct Node*)pipe;
  tcb->file_descriptors[write_end]->offset = 0;
  tcb->file_descriptors[write_end]->type = FILE_DESCRIPTOR_PIPE_WRITE;
  tcb->file_descriptors[write_end]->refcount = 1;
      
  int fd_arr[PIPE_DESCRIPTOR_COUNT] = {read_end, write_end};

  int rc = copy_to_user(fds, fd_arr, sizeof(fd_arr), tcb);
  if (rc != 0){
    deallocate_descriptor(tcb, DESCRIPTOR_FILE, read_end);
    deallocate_descriptor(tcb, DESCRIPTOR_FILE, write_end);

    return -1;
  }
  
  return 0;
}

int handle_open(char* path){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);

  if (rc != 0){
    free(buf);
    return -1;
  }

  if (!ext2_path_component_lengths_valid(buf)){
    free(buf);
    return -1;
  }

  struct Node* file_node = node_find(tcb->cwd, buf);
  if (file_node == NULL){
    // create the file if it does not exist
    
    // `node_make_file()` only accepts one basename component, so normalize the
    // missing path first, then walk each component from the requested root.
    // Every non-final missing component becomes a directory, while the final
    // missing component becomes the new regular file.
    unsigned max_parts = SYSCALL_MAX_PATH_BYTES / 2 + 1;
    char** parts = malloc(sizeof(char*) * max_parts);
    unsigned part_count = 0;
    bool absolute = buf[0] == '/';
    char* cursor = buf;

    // normalize path and split into components, e.g. "/a/b/../c" -> ["a", "c"]
    while (*cursor != 0){
      while (*cursor == '/'){
        cursor++;
      }
      if (*cursor == 0){
        break;
      }

      if (part_count >= max_parts){
        free(parts);
        free(buf);
        return -1;
      }

      char* component = cursor;
      while (*cursor != '/' && *cursor != 0){
        cursor++;
      }
      char separator = *cursor;
      *cursor = 0;

      if (component[0] == '.' && component[1] == 0){
        // Ignore no-op path components.
      } else if (component[0] == '.' && component[1] == '.' && component[2] == 0){
        if (part_count > 0 && !streq(parts[part_count - 1], "..")){
          part_count--;
        } else if (!absolute){
          // Relative paths may still need to walk above the starting cwd.
          parts[part_count++] = component;
        }
      } else {
        parts[part_count++] = component;
      }

      if (separator == 0){
        break;
      }
      cursor++;
    }

    if (part_count == 0){
      free(parts);
      free(buf);
      return -1;
    }

    struct Node* current = absolute ? node_find(tcb->cwd, "/") : node_clone(tcb->cwd);
    if (current == NULL){
      free(parts);
      free(buf);
      return -1;
    }

    // Walk through the normalized path components, 
    // creating missing directories or the final file as needed.
    for (unsigned i = 0; i < part_count; i++){
      bool is_final = i + 1 == part_count;
      struct Node* next = node_find(current, parts[i]);

      if (next == NULL){
        // next not found, so we create it
        if (is_final){
          file_node = node_make_file(current, parts[i]);
          node_free(current);
          current = NULL;
          break;
        }

        next = node_make_dir(current, parts[i]);
      }

      if (next == NULL){
        // failed to create needed file or directory
        node_free(current);
        current = NULL;
        break;
      }

      // Intermediate directory symlinks must expand to their targets, matching
      // full-path node_find()/open_existing() traversal. A single-component
      // lookup returns the link inode itself because the path queue is empty.
      if (!is_final && node_is_symlink(next)){
        struct Node* target = node_find(next, ".");
        node_free(next);
        next = target;
        if (next == NULL){
          node_free(current);
          current = NULL;
          break;
        }
      }

      if (!is_final && !node_is_dir(next)){
        // expected a directory but found a non-directory component, so fail
        node_free(next);
        node_free(current);
        current = NULL;
        break;
      }

      node_free(current);
      current = next;
      if (is_final){
        file_node = current;
        current = NULL;
      }
    }

    if (current != NULL){
      node_free(current);
    }
    free(parts);
  }

  free(buf);

  if (file_node == NULL){
    // failed to find or create file
    return -1;
  }

  int fd = allocate_descriptor(tcb, DESCRIPTOR_FILE, true);
  if (fd < 0){
    // could not allocate file descriptor
    node_free(file_node);
    return -1;
  }

  tcb->file_descriptors[fd]->file = file_node;
  tcb->file_descriptors[fd]->offset = 0;
  return fd;
}

// Open a path that must already exist, without publishing any namespace
// mutation.
//
// Preconditions:
// - This runs in a syscall continuation for the current TCB. The trap wrapper
//   has entered kernel mode; interrupts may be enabled while filesystem lookup
//   blocks, and asynchronous signal delivery is deferred until the continuation
//   returns.
// - `path` is an untrusted user pointer. No byte may be dereferenced directly.
//
// Postconditions:
// - Success installs one normal descriptor owned by the current TCB at offset
//   zero and transfers the lookup's Node reference to that descriptor.
// - Every failure returns -1. In particular, a missing path never enters any
//   node_make_* path, and a descriptor-allocation failure releases the Node.
// - `node_find()` supplies all cwd/absolute/symlink traversal semantics; this
//   function adds no architecture- or host-OS-specific pathname assumptions.
static int handle_open_existing(char* path){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);
  if (rc != 0){
    free(buf);
    return -1;
  }

  if (!ext2_path_component_lengths_valid(buf)){
    free(buf);
    return -1;
  }

  struct Node* file_node = node_find(tcb->cwd, buf);
  free(buf);
  if (file_node == NULL){
    return -1;
  }

  int fd = allocate_descriptor(tcb, DESCRIPTOR_FILE, true);
  if (fd < 0){
    node_free(file_node);
    return -1;
  }

  tcb->file_descriptors[fd]->file = file_node;
  tcb->file_descriptors[fd]->offset = 0;
  return fd;
}

int handle_read(int fd, char* buf, unsigned count){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  if (count > SYSCALL_MAX_IO_BYTES){
    // max read size
    count = SYSCALL_MAX_IO_BYTES;
  }

  if (count == 0){
    return 0;
  }

  enum FileDescriptorType type = tcb->file_descriptors[fd]->type;
  if (type == FILE_DESCRIPTOR_STDIN){
    if (!user_range_ok(tcb, buf, count, MMAP_WRITE)){
      return -1;
    }
    char* kbuf = malloc(count);
    for (unsigned i = 0; i < count; i++){
      kbuf[i] = waitkey();
    }
    int rc = copy_to_user(buf, kbuf, count, tcb);
    free(kbuf);
    if (rc != 0){
      return -1;
    }
    return count;
  } else if (type == FILE_DESCRIPTOR_STDOUT || type == FILE_DESCRIPTOR_STDERR
            || type == FILE_DESCRIPTOR_PIPE_WRITE){
    return -1;
  } else if (type == FILE_DESCRIPTOR_PIPE_READ){
    struct Pipe* pipe = (struct Pipe*)tcb->file_descriptors[fd]->file;

    /*
     * Establish delivery for the complete clamped request before consuming a
     * byte. The current TCB owns its VME list throughout this kernel-mode trap
     * execution, so a successful check remains valid through the final copy.
     * copy_to_user() is nevertheless checked to keep that invariant explicit
     * if VM sharing rules change later.
     */
    if (!user_range_ok(tcb, buf, count, MMAP_WRITE)){
      return -1;
    }

    char* kbuf = malloc(count);
    unsigned bytes_read = 0;
    while (bytes_read < count){
      char byte = 0;
      if (!blocking_ringbuf_remove_fallible(&pipe->buf, &byte)){
        break;
      }
      kbuf[bytes_read] = byte;
      bytes_read += 1;
    }

    int rc = copy_to_user(buf, kbuf, bytes_read, tcb);
    free(kbuf);
    if (rc != 0){
      return -1;
    }

    // A zero-byte result after producer close is EOF. If producer close races
    // a nonempty read, already-buffered bytes are returned first and a later
    // call observes EOF.
    return bytes_read;
  }

  struct Node* file_node = tcb->file_descriptors[fd]->file;
  if (file_node == NULL){
    return -1;
  }

  if (!node_is_file(file_node)){
    return -1;
  }

  blocking_lock_acquire(&tcb->file_descriptors[fd]->offset_lock);
  int offset = tcb->file_descriptors[fd]->offset;
  if (offset < 0){
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    return -1;
  }

  unsigned file_size = node_size_in_bytes(file_node);
  if ((unsigned)offset >= file_size){
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    return 0;
  }

  unsigned bytes_to_read = count;
  if (count > file_size - (unsigned)offset){
    bytes_to_read = file_size - (unsigned)offset;
  } 

  char* kbuf = malloc(bytes_to_read);
  unsigned rounded_offset = (unsigned)offset & ~(FRAME_SIZE - 1);
  unsigned rounded_bytes = (bytes_to_read + ((unsigned)offset - rounded_offset) + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  char* mmapped_file = mmap(rounded_bytes, file_node, rounded_offset,
    MMAP_READ | MMAP_SHARED);
  if (mmapped_file == NULL){
    // Kernel-half VME capacity is finite. A valid read must unwind as a
    // syscall failure if its temporary mapping cannot be reserved.
    free(kbuf);
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    return -1;
  }
  memcpy(kbuf, mmapped_file + ((unsigned)offset - rounded_offset),
    bytes_to_read);
  munmap(mmapped_file);

  int rc = copy_to_user(buf, kbuf, bytes_to_read, tcb);
  free(kbuf);
  if (rc != 0){
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    return -1;
  }

  __atomic_fetch_add(&tcb->file_descriptors[fd]->offset, bytes_to_read);
  blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);

  return bytes_to_read;
}

int handle_write(int fd, char* buf, unsigned count){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  if (count > SYSCALL_MAX_IO_BYTES){
    count = SYSCALL_MAX_IO_BYTES;
  }

  if (count == 0){
    return 0;
  }

  char* kbuf = malloc(count);
  int rc = copy_from_user(kbuf, buf, count, tcb);

  if (rc != 0){
    // failed to copy from user space
    free(kbuf);
    return -1;
  }

  enum FileDescriptorType type = tcb->file_descriptors[fd]->type;
  if (type == FILE_DESCRIPTOR_STDIN || type == FILE_DESCRIPTOR_PIPE_READ){
    free(kbuf);
    return -1;
  } else if (type == FILE_DESCRIPTOR_STDOUT || type == FILE_DESCRIPTOR_STDERR){
    /*
     * The trap entry runs in kernel mode and may have re-enabled interrupts.
     * console_write() owns the global cursor/color/MMIO state for this entire
     * copied buffer, so writes from other cores cannot corrupt the cursor or
     * interleave inside one syscall. It restores this TCB's prior interrupt
     * and preemption state before returning.
     */
    console_write(kbuf, count);
    free(kbuf);
    return count;
  } else if (type == FILE_DESCRIPTOR_PIPE_WRITE){
    struct Pipe* pipe = (struct Pipe*)tcb->file_descriptors[fd]->file;
    unsigned bytes_written = 0;
    while (bytes_written < count){
      if (!blocking_ringbuf_add_fallible(&pipe->buf,
          kbuf[bytes_written])){
        break;
      }
      bytes_written += 1;
    }
    free(kbuf);

    // Closing the final reader wakes a blocked writer. Preserve bytes already
    // published in this syscall, but report a broken endpoint when no byte
    // could be committed.
    return bytes_written == 0 ? -1 : (int)bytes_written;
  }

  struct Node* file_node = tcb->file_descriptors[fd]->file;
  if (file_node == NULL){
    free(kbuf);
    return -1;
  }

  if (!node_is_file(file_node)){
    free(kbuf);
    return -1;
  }

  blocking_lock_acquire(&tcb->file_descriptors[fd]->offset_lock);
  int offset = tcb->file_descriptors[fd]->offset;
  if (offset < 0){
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    free(kbuf);
    return -1;
  }

  if ((unsigned)offset > INT_MAX - count){
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    free(kbuf);
    return -1;
  }

  unsigned rounded_offset = (unsigned)offset & ~(FRAME_SIZE - 1);
  unsigned rounded_bytes = count + ((unsigned)offset - rounded_offset);

  char* mmapped_file = mmap(rounded_bytes, file_node, rounded_offset,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  if (mmapped_file == NULL){
    // Do not modify the shared descriptor offset or file if the temporary
    // kernel mapping cannot be represented.
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    free(kbuf);
    return -1;
  }
  memcpy(mmapped_file + ((unsigned)offset - rounded_offset), kbuf, count);
  munmap(mmapped_file);
  free(kbuf);
  
  __atomic_fetch_add(&tcb->file_descriptors[fd]->offset, count);
  blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
  
  return count;
}

int handle_close(int fd){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);
  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }
  
  deallocate_descriptor(tcb, DESCRIPTOR_FILE, fd);
  return 0;
}

int handle_sem_open(int sem_count){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (sem_count < 0){
    return -1;
  }

  int sem_d = allocate_descriptor(tcb, DESCRIPTOR_SEM, true);
  if (sem_d < 0){
    return -1;
  }

  sem_init(tcb->sem_descriptors[sem_d]->sem, sem_count);

  return sem_d + SEM_DESCRIPTORS_START;
}

int handle_sem_up(int sem_d){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  sem_d -= SEM_DESCRIPTORS_START;
  if (sem_d < 0 || sem_d >= MAX_SEM_DESCRIPTORS || tcb->sem_descriptors[sem_d] == NULL){
    return -1;
  }

  // INT_MAX is a valid initial count, but incrementing it is not. Preserve
  // the semaphore state and report a public resource/range error instead of
  // routing user input through sem_up()'s kernel-invariant panic wrapper.
  return sem_try_up(tcb->sem_descriptors[sem_d]->sem) ? 0 : -1;
}

int handle_sem_down(int sem_d){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  sem_d -= SEM_DESCRIPTORS_START;
  if (sem_d < 0 || sem_d >= MAX_SEM_DESCRIPTORS || tcb->sem_descriptors[sem_d] == NULL){
    return -1;
  }

  sem_down(tcb->sem_descriptors[sem_d]->sem);
  return 0;
}

int handle_sem_close(int sem_d){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  sem_d -= SEM_DESCRIPTORS_START;
  if (sem_d < 0 || sem_d >= MAX_SEM_DESCRIPTORS || tcb->sem_descriptors[sem_d] == NULL){
    return -1;
  }

  deallocate_descriptor(tcb, DESCRIPTOR_SEM, sem_d);
  return 0;
}

// Compute one shared file-descriptor seek target while preserving the
// user-visible invariant that descriptor offsets stay within the non-negative
// signed 32-bit range.
static bool seek_target_ok(int base, int delta, int* out){
  if (delta >= 0){
    if (base > INT_MAX - delta){
      return false;
    }

    *out = base + delta;
    return true;
  }

  unsigned magnitude = 0u - (unsigned)delta;
  if ((unsigned)base < magnitude){
    return false;
  }

  *out = base - (int)magnitude;
  return true;
}

int handle_seek(int fd, int offset, int whence){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // validate descriptor
  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  struct FileDescriptor* descriptor = tcb->file_descriptors[fd];
  // Pipe endpoints deliberately store `struct Pipe*` in the same field as a
  // normal descriptor's Node. Reject the kind before any Node operation;
  // seeking a pipe is unsupported for every whence, including SEEK_SET/CUR.
  if (descriptor->type != FILE_DESCRIPTOR_NORMAL || descriptor->file == NULL){
    return -1;
  }
  
  int new_offset = 0;

  blocking_lock_acquire(&descriptor->offset_lock);

  switch (whence){
    case SEEK_SET: {
      if (offset < 0){
        blocking_lock_release(&descriptor->offset_lock);
        return -1;
      }

      descriptor->offset = offset;
      new_offset = offset;
      break;
    }
    case SEEK_CUR: {
      if (!seek_target_ok(descriptor->offset, offset, &new_offset)){
        blocking_lock_release(&descriptor->offset_lock);
        return -1;
      }

      descriptor->offset = new_offset;
      break;
    }
    case SEEK_END: {
      unsigned file_size = node_size_in_bytes(descriptor->file);

      if (file_size > INT_MAX ||
          !seek_target_ok((int)file_size, offset, &new_offset)){
        blocking_lock_release(&descriptor->offset_lock);
        return -1;
      }

      descriptor->offset = new_offset;
      break;
    }
    default: {
      blocking_lock_release(&descriptor->offset_lock);
      return -1;
    }
  }

  blocking_lock_release(&descriptor->offset_lock);
  return new_offset;
}

int handle_truncate(int fd, unsigned size){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  struct FileDescriptor* descriptor = tcb->file_descriptors[fd];
  if (descriptor->type != FILE_DESCRIPTOR_NORMAL || descriptor->file == NULL){
    return -1;
  }

  if (!node_is_file(descriptor->file)){
    return -1;
  }

  if (!vmem_truncate_file(descriptor->file, size)){
    return -1;
  }

  return 0;
}

int handle_dup(int fd){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  int new_fd = allocate_descriptor(tcb, DESCRIPTOR_FILE, false);
  if (new_fd < 0){
    return -1;
  }

  __atomic_fetch_add(&tcb->file_descriptors[fd]->refcount, 1);
  tcb->file_descriptors[new_fd] = tcb->file_descriptors[fd];
  
  return new_fd;
}

int handle_play_audio(int fd){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  struct FileDescriptor* descriptor = tcb->file_descriptors[fd];
  if (descriptor->type != FILE_DESCRIPTOR_NORMAL){
    return -1;
  }

  struct Node* audio_file = descriptor->file;
  if (audio_file == NULL || !node_is_file(audio_file)){
    return -1;
  }

  /*
   * The persistent audio daemon validates and plays from its own address space,
   * so this syscall only clones the Node into a request and waits for the
   * daemon's admission result. A full request queue is an ordinary -1.
   */
  struct AudioRequest* audio_request = audio_request_create(audio_file);
  if (audio_request == NULL){
    return -1;
  }

  if (!audio_request_submit(audio_request)){
    audio_request_destroy_unsubmitted(audio_request);
    return -1;
  }

  /*
   * Playback remains asynchronous after admission. Do not report success until
   * the daemon has mapped and parsed the exact VME it retains for playback;
   * once this helper returns, request ownership belongs exclusively to the
   * daemon and this syscall must not dereference it again.
   */
  return audio_request_wait_until_ready(audio_request) ? 0 : -1;
}

struct CleanPathPart {
  char* start;
  unsigned length;
};

// Return one heap-owned absolute lexical normalization of `path`.
//
// The previous implementation reserved 16 component pointers even though a
// 1023-byte input can contain 512 one-byte components. This representation
// points into the still-live input string and sizes the array from its actual
// length, so every accepted component has storage without hundreds of small
// allocations. The result resolves repeated '/', '.', and '..'; it does not
// perform filesystem or symlink traversal.
static char* clean_path(char* path){
  if (path == NULL){
    return NULL;
  }

  unsigned path_length = strlen(path);
  unsigned maximum_parts = path_length / 2 + 1;
  if (maximum_parts > UINT_MAX / sizeof(struct CleanPathPart)){
    return NULL;
  }
  struct CleanPathPart* parts =
    malloc(sizeof(struct CleanPathPart) * maximum_parts);
  unsigned part_count = 0;
  char* current = path;

  while (*current != 0){
    while (*current == '/'){
      current++;
    }
    if (*current == 0){
      break;
    }

    char* start = current;
    while (*current != '/' && *current != 0){
      current++;
    }
    unsigned length = (unsigned)current - (unsigned)start;

    if (length == 1 && start[0] == '.'){
      continue;
    }
    if (length == 2 && start[0] == '.' && start[1] == '.'){
      if (part_count != 0){
        part_count--;
      }
      continue;
    }

    // Every additional component needs at least one input byte and, except
    // for the first, a separating slash. `path_length / 2 + 1` is therefore
    // a conservative bound; keep a defensive check in case this parser is
    // later changed without updating the bound.
    if (part_count >= maximum_parts){
      free(parts);
      return NULL;
    }
    parts[part_count].start = start;
    parts[part_count].length = length;
    part_count++;
  }

  // Root is '/' plus NUL. Each component contributes its bytes, and every
  // component after the first contributes one separator.
  unsigned cleaned_bytes = 2;
  for (unsigned i = 0; i < part_count; i++){
    unsigned separator_bytes = (i == 0) ? 0 : 1;
    unsigned added_bytes = parts[i].length + separator_bytes;
    if (cleaned_bytes > UINT_MAX - added_bytes){
      free(parts);
      return NULL;
    }
    cleaned_bytes += added_bytes;
  }

  char* cleaned = malloc(cleaned_bytes);
  unsigned cleaned_index = 0;
  cleaned[cleaned_index++] = '/';
  for (unsigned i = 0; i < part_count; i++){
    if (i != 0){
      cleaned[cleaned_index++] = '/';
    }
    memcpy(cleaned + cleaned_index, parts[i].start, parts[i].length);
    cleaned_index += parts[i].length;
  }
  cleaned[cleaned_index] = 0;
  free(parts);
  return cleaned;
}

int handle_chdir(char* path){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);

  if (rc != 0){
    free(buf);
    return -1;
  }

  struct Node* file_node = node_find(tcb->cwd, buf);
  if (file_node == NULL){
    // could not find file
    free(buf);
    return -1;
  }

  // don't chdir into a non-directory
  if (!node_is_dir(file_node)){
    free(buf);
    node_free(file_node);
    return -1;
  }
  
  // Construct and normalize the textual cwd before publishing either half of
  // the `(cwd, cwd_path)` pair. This syscall executes in kernel mode. The
  // current TCB is not concurrently executing on another core, and pending
  // user handlers run only at the final trap return, so no other context can
  // observe a half-committed pair from this thread.
  char *old_path = tcb->cwd_path;
  if (old_path == NULL){
    free(buf);
    node_free(file_node);
    return -1;
  }
  unsigned old_length = strlen(old_path);
  unsigned new_length = strlen(buf);

  unsigned new_offset = 0;
  char *final_path;

  if (buf[0] == '/') {
      // Absolute path.
      if (new_length == UINT_MAX){
        free(buf);
        node_free(file_node);
        return -1;
      }
      final_path = malloc(new_length + 1);
  } else {
      // Relative path.
      if (old_length > UINT_MAX - new_length ||
          old_length + new_length > UINT_MAX - 2){
        free(buf);
        node_free(file_node);
        return -1;
      }
      new_offset = old_length + 1; // Include '/'.
      final_path = malloc(new_offset + new_length + 1);
      // Copy old path.
      memcpy(final_path, old_path, old_length);
      final_path[old_length] = '/';
  }
  // Copy cwd path.
  memcpy(final_path + new_offset, buf, new_length);
  final_path[new_offset + new_length] = 0;

  char* cleaned_path = clean_path(final_path);
  if (cleaned_path == NULL){
    free(final_path);
    free(buf);
    node_free(file_node);
    return -1;
  }

  struct Node* old_node = tcb->cwd;
  tcb->cwd = file_node;
  tcb->cwd_path = cleaned_path;

  free(final_path);
  node_free(old_node);
  free(old_path);
  free(buf);
  return 0;
}

int handle_mmap(int size, int fd, int offset, int flags){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // Establish every public precondition before calling the lower VM layer,
  // whose assertions describe kernel invariants rather than user errors.
  if (size <= 0 || offset < 0){
    return -1;
  }
  if ((flags & ~USER_MMAP_ALLOWED_FLAGS) != 0){
    return -1;
  }

  unsigned mapping_size = (unsigned)size;
  unsigned file_offset = (unsigned)offset;
  if (mapping_size > UINT_MAX - (FRAME_SIZE - 1) ||
      file_offset > UINT_MAX - mapping_size){
    return -1;
  }

  struct Node* file_node = NULL;
  if (fd == USER_MMAP_ANON_FD){
    // Anonymous mappings have no meaningful backing-file offset. Requiring
    // zero prevents a malformed argument from being silently ignored.
    if (file_offset != 0 || (flags & MMAP_SHARED)){
      return -1;
    }
  } else {
    // Every negative value other than the exact sentinel is invalid.
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS ||
        tcb->file_descriptors[fd] == NULL){
      return -1;
    }

    struct FileDescriptor* descriptor = tcb->file_descriptors[fd];
    if (descriptor->type != FILE_DESCRIPTOR_NORMAL ||
        descriptor->file == NULL || !node_is_file(descriptor->file)){
      return -1;
    }

    if ((file_offset & (FRAME_SIZE - 1)) != 0){
      return -1;
    }
    file_node = descriptor->file;
  }

  flags |= MMAP_USER;
  char* mmapped_file = mmap(mapping_size, file_node, file_offset, flags);
  return mmapped_file == NULL ? -1 : (int)mmapped_file;
}

int child_thread(unsigned* arg){
  unsigned pc = arg[0];
  unsigned sp = arg[1];

  // This new child has finished its kernel-side fork trampoline. Process any
  // asynchronous notification that arrived after fork publication only now,
  // at the final transition into its saved user frame.
  process_pending_signals_before_user_return();
  return jump_to_user(pc, sp, 0, 0);
}

struct TCB* fork_tcb(struct TCB* parent, int child_desc, unsigned pc, unsigned sp){
  struct TCB* child = malloc(sizeof(struct TCB));
  memset(child, 0, sizeof(struct TCB));

  child->flags = 0;
  child->psr = 1;
  child->imr = DEFAULT_INTERRUPT_MASK;

  child->can_preempt = parent->can_preempt;
  child->core_affinity = parent->core_affinity;
  child->priority = parent->priority;
  child->mlfq_level = parent->mlfq_level;
  child->remaining_quantum = parent->remaining_quantum;
  child->wakeup_jiffies = parent->wakeup_jiffies;

  // alloc new kernel stack
  unsigned* the_stack = malloc(TCB_STACK_SIZE);
  child->stack = the_stack;
  child->ksp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);
  child->bp = (unsigned)(&the_stack[TCB_STACK_SIZE / sizeof (unsigned) - 1]);

  // child inherits signal state/handlers from parent
  child->pending_signals = 0;
  child->signal_mask = parent->signal_mask;
  for (int i = 0; i < MAX_SIGNALS; i++){
    child->signal_handlers[i] = parent->signal_handlers[i];
  }
  child->in_signal_handler = false;
  child->signal_stack_top = parent->signal_stack_top;

  child->my_node = malloc(sizeof(struct CLHNode));
  child->my_node->locked = false;
  child->my_node->interrupt_state = 0;
  child->my_pred = NULL;

  /*
   * Snapshot the descriptor tables as they existed at fork syscall entry.
   * handle_fork() has already installed one new ChildDescriptor solely so the
   * parent can name this child after the syscall returns. Exclude that slot:
   * inheriting it would give the child a descriptor whose child_tcb points
   * back to itself, permitting self-wait deadlock and self-signal delivery.
   * All older child handles remain ordinary inherited references.
   */
  copy_descriptors(parent, child, child_desc);

  // copy cwd
  child->cwd = node_clone(parent->cwd);

  // copy cwd path
  if (parent->cwd_path != NULL){
    unsigned cwd_path_bytes = strlen(parent->cwd_path) + 1;
    child->cwd_path = malloc(cwd_path_bytes);
    memcpy(child->cwd_path, parent->cwd_path, cwd_path_bytes);
  }

  // set up vme_list and pid
  if (!vmem_fork(parent, child)){
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++){
      if (child->file_descriptors[i]){
        deallocate_descriptor(child, DESCRIPTOR_FILE, i);
      }
    }
    for (int i = 0; i < MAX_SEM_DESCRIPTORS; i++){
      if (child->sem_descriptors[i]){
        deallocate_descriptor(child, DESCRIPTOR_SEM, i);
      }
    }
    for (int i = 0; i < MAX_CHILD_DESCRIPTORS; i++){
      if (child->child_descriptors[i]){
        deallocate_descriptor(child, DESCRIPTOR_CHILD, i);
      }
    }
    node_free(child->cwd);
    free(child->cwd_path);
    free(child->my_node);
    free(child->stack);
    free(child);
    return NULL;
  }

  // set up thread fun
  struct Fun* child_fun = malloc(sizeof(struct Fun));
  child_fun->func = (void(*)(void*))child_thread;
  
  unsigned* arg = malloc(2 * sizeof(unsigned));
  arg[0] = pc;
  arg[1] = sp;
  child_fun->arg = arg;

  child->thread_fun = child_fun;
  child->ra = (unsigned)thread_entry;

  child->parent_promise = parent->child_descriptors[child_desc];

  __atomic_fetch_add(&parent->child_descriptors[child_desc]->refcount, 1);

  child->next = NULL;

  __atomic_fetch_add(&n_active, 1);

  return child;
}

int handle_fork(unsigned pc, unsigned sp){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  int child_desc = allocate_descriptor(tcb, DESCRIPTOR_CHILD, true);
  if (child_desc < 0){
    return -1;
  }

  struct TCB* child = fork_tcb(tcb, child_desc, pc, sp);
  if (child == NULL){
    deallocate_descriptor(tcb, DESCRIPTOR_CHILD, child_desc);
    return -1;
  }

  struct ChildDescriptor* descriptor = tcb->child_descriptors[child_desc];
  clh_lock_acquire(&descriptor->state_lock);
  descriptor->child_tcb = child;
  clh_lock_release(&descriptor->state_lock);
  
  scheduler_wake_thread(child);

  return child_desc + CHILD_DESCRIPTORS_START;
}

int handle_wait_child(int child_desc){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  child_desc -= CHILD_DESCRIPTORS_START;
  if (child_desc < 0 || child_desc >= MAX_CHILD_DESCRIPTORS){
    return -1;
  }

  struct ChildDescriptor* child = tcb->child_descriptors[child_desc];
  if (child == NULL){
    return -1;
  }

  unsigned rc = (unsigned)promise_get(child->child_promise);

  // can only wait on a given child descriptor once; after this call the
  // descriptor is consumed and must not be used again
  deallocate_descriptor(tcb, DESCRIPTOR_CHILD, child_desc);
  
  return rc;
}

int handle_exec(char* path, int argc, char** argv){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);
    
  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);
    
  if (rc != 0){
    free(buf);
    return -1;
  }
  
  struct Node* prog = node_find(tcb->cwd, buf);
  free(buf);
  if (prog == NULL){
    // could not find file
    return -1;
  }

  // don't exec non-files
  if (!node_is_file(prog)){
    node_free(prog);
    return -1;
  }

  unsigned prog_size = node_size_in_bytes(prog);
  void* snapshot = NULL;

  if (prog_size == 0){
    node_free(prog);
    return -1;
  }

  {
    unsigned* prog_bytes = mmap(prog_size, prog, 0, MMAP_READ);
    if (prog_bytes == NULL || !elf_validate_image(prog_bytes, prog_size)){
      if (prog_bytes != NULL){
        munmap(prog_bytes);
      }
      node_free(prog);
      return -1;
    }

    // Pin the validated bytes in a private heap snapshot so a concurrent writer
    // cannot change the image between validation and load, and so loading does
    // not depend on a second file mapping.
    snapshot = malloc(prog_size);
    if (snapshot == NULL){
      munmap(prog_bytes);
      node_free(prog);
      return -1;
    }
    memcpy(snapshot, prog_bytes, prog_size);
    munmap(prog_bytes);
  }

  node_free(prog);
  prog = NULL;

  char** kargv = NULL;
  if (copy_exec_argv_from_user(&kargv, argc, argv, tcb) != 0){
    free(snapshot);
    return -1;
  }

  // Keep the pre-exec address space and signal-handler table intact until the
  // new image is fully constructed. On any construction failure, destroy the
  // in-progress AS and restore the old one so exec can return -1 without
  // killing the process or stripping its previous handlers.
  unsigned old_pid = tcb->pid;
  struct VME* old_vme_list = tcb->vme_list;

  unsigned new_pid = create_page_directory();
  if (new_pid == 0){
    free_exec_argv(argc, kargv);
    free(snapshot);
    return -1;
  }
  tcb->pid = new_pid;
  tcb->vme_list = NULL;
  set_pid(new_pid);
  tlb_flush();

  unsigned entry = 0;
  unsigned initial_sp = 0;
  unsigned user_argv = 0;
  if (construct_user_program_from_image(snapshot, prog_size, argc, kargv,
      &entry, &initial_sp, &user_argv) != 0){
    vmem_destroy_address_space(tcb);
    free_vme_list(tcb->vme_list);
    tcb->pid = old_pid;
    tcb->vme_list = old_vme_list;
    set_pid(old_pid);
    tlb_flush();
    return -1;
  }

  // Construction succeeded: invalidate handlers from the retired image and
  // commit by discarding the old address space while the new one remains active.
  //
  // This TCB is current and cannot execute on another core. Signal senders only
  // mutate pending_signals under ChildDescriptor.state_lock, so clearing this
  // current-thread-only handler state requires no cross-core lock. The
  // architecture memory model is sequentially consistent. Pending signals and
  // the mask are preserved, matching fork/exec process state.
  for (int i = 0; i < MAX_SIGNALS; i++){
    tcb->signal_handlers[i] = NULL;
  }
  tcb->in_signal_handler = false;

  {
    unsigned committed_pid = tcb->pid;
    struct VME* committed_list = tcb->vme_list;
    tcb->pid = old_pid;
    tcb->vme_list = old_vme_list;
    vmem_destroy_address_space(tcb);
    free_vme_list(tcb->vme_list);
    tcb->pid = committed_pid;
    tcb->vme_list = committed_list;
  }

  process_pending_signals_before_user_return();
  rc = jump_to_user(entry, initial_sp, argc, user_argv);
  stop(rc);

  return -1;
}

int handle_getdents(int fd, char* buffer, unsigned buffer_size) {
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  if (tcb->file_descriptors[fd]->type != FILE_DESCRIPTOR_NORMAL) {
    return -1;
  }

  struct Node* file_node = tcb->file_descriptors[fd]->file;
  if (file_node == NULL || !node_is_dir(file_node)) {
    return -1;
  }

  // Zero capacity can never hold even the fixed linux_dirent header. Reject
  // it before malloc(0), which is outside the kernel heap's allocation
  // contract, and before touching the shared descriptor offset.
  if (buffer_size == 0){
    return -1;
  }

  blocking_lock_acquire(&tcb->file_descriptors[fd]->offset_lock);
  int offset = tcb->file_descriptors[fd]->offset;
  if (offset < 0) {
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    return -1;
  }

  if (buffer_size > MAX_GETDENTS_BUFFER_SIZE) {
    buffer_size = MAX_GETDENTS_BUFFER_SIZE;
  }

  char* kbuf = malloc(buffer_size);
  int new_offset = offset;
  int bytes_read = node_getdents(file_node, offset, kbuf, buffer_size, &new_offset);
  if (bytes_read < 0){
    // The ext iterator validates exact record boundaries and EOF under the
    // inode lock. Interior/beyond-EOF offsets are user errors, not VM or ext
    // invariants, and leave the shared descriptor position unchanged.
    free(kbuf);
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    return -1;
  }

  int rc = copy_to_user(buffer, kbuf, (unsigned)bytes_read, tcb);
  free(kbuf);
  if (rc != 0) {
    blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);
    return -1;
  }

  __atomic_store_n(&tcb->file_descriptors[fd]->offset, new_offset);
  blocking_lock_release(&tcb->file_descriptors[fd]->offset_lock);

  return bytes_read;
}

int handle_getcwd(char* buffer, unsigned buffer_size) {
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // guard against invalid cwd_path
  if (tcb->cwd_path == NULL){
    return -1;
  }

  unsigned cwd_len = strlen(tcb->cwd_path);
  if (buffer_size <= cwd_len) {
    return -1;
  }

  int rc = copy_to_user(buffer, tcb->cwd_path, cwd_len + 1, tcb);
  if (rc != 0) {
    return -1;
  }
  return (unsigned) buffer;
}

int handle_readlink(char* path, char* buffer, unsigned buffer_size) {
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);

  if (rc != 0) {
    free(buf);
    return -1;
  }

  struct Node* file_node = node_find(tcb->cwd, buf);
  free(buf);

  if (file_node == NULL) {
    // Symlink not found.
    return -1;
  }

  if (!node_is_symlink(file_node)) {
    // Not a symlink.
    node_free(file_node);
    return -1;
  }

  // Capture size and contents in one inode-lock transaction. A symlink may be
  // read while another core traverses it; allocating from an unlocked size
  // followed by a separately locked copy could otherwise overflow `target`.
  unsigned total_bytes = 0;
  char* target = node_copy_symlink_target(file_node, &total_bytes);
  if (target == NULL){
    node_free(file_node);
    return -1;
  }
  unsigned read_bytes = 0;

  if (buffer_size < total_bytes + 1) {
    // Partial read.
    read_bytes = buffer_size;
  } else {
    // Full read.
    read_bytes = total_bytes + 1;
  }
  
  rc = copy_to_user(buffer, target, read_bytes, tcb);
  node_free(file_node);
  free(target);

  if (rc != 0) {
    return -1;
  }
  
  return read_bytes;
}

int handle_fd_bytes_available(int fd){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
    return -1;
  }

  if (tcb->file_descriptors[fd]->type != FILE_DESCRIPTOR_PIPE_READ &&
      tcb->file_descriptors[fd]->type != FILE_DESCRIPTOR_PIPE_WRITE){
    return -1;
  }

  struct Pipe* pipe = (struct Pipe*)tcb->file_descriptors[fd]->file;

  return blocking_ringbuf_size(&pipe->buf);
}

int handle_mkdir(char* path){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);

  if (rc != 0){
    free(buf);
    return -1;
  }

  if (buf[0] == '\0'){
    // empty name
    free(buf);
    return -1;
  }

  if (strlen(buf) > EXT2_MAX_NAME_BYTES){
    free(buf);
    return -1;
  }

  if (buf[0] == '.' && buf[1] == '\0'){
    // can't create directory with name "."
    free(buf);
    return -1;
  }

  if (buf[0] == '.' && buf[1] == '.' && buf[2] == '\0'){
    // can't create directory with name ".."
    free(buf);
    return -1;
  }

  for (unsigned i = 0; buf[i] != '\0'; ++i){
    // can't create directory with '/' in the name
    if (buf[i] == '/'){
      free(buf);
      return -1;
    }
  }

  struct Node* new_node = node_make_dir(tcb->cwd, buf);
  rc = (new_node == NULL) ? -1 : 0;
  free(buf);
  node_free(new_node);
  return rc;
}

int handle_rmdir(char* path){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);

  if (rc != 0){
    free(buf);
    return -1;
  }

  if (buf[0] == '\0'){
    // empty name
    free(buf);
    return -1;
  }

  if (buf[0] == '.' && buf[1] == '\0'){
    // can't remove "."
    free(buf);
    return -1;
  }

  if (buf[0] == '.' && buf[1] == '.' && buf[2] == '\0'){
    // can't remove ".."
    free(buf);
    return -1;
  }

  for (unsigned i = 0; buf[i] != '\0'; ++i){
    // can only remove stuff in current dir
    if (buf[i] == '/'){
      free(buf);
      return -1;
    }
  }

  // Lookup, directory-kind validation, emptiness validation, and removal are
  // one parent-lock transaction. A competing unlink/create cannot replace the
  // checked directory with a file before the final namespace mutation.
  rc = node_delete_typed(tcb->cwd, buf, NODE_DELETE_EMPTY_DIRECTORY);
  free(buf);
  return rc;
}

int handle_unlink(char* path){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  char* buf = malloc(SYSCALL_MAX_PATH_BYTES);
  int rc = copy_cstr_from_user(buf, path, SYSCALL_MAX_PATH_BYTES, tcb);

  if (rc != 0){
    free(buf);
    return -1;
  }

  if (buf[0] == '\0'){
    free(buf);
    return -1;
  }

  if (buf[0] == '.' && buf[1] == '\0'){
    free(buf);
    return -1;
  }

  if (buf[0] == '.' && buf[1] == '.' && buf[2] == '\0'){
    free(buf);
    return -1;
  }

  for (unsigned i = 0; buf[i] != '\0'; ++i){
    if (buf[i] == '/'){
      free(buf);
      return -1;
    }
  }

  // Keep exact-name lookup, regular-file/symlink validation, and removal under
  // one parent lock. This closes the replacement window between the old
  // syscall-side type check and node_delete()'s second lookup.
  rc = node_delete_typed(tcb->cwd, buf, NODE_DELETE_FILE_OR_SYMLINK);
  free(buf);
  return rc;
}

// Release one ChildDescriptor reference that was not associated with a
// descriptor-table slot.
//
// Preconditions:
// - The caller owns exactly one reference to descriptor.
// - descriptor is not reachable from foreground_child unless the caller has
//   already removed it from that global slot while holding foreground_child_lock.
//
// Postconditions:
// - The reference is dropped.
// - If this was the final reference, the child promise and descriptor storage
//   are destroyed.
void child_descriptor_release(struct ChildDescriptor* descriptor){
  if (descriptor == NULL){
    return;
  } 

  if (__atomic_fetch_add(&descriptor->refcount, -1) > 1){
    return;
  }

  if (descriptor->child_promise != NULL){
    promise_free(descriptor->child_promise);
  }

  clh_lock_destroy(&descriptor->state_lock);
  free(descriptor);
}

// The descriptor owns this lock, so it can safely protect the target TCB's
// lifetime before a sender dereferences child_tcb.
static int send_signal_to_child(struct ChildDescriptor* descriptor, int signal){
  if (descriptor == NULL || signal < 0 || signal >= MAX_SIGNALS){
    return -1;
  }

  clh_lock_acquire(&descriptor->state_lock);
  struct TCB* child = descriptor->child_tcb;
  if (child == NULL){
    clh_lock_release(&descriptor->state_lock);
    return -1;
  }

  child->pending_signals |= 1u << signal;
  clh_lock_release(&descriptor->state_lock);
  return 0;
}

// Record that the current user thread used a direct display trap while it was
// installed as the interactive foreground child.
//
// Concurrency and ordering:
// - foreground_child_lock stabilizes the descriptor reference and serializes
//   this operation with foreground replacement/removal.
// - state_lock serializes display_claimed with child exit and with the shell
//   reading the completed foreground child's claim.
// - All paths taking both locks use foreground_child_lock -> state_lock.
// - The architecture memory model is sequentially consistent; the locks make
//   the claim update atomic with respect to foreground removal.
//
// Postcondition:
// - display_claimed is set only if the caller is still the live TCB named by
//   the current foreground descriptor. Calls made by the terminal, shell, or a
//   background child do not claim foreground display recovery.
static void claim_foreground_display(void){
  int was = interrupts_disable();
  struct TCB* current = get_current_tcb();
  interrupts_restore(was);

  blocking_lock_acquire(&foreground_child_lock);
  struct ChildDescriptor* descriptor = foreground_child;
  if (descriptor != NULL){
    clh_lock_acquire(&descriptor->state_lock);
    if (descriptor->child_tcb == current){
      descriptor->display_claimed = true;
    }
    clh_lock_release(&descriptor->state_lock);
  }
  blocking_lock_release(&foreground_child_lock);
}

// Install or clear the single interactive foreground child.
int handle_set_foreground_child(int child_desc){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  struct ChildDescriptor* new_child = NULL;
  if (child_desc != -1){
    child_desc -= CHILD_DESCRIPTORS_START;
    if (child_desc < 0 || child_desc >= MAX_CHILD_DESCRIPTORS){
      return -1;
    }

    new_child = tcb->child_descriptors[child_desc];
    if (new_child == NULL){
      return -1;
    }

    __atomic_fetch_add(&new_child->refcount, 1);
  }

  blocking_lock_acquire(&foreground_child_lock);
  if (new_child != NULL){
    // All users that take both locks use this order.  Exit paths only take
    // state_lock, so they cannot form an inverse-order dependency.
    clh_lock_acquire(&new_child->state_lock);
    if (new_child->child_tcb == NULL){
      clh_lock_release(&new_child->state_lock);
      blocking_lock_release(&foreground_child_lock);
      child_descriptor_release(new_child);
      return -1;
    }
    clh_lock_release(&new_child->state_lock);
  }

  struct ChildDescriptor* old_child = foreground_child;
  int old_display_claimed = 0;
  if (new_child == NULL && old_child != NULL){
    // The global reference keeps old_child allocated while state_lock
    // serializes this read with both display claims and child exit.
    clh_lock_acquire(&old_child->state_lock);
    old_display_claimed = old_child->display_claimed;
    clh_lock_release(&old_child->state_lock);
  }

  foreground_child = new_child;
  blocking_lock_release(&foreground_child_lock);

  child_descriptor_release(old_child);
  return old_display_claimed;
}

int handle_signal_child(int child_desc, int signal){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  child_desc -= CHILD_DESCRIPTORS_START;
  if (child_desc < 0 || child_desc >= MAX_CHILD_DESCRIPTORS){
    return -1;
  }

  struct ChildDescriptor* child = tcb->child_descriptors[child_desc];
  return send_signal_to_child(child, signal);
}

int handle_signal_foreground(int signal){
  blocking_lock_acquire(&foreground_child_lock);

  struct ChildDescriptor* child = foreground_child;
  int rc = send_signal_to_child(child, signal);
  blocking_lock_release(&foreground_child_lock);
  return rc;
}

static bool is_valid_thread_priority(int priority){
  return priority >= LOW_PRIORITY && priority <= HIGH_PRIORITY;
}

int handle_request_priority(int priority){
  if (!is_valid_thread_priority(priority)){
    return -1;
  }

  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();

  if (tcb == NULL){
    interrupts_restore(was);
    return -1;
  }

  // tcb is running, so it is not in the ready queue
  // threfore is safe to change priority here
  tcb->priority = (enum ThreadPriority)priority;
  interrupts_restore(was);

  return 0;
}

int handle_register_handler(int signal, void* handler){ 
  struct TCB* me = get_current_tcb();

  if (signal < 0 || signal >= MAX_SIGNALS || signal == SIGNAL_KILL){
    return -1;
  }

  // The kernel later uses this value as a user PC. Dioptase instructions are
  // four-byte aligned and fixed-width, so validate the complete first
  // instruction before storing the entry point.
  if (((unsigned)handler & (USER_INSTRUCTION_BYTES - 1)) != 0 ||
      !user_range_ok(me, handler, USER_INSTRUCTION_BYTES, MMAP_EXEC)){
    return -1;
  }

  me->signal_handlers[signal] = handler;
  return 0;
}

int handle_mask_signal(int signal){
  if (signal < 0 || signal >= MAX_MASKABLE_SIGNAL){
    return -1;
  }

  // Only the current TCB changes its mask. It cannot be running on another
  // core, and pending-signal inspection occurs only at a final user-return
  // path after this trap continuation completes. A PIT preemption may observe
  // the old value before this syscall completes, which is equivalent to
  // delivery immediately before the mask operation. Sequential consistency
  // requires no additional ordering.
  struct TCB* me = get_current_tcb();
  me->signal_mask |= 1u << signal;
  return 0;
}

int handle_unmask_signal(int signal){
  if (signal < 0 || signal >= MAX_MASKABLE_SIGNAL){
    return -1;
  }

  // Pending signals are intentionally retained while masked. Clearing this bit
  // makes any coalesced pending instance eligible at the next final
  // kernel-to-user transition.
  struct TCB* me = get_current_tcb();
  me->signal_mask &= ~(1u << signal);
  return 0;
}

// Dispatch user-mode trap requests after trap_handler_ has preserved
// the hardware trap frame and switched into the kernel C calling convention
int trap_handler(unsigned code,
    int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7,
    bool* return_to_user, unsigned pc, unsigned sp){

  // most sycalls return to the user program
  *return_to_user = true;

  switch (code){
    case TRAP_EXIT: {
      // return instead to the kernel thread that called jump_to_user
      *return_to_user = false;
      return arg1;
    }
    case TRAP_TEST_SYSCALL: {
      return trap_test_syscall_handler(arg1);
    }
    case TRAP_GET_CURRENT_JIFFIES: {
      return current_jiffies;
    }
    case TRAP_GET_KEY: {
      return getkey();
    }
    case TRAP_SET_TILE_SCALE: {
      claim_foreground_display();
      console_set_tile_scale(arg1);
      return 0;
    }
    case TRAP_SET_VSCROLL: {
      claim_foreground_display();
      console_set_tile_vscroll(arg1);
      return 0;
    }
    case TRAP_SET_HSCROLL: {
      claim_foreground_display();
      console_set_tile_hscroll(arg1);
      return 0;
    }
    case TRAP_LOAD_TEXT_TILES: {
      claim_foreground_display();
      load_text_tiles();
      return 0;
    }
    case TRAP_CLEAR_SCREEN: {
      claim_foreground_display();
      clear_screen();
      return 0;
    }
    case TRAP_GET_TILEMAP: {
      void* mapping = mmap_physmem(TILEMAP_SIZE, (unsigned)TILEMAP,
        MMAP_READ | MMAP_WRITE | MMAP_USER);
      if (mapping == NULL) return -1;
      claim_foreground_display();
      return (int)mapping;
    }
    case TRAP_GET_TILE_FB: {
      void* mapping = mmap_physmem(TILE_FB_SIZE, (unsigned)TILE_FB,
        MMAP_READ | MMAP_WRITE | MMAP_USER);
      if (mapping == NULL) return -1;
      claim_foreground_display();
      return (int)mapping;
    }
    case TRAP_GET_VGA_STATUS: {
      return (unsigned char)(*VGA_STATUS);
    }
    case TRAP_GET_VGA_FRAME_COUNTER: {
      return *VGA_FRAME_COUNTER;
    }
    case TRAP_SLEEP: {
      // Modular jiffy ordering is unambiguous only within half the 32-bit
      // counter range. Reject a user-supplied high-bit duration before the
      // kernel sleep primitive enforces its internal-call invariant.
      if ((unsigned)arg1 > INT_MAX){
        return -1;
      }
      sleep(arg1);
      return 0;
    }
    case TRAP_OPEN: {
      return handle_open((char*)arg1);
    }
    case TRAP_READ: {
      return handle_read(arg1, (char*)arg2, (unsigned)arg3);
    }
    case TRAP_WRITE: {
      return handle_write(arg1, (char*)arg2, (unsigned)arg3);
    }
    case TRAP_CLOSE: {
      return handle_close(arg1);
    }
    case TRAP_SEM_OPEN: {
      return handle_sem_open(arg1);
    }
    case TRAP_SEM_UP: {
      return handle_sem_up(arg1);
    }
    case TRAP_SEM_DOWN: {
      return handle_sem_down(arg1);
    }
    case TRAP_SEM_CLOSE: {
      return handle_sem_close(arg1);
    }
    case TRAP_MMAP: {
      return handle_mmap(arg1, arg2, arg3, arg4);
    }
    case TRAP_FORK: {
      return handle_fork(pc, sp);
    }
    case TRAP_EXEC: {
      return handle_exec((char*)arg1, arg2, (char**)arg3);
    }
    case TRAP_PLAY_AUDIO: {
      return handle_play_audio(arg1);
    }
    case TRAP_SET_TEXT_COLOR: {
      int color = arg1;
      console_set_text_color(color);
      return 0;
    }
    case TRAP_WAIT_CHILD: {
      return handle_wait_child(arg1);
    }
    case TRAP_CHDIR: {
      return handle_chdir((char*)arg1);
    }
    case TRAP_PIPE: {
      return handle_pipe((int*)arg1);
    }
    case TRAP_DUP: {
      return handle_dup(arg1);
    }
    case TRAP_SEEK: {
      return handle_seek(arg1, arg2, arg3);
    }
    case TRAP_YIELD: {
      yield();
      return 0;
    }
    case TRAP_GETDENTS: {
      return handle_getdents(arg1, (char*)arg2, (unsigned)arg3);
    }
    case TRAP_GETCWD: {
      return handle_getcwd((char*)arg1, (unsigned)arg2);
    }
    case TRAP_READLINK: {
      return handle_readlink((char*)arg1, (char*)arg2, (unsigned)arg3);
    }
    case TRAP_MOVE_VSCROLL: {
      claim_foreground_display();
      console_move_tile_vscroll(arg1);
      return 0;
    }
    case TRAP_MOVE_HSCROLL: {
      claim_foreground_display();
      console_move_tile_hscroll(arg1);
      return 0;
    }
    case TRAP_FD_BYTES_AVAILABLE: {
      return handle_fd_bytes_available(arg1);
    }
    case TRAP_TRUNCATE: {
      return handle_truncate(arg1, (unsigned)arg2);
    }
    case TRAP_MKDIR: {
      return handle_mkdir((char*)arg1);
    }
    case TRAP_RMDIR: {
      return handle_rmdir((char*)arg1);
    }
    case TRAP_UNLINK: {
      return handle_unlink((char*)arg1);
    }
    case TRAP_SET_SPRITE_SCALE: {
      if (arg1 < 0 || arg1 >= NUM_SPRITES){
        return -1;
      }
      claim_foreground_display();
      SPRITE_SCALES[arg1] = arg2;
      return 0;
    }
    case TRAP_SET_SPRITE_COORDS: {
      if (arg1 < 0 || arg1 >= NUM_SPRITES){
        return -1;
      }
      claim_foreground_display();
      SPRITE_COORDS[arg1 * 2] = arg2;
      SPRITE_COORDS[arg1 * 2 + 1] = arg3;
      return 0;
    }
    case TRAP_LOAD_TEXT_TILES_COLORED: {
      claim_foreground_display();
      load_text_tiles_colored(arg1, arg2);
      return 0;
    }
    case TRAP_GET_SPRITEMAP: {
      void* mapping = mmap_physmem(SPRITEMAP_SIZE, (unsigned)SPRITEMAP,
        MMAP_READ | MMAP_WRITE | MMAP_USER);
      if (mapping == NULL) return -1;
      claim_foreground_display();
      return (int)mapping;
    }
    case TRAP_SIGNAL_CHILD: {
      return handle_signal_child(arg1, arg2);
    }
    case TRAP_REQUEST_PRIORITY: {
      return handle_request_priority(arg1);
    }
    case TRAP_SET_FOREGROUND_CHILD: {
      return handle_set_foreground_child(arg1);
    }
    case TRAP_SIGNAL_FOREGROUND: {
      return handle_signal_foreground(arg1);
    }
    case TRAP_REGISTER_HANDLER: {
      return handle_register_handler(arg1, (void*)arg2);
    }
    case TRAP_SIGRETURN: {
      struct TCB* me = get_current_tcb();
      if (!me->in_signal_handler){
        // Outside a handler there is no saved nested jump_to_user activation
        // to resume. Treat this as an ordinary invalid syscall request.
        return -1;
      }

      // return instead to the kernel thread that called jump_to_user
      *return_to_user = false;
      finish_current_signal_handler();
      return arg1;
    }
    case TRAP_MASK_SIGNAL: {
      return handle_mask_signal(arg1);
    }
    case TRAP_UNMASK_SIGNAL: {
      return handle_unmask_signal(arg1);
    }
    case TRAP_OPEN_EXISTING: {
      return handle_open_existing((char*)arg1);
    }
    default: {
      // bad syscall, program dies
      *return_to_user = false;
      return -1;
    }
  }
}

void trap_init(void) {
  blocking_lock_init(&foreground_child_lock);
  foreground_child = NULL;
  register_handler((void*)trap_handler_, (void*)TRAP_IVT_ENTRY);
}

void trap_destroy(void) {
  // kernel_shutdown() calls this after every core has entered the shutdown
  // barrier with interrupts disabled, so no trap can concurrently access the
  // foreground slot and the blocking lock must not be acquired here.
  struct ChildDescriptor* old_child = foreground_child;
  foreground_child = NULL;
  child_descriptor_release(old_child);
  blocking_lock_destroy(&foreground_child_lock);
}

/*
 * Build the user image in the current address space from an immutable byte
 * snapshot that the caller has already validated. Consumes `image` and `argv`
 * on every path.
 *
 * On success: every nonempty PT_LOAD plus both stacks are installed, argv is
 * on the user stack, and *entry_out / *initial_sp_out / *user_argv_out are set.
 * On failure: returns -1 after freeing image/argv. Any partial VME list remains
 * for the caller to destroy with the in-construction address space.
 */
static int construct_user_program_from_image(void* image, unsigned image_size,
    int argc, char** argv, unsigned* entry_out, unsigned* initial_sp_out,
    unsigned* user_argv_out){
  (void)image_size;

  if (argc < 0 || image == NULL || entry_out == NULL ||
      initial_sp_out == NULL || user_argv_out == NULL){
    free(image);
    free_exec_argv(argc, argv);
    return -1;
  }

  unsigned entry = 0;
  if (!elf_load(image, &entry)){
    free(image);
    free_exec_argv(argc, argv);
    return -1;
  }

  // elf_load() has copied every segment byte into its final user VME. The
  // immutable snapshot is no longer needed for mapping construction.
  free(image);

  unsigned* stack = mmap_stack(INITIAL_USER_STACK_SIZE,
    MMAP_READ | MMAP_WRITE | MMAP_USER);
  unsigned* signal_stack = mmap_stack(INITIAL_SIGNAL_STACK_SIZE,
    MMAP_READ | MMAP_WRITE | MMAP_USER);
  if (stack == NULL || signal_stack == NULL){
    free_exec_argv(argc, argv);
    return -1;
  }

  unsigned stack_top = (unsigned)stack + INITIAL_USER_STACK_SIZE;
  unsigned signal_stack_top = (unsigned)signal_stack + INITIAL_SIGNAL_STACK_SIZE;
  unsigned initial_sp = 0;
  unsigned user_argv = 0;

  struct TCB* me = get_current_tcb();
  me->signal_stack_top = signal_stack_top;

  int rc = build_exec_argv_on_stack((unsigned)stack, stack_top, argv, argc,
    &initial_sp, &user_argv, me);
  free_exec_argv(argc, argv);

  if (rc != 0){
    return -1;
  }

  *entry_out = entry;
  *initial_sp_out = initial_sp;
  *user_argv_out = user_argv;
  return 0;
}

// Run a user program given a node representing its ELF file.
// Consumes the node; the caller cannot use it after calling this function.
// Used for initial /sbin/init entry where there is no prior user image to
// restore: construction failure simply returns -1.
int run_user_program(struct Node* prog_node, int argc, char** argv){
  if (argc < 0){
    node_free(prog_node);
    free_exec_argv(argc, argv);
    return -1;
  }

  unsigned size = node_size_in_bytes(prog_node);
  if (size == 0){
    node_free(prog_node);
    free_exec_argv(argc, argv);
    return -1;
  }

  unsigned* prog = mmap(size, prog_node, 0, MMAP_READ);
  node_free(prog_node);
  if (prog == NULL){
    free_exec_argv(argc, argv);
    return -1;
  }

  if (!elf_validate_image(prog, size)){
    munmap(prog);
    free_exec_argv(argc, argv);
    return -1;
  }

  void* snapshot = malloc(size);
  if (snapshot == NULL){
    munmap(prog);
    free_exec_argv(argc, argv);
    return -1;
  }
  memcpy(snapshot, prog, size);
  munmap(prog);

  unsigned entry = 0;
  unsigned initial_sp = 0;
  unsigned user_argv = 0;
  if (construct_user_program_from_image(snapshot, size, argc, argv,
      &entry, &initial_sp, &user_argv) != 0){
    return -1;
  }

  // ELF loading, stack construction, and all filesystem/VM locks have
  // unwound. This is the final transition for initial entry, so pending
  // asynchronous signals are safe to process here.
  process_pending_signals_before_user_return();
  return jump_to_user(entry, initial_sp, argc, user_argv);
}

void init_descriptors(struct TCB* tcb, bool init_stdio){
  if (init_stdio){
    // User-entering threads need the conventional stdio descriptors from boot.
    tcb->file_descriptors[0] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[0]->refcount = 1;
    tcb->file_descriptors[0]->offset = 0;
    blocking_lock_init(&tcb->file_descriptors[0]->offset_lock);
    tcb->file_descriptors[0]->type = FILE_DESCRIPTOR_STDIN;
    tcb->file_descriptors[0]->file = NULL;
    
    tcb->file_descriptors[1] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[1]->refcount = 1;
    tcb->file_descriptors[1]->offset = 0;
    blocking_lock_init(&tcb->file_descriptors[1]->offset_lock);
    tcb->file_descriptors[1]->type = FILE_DESCRIPTOR_STDOUT;
    tcb->file_descriptors[1]->file = NULL;

    tcb->file_descriptors[2] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[2]->refcount = 1;
    tcb->file_descriptors[2]->offset = 0;
    blocking_lock_init(&tcb->file_descriptors[2]->offset_lock);
    tcb->file_descriptors[2]->type = FILE_DESCRIPTOR_STDERR;
    tcb->file_descriptors[2]->file = NULL;
  } else {
    tcb->file_descriptors[0] = NULL;
    tcb->file_descriptors[1] = NULL;
    tcb->file_descriptors[2] = NULL;
  }

  for (int i = 3; i < MAX_FILE_DESCRIPTORS; i++){
    tcb->file_descriptors[i] = NULL;
  }
  for (int i = 0; i < MAX_SEM_DESCRIPTORS; i++){
    tcb->sem_descriptors[i] = NULL;
  }
  for (int i = 0; i < MAX_CHILD_DESCRIPTORS; i++){
    tcb->child_descriptors[i] = NULL;
  }
}

int allocate_descriptor(struct TCB* tcb, enum DescriptorType type, bool fill){
  switch (type){
    case DESCRIPTOR_FILE: {
      for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++){
        if (tcb->file_descriptors[i] == NULL){
          if (fill){
            tcb->file_descriptors[i] = malloc(sizeof(struct FileDescriptor));
            tcb->file_descriptors[i]->refcount = 1;
            tcb->file_descriptors[i]->offset = 0;
            blocking_lock_init(&tcb->file_descriptors[i]->offset_lock);
            tcb->file_descriptors[i]->type = FILE_DESCRIPTOR_NORMAL;
            tcb->file_descriptors[i]->file = NULL;
          }
          return i;
        }
      }
      break;
    }
    case DESCRIPTOR_SEM: {
      for (int i = 0; i < MAX_SEM_DESCRIPTORS; i++){
        if (tcb->sem_descriptors[i] == NULL){
          if (fill){
            tcb->sem_descriptors[i] = malloc(sizeof(struct SemDescriptor));
            tcb->sem_descriptors[i]->refcount = 1;
            tcb->sem_descriptors[i]->sem = malloc(sizeof(struct Semaphore));
          }
          return i;
        }
      }
      break;
    }
    case DESCRIPTOR_CHILD: {
      for (int i = 0; i < MAX_CHILD_DESCRIPTORS; i++){
        if (tcb->child_descriptors[i] == NULL){
          if (fill){
            tcb->child_descriptors[i] = malloc(sizeof(struct ChildDescriptor));
            tcb->child_descriptors[i]->refcount = 1;
            tcb->child_descriptors[i]->child_tcb = NULL;
            tcb->child_descriptors[i]->child_promise = malloc(sizeof(struct Promise));
            tcb->child_descriptors[i]->display_claimed = false;
            clh_lock_init(&tcb->child_descriptors[i]->state_lock);
            promise_init(tcb->child_descriptors[i]->child_promise);
          }
          return i;
        }
      }
      break;
    }
  }

  return -1;
}

void copy_descriptors(struct TCB* src, struct TCB* dst,
    int parent_only_child_desc){
  for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++){
    if (src->file_descriptors[i] != NULL){
      dst->file_descriptors[i] = src->file_descriptors[i];
      __atomic_fetch_add(&dst->file_descriptors[i]->refcount, 1);
    } else {
      dst->file_descriptors[i] = NULL;
    }
  }

  for (int i = 0; i < MAX_SEM_DESCRIPTORS; i++){
    if (src->sem_descriptors[i] != NULL){
      dst->sem_descriptors[i] = src->sem_descriptors[i];
      __atomic_fetch_add(&dst->sem_descriptors[i]->refcount, 1);
    } else {
      dst->sem_descriptors[i] = NULL;
    }
  }

  for (int i = 0; i < MAX_CHILD_DESCRIPTORS; i++){
    if (i == parent_only_child_desc){
      // This handle was allocated by the in-progress fork and is returned
      // only to the parent; it was not part of the entry-time table snapshot.
      dst->child_descriptors[i] = NULL;
    } else if (src->child_descriptors[i] != NULL){
      dst->child_descriptors[i] = src->child_descriptors[i];
      __atomic_fetch_add(&dst->child_descriptors[i]->refcount, 1);
    } else {
      dst->child_descriptors[i] = NULL;
    }
  }
}

// Release the one logical pipe endpoint object owned by a final
// FileDescriptor reference.
//
// Preconditions:
// - `descriptor` is a read or write pipe endpoint whose FileDescriptor
//   refcount just transitioned from one to zero.
// - No future table reference can reach this endpoint object. An in-flight
//   syscall through another table reference would have kept that refcount
//   above zero, so the closing side itself is externally quiescent.
//
// Postconditions:
// - The last object for one side publishes closure and wakes opposite-side
//   waiters before dropping the pipe's total endpoint-object reference.
// - The final overall endpoint destroys the ring only after both close calls
//   and every operation retained by the other endpoint object have returned.
static void pipe_endpoint_release(struct FileDescriptor* descriptor){
  struct Pipe* pipe = (struct Pipe*)descriptor->file;
  assert_always(pipe != NULL,
    "pipe endpoint release: descriptor had a NULL pipe pointer.\n");

  int old_side_count = 0;
  if (descriptor->type == FILE_DESCRIPTOR_PIPE_READ){
    old_side_count = __atomic_fetch_add(&pipe->read_endpoint_objects, -1);
    if (old_side_count <= 0){
      int args[3] = {(int)pipe, old_side_count, descriptor->type};
      say("| pipe endpoint release rejected pipe=0x%X readers=%d type=%d\n",
        args);
      panic("pipe endpoint release: read endpoint-object count underflow.\n");
    }
    if (old_side_count == 1){
      blocking_ringbuf_close_consumers(&pipe->buf);
    }
  } else {
    assert_always(descriptor->type == FILE_DESCRIPTOR_PIPE_WRITE,
      "pipe endpoint release: descriptor kind was not a pipe endpoint.\n");
    old_side_count = __atomic_fetch_add(&pipe->write_endpoint_objects, -1);
    if (old_side_count <= 0){
      int args[3] = {(int)pipe, old_side_count, descriptor->type};
      say("| pipe endpoint release rejected pipe=0x%X writers=%d type=%d\n",
        args);
      panic("pipe endpoint release: write endpoint-object count underflow.\n");
    }
    if (old_side_count == 1){
      blocking_ringbuf_close_producers(&pipe->buf);
    }
  }

  int old_endpoint_count = __atomic_fetch_add(&pipe->endpoint_objects, -1);
  if (old_endpoint_count <= 0){
    int args[3] = {(int)pipe, old_endpoint_count, descriptor->type};
    say("| pipe endpoint release rejected pipe=0x%X endpoints=%d type=%d\n",
      args);
    panic("pipe endpoint release: total endpoint-object count underflow.\n");
  }

  if (old_endpoint_count == 1){
    // Both logical sides are now closed. Because a syscall retains the
    // descriptor object used to enter it, no add/remove operation can still
    // be running when the last endpoint object reaches this point.
    blocking_ringbuf_destroy(&pipe->buf);
    free(pipe);
  }
}

void deallocate_descriptor(struct TCB* tcb, enum DescriptorType type, int index){
  switch (type){
    case DESCRIPTOR_FILE: {
      struct FileDescriptor* descriptor = tcb->file_descriptors[index];
      tcb->file_descriptors[index] = NULL;

      if (descriptor == NULL)
        return;

      if (__atomic_fetch_add(&descriptor->refcount, -1) > 1){
        return;
      }

      if (descriptor->type == FILE_DESCRIPTOR_PIPE_READ || 
          descriptor->type == FILE_DESCRIPTOR_PIPE_WRITE){
        pipe_endpoint_release(descriptor);
      } else if (descriptor->file != NULL){
        node_free(descriptor->file);
      }

      blocking_lock_destroy(&descriptor->offset_lock);

      free(descriptor);

      break;
    }
    case DESCRIPTOR_SEM: {
      struct SemDescriptor* descriptor = tcb->sem_descriptors[index];
      tcb->sem_descriptors[index] = NULL;

      if (descriptor == NULL)
        return;

      if (__atomic_fetch_add(&descriptor->refcount, -1) > 1){
        return;
      }

      if (descriptor->sem != NULL){
        sem_free(descriptor->sem);
      }

      free(descriptor);

      break;
    }
    case DESCRIPTOR_CHILD: {
      struct ChildDescriptor* descriptor = tcb->child_descriptors[index];
      tcb->child_descriptors[index] = NULL;

      child_descriptor_release(descriptor);
      break;
    }
  }
}
