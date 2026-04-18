/* Copyright (C) 2025 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "heap.h"
#include "constants.h"
#include "atomic.h"
#include "sys.h"
#include "print.h"
#include "debug.h"

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

static bool isTaken(unsigned i) { return array[i] & 1; }
static bool isAvail(unsigned i) { return !(array[i] & 1); }
static unsigned size(unsigned i) { return array[i] & ~(unsigned)(1); }

unsigned headerFromFooter(unsigned i) { return i - size(i) + 1; }

unsigned footerFromHeader(unsigned i) { return i + size(i) - 1; }

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

static unsigned left(unsigned i) { return sanity(headerFromFooter(i - 1)); }

static unsigned right(unsigned i) { return sanity(i + size(i)); }

static unsigned next(unsigned i) { return sanity(array[i + 1]); }

static unsigned prev(unsigned i) { return sanity(array[i + 2]); }

static void setNext(unsigned i, unsigned x) { array[i + 1] = x; }

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

void heap_init(unsigned bytes) {
 void* base = mmap(bytes, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);

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

void *malloc(unsigned bytes) {
  // printf("malloc(%d)\n", &bytes);
  if (bytes == 0)
    return (void *)array;

  unsigned entries = ((bytes + 3) / 4) + 4;
  if (entries < 4)
    entries = 4;

  if (entries & 1) {
    entries++;
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

void* leak(unsigned bytes){
  __atomic_fetch_add((int*)&n_leak, 1);
  return malloc(bytes);
}

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
