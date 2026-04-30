#ifndef ARENA_H
#define ARENA_H

#include "../crt/stddef.h"

struct ArenaBlock {
  struct ArenaBlock* next;
  size_t used;
  size_t cap;
  unsigned char data[1];
};

struct Arena {
  struct ArenaBlock* head;
  size_t block_size;
};

extern struct Arena* arena;

void arena_init(size_t block_size);
void* arena_alloc(size_t size);
void arena_destroy(void);

#endif
