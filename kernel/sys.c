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

#define INITIAL_USER_STACK_SIZE 0x4000
#define SYSCALL_MAX_PATH_BYTES 1024
#define SYSCALL_MAX_IO_BYTES 1024
#define EXEC_MAX_ARGC 16
#define EXEC_MAX_ARG_BYTES 256

#define PIPE_BUFFER_CAPACITY 1024

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
//   MMAP_WRITE for kernel writes to user memory.
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

  unsigned argv_bytes = argc * sizeof(char*);
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

  char** kargv = malloc(sizeof(char*) * argc);
  memset(kargv, 0, sizeof(char*) * argc);

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

  char** user_argv_buf = malloc(sizeof(char*) * argc);
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

  unsigned argv_bytes = argc * sizeof(char*);
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

  pipe->refcount = 2;
  blocking_ringbuf_init(&pipe->buf, PIPE_BUFFER_CAPACITY);

  tcb->file_descriptors[read_end]->file = (struct Node*)pipe;
  tcb->file_descriptors[read_end]->offset = 0;
  tcb->file_descriptors[read_end]->type = FILE_DESCRIPTOR_PIPE_READ;
  tcb->file_descriptors[read_end]->refcount = 1;

  tcb->file_descriptors[write_end]->file = (struct Node*)pipe;
  tcb->file_descriptors[write_end]->offset = 0;
  tcb->file_descriptors[write_end]->type = FILE_DESCRIPTOR_PIPE_WRITE;
  tcb->file_descriptors[write_end]->refcount = 1;
      
  int fd_arr[2] = {read_end, write_end};

  int rc = copy_to_user(fds, fd_arr, sizeof(int) * 2, tcb);
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

  struct Node* file_node = node_find(tcb->cwd, buf);
  free(buf);
  if (file_node == NULL){
    // could not find file
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
    char* kbuf = malloc(count);
    for (int i = 0; i < count; i++){
      kbuf[i] = blocking_ringbuf_remove(&pipe->buf);
    }
    copy_to_user(buf, kbuf, count, tcb);
    free(kbuf);

    return count;
  }

  struct Node* file_node = tcb->file_descriptors[fd]->file;
  if (file_node == NULL){
    return -1;
  }

  int offset = tcb->file_descriptors[fd]->offset;
  if (offset < 0){
    return -1;
  }

  unsigned file_size = node_size_in_bytes(file_node);
  if ((unsigned)offset >= file_size){
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
  memcpy(kbuf, mmapped_file + ((unsigned)offset - rounded_offset),
    bytes_to_read);
  munmap(mmapped_file);

  int rc = copy_to_user(buf, kbuf, bytes_to_read, tcb);
  free(kbuf);
  if (rc != 0){
    return -1;
  }

  __atomic_fetch_add(&tcb->file_descriptors[fd]->offset, bytes_to_read);
  
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
    for (unsigned i = 0; i < count; i++){
      putchar(kbuf[i]);
    }
    free(kbuf);
    return count;
  } else if (type == FILE_DESCRIPTOR_PIPE_WRITE){
    struct Pipe* pipe = (struct Pipe*)tcb->file_descriptors[fd]->file;
    for (unsigned i = 0; i < count; i++){
      blocking_ringbuf_add(&pipe->buf, kbuf[i]);
    }
    free(kbuf);
    return count;
  }

  struct Node* file_node = tcb->file_descriptors[fd]->file;
  if (file_node == NULL){
    free(kbuf);
    return -1;
  }

  int offset = tcb->file_descriptors[fd]->offset;
  if (offset < 0){
    free(kbuf);
    return -1;
  }

  if ((unsigned)offset > INT_MAX - count){
    free(kbuf);
    return -1;
  }

  unsigned rounded_offset = (unsigned)offset & ~(FRAME_SIZE - 1);
  unsigned rounded_bytes = count + ((unsigned)offset - rounded_offset);

  char* mmapped_file = mmap(rounded_bytes, file_node, rounded_offset,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  memcpy(mmapped_file + ((unsigned)offset - rounded_offset), kbuf, count);
  munmap(mmapped_file);
  free(kbuf);
  
  __atomic_fetch_add(&tcb->file_descriptors[fd]->offset, count);
  
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

  sem_up(tcb->sem_descriptors[sem_d]->sem);
  return 0;
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
  if (descriptor->file == NULL){
    return -1;
  }
  
  int new_offset = 0;

  spin_lock_acquire(&descriptor->offset_lock);

  switch (whence){
    case SEEK_SET: {
      if (offset < 0){
        spin_lock_release(&descriptor->offset_lock);
        return -1;
      }

      descriptor->offset = offset;
      new_offset = offset;
      break;
    }
    case SEEK_CUR: {
      if (!seek_target_ok(descriptor->offset, offset, &new_offset)){
        spin_lock_release(&descriptor->offset_lock);
        return -1;
      }

      descriptor->offset = new_offset;
      break;
    }
    case SEEK_END: {
      unsigned file_size = node_size_in_bytes(descriptor->file);

      if (file_size > INT_MAX ||
          !seek_target_ok((int)file_size, offset, &new_offset)){
        spin_lock_release(&descriptor->offset_lock);
        return -1;
      }

      descriptor->offset = new_offset;
      break;
    }
    default: {
      spin_lock_release(&descriptor->offset_lock);
      return -1;
    }
  }

  spin_lock_release(&descriptor->offset_lock);
  return new_offset;
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

  struct Node* audio_file = tcb->file_descriptors[fd]->file;
  if (audio_file == NULL || !node_is_file(audio_file)){
    return -1;
  }

  struct Node** audio_node_arg = malloc(sizeof(struct Node*));
  *audio_node_arg = node_clone(audio_file);
  if (*audio_node_arg == NULL){
    free(audio_node_arg);
    return -1;
  }

  struct Fun* audio_worker_fun = malloc(sizeof(struct Fun));
  audio_worker_fun->func = audio_worker;
  audio_worker_fun->arg = audio_node_arg;
  thread_(audio_worker_fun, HIGH_PRIORITY, ANY_CORE);
  
  return 0;
}

char *clean_path(char *path) {
    // Split by '/' to resolve "." and "..".
    unsigned maximum_parts = SYSCALL_MAX_PATH_BYTES / 64;
    char **parts = malloc(sizeof(char*) * maximum_parts);
    for (unsigned i = 0; i < maximum_parts; i++) {
        parts[i] = NULL;
    }
    unsigned part_count = 0;
    char *current = path;

    unsigned total_length = 2; // For '/' and null terminator.

    while (*current != 0) {
        // Skip leading '/'.
        while (*current == '/') {
            current++;
        }
        if (*current == 0) {
            break;
        }
        char *start = current;
        while (*current != '/' && *current != 0) {
            current++;
        }
        unsigned length = (unsigned) current - (unsigned) start;
        if (length == 1 && start[0] == '.') {
            // Do nothing.
            continue;
        } else if (length == 2 && start[0] == '.' && start[1] == '.') {
            if (part_count == 0) {
                // At root. Do nothing.
                continue;
            }
            total_length -= (strlen((char*) parts[part_count - 1]) + 1);
            free((char*) parts[--part_count]);
            continue;
        }

        char *part = malloc(length + 1);
        memcpy(part, start, length);
        part[length] = 0;
        parts[part_count++] = part;
        total_length += length + 1;
    }

    if (part_count > 0) {
        total_length--; // No trailing '/'.
    }

    // Reconstruct.
    char *cleaned = malloc(total_length);
    cleaned[0] = '/';
    unsigned cleaned_index = 1;
    for (unsigned i = 0; i < part_count; i++) {
        unsigned part_offset = 0;
        while (parts[i][part_offset] != 0) {
            cleaned[cleaned_index++] = parts[i][part_offset++];
        }
        cleaned[cleaned_index++] = '/';
        free((char*) parts[i]);
    }
    cleaned[total_length - 1] = 0; // Overwrite last '/' with null terminator.
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
  
  node_free(tcb->cwd);

  tcb->cwd = file_node;

  // Combine path.
  char *old_path = tcb->cwd_path;
  unsigned old_length = strlen(old_path);
  unsigned new_length = strlen(buf);

  unsigned new_offset = 0;
  char *final_path;

  if (buf[0] == '/') {
      // Absolute path.
      final_path = malloc(new_length + 1);
  } else {
      // Relative path.
      new_offset = old_length + 1; // Include '/'.
      final_path = malloc(new_offset + new_length + 1);
      // Copy old path.
      memcpy(final_path, old_path, old_length);
      final_path[old_length] = '/';
  }
  // Copy cwd path.
  memcpy(final_path + new_offset, buf, new_length);
  final_path[new_offset + new_length] = 0;

  tcb->cwd_path = clean_path(final_path);

  free(final_path);
  free(old_path);
  free(buf);
  return 0;
}

int handle_mmap(int size, int fd, int offset, int flags){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  struct Node* file_node = NULL;
  if (fd >= 0){
    // file backed mmap request
    if (fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
      return -1;
    }

    file_node = tcb->file_descriptors[fd]->file;
    if (file_node == NULL){
      return -1;
    }
  } else {
    // anonymous mmap request
    if (flags & MMAP_SHARED){
      // shared anonymous mappings are not supported
      return -1;
    }
  }

  if (offset < 0) return -1;

  flags &= USER_MMAP_FLAGS_MASK;
  flags |= MMAP_USER;

  char* mmapped_file = mmap(size, file_node, offset, flags);

  return (unsigned)mmapped_file;
}

int child_thread(unsigned* arg){
  unsigned pc = arg[0];
  unsigned sp = arg[1];
  
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

  // set up descriptors
  copy_descriptors(parent, child);

  // copy cwd
  child->cwd = node_clone(parent->cwd);

  // set up vme_list and pid
  vmem_fork(parent, child);

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

  unsigned rc = (unsigned)promise_get(child->child);

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

  char** kargv = NULL;
  if (copy_exec_argv_from_user(&kargv, argc, argv, tcb) != 0){
    node_free(prog);
    return -1;
  }

  vmem_destroy_address_space(tcb);
  free_vme_list(tcb->vme_list);
  tcb->vme_list = NULL;
  tcb->pid = create_page_directory();

  rc = run_user_program(prog, argc, kargv);
  free_exec_argv(argc, kargv);
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

  struct Node* file_node = tcb->file_descriptors[fd]->file;
  if (file_node == NULL || !node_is_dir(file_node)) {
    return -1;
  }

  int offset = tcb->file_descriptors[fd]->offset;
  if (offset < 0){
    return -1;
  }

  unsigned file_size = node_size_in_bytes(file_node);
  if ((unsigned) offset >= file_size){
    return 0;
  }

  char* kbuf = malloc(buffer_size);
  int new_offset = offset;
  unsigned bytes_read = node_getdents(file_node, offset, kbuf, buffer_size, &new_offset);

  int rc = copy_to_user(buffer, kbuf, bytes_read, tcb);
  free(kbuf);
  if (rc != 0) {
    return -1;
  }

  __atomic_store_n(&tcb->file_descriptors[fd]->offset, new_offset);
  
  return bytes_read;
}

int handle_getcwd(char* buffer, unsigned buffer_size) {
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);
  
  int cwd_len = strlen(tcb->cwd_path);
  if (buffer_size < (unsigned)cwd_len + 1) {
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

  if (file_node == NULL){
    // Symlink not found.
    return -1;
  }

  if (!node_is_symlink(file_node)) {
    // Not a symlink.
    node_free(file_node);
    return -1;
  }

  unsigned total_bytes = node_size_in_bytes(file_node);

  // node_get_symlink_target adds null terminator.
  char *target = malloc(total_bytes + 1);
  node_get_symlink_target(file_node, target);
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

// Dispatch user-mode trap requests after trap_handler_ has preserved
// the hardware trap frame and switched into the kernel C calling convention
int trap_handler(unsigned code,
    int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7,
    bool* return_to_user, unsigned pc, unsigned sp){

  // most sycalls return to the user program
  *return_to_user = true;

  switch (code){
    case TRAP_EXIT: {
      // return instead to the kernel thread that called switch_to_user
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
      *TILE_SCALE = arg1;
      return 0;
    }
    case TRAP_SET_VSCROLL: {
      *TILE_VSCROLL = arg1;
      return 0;
    }
    case TRAP_SET_HSCROLL: {
      *TILE_HSCROLL = arg1;
      return 0;
    }
    case TRAP_LOAD_TEXT_TILES: {
      load_text_tiles();
      return 0;
    }
    case TRAP_CLEAR_SCREEN: {
      clear_screen();
      return 0;
    }
    case TRAP_GET_TILEMAP: {
      return (int)mmap_physmem(TILEMAP_SIZE, (unsigned)TILEMAP, MMAP_READ | MMAP_WRITE | MMAP_USER);
    }
    case TRAP_GET_TILE_FB: {
      return (int)mmap_physmem(TILE_FB_SIZE, (unsigned)TILE_FB, MMAP_READ | MMAP_WRITE | MMAP_USER);
    }
    case TRAP_GET_VGA_STATUS: {
      return (unsigned char)(*VGA_STATUS);
    }
    case TRAP_GET_VGA_FRAME_COUNTER: {
      return *VGA_FRAME_COUNTER;
    }
    case TRAP_SLEEP: {
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
      preempt_spin_lock_acquire(&print_lock);
      text_color = color;
      preempt_spin_lock_release(&print_lock);
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
    default: {
      *return_to_user = false;
      return -1;
    }
  }
}

void trap_init(void) {
  register_handler((void*)trap_handler_, (void*)TRAP_IVT_ENTRY);
}

// run a user program given a node representing its ELF file
// consumes the node, so the caller cannot use it after calling this function
int run_user_program(struct Node* prog_node, int argc, char** argv){
  if (argc < 0){
    node_free(prog_node);
    return -1;
  }

  unsigned size = node_size_in_bytes(prog_node);
  unsigned* prog = mmap(size, prog_node, 0, MMAP_READ);
  node_free(prog_node);
  
  unsigned entry = elf_load(prog);

  // The initial user stack grows downward, so reserve it from the top of the
  // user half and enter at the last word in that reservation.
  unsigned* stack = mmap_stack(INITIAL_USER_STACK_SIZE,
    MMAP_READ | MMAP_WRITE | MMAP_USER);
  unsigned stack_bottom = (unsigned)stack;
  unsigned stack_top = (unsigned)stack + INITIAL_USER_STACK_SIZE;
  unsigned initial_sp = 0;
  unsigned user_argv = 0;

  int rc = build_exec_argv_on_stack(stack_bottom, stack_top, argv, argc,
    &initial_sp, &user_argv, get_current_tcb());
  if (rc != 0){
    return -1;
  }

  return jump_to_user(entry, initial_sp, argc, user_argv);
}

void init_descriptors(struct TCB* tcb, bool init_stdio){
  if (init_stdio){
    // User-entering threads need the conventional stdio descriptors from boot.
    tcb->file_descriptors[0] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[0]->refcount = 1;
    tcb->file_descriptors[0]->offset = 0;
    spin_lock_init(&tcb->file_descriptors[0]->offset_lock);
    tcb->file_descriptors[0]->type = FILE_DESCRIPTOR_STDIN;
    tcb->file_descriptors[0]->file = NULL;
    
    tcb->file_descriptors[1] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[1]->refcount = 1;
    tcb->file_descriptors[1]->offset = 0;
    spin_lock_init(&tcb->file_descriptors[1]->offset_lock);
    tcb->file_descriptors[1]->type = FILE_DESCRIPTOR_STDOUT;
    tcb->file_descriptors[1]->file = NULL;

    tcb->file_descriptors[2] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[2]->refcount = 1;
    tcb->file_descriptors[2]->offset = 0;
    spin_lock_init(&tcb->file_descriptors[2]->offset_lock);
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
            spin_lock_init(&tcb->file_descriptors[i]->offset_lock);
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
            tcb->child_descriptors[i]->child = malloc(sizeof(struct Promise));
            promise_init(tcb->child_descriptors[i]->child);
          }
          return i;
        }
      }
      break;
    }
  }

  return -1;
}

void copy_descriptors(struct TCB* src, struct TCB* dst){
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
    if (src->child_descriptors[i] != NULL){
      dst->child_descriptors[i] = src->child_descriptors[i];
      __atomic_fetch_add(&dst->child_descriptors[i]->refcount, 1);
    } else {
      dst->child_descriptors[i] = NULL;
    }
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
        struct Pipe* pipe = (struct Pipe*)descriptor->file;
        if (__atomic_fetch_add((int*)&pipe->refcount, -1) == 1){
          // The pipe owns its ring buffer backing storage and semaphore state.
          // Tear that down exactly once when the last pipe endpoint goes away.
          blocking_ringbuf_destroy(&pipe->buf);
          free(pipe);
        }
      } else if (descriptor->file != NULL){
        node_free(descriptor->file);
      }

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

      if (descriptor == NULL)
        return;

      if (__atomic_fetch_add(&descriptor->refcount, -1) > 1){
        return;
      }
      
      if (descriptor->child != NULL){
        promise_free(descriptor->child);
      }

      free(descriptor);
      break;
    }
  }
}
