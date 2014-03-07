#include "vm/frame.h"
#include <debug.h>
#include <list.h>
#include <string.h>
#include <round.h>
#include <stdio.h>
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

static inline size_t *prev(size_t i) {
  return &frame_table[i].prev;
}

static inline size_t *next(size_t i) {
  return &frame_table[i].next;
}

static void free_index(size_t i);

void frame_make(struct frame_entry *f, 
                struct mm_struct *mm, 
                void *upage)
{
  f->mm = mm;
  f->pagedir = mm->pagedir;
  f->upage = upage;
  f->flags = 0;
}

void frame_push(struct frame_entry *f)
{
    lock_acquire(&table_lock);

    frame_table[table_size] = *f;
    f = frame_table + table_size;

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

bool frame_pull(struct frame_entry *f, frame_func *func, void *aux)
{
  size_t i;
  size_t count;

  lock_acquire(&table_lock);

  count = table_size;

  if (count == 0) {
    lock_release(&table_lock);
    return false;
  }

  do {
    i = clock_hand;
    clock_hand = *next(clock_hand);
  } while (!func(&frame_table[i], aux) && --count > 0);

  bool success = (count > 0) || func(&frame_table[i], aux);

  if (success) {
    *f = frame_table[i];
    free_index(i);
  }

  lock_release(&table_lock);

  return success;
}

void frame_remove_if(frame_func *func, void *aux)
{
  size_t i;

  lock_acquire(&table_lock);

  for (i = 0; i < table_size; i++)
    if (func(&frame_table[i], aux))
      free_index(i);

  lock_release(&table_lock);
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
    memset(frame_table + table_size, 0xaa, sizeof(struct frame_entry));
  }
}

void frame_dump(void)
{
  size_t i;
  lock_acquire(&table_lock);
  printf("clock_hand = %x\n", clock_hand);
  for (i = 0; i < table_size; i++)
  {
    printf("%x: %x <- pd = %x, up = %x -> %x\n",
      i, 
      *prev(i), 
      (uintptr_t) frame_table[i].pagedir, 
      (uintptr_t) frame_table[i].upage, 
      *next(i));
  }
  lock_release(&table_lock);
}
