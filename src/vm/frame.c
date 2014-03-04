#include "vm/frame.h"
#include <debug.h>
#include <list.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"

/* An array holding all struct frame_entry */
static struct frame_entry *frame_table;

/* Clock hand */
static int32_t frame_to_evict;

/* realloc frame_table when table_size > table_capacity */
static size_t table_capacity;
static size_t table_size;

/* A lock protecting frame_table */
static struct lock table_lock;

void frame_init(void) {
  table_size = 0;
  table_capacity = 128;
  frame_table = malloc(table_capacity * sizeof(struct frame_entry));
  lock_init(&table_lock);

  // can be anything since frame_list is empty initially
  frame_to_evict = -1;
}

static inline int32_t *prev(int32_t i) {
  return &frame_table[i].prev;
}

static inline int32_t *next(int32_t i) {
  return &frame_table[i].prev;
}

void *frame_get_page(enum palloc_flags flags) {
  void *page = palloc_get_page(flags);
  struct frame_entry *f;

  /* Skip for kernel page */
  if (flags & PAL_USER) {
    lock_acquire(&table_lock);

    if (table_size >= table_capacity) {
      /* Double the capacity */
      table_capacity *= 2;
      /* and realloc */
      frame_table = realloc(frame_table, 
                            table_capacity * sizeof(struct frame_entry));
    }

    /* TODO: Paging */
    if (page == NULL)
      PANIC("frame_get_page: out of user pages");

    f = frame_table + table_size;

    f->page = page;

    if (table_size > 0) {
      /* Insert frame just before frame_to_evict */
      f->prev = *prev(frame_to_evict);
      f->next = frame_to_evict;
      *next(f->prev) = table_size;
      *prev(f->next) = table_size;
    } else {
      f->prev = f->next = frame_to_evict = 0;
    }

    ++table_size;

    lock_release(&table_lock);
  }

  return page;
}

void frame_free_page(void *page)
{
  if (palloc_from_user(page))
  {
    size_t i;

    palloc_free_page(page);

    lock_acquire(&table_lock);

    /* Locate the page in the frame table */
    /* TODO: Use hash table? */
    for (i = 0; i < table_size; i++) {
      if (frame_table[i].page == page)
        break;
    }

    if (i >= table_size)
      PANIC("frame_free_page: cannot find page in frame table");

    --table_size;

    if (table_size == 0) {
      frame_to_evict = -1;
    } else {
      /* Update frame_to_evict if needed */
      if (i == frame_to_evict)
        frame_to_evict = *next(frame_to_evict);

      /* Remove frame_table[i] from the circular list */
      *next(*prev(i)) = *next(i);
      *prev(*next(i)) = *prev(i);

      if (i != table_size) {
        /* Move frame_table[table_size] to frame_table[i] */
        frame_table[i] = frame_table[table_size];

        /* Repair the circular list */
        *next(*prev(i)) = i;
        *prev(*next(i)) = i;

        /* Update frame_to_evict again if needed */
        if (frame_to_evict == table_size)
          frame_to_evict = i;
      }

      /* Fill 0xcc to frame_table[table_size] to make debugging easier */
      memset(frame_table + table_size, 0xcc, sizeof(struct frame_entry));
    }

    lock_release(&table_lock);
  }
  else if (palloc_from_kernel(page))
  {
    palloc_free_page(page);
  }
  else
  {
    NOT_REACHED();
  }
}
