#include "filesys/cache.h"

#include <hash.h>
#include <round.h>
#include <string.h>

#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/malloc.h"

struct block {
  char c[BLOCK_SECTOR_SIZE];
};

enum cache_flags {
  CACHE_PRESENT =   0x01,
  CACHE_ACCESS =    0x02,
  CACHE_DIRTY =     0x04
};

struct cache_entry {
  block_sector_t sector;
  struct block *data;

  size_t prev;
  size_t next;

  uint32_t flags;

  struct hash_elem elem;
};

static struct cache_entry *cache_header;

static struct block *cache_data;

static struct cache_entry *clock_hand;
static struct cache_entry *empty_list;

static struct lock cache_lock;

static inline size_t *prev(size_t i) {
  return &cache_header[i].prev;
}

static inline size_t *next(size_t i) {
  return &cache_header[i].next;
}

static void header_insert(size_t after, size_t e) {
  *next(e) = *next(after);
  *next(after) = e;
}

static void header_remove(size_t after) {
  *next(after) = *next(*next(after));
}

void cache_init(void) {
  cache_data = palloc_get_multiple(PAL_ASSERT, 
    DIV_ROUND_UP(CACHE_SECTORS, PGSIZE / BLOCK_SECTOR_SIZE));

  cache_header = malloc(sizeof(*cache_header) * CACHE_SECTORS);
  memset(cache_header, 0, sizeof(*cache_header) * CACHE_SECTORS);

  lock_init(&cache_lock);

  clock_hand = NULL;

  uint32_t i;
  for (i = 0; i < CACHE_SECTORS; ++i) {
    cache_header[i].data = cache_data + i;
    // cache_header[i].prev = (i + CACHE_SECTORS - 1) % CACHE_SECTORS;
    cache_header[i].next = (i + 1) % CACHE_SECTORS;
  }

  empty_list = cache_header + (CACHE_SECTORS - 1);
}