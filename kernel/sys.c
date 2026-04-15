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

#define INITIAL_USER_STACK_SIZE 0x4000
#define SYSCALL_MAX_PATH_BYTES 1024
#define SYSCALL_MAX_IO_BYTES 1024

static unsigned trap_test_syscall_handler(int arg){
  say("***test_syscall arg = %d\n", &arg);
  return arg + 7;
}

static unsigned unrecognized_trap_handler(unsigned code){
  say("| trap: unrecognized trap code = %d\n", &code);
  panic("trap: halting due to unrecognized trap.\n");
  return 0;
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

// Dispatch user-mode trap requests after trap_handler_ has preserved
// the hardware trap frame and switched into the kernel C calling convention
int trap_handler(unsigned code,
    int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7,
    bool* return_to_user){

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
      int was = interrupts_disable();
      struct TCB* tcb = get_current_tcb();
      interrupts_restore(was);

      char* path = (char*)arg1;
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
    case TRAP_READ: {
      int fd = arg1;
      char* buf = (char*)arg2;
      unsigned count = arg3;

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
      } else if (type == FILE_DESCRIPTOR_STDOUT || type == FILE_DESCRIPTOR_STDERR){
        return 0;
      }

      struct Node* file_node = tcb->file_descriptors[fd]->file;
      if (file_node == NULL){
        return -1;
      }

      unsigned offset = tcb->file_descriptors[fd]->offset;
      unsigned file_size = node_size_in_bytes(file_node);
      if (offset >= file_size){
        return 0;
      }

      unsigned bytes_to_read = count;
      if (count > file_size - offset){
        bytes_to_read = file_size - offset;
      } 

      char* kbuf = malloc(bytes_to_read);
      unsigned rounded_offset = offset & ~(FRAME_SIZE - 1);
      unsigned rounded_bytes = (bytes_to_read + (offset - rounded_offset) + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
      char* mmapped_file = mmap(rounded_bytes, file_node, rounded_offset, MMAP_READ | MMAP_SHARED);
      memcpy(kbuf, mmapped_file + (offset - rounded_offset), bytes_to_read);
      munmap(mmapped_file);

      int rc = copy_to_user(buf, kbuf, bytes_to_read, tcb);
      free(kbuf);
      if (rc != 0){
        return -1;
      }

      tcb->file_descriptors[fd]->offset = offset + bytes_to_read;
      return bytes_to_read;
    }
    case TRAP_WRITE: {
      int fd = arg1;
      char* buf = (char*)arg2;
      unsigned count = arg3;

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
      if (type == FILE_DESCRIPTOR_STDIN){
        free(kbuf);
        return 0;
      } else if (type == FILE_DESCRIPTOR_STDOUT || type == FILE_DESCRIPTOR_STDERR){
        for (unsigned i = 0; i < count; i++){
          putchar(kbuf[i]);
        }
        free(kbuf);
        return count;
      }

      struct Node* file_node = tcb->file_descriptors[fd]->file;
      if (file_node == NULL){
        free(kbuf);
        return -1;
      }

      unsigned offset = tcb->file_descriptors[fd]->offset;
      if (count > UINT_MAX - offset){
        free(kbuf);
        return -1;
      }

      unsigned rounded_offset = offset & ~(FRAME_SIZE - 1);
      unsigned rounded_bytes = count + (offset - rounded_offset);

      char* mmapped_file = mmap(rounded_bytes, file_node, rounded_offset, MMAP_READ | MMAP_WRITE | MMAP_SHARED);
      memcpy(mmapped_file + (offset - rounded_offset), kbuf, count);
      munmap(mmapped_file);
      free(kbuf);
      
      tcb->file_descriptors[fd]->offset = offset + count;
      return count;
    }
    case TRAP_CLOSE: {
      int was = interrupts_disable();
      struct TCB* tcb = get_current_tcb();
      interrupts_restore(was);

      if (arg1 < 0 || arg1 >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[arg1] == NULL){
        return -1;
      }
      
      deallocate_descriptor(tcb, DESCRIPTOR_FILE, arg1);
      return 0;
    }
    case TRAP_SEM_OPEN: {
      panic("sem syscalls not implemented\n");
    }
    case TRAP_SEM_UP: {
      panic("sem syscalls not implemented\n");
    }
    case TRAP_SEM_DOWN: {
      panic("sem syscalls not implemented\n");
    }
    case TRAP_SEM_CLOSE: {
      panic("sem syscalls not implemented\n");
    }
    case TRAP_MMAP: {
      panic("mmap syscall not implemented\n");
    }
    case TRAP_FORK: {
      panic("fork syscall not implemented\n");
    }
    case TRAP_EXEC: {
      panic("exec syscall not implemented\n");
    }
    case TRAP_PLAY_AUDIO: {
      int fd = arg1;

      int was = interrupts_disable();
      struct TCB* tcb = get_current_tcb();
      interrupts_restore(was);

      if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[fd] == NULL){
        return -1;
      }

      struct Node** audio_node_arg = malloc(sizeof(struct Node*));
      *audio_node_arg = node_clone(tcb->file_descriptors[fd]->file);
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
    case TRAP_SET_TEXT_COLOR: {
      int color = arg1;
      preempt_spin_lock_acquire(&print_lock);
      text_color = color;
      preempt_spin_lock_release(&print_lock);
      return 0;
    }
    case TRAP_WAIT_CHILD: {
      panic("wait_child syscall not implemented\n");
    }
    case TRAP_CHDIR: {
      int was = interrupts_disable();
      struct TCB* tcb = get_current_tcb();
      interrupts_restore(was);

      char* path = (char*)arg1;
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
      
      node_free(tcb->cwd);

      tcb->cwd = file_node;
      return 0;
    }
    case TRAP_PIPE: {
      panic("pipe syscall not implemented\n");
    }
    case TRAP_DUP: {
      int was = interrupts_disable();
      struct TCB* tcb = get_current_tcb();
      interrupts_restore(was);

      if (arg1 < 0 || arg1 >= MAX_FILE_DESCRIPTORS || tcb->file_descriptors[arg1] == NULL){
        return -1;
      }

      int fd = allocate_descriptor(tcb, DESCRIPTOR_FILE, false);
      if (fd < 0){
        return -1;
      }

      __atomic_fetch_add(&tcb->file_descriptors[arg1]->refcount, 1);
      tcb->file_descriptors[fd] = tcb->file_descriptors[arg1];
      
      return fd;
    }
    default: {
      unrecognized_trap_handler(code);
      break;
    }
  }
}

void trap_init(void) {
  register_handler((void*)trap_handler_, (void*)TRAP_IVT_ENTRY);
}

// run a user program given a node representing its ELF file
// consumes the node, so the caller cannot use it after calling this function
int run_user_program(struct Node* prog_node){
  unsigned size = node_size_in_bytes(prog_node);
  unsigned* prog = mmap(size, prog_node, 0, MMAP_READ);
  node_free(prog_node);
  
  unsigned entry = elf_load(prog);

  // The initial user stack grows downward, so reserve it from the top of the
  // user half and enter at the last word in that reservation.
  unsigned* stack = mmap_stack(INITIAL_USER_STACK_SIZE,
    MMAP_READ | MMAP_WRITE | MMAP_USER);

  return jump_to_user(entry,
    (unsigned)stack + INITIAL_USER_STACK_SIZE - sizeof(unsigned));
}

void init_descriptors(struct TCB* tcb, bool init_stdio){
  if (init_stdio){
    // User-entering threads need the conventional stdio descriptors from boot.
    tcb->file_descriptors[0] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[0]->refcount = 1;
    tcb->file_descriptors[0]->offset = 0;
    tcb->file_descriptors[0]->type = FILE_DESCRIPTOR_STDIN;
    tcb->file_descriptors[0]->file = NULL;
    
    tcb->file_descriptors[1] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[1]->refcount = 1;
    tcb->file_descriptors[1]->offset = 0;
    tcb->file_descriptors[1]->type = FILE_DESCRIPTOR_STDOUT;
    tcb->file_descriptors[1]->file = NULL;

    tcb->file_descriptors[2] = malloc(sizeof(struct FileDescriptor));
    tcb->file_descriptors[2]->refcount = 1;
    tcb->file_descriptors[2]->offset = 0;
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
            tcb->sem_descriptors[i]->sem = NULL;
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
            tcb->child_descriptors[i]->child = NULL;
          }
          return i;
        }
      }
      break;
    }
  }

  return -1;
}

void deallocate_descriptor(struct TCB* tcb, enum DescriptorType type, int index){
  switch (type){
    case DESCRIPTOR_FILE: {
      if (__atomic_fetch_add(&tcb->file_descriptors[index]->refcount, -1) > 1){
        return;
      }

      if (tcb->file_descriptors[index]->file != NULL){
        node_free(tcb->file_descriptors[index]->file);
      }

      free(tcb->file_descriptors[index]);
      tcb->file_descriptors[index] = NULL;

      break;
    }
    case DESCRIPTOR_SEM: {
      if (__atomic_fetch_add(&tcb->sem_descriptors[index]->refcount, -1) > 1){
        return;
      }

      if (tcb->sem_descriptors[index]->sem != NULL){
        sem_free(tcb->sem_descriptors[index]->sem);
      }

      free(tcb->sem_descriptors[index]);
      tcb->sem_descriptors[index] = NULL;

      break;
    }
    case DESCRIPTOR_CHILD: {
      if (__atomic_fetch_add(&tcb->child_descriptors[index]->refcount, -1) > 1){
        return;
      }
      
      if (tcb->child_descriptors[index]->child != NULL){
        promise_free(tcb->child_descriptors[index]->child);
      }

      free(tcb->child_descriptors[index]);
      tcb->child_descriptors[index] = NULL;
      break;
    }
  }
}
