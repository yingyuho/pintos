#include "vm/swap.h"

#include <debug.h>
#include <bitmap.h>
#include <stdio.h>
#include <round.h>
#include "threads/pte.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/interrupt.h"

static struct block *swap_block;        /* Point to swap disk */
static size_t swap_size;                /* Number of slots */

static struct bitmap *data_map;      /* Record which slots have data */
static struct lock data_map_lock;    /* Lock for accessing the above */

static struct bitmap *busy_map;      /* Record which slots are being accessed */
static struct list busy_waiters;

/* PGSIZE = 4096, BLOCK_SECTOR_SIZE = 512 */
#define TO_SECTOR(slot) (slot)   * (PGSIZE / BLOCK_SECTOR_SIZE)
#define TO_SLOT(sector) (sector) / (PGSIZE / BLOCK_SECTOR_SIZE)

void swap_init(void)
{
    swap_block = block_get_role(BLOCK_SWAP);
    ASSERT(swap_block != NULL);

    block_sector_t swap_sectors = block_size(swap_block);
    ASSERT(swap_sectors > 0);

    /* I want to store swap index in PTE so some bits cannot be used.
     * Max swap size = 256GB */
    swap_sectors = (swap_sectors > (1 << 29)) ? (1 << 29) : swap_sectors;

    swap_size = TO_SLOT(swap_sectors);

    lock_init(&data_map_lock);
    data_map = bitmap_create(swap_size);

    list_init(&busy_waiters);
    busy_map = bitmap_create(swap_size);

    /* 0-th is reserved to keep unused */
    bitmap_mark(data_map, 0);
}

void swap_lock_acquire (size_t idx) {
    enum intr_level old_level;
    struct thread *cur;

    ASSERT(!intr_context());
    ASSERT((0 < idx) && (idx < swap_size));

    cur = thread_current();

    cur->swap_waiting = idx;

    old_level = intr_disable();
    while (bitmap_test(busy_map, idx)) {
        list_push_back(&busy_waiters, &cur->elem);
        thread_block();
    }
    bitmap_mark(busy_map, idx);
    intr_set_level(old_level);
}

void swap_lock_release (size_t idx) {
    enum intr_level old_level;
    struct list_elem *e;
    struct thread *waiter;

    ASSERT((0 < idx) && (idx < swap_size));

    old_level = intr_disable();
    if (!list_empty(&busy_waiters)) {
        for (e = list_front(&busy_waiters); 
             e != list_tail(&busy_waiters); 
             e = list_next(e))
        {
            waiter = list_entry(e, struct thread, elem);
            if (idx == waiter->swap_waiting) {
                list_remove(e);
                thread_unblock(waiter);
                break;
            }
        }
    }
    bitmap_reset(busy_map, idx);
    intr_set_level(old_level);
    maybe_yield();
}

size_t swap_get() {
    size_t idx;
    size_t start = (97 * (uintptr_t) thread_current()) % swap_size;

    lock_acquire(&data_map_lock);
    idx = bitmap_scan_and_flip(data_map, start, 1, false);
    if (idx == BITMAP_ERROR)
        idx = bitmap_scan_and_flip(data_map, 0, 1, false);
    lock_release(&data_map_lock);

    if (idx == BITMAP_ERROR)
        PANIC("swap_get: out of swap slots");
    else
        return idx;
}

void swap_free(size_t idx) {
    lock_acquire(&data_map_lock);
    ASSERT(bitmap_all(data_map, idx, 1));
    bitmap_reset(data_map, idx);
    lock_release(&data_map_lock);
}

void swap_read(size_t idx, void *kpage) {
    ASSERT((0 < idx) && (idx < swap_size));

    uint8_t *ptr = kpage;
    block_sector_t sector;

    for (sector = TO_SECTOR(idx); sector < TO_SECTOR(idx + 1); ++sector)
    {
        block_read(swap_block, sector, ptr);
        ptr += BLOCK_SECTOR_SIZE;
    }
}

void swap_write(size_t idx, const void *kpage) {
    ASSERT((0 < idx) && (idx < swap_size));

    const uint8_t *ptr = kpage;
    block_sector_t sector;

    for (sector = TO_SECTOR(idx); sector < TO_SECTOR(idx + 1); ++sector)
    {
        block_write(swap_block, sector, ptr);
        ptr += BLOCK_SECTOR_SIZE;
    }
}