#include "heap.h"

#include "atomic.h"

#define MIN_ALLOC_SZ 4

#define MIN_WILDERNESS 0x2000
#define MAX_WILDERNESS 0x1000000

#define BIN_COUNT 9
#define BIN_MAX_IDX 8 // 9 - 1

static int heap_lock = 0;

struct node_t {
  unsigned hole;
  unsigned size;
  struct node_t* next;
  struct node_t* prev;
};

struct footer_t { 
  struct node_t* header;
};

struct bin_t {
  struct node_t* head;
};

struct heap_t {
  int start;
  int end;
  struct bin_t* bins[BIN_COUNT];
};

static unsigned overhead = sizeof(struct footer_t) + sizeof(struct node_t);

static unsigned offset = 8;

static struct heap_t global_heap;

static unsigned get_bin_index(unsigned sz);
static void create_foot(struct node_t* head);
static struct footer_t* get_foot(struct node_t* head);

static void add_node(struct bin_t *bin, struct node_t *node);

static void remove_node(struct bin_t *bin, struct node_t *node);

static struct node_t* get_best_fit(struct bin_t* list, unsigned size);
static struct node_t* get_last_node(struct bin_t* list);

static struct node_t* next(struct node_t* current);
static struct node_t* prev(struct node_t* current);

void add_node(struct bin_t* bin, struct node_t* node) {
  node->next = 0;
  node->prev = 0;

  struct node_t* temp = bin->head;

  if (bin->head == 0) {
    bin->head = node;
    return;
  }
  
  // we need to save next and prev while we iterate
  struct node_t* current = bin->head;
  struct node_t* previous = 0;
  // iterate until we get the the end of the list or we find a 
  // node whose size is
  while (current != 0 && current->size <= node->size) {
    previous = current;
    current = current->next;
  }

  if (current == 0) { // we reached the end of the list
    previous->next = node;
    node->prev = previous;
  }
  else {
    if (previous != 0) { // middle of list, connect all links!
      node->next = current;
      previous->next = node;

      node->prev = previous;
      current->prev = node;
    }
    else { // head is the only element
      node->next = bin->head;
      bin->head->prev = node;
      bin->head = node;
    }
  }
}

void remove_node(struct bin_t * bin, struct node_t *node) {
  if (bin->head == 0) return; 
  if (bin->head == node) { 
      bin->head = bin->head->next;
      return;
  }
  
  struct node_t* temp = bin->head->next;
  while (temp != 0) {
    if (temp == node) { // found the node
      if (temp->next == 0) { // last item
        temp->prev->next = 0;
      }
      else { // middle item
        temp->prev->next = temp->next;
        temp->next->prev = temp->prev;
      }
      // we dont worry about deleting the head here because we already checked that
      return;
    }
    temp = temp->next;
  }
}

struct node_t* get_best_fit(struct bin_t *bin, unsigned size) {
  if (bin->head == 0) return 0; // empty list!

  struct node_t* temp = bin->head;

  while (temp != 0) {
    if (temp->size >= size) {
      return temp; // found a fit!
    }
    temp = temp->next;
  }
  return 0; // no fit!
}

struct node_t *get_last_node(struct bin_t *bin) {
  struct node_t *temp = bin->head;

  while (temp->next != 0) {
      temp = temp->next;
  }
  return temp;
}

void heap_init(void* start, unsigned size) {
  get_spinlock(&heap_lock);

  for (unsigned i = 0; i < BIN_COUNT; i++) {
    global_heap.bins[i] = (struct bin_t*)((unsigned)start - 4 * (i + 1)); // reserve a struct bin_t (4 bytes)
    global_heap.bins[i]->head = 0;
  }

  struct node_t *init_region = (struct node_t*) start;
  init_region->hole = 1;
  init_region->size = size - sizeof(struct node_t) - sizeof(struct footer_t);

  create_foot(init_region);

  add_node(global_heap.bins[get_bin_index(init_region->size)], init_region);

  global_heap.start = (unsigned)start;
  global_heap.end   = (unsigned)(start) + size;

  release_spinlock(&heap_lock);
}

void* malloc(unsigned size) {
  get_spinlock(&heap_lock);

  unsigned index = get_bin_index(size);
  struct bin_t* temp = (struct bin_t*)global_heap.bins[index];
  struct node_t* found = get_best_fit(temp, size);

  while (found == 0) {
    if (index + 1 >= BIN_COUNT){
      release_spinlock(&heap_lock);
      return 0;
    }

    temp = global_heap.bins[++index];
    found = get_best_fit(temp, size);
  }

  if ((found->size - size) > (overhead + MIN_ALLOC_SZ)) {
    struct node_t* split = (struct node_t*)(((char *) found + sizeof(struct node_t) + sizeof(struct footer_t)) + size);
    split->size = found->size - size - sizeof(struct node_t) - sizeof(struct footer_t);
    split->hole = 1;
  
    create_foot(split);

    unsigned new_idx = get_bin_index(split->size);

    add_node(global_heap.bins[new_idx], split); 

    found->size = size; 
    create_foot(found); 
  }

  found->hole = 0; 
  remove_node(global_heap.bins[index], found); 

  found->prev = 0;
  found->next = 0;

  release_spinlock(&heap_lock);

  return &found->next; 
}

void free(void *p) {
  get_spinlock(&heap_lock);

  struct bin_t *list;
  struct footer_t *new_foot;
  struct footer_t *old_foot;

  struct node_t *head = (struct node_t*) ((char *) p - offset);
  if (head == (struct node_t*)global_heap.start) {
      head->hole = 1; 
      add_node(global_heap.bins[get_bin_index(head->size)], head);
      release_spinlock(&heap_lock);
      return;
  }

  struct node_t *next = (struct node_t *) ((char *) get_foot(head) + sizeof(struct footer_t));
  struct footer_t *f = (struct footer_t *) ((char *) head - sizeof(struct footer_t));
  struct node_t *prev = f->header;
  
  if (prev->hole) {
    list = global_heap.bins[get_bin_index(prev->size)];
    remove_node(list, prev);

    prev->size += overhead + head->size;
    new_foot = get_foot(head);
    new_foot->header = prev;

    head = prev;
  }

  if (next->hole) {
    list = global_heap.bins[get_bin_index(next->size)];
    remove_node(list, next);

    head->size += overhead + next->size;

    old_foot = get_foot(next);
    old_foot->header = 0;
    next->size = 0;
    next->hole = 0;
    
    new_foot = get_foot(head);
    new_foot->header = head;
  }

  head->hole = 1;
  add_node(global_heap.bins[get_bin_index(head->size)], head);

  release_spinlock(&heap_lock);
}

static unsigned get_bin_index(unsigned sz) {
  unsigned index = 0;
  sz = sz < 4 ? 4 : sz;

  while (sz >>= 1) index++; 
  index -= 2; 
    
  if (index > BIN_MAX_IDX) index = BIN_MAX_IDX; 
  return index;
}

static void create_foot(struct node_t *head) {
  struct footer_t *foot = get_foot(head);
  foot->header = head;
}

static struct footer_t *get_foot(struct node_t *node) {
  return (struct footer_t*) ((char*) node + sizeof(struct node_t) + node->size);
}
