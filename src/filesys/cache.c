#include "filesys/cache.h"

#include <hash.h>
#include <round.h>
#include <string.h>
#include <stdio.h>
#include <bitmap.h>
#include <stdbool.h>

#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "filesys/filesys.h"

struct block {
  uint8_t byte[BLOCK_SECTOR_SIZE];
};

enum cache_flags {
  CACHE_PINNED =    0x01,
  CACHE_ACCESS =    0x02,
  CACHE_DIRTY =     0x04
};

#define CACHE_SEMA_NUM 16

struct cache_entry {
  block_sector_t sector;
  struct block *data;

  int32_t prev;
  int32_t next;

  uint32_t flags;

  struct semaphore sema;
  struct hash_elem elem;
};

static struct cache_entry *cache_header;

static struct block *cache_data;

static int32_t clock_hand;
static int32_t empty_list;

static struct lock cache_lock;

static struct hash cache_table;

static struct bitmap *busy_map;      /* Record which slots are being accessed */
static struct list busy_waiters;

static inline int32_t *prev(int32_t i) {
  return &cache_header[i].prev;
}

static inline int32_t *next(int32_t i) {
  return &cache_header[i].next;
}

static void header_insert(size_t before, size_t e) {
  *next(e) = before;
  *prev(e) = *prev(before);
  *next(*prev(e)) = e;
  *prev(*next(e)) = e;
}

static void header_remove(size_t e) {
  *next(*prev(e)) = *next(e);
  *prev(*next(e)) = *prev(e);
}


static inline struct cache_entry *elem_to_header(const struct hash_elem *e) {
  return hash_entry(e, struct cache_entry, elem);
}

static inline size_t header_to_idx(const struct cache_entry * const ce) {
  return ce - cache_header;
}

/* Hashing function */
static unsigned header_hash (const struct hash_elem *e, void *aux UNUSED) {
  return hash_int(elem_to_header(e)->sector);
}

static bool header_less (const struct hash_elem *a, 
                         const struct hash_elem *b, 
                         void *aux UNUSED) {
  return elem_to_header(a)->sector < elem_to_header(b)->sector;
}

void cache_init(void) {
  cache_data = palloc_get_multiple(PAL_ASSERT, 
    DIV_ROUND_UP(CACHE_SECTORS, PGSIZE / BLOCK_SECTOR_SIZE));

  lock_init(&cache_lock);

  list_init(&busy_waiters);
  busy_map = bitmap_create(CACHE_SECTORS);

  hash_init(&cache_table, header_hash, header_less, NULL);

  uint32_t i;
  cache_header = malloc(sizeof(*cache_header) * CACHE_SECTORS);
  memset(cache_header, 0, sizeof(*cache_header) * CACHE_SECTORS);

  for (i = 0; i < CACHE_SECTORS; ++i) {
    cache_header[i].data = &cache_data[i];
    cache_header[i].prev = (i + CACHE_SECTORS - 1) % CACHE_SECTORS;
    cache_header[i].next = (i + 1) % CACHE_SECTORS;
  }

  clock_hand = -1;
  empty_list = 0;
}

static int32_t evict(void) {
  int32_t idx;
  int32_t i;
  // printf("evict\n");
  idx = clock_hand;
  header_remove(idx);

  for (i = 0; i < CACHE_SEMA_NUM; ++i)
      sema_down(&cache_header[idx].sema);

  if (*next(clock_hand) != clock_hand)
    clock_hand = *next(clock_hand);
  else
    clock_hand = -1;

  return idx;
}

static int32_t blank(void) {
  int32_t idx;
  // printf("blank\n");
  idx = empty_list;
  header_remove(empty_list);

  if (*next(empty_list) != empty_list)
    empty_list = *next(empty_list);
  else
    empty_list = -1;

  return idx; 
}


static void push_cache(int32_t idx) {
  if (clock_hand == -1) {
    clock_hand = *prev(idx) = *next(idx) = idx;
  } else {
    header_insert(clock_hand, idx);
  }
}

static int32_t get_cache(block_sector_t idx, struct cache_entry **ce) {
  struct cache_entry secidx;
  struct hash_elem *e;
  int32_t old_sector = -1;
  secidx.sector = idx;
  e = hash_find(&cache_table, &secidx.elem);

  if (e != NULL) {
    *ce = hash_entry(e, struct cache_entry, elem);
    old_sector = (*ce)->sector;
  } else {

    if (empty_list != -1) {
      *ce = &cache_header[blank()];
      old_sector = -1;
    }
    else {
      *ce = &cache_header[evict()];
      hash_delete(&cache_table, &(*ce)->elem);
      old_sector = (*ce)->sector;
    }

    (*ce)->sector = idx;
    sema_init(&(*ce)->sema, 0);

    e = hash_insert(&cache_table, &(*ce)->elem);
    ASSERT(e == NULL);
  }

  return old_sector;
}

static void prepare_cache(int32_t old_sector, struct cache_entry *ce) {
  struct hash_elem *e;
  int32_t i;
  if (old_sector != (int32_t) ce->sector) {
    if (old_sector >= 0) {
      if (ce->flags & CACHE_DIRTY) {
        // printf("wb = %x\n", old_sector);
        ce->flags &= ~CACHE_DIRTY;
        block_write(fs_device, old_sector, ce->data);
      }
    }

    block_read(fs_device, ce->sector, ce->data);

    for (i = 0; i < CACHE_SEMA_NUM - 1; ++i)
      sema_up(&ce->sema);

    lock_acquire(&cache_lock);
    push_cache(header_to_idx(ce));
    lock_release(&cache_lock);
  }
}

void cache_read(block_sector_t idx, off_t ofs, uint8_t *dest, size_t size) {
  struct cache_entry *ce;
  int32_t old_sector;
  // printf("r idx = %x\n", idx);

  lock_acquire(&cache_lock);
  old_sector = get_cache(idx, &ce);
  lock_release(&cache_lock);
  
  prepare_cache(old_sector, ce);

  // printf("c = %x, d = %x, o = %x, s = %x\n", header_to_idx(ce), (uintptr_t) dest, ofs, size);
  
  ce->flags |= CACHE_ACCESS;
  memcpy(dest, (uint8_t *) ce->data + ofs, size);
  sema_up(&ce->sema);
}

void cache_write(block_sector_t idx, off_t ofs, 
                 const uint8_t *src, size_t size) {
  struct cache_entry *ce;
  int32_t old_sector;
  // printf("w idx = %x\n", idx);

  lock_acquire(&cache_lock);
  old_sector = get_cache(idx, &ce);
  lock_release(&cache_lock);

  prepare_cache(old_sector, ce);

  // printf("c = %x, s = %x, o = %x, s = %x\n", header_to_idx(ce), (uintptr_t) src, ofs, size);

  ce->flags |= (CACHE_ACCESS | CACHE_DIRTY);
  memcpy((uint8_t *) ce->data + ofs, src, size);
  sema_up(&ce->sema);
}

void cache_flush(void) {
  int32_t start;
  struct cache_entry *ce;

  if (clock_hand == -1)
    return;

  start = clock_hand; 

  do {
    ce = &cache_header[clock_hand];

    if (ce->flags & CACHE_DIRTY) {
      ce->flags &= ~CACHE_DIRTY;
      block_write(fs_device, ce->sector, ce->data);
    }

    clock_hand = *next(clock_hand);

  } while (clock_hand != start);
}

static void daemon(void *aux UNUSED) {
  
}