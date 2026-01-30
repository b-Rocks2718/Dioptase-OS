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
#include "atomic.h"
#include "print.h"
#include "machine.h"
#include "constants.h"
#include "debug.h"

static unsigned n_malloc = 0;
static unsigned n_free = 0;
static unsigned n_leak = 0;

static unsigned *array;
static unsigned len;
static bool safe = false;
static unsigned avail = 0; // index 0 will not be available by design
static int theLock = 0;

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
      panic("bad header index\n");
    }
    unsigned footer = footerFromHeader(i);
    if (footer >= len) {
      panic("bad footer index\n");
    }
    unsigned hv = array[i];
    unsigned fv = array[footer];

    if (hv != fv) {
      panic("bad block\n"); // at i, hv, fv
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

static void makeTaken(unsigned i, unsigned entry_count) {
  assert((entry_count & 1) == 0, "making taken with odd entry count\n");
  array[i] = entry_count + 1;
  array[footerFromHeader(i)] = entry_count + 1;
}

void check_leaks() {
  spin_lock_get(&theLock);
  int args[4] = {n_free, n_leak, n_free + n_leak, n_malloc};
  if (n_free + n_leak != n_malloc) {
    say("| Heap leaks detected: (n_free:%d+n_leak:%d)==%d != n_malloc:%d\n", args);
  } else {
    say("| No heap leaks detected: (n_free:%d+n_leak:%d) == n_malloc:%d\n", args);
  }
  spin_lock_release(&theLock);
}

void heap_init(void* base, unsigned bytes) {
  {
    int args[2] = {(int)base, bytes};
    say("| Heap init (start=0x%X, size=0x%X)\n", args); 
  }

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

  {
    int args[2] = {(int)base, (int)(base) + bytes};
    say("| Heap range 0x%X - 0x%X\n", args);
  }

  /* can't say new becasue we're initializing the heap */
  array = (unsigned *)base;

  len = bytes / 4;
  makeTaken(0, 2);
  makeAvail(2, len - 4);
  makeTaken(len - 2, 2);
}

void *malloc(unsigned bytes) {
  // say("malloc(%d)\n", &bytes);
  if (bytes == 0)
    return (void *)array;

  unsigned entries = ((bytes + 3) / 4) + 4;
  if (entries < 4)
    entries = 4;

  if (entries & 1) {
    entries++;
  }

  spin_lock_get(&theLock);

  void *res = 0;

  unsigned mx = UINT_MAX;
  unsigned it = 0;

  {
    int countDown = 20;
    unsigned p = avail;
    while (p != 0) {
      if (isTaken(p)) {
        say("| block is taken in malloc 0x%X\n", (int*)&p);
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

  spin_lock_get(&theLock);

  n_free += 1;
  int idx = ((((unsigned)p) - ((unsigned)array)) / 4) - 3;
  sanity(idx);
  if (isAvail(idx)) {
    int args[2] = { (int)p, idx };
    say("| freeing free block, p:0x%X idx:%d\n", args);
    panic("double free detected\n");
  }

  int sz = size(idx);

  int leftIndex = left(idx);
  int rightIndex = right(idx);

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