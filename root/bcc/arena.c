#include "../crt/stdlib.h"
#include "../crt/stdint.h"

#include "arena.h"

struct Arena* arena = NULL;

static size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

void arena_init(size_t block_size) {
  arena = (struct Arena*)malloc(sizeof(struct Arena));
  arena->head = NULL;
  if (block_size < 1024) block_size = 1024;
  arena->block_size = block_size;
}

void* arena_alloc(size_t size) {
  if (arena == NULL) return NULL;
  size_t alignment = sizeof(void*);
  size = align_up(size, alignment);
  if (size == 0) size = alignment;

  if (arena->head == NULL || arena->head->used + size > arena->head->cap) {
    size_t cap = arena->block_size;
    if (cap < size) cap = size;
    struct ArenaBlock* block = malloc(sizeof(struct ArenaBlock) + cap - 1);
    if (block == NULL) return NULL;
    block->next = arena->head;
    block->used = 0;
    block->cap = cap;
    arena->head = block;
  }

  void* out = arena->head->data + arena->head->used;
  arena->head->used += size;
  return out;
}

void arena_destroy(void) {
  if (arena == NULL) return;
  struct ArenaBlock* block = arena->head;
  while (block != NULL) {
    struct ArenaBlock* next = block->next;
    free(block);
    block = next;
  }
  arena->head = NULL;
  arena->block_size = 0;
  free(arena);
  arena = NULL;
}
