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
#include "devices/timer.h"

/* Stores data in a sector */
struct block {
  uint8_t byte[BLOCK_SECTOR_SIZE];
};

enum cache_flags {
  CACHE_PRESENT =   0x01, /* Has data */
  CACHE_ACCESS =    0x02, /* Accessed recently */
  CACHE_DIRTY =     0x04, /* Dirty */
  CACHE_EMPTYLIST = 0x10, /* In the empty list */
  CACHE_CLOCKLIST = 0x20  /* Evictable */
};

/* Parameter for mimicking RW lock with semaphore */
#define CACHE_SEMA_NUM 16

struct cache_entry {
  block_sector_t sector;  /* Sector index */
  struct block *data;     /* Pointer to content */

  int32_t prev;           /* Previous one in the list */
  int32_t next;           /* Next one in the list */

  uint32_t flags;

  struct semaphore sema;  /* Read and write semaphores */
  struct hash_elem elem;
};

/* Start of cache descriptor */
static struct cache_entry *cache_header;

/* Start of cache storage */
static struct block *cache_data;

/* Candidate cache slot to evict */
static int32_t clock_hand;

/* Unused cache slots */
static int32_t empty_list;

/* Protects cache data structure */
static struct lock cache_lock;

/* Lookup cache descriptor from sector index */
static struct hash cache_table;

/* Store the read-ahead sector */
static block_sector_t read_ahead_sector;
/* Semaphores for passing read_ahead_sector to background service */
static struct semaphore read_ahead_sema_r;
static struct semaphore read_ahead_sema_w;

/* Indicates whether the cache system is running */
static bool running;

static void write_behind_daemon(void *aux UNUSED);
static void read_ahead_daemon(void *aux UNUSED);

/* Pointer to index of the previous element in the list */
static inline int32_t *prev(int32_t i) {
  return &cache_header[i].prev;
}

/* Pointer to index of the next element in the list */
static inline int32_t *next(int32_t i) {
  return &cache_header[i].next;
}

/* Insert descriptor into a list */
static void header_insert(int32_t before, int32_t e) {
  ASSERT(before >= 0);
  ASSERT(e >= 0);

  /* Update linkage */
  *next(e) = before;
  *prev(e) = *prev(before);
  *next(*prev(before)) = e;
  *prev(before) = e;

  /* Copy some flags */
  cache_header[e].flags |= 
      cache_header[before].flags & (CACHE_EMPTYLIST | CACHE_CLOCKLIST);
}

/* Remove descriptor from a list */
static void header_remove(int32_t e) {
  ASSERT(e >= 0);
  /* Update linkage */
  *next(*prev(e)) = *next(e);
  *prev(*next(e)) = *prev(e);

  /* Clear some flags */
  cache_header[e].flags &= ~(CACHE_EMPTYLIST | CACHE_CLOCKLIST);
}

static inline struct cache_entry *elem_to_header(const struct hash_elem *e) {
  return hash_entry(e, struct cache_entry, elem);
}

/* Pointer to array index */
static inline size_t header_to_idx(const struct cache_entry * const ce) {
  return ce - cache_header;
}

/* Hash function */
static unsigned header_hash (const struct hash_elem *e, void *aux UNUSED) {
  return hash_int(elem_to_header(e)->sector);
}

/* Hash comparator */
static bool header_less (const struct hash_elem *a, 
                         const struct hash_elem *b, 
                         void *aux UNUSED) {
  return elem_to_header(a)->sector < elem_to_header(b)->sector;
}

/* Initialize cache system */
void cache_init(void) {
  uint32_t i;

  cache_data = palloc_get_multiple(PAL_ASSERT, 
    DIV_ROUND_UP(CACHE_SECTORS, PGSIZE / BLOCK_SECTOR_SIZE));

  lock_init(&cache_lock);

  hash_init(&cache_table, header_hash, header_less, NULL);

  cache_header = malloc(sizeof(*cache_header) * CACHE_SECTORS);
  memset(cache_header, 0, sizeof(*cache_header) * CACHE_SECTORS);

  for (i = 0; i < CACHE_SECTORS; ++i) {
    cache_header[i].data = &cache_data[i];
    cache_header[i].prev = (i + CACHE_SECTORS - 1) % CACHE_SECTORS;
    cache_header[i].next = (i + 1) % CACHE_SECTORS;
  }

  clock_hand = -1;
  empty_list = 0;

  sema_init(&read_ahead_sema_r, 0);
  sema_init(&read_ahead_sema_w, 1);

  running = true;

  /* Start background services */
  thread_create("writed", PRI_DEFAULT, write_behind_daemon, NULL);
  thread_create("readd", PRI_DEFAULT, read_ahead_daemon, NULL);
}

/* Find a cache slot to evict */
static int32_t evict(void) {
  int32_t idx;
  int32_t i;
  bool found = false;

  idx = clock_hand;

  /* Not dirty, not accessed recently */
  do {
    if (!(cache_header[idx].flags & (CACHE_DIRTY | CACHE_ACCESS))) {
      found = true;
      break;
    }
    idx = *next(idx);
  } while (idx != clock_hand);

  /* Not dirty */
  if (!found) {
    do {
      if (!(cache_header[idx].flags & CACHE_DIRTY)) {
        found = true;
        break;
      }
      idx = *next(idx);
    } while (idx != clock_hand);
  }

  /* Not accessed recently */
  if (!found) {
    do {
      if (!(cache_header[idx].flags & CACHE_ACCESS)) {
        found = true;
        break;
      }
      idx = *next(idx);
    } while (idx != clock_hand);
  }

  /* FIFO */
  clock_hand = idx;

  header_remove(idx);

  /* Requires exclusive access */
  for (i = 0; i < CACHE_SEMA_NUM; ++i)
      sema_down(&cache_header[idx].sema);

  if (*next(clock_hand) != clock_hand)
    clock_hand = *next(clock_hand);
  else
    clock_hand = -1;

  return idx;
}

/* Get a cache slot from empty list */
static int32_t blank(void) {
  int32_t idx;

  idx = empty_list;
  header_remove(empty_list);

  if (*next(empty_list) != empty_list)
    empty_list = *next(empty_list);
  else
    empty_list = -1;

  return idx; 
}

/* Make the cache evictable */
static void push_cache(int32_t idx) {
  if (clock_hand == -1) {
    clock_hand = idx;
    *prev(idx) = idx;
    *next(idx) = idx;
    cache_header[idx].flags &= ~CACHE_EMPTYLIST;
    cache_header[idx].flags |= CACHE_CLOCKLIST;
  } else {
    header_insert(clock_hand, idx);
  }
}

/* Secures a cache slot, either from empty list or evicting one */
static int32_t get_cache(block_sector_t idx, struct cache_entry **ce) {
  struct cache_entry secidx;
  struct hash_elem *e;
  int32_t old_sector = -1;
  secidx.sector = idx;
  e = hash_find(&cache_table, &secidx.elem);

  if (e != NULL) {
    /* Data already in cache */
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

/* Write back data from evicted cache to disk */
static void write_back(int32_t old_sector, struct cache_entry *ce) {
  if (old_sector != (int32_t) ce->sector) {
    if (old_sector >= 0) {
      if (ce->flags & CACHE_DIRTY) {
        block_write(fs_device, old_sector, ce->data);
        ce->flags &= ~CACHE_DIRTY;
      }
    }

    ce->flags = 0;
  }
}

/* Acquires a cache slot ready for R/W */
static struct cache_entry *fetch(block_sector_t idx) {
  struct cache_entry *ce;
  int32_t old_sector;

  lock_acquire(&cache_lock);
  old_sector = get_cache(idx, &ce);
  lock_release(&cache_lock);

  write_back(old_sector, ce);

  return ce;
}

/* Request read-ahead */
void cache_prefetch(block_sector_t idx) {
  return;
  if (running && sema_try_down(&read_ahead_sema_w)) {
    read_ahead_sector = idx;
    sema_up(&read_ahead_sema_r);
  }
}

/* Read from sector via cache
 * idx: sector index
 * ofs: offset within the sector
 * dest: buffer
 * size: bytes to read */
void cache_read(block_sector_t idx, off_t ofs, uint8_t *dest, size_t size) {
  struct cache_entry *ce;
  int32_t i;

  ce = fetch(idx);

  if (!sema_try_down(&ce->sema) && !(ce->flags & CACHE_PRESENT)) {
    block_read(fs_device, ce->sector, ce->data);
    ce->flags = CACHE_PRESENT;

    /* Allows R/W access */
    for (i = 0; i < CACHE_SEMA_NUM - 1; ++i)
      sema_up(&ce->sema);

    lock_acquire(&cache_lock);
    push_cache(header_to_idx(ce));
    lock_release(&cache_lock);
  }

  ce->flags |= CACHE_ACCESS;
  if (size > 0)
    memcpy(dest, (uint8_t *) ce->data + ofs, size);
  sema_up(&ce->sema);
}

/* Write to sector via cache
 * idx: sector index
 * ofs: offset within the sector
 * dest: buffer
 * size: bytes to write
 * toread: true when only part of the sector is written */
void cache_write(block_sector_t idx, off_t ofs, 
                 const uint8_t *src, size_t size, bool toread) {
  struct cache_entry *ce;
  int32_t i;

  ce = fetch(idx);

  if (!sema_try_down(&ce->sema) && !(ce->flags & CACHE_PRESENT)) {
    if (toread)
      /* Read from disk for partial writing */
      block_read(fs_device, ce->sector, ce->data);
    else
      /* Or fill with zeroes */
      memset(ce->data, 0, BLOCK_SECTOR_SIZE);

    ce->flags = CACHE_PRESENT;

    /* Allows R/W access */
    for (i = 0; i < CACHE_SEMA_NUM - 1; ++i)
      sema_up(&ce->sema);

    lock_acquire(&cache_lock);
    push_cache(header_to_idx(ce));
    lock_release(&cache_lock);
  }

  ce->flags |= (CACHE_ACCESS | CACHE_DIRTY);
  memcpy((uint8_t *) ce->data + ofs, src, size);
  sema_up(&ce->sema);
}

/* Flushes cache and terminates background services */
void cache_close(void) {
  int32_t start, i;
  struct cache_entry *ce;

  running = false;
  sema_up(&read_ahead_sema_r);

  if (clock_hand == -1)
    return;

  start = clock_hand; 

  do {
    ce = &cache_header[clock_hand];

    if (ce->flags & CACHE_DIRTY) {
      /* Requires exclusive access */
      for (i = 0; i < CACHE_SEMA_NUM; ++i)
        sema_down(&ce->sema);
      ce->flags &= ~CACHE_DIRTY;
      block_write(fs_device, ce->sector, ce->data);
    }

    clock_hand = *next(clock_hand);

  } while (clock_hand != start);
}

/* Performs write-back periodically */
static void write_behind_daemon(void *aux UNUSED) {
  int32_t start, i;
  struct cache_entry *ce;

  while (running) {
    timer_sleep(1);
    if (!running) {
      thread_current()->exit_status = 0;
      thread_current()->ashes->exit_status = 0;
      thread_exit();
    }

    if (clock_hand == -1)
      continue;

    start = clock_hand;

    do {
      ce = &cache_header[clock_hand];

      if (ce->flags & CACHE_DIRTY) {
        /* Requires exclusive access. Gives up without waiting if failed */
        for (i = 0; i < CACHE_SEMA_NUM; ++i)
          if (!sema_try_down(&ce->sema))
            break;
        if (i == CACHE_SEMA_NUM) {
          block_write(fs_device, ce->sector, ce->data);
          ce->flags &= ~CACHE_DIRTY;
        }
        for (; i > 0; --i)
          sema_up(&ce->sema);
      }

      ce->flags &= ~CACHE_ACCESS;

      clock_hand = *next(clock_hand);

    } while (clock_hand != start);
  }
}

/* Performs read-ahead upon request asynchronously */
static void read_ahead_daemon(void *aux UNUSED) {
  uint8_t dest;
  block_sector_t idx;
  while (running) {
    sema_down(&read_ahead_sema_r);
    if (!running) {
      thread_current()->exit_status = 0;
      thread_current()->ashes->exit_status = 0;
      thread_exit();
    }
    idx = read_ahead_sector;
    sema_up(&read_ahead_sema_w);

    /* Load data from disk into cache by fake reading */
    cache_read(idx, 0, &dest, 0);
  }
}
