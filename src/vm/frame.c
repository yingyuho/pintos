#include "vm/frame.h"
#include <debug.h>
#include <list.h>
#include <string.h>
#include <round.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

/* An array holding all struct frame_entry */
static struct frame_entry *frame_table;

/* The next candidate frame to be evicted */
static int32_t clock_hand;
#define CLOCK_HAND_NONE -1

/* realloc frame_table when table_size > table_capacity */
static size_t table_capacity;
static size_t table_size;

/* A lock protecting frame_table */
static struct lock table_lock;

void frame_entry_replace (struct frame_entry * f, uint32_t *pd, void *upage)
{
  f->pagedir = pd;
  f->upage = upage;
}



void frame_init(size_t user_page_limit) {
  /* Free memory starts at 1 MB and runs to the end of RAM. */
  uint8_t *free_start = ptov(1024 * 1024);
  uint8_t *free_end = ptov(init_ram_pages * PGSIZE);
  size_t free_pages = (free_end - free_start) / PGSIZE;
  size_t user_pages = free_pages / 2;
  if (user_pages > user_page_limit)
    user_pages = user_page_limit;

  frame_table = palloc_get_multiple(PAL_ASSERT, 
    DIV_ROUND_UP(user_pages * sizeof(struct frame_entry), PGSIZE));

  table_size = 0;
  table_capacity = 128;
  lock_init(&table_lock);

  // can be anything since frame_list is empty initially
  clock_hand = CLOCK_HAND_NONE;
}

static inline size_t *prev(size_t i) {
  return &frame_table[i].prev;
}

static inline size_t *next(size_t i) {
  return &frame_table[i].prev;
}

struct frame_entry *frame_clock_hand(void) {
  return (clock_hand == CLOCK_HAND_NONE) ? NULL : &frame_table[clock_hand];
}

void frame_clock_advance(void) {
  lock_acquire(&table_lock);
  clock_hand = *next(clock_hand);
  lock_release(&table_lock);
}

void *frame_get_page(uint32_t *pd, void *upage, enum palloc_flags flags) {
  void *kpage = palloc_get_page(flags);
  struct frame_entry *f;

  /* Skip for kernel page or out of pages */
  if ((kpage != NULL) && (flags & PAL_USER)) {
    lock_acquire(&table_lock);

    f = frame_table + table_size;

    f->pagedir = pd;
    f->upage = upage;

    if (table_size > 0) {
      /* Insert frame just before clock_hand */
      f->prev = *prev(clock_hand);
      f->next = clock_hand;
      *next(f->prev) = table_size;
      *prev(f->next) = table_size;
    } else {
      f->prev = f->next = clock_hand = 0;
    }

    ++table_size;

    lock_release(&table_lock);
  }

  return kpage;
}

static void free_index(size_t i)
{
  --table_size;

  if (table_size == 0)
  {
    clock_hand = CLOCK_HAND_NONE;
  }
  else
  {
    /* Update clock_hand if needed */
    if ((int) i == clock_hand)
      clock_hand = *next(clock_hand);

    /* Remove frame_table[i] from the circular list */
    *next(*prev(i)) = *next(i);
    *prev(*next(i)) = *prev(i);

    if (i != table_size) {
      /* Move frame_table[table_size] to frame_table[i] */
      frame_table[i] = frame_table[table_size];

      /* Repair the circular list */
      *next(*prev(i)) = i;
      *prev(*next(i)) = i;

      /* Update clock_hand again if needed */
      if (clock_hand == (int) table_size)
        clock_hand = (int) i;
    }

    /* Fill 0xcc to frame_table[table_size] to make debugging easier */
    memset(frame_table + table_size, 0xcc, sizeof(struct frame_entry));
  }
}

void frame_free_pagedir(uint32_t *pd)
{
  void *kpage;
  size_t i;
  lock_acquire(&table_lock);
  /* Locate all pages under PD in the frame table */
  for (i = 0; i < table_size; i++)
  if (frame_table[i].pagedir == pd)
  {
    kpage = pagedir_get_page(pd, frame_table[i].upage);

    if (kpage != NULL)
      palloc_free_page(kpage);

    free_index(i);
  }
  lock_release(&table_lock);
}

void frame_free_page(uint32_t *pd, void *upage)
{
  void *kpage = pagedir_get_page(pd, upage);

  if (kpage == NULL)
    PANIC(
      "frame_free_page: cannot find (pd = %x, upage = %x) in frame table", 
      (uintptr_t) pd, (uintptr_t) upage);

  if (palloc_from_user(kpage))
  {
    size_t i;

    palloc_free_page(kpage);

    lock_acquire(&table_lock);

    /* Locate the page in the frame table */
    /* TODO: Use hash table? */
    for (i = 0; i < table_size; i++) {
      if (frame_table[i].pagedir == pd && frame_table[i].upage == upage)
        break;
    }

    if (i >= table_size)
      PANIC(
        "frame_free_page: cannot find (pd = %x, upage = %x) in frame table", 
        (uintptr_t) pd, (uintptr_t) upage);

    free_index(i);

    lock_release(&table_lock);
  }
  else if (palloc_from_kernel(kpage))
  {
    palloc_free_page(kpage);
  }
  else
  {
    NOT_REACHED();
  }
}
