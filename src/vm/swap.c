#include "vm/swap.h"

#include <debug.h>
#include <bitmap.h>
#include <stdio.h>
#include <round.h>
#include "threads/pte.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"

static struct block *swap_block;
static struct lock swap_bitmap_lock;
static struct bitmap *swap_bitmap;
static size_t swap_size;

static struct lock *swap_slot_locks;

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
    // printf("sectors = %x\n", swap_sectors);

    lock_init(&swap_bitmap_lock);
    swap_size = TO_SLOT(swap_sectors);
    swap_bitmap = bitmap_create(swap_size);

    /* 0-th is reserved to keep unused */
    bitmap_mark(swap_bitmap, 0);

    swap_slot_locks = palloc_get_multiple(PAL_ASSERT, 
        DIV_ROUND_UP(swap_sectors * sizeof(struct lock), PGSIZE));

    size_t i;
    for (i = 0; i < swap_sectors; ++i)
        lock_init(swap_slot_locks + i);
}

void swap_lock_acquire (size_t idx) {
    lock_acquire(swap_slot_locks + idx);
}

bool swap_lock_try_acquire (size_t idx) {
    return lock_try_acquire(swap_slot_locks + idx);
}

void swap_lock_release (size_t idx) {
    lock_release(swap_slot_locks + idx);
}

size_t swap_get() {
    size_t idx;

    lock_acquire(&swap_bitmap_lock);
    idx = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
    lock_release(&swap_bitmap_lock);

    if (idx == BITMAP_ERROR)
        PANIC("swap_get: out of swap slots");
    else
        return idx;
}

void swap_free(size_t idx) {
    lock_acquire(&swap_bitmap_lock);
    ASSERT(bitmap_all(swap_bitmap, idx, 1));
    bitmap_reset(swap_bitmap, idx);
    lock_release(&swap_bitmap_lock);
}

void swap_read(size_t idx, void *kpage) {
    ASSERT(idx < swap_size);
    ASSERT(idx > 0);

    uint8_t *ptr = kpage;
    block_sector_t sector;

    for (sector = TO_SECTOR(idx); sector < TO_SECTOR(idx + 1); ++sector)
    {
        block_read(swap_block, sector, ptr);
        ptr += BLOCK_SECTOR_SIZE;
    }
}

void swap_write(size_t idx, const void *kpage) {
    ASSERT(idx < swap_size);
    ASSERT(idx > 0);

    const uint8_t *ptr = kpage;
    block_sector_t sector;

    for (sector = TO_SECTOR(idx); sector < TO_SECTOR(idx + 1); ++sector)
    {
        block_write(swap_block, sector, ptr);
        ptr += BLOCK_SECTOR_SIZE;
    }
}