#include "heap.h"
#include "assert.h"
#include "atomic.h"
#include "stdbool.h"
#include "stddef.h"
#include "limits.h"
#include "print.h"
#include "debug.h"
#include "stdlib.h"
#include "sys.h"
#include "string.h"

#define HEAP_WORD_BYTES 4u

static unsigned n_malloc = 0;
static unsigned n_free = 0;
static unsigned n_leak = 0;

static unsigned *array;
static unsigned len;
static bool safe = false;
static unsigned avail = 0; // head of the free-list; index 0 stays reserved as a sentinel
struct SpinLock theLock;

static void makeTaken(unsigned i, unsigned entries);
static void makeAvail(unsigned i, unsigned entries);

// Return whether an allocator block is marked as in use.
static bool isTaken(unsigned i) { return array[i] & 1; }
// Return whether an allocator block is available for reuse.
static bool isAvail(unsigned i) { return !(array[i] & 1); }
// Read the payload size encoded in a block header.
static unsigned size(unsigned i) { return array[i] & ~(unsigned)(1); }

// Recover the block header address from its footer metadata.
unsigned headerFromFooter(unsigned i) { return i - size(i) + 1; }

// Locate the footer corresponding to a block header.
unsigned footerFromHeader(unsigned i) { return i + size(i) - 1; }

// Validate allocator metadata before using a block.
unsigned sanity(unsigned i) {
  if (safe) {
    if (i == 0)
      return 0;
    if (i >= len) {
      int args[3] = {(int)i, (int)len, (int)array};
      printf("| HEAP sanity: i=%d len=%d base=0x%X\n", args);
      panic("bad header index\n");
    }
    unsigned footer = footerFromHeader(i);
    if (footer >= len) {
      int args[3] = {(int)i, (int)footer, (int)len};
      printf("| HEAP bad footer: header=%d footer=%d len=%d\n", args);
      panic("bad footer index\n");
    }
    unsigned hv = array[i];
    unsigned fv = array[footer];

    if (hv != fv) {
      int args[3] = {(int)i, (int)hv, (int)fv};
      printf("| HEAP bad block: header=%d hv=0x%X fv=0x%X\n", args);
      panic("bad block\n");
    }
  }

  return i;
}

// Return the neighboring free block on the left, if present.
static unsigned left(unsigned i) { return sanity(headerFromFooter(i - 1)); }

// Return the neighboring free block on the right, if present.
static unsigned right(unsigned i) { return sanity(i + size(i)); }

// Return the next block in the free-list linkage.
static unsigned next(unsigned i) { return sanity(array[i + 1]); }

// Return the previous block in the free-list linkage.
static unsigned prev(unsigned i) { return sanity(array[i + 2]); }

// Update a free block's forward link.
static void setNext(unsigned i, unsigned x) { array[i + 1] = x; }

// Update a free block's backward link.
static void setPrev(unsigned i, unsigned x) { array[i + 2] = x; }

// detach one free block from the intrusive avail list
static void remove(unsigned i) {
  unsigned prevIndex = prev(i);
  unsigned nextIndex = next(i);

  if (prevIndex == 0) {
    /* at head */
    avail = nextIndex;
  } else {
    /* in the middle */
    setNext(prevIndex, nextIndex);
  }
  if (nextIndex != 0) {
    setPrev(nextIndex, prevIndex);
  }
}

// mark one block free and splice it at the head of the avail list
static void makeAvail(unsigned i, unsigned entry_count) {
  assert((entry_count & 1) == 0, "making avail with odd entry count\n");
  array[i] = entry_count;
  array[footerFromHeader(i)] = entry_count;
  setNext(i, avail);
  setPrev(i, 0);
  if (avail != 0) {
    setPrev(avail, i);
  }
  avail = i;
}

// mark one block allocated by setting the low-bit tag in header/footer
static void makeTaken(unsigned i, unsigned entry_count) {
  assert((entry_count & 1) == 0, "making taken with odd entry count\n");
  array[i] = entry_count + 1;
  array[footerFromHeader(i)] = entry_count + 1;
}

// Map the process heap and seed its boundary-tag free list with one block.
void heap_init(unsigned bytes) {
  void* base = mmap(bytes, MAP_ANON, 0, PROT_READ | PROT_WRITE);

  spin_lock_init(&theLock);

  unsigned alignedBase = ((unsigned)base + 4 + 3) / 4 * 4 - 4;
  assert((alignedBase % 4) == 0, "heap base not aligned to 4 bytes after alignment\n");
  assert(alignedBase >= (unsigned)base, "aligned base is less than base\n");
  unsigned delta = alignedBase - (unsigned)base;
  assert(delta < 4, "delta is too large\n");

  base = (void *)alignedBase;

  assert(bytes >= delta, "bytes is less than delta\n");
  bytes -= delta;

  bytes = bytes / 4 * 4;
  assert((bytes % 4) == 0, "bytes is not a multiple of 4\n");

  assert(bytes >
         32, "bytes is too small\n"); // 8 (start marker) + 16 (one available node) + 8 (end marker)

  /* can't printf new becasue we're initializing the heap */
  array = (unsigned *)base;

  len = bytes / 4;
  // The heap is framed by permanently taken sentinels so free() can safely
  // inspect left/right neighbors without special-casing the ends.
  makeTaken(0, 2);
  makeAvail(2, len - 4);
  makeTaken(len - 2, 2);
}

// Allocate a suitably aligned block from the process heap.
void *malloc(unsigned bytes) {
  // printf("malloc(%d)\n", &bytes);
  if (bytes == 0)
    return (void *)array;

  // Round the payload up without evaluating `bytes + 3`: for requests near
  // UINT_MAX that expression wraps and can turn a huge request into a tiny,
  // apparently successful allocation. Metadata requires four more entries;
  // ceil(UINT_MAX / HEAP_WORD_BYTES) leaves ample unsigned range for them.
  unsigned entries = bytes / HEAP_WORD_BYTES;
  if (bytes % HEAP_WORD_BYTES != 0) {
    entries++;
  }
  entries += 4;
  if (entries < 4)
    entries = 4;

  if (entries & 1) {
    entries++;
  }

  /*
   * Reject a request that cannot fit even in a completely empty configured
   * heap. This is a caller input/range error, not runtime fragmentation, and
   * lets near-UINT_MAX requests fail without perturbing allocator state.
   * Ordinary exhaustion retains the CRT's existing fatal policy so legacy
   * callers that rely on malloc not returning NULL do not dereference it.
   */
  if (entries > len - 4) {
    return NULL;
  }

  spin_lock_acquire(&theLock);

  void *res = 0;

  unsigned mx = UINT_MAX;
  unsigned it = 0;

  {
    // Best-fit search keeps fragmentation lower than first-fit for this simple
    // free-list allocator.
    int countDown = 20;
    unsigned p = avail;
    while (p != 0) {
      if (isTaken(p)) {
        printf("| block is taken in malloc 0x%X\n", (int*)&p);
        panic("heap corruption detected\n");
      }
      unsigned sz = size(p);

      if (sz >= entries) {
        if (sz < mx) {
          mx = sz;
          it = p;
        }
        countDown--;
        if ((countDown == 0) || (sz == entries))
          break;
      }
      p = next(p);
    }
  }

  if (it != 0) {
    remove(it);
    int extra = mx - entries;
    if (extra >= 4) {
      makeTaken(it, entries);
      makeAvail(it + entries, extra);
    } else {
      makeTaken(it, mx);
    }
    res = &array[it + 3];
  }

  if (res != NULL) {
    n_malloc += 1;
  } else {
    int args[1] = {bytes};
    panic("malloc failed\n");
  }

  spin_lock_release(&theLock);

  return res;
}

// Allocate process memory that is intentionally never reclaimed.
void* leak(unsigned bytes){
  __atomic_fetch_add((int*)&n_leak, 1);
  return malloc(bytes);
}

// Compute the payload size for one heap allocation from its user pointer.
// The allocator stores three words of metadata ahead of the payload and one
// trailing footer word, so usable bytes are `(entries - 4) * 4`.
static unsigned payload_bytes_from_pointer(void* p) {
  unsigned p_addr;
  unsigned heap_start;
  unsigned heap_end;
  int idx;

  if (p == NULL || p == (void*)array) {
    return 0;
  }

  p_addr = (unsigned)p;
  heap_start = (unsigned)array;
  heap_end = heap_start + (len * HEAP_WORD_BYTES);
  if (p_addr < heap_start || p_addr >= heap_end ||
      ((p_addr - heap_start) & (HEAP_WORD_BYTES - 1)) != 0) {
    panic("heap: realloc pointer out of range\n");
  }

  idx = ((p_addr - heap_start) / HEAP_WORD_BYTES) - 3;
  if (idx < 0 || (unsigned)idx >= len) {
    panic("heap: realloc index out of range\n");
  }

  sanity((unsigned)idx);
  if (isAvail((unsigned)idx)) {
    panic("heap: realloc on free block\n");
  }

  return (size((unsigned)idx) - 4u) * HEAP_WORD_BYTES;
}

// Resize an allocation while preserving its existing contents.
void* realloc(void* p, unsigned bytes) {
  void* new_ptr;
  unsigned old_bytes;

  if (p == NULL) {
    return malloc(bytes);
  }
  if (bytes == 0) {
    free(p);
    return NULL;
  }

  old_bytes = payload_bytes_from_pointer(p);
  if (bytes <= old_bytes) {
    return p;
  }

  new_ptr = malloc(bytes);
  if (new_ptr == NULL) {
    // Match the malloc failure contract: the original allocation remains
    // owned by the caller when growth cannot be satisfied.
    return NULL;
  }
  memcpy(new_ptr, p, old_bytes);
  free(p);
  return new_ptr;
}

// Return an allocation to the free list and coalesce adjacent free blocks.
void free(void *p) {
  if (p == 0)
    return;
  if (p == (void *)array)
    return;

  spin_lock_acquire(&theLock);

  n_free += 1;
  unsigned p_addr = (unsigned)p;
  unsigned heap_start = (unsigned)array;
  unsigned heap_end = heap_start + (len * HEAP_WORD_BYTES);
  if (p_addr < heap_start || p_addr >= heap_end || ((p_addr - heap_start) & (HEAP_WORD_BYTES - 1)) != 0) {
    panic("heap: free pointer out of range\n");
  }

  int idx = ((p_addr - heap_start) / HEAP_WORD_BYTES) - 3;
  if (idx < 0 || (unsigned)idx >= len) {
    panic("heap: free index out of range\n");
  }
  sanity(idx);
  if (isAvail(idx)) {
    int args[2] = { (int)p, idx };
    printf("| freeing free block, p:0x%X idx:%d\n", args);
    panic("double free detected\n");
  }

  int sz = size(idx);

  int leftIndex = left(idx);
  int rightIndex = right(idx);

  // Coalesce adjacent free blocks before re-inserting the merged block.
  if (isAvail(leftIndex)) {
    remove(leftIndex);
    idx = leftIndex;
    sz += size(leftIndex);
  }

  if (isAvail(rightIndex)) {
    remove(rightIndex);
    sz += size(rightIndex);
  }

  makeAvail(idx, sz);

  spin_lock_release(&theLock);
}
