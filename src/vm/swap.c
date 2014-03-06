#include "vm/swap.h"

#include <debug.h>
#include <bitmap.h>
#include "threads/pte.h"
#include "threads/synch.h"

static struct block *swap_block;
static struct lock swap_lock;
static struct bitmap *swap_bitmap;
static size_t swap_size;

/* PGSIZE = 4096, BLOCK_SECTOR_SIZE = 512 */
#define TO_SECTOR(slot) slot   * (PGSIZE / BLOCK_SECTOR_SIZE)
#define TO_SLOT(sector) sector / (PGSIZE / BLOCK_SECTOR_SIZE)

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

    lock_init(&swap_lock);
    swap_size = TO_SLOT(swap_sectors);
    swap_bitmap = bitmap_create(swap_size); 
}

size_t swap_get() {
    block_sector_t sector;

    lock_acquire(&swap_lock);
    sector = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
    lock_release(&swap_lock);

    if (sector == BITMAP_ERROR)
        PANIC("swap_get: out of swap slots");
    else
        return TO_SLOT(sector);
}

void swap_free(size_t idx) {
    const block_sector_t sector = TO_SECTOR(idx);
    ASSERT(bitmap_all(swap_bitmap, sector, 1));
    bitmap_reset(swap_bitmap, sector);
}

void swap_read(size_t idx, void *buffer) {
    ASSERT(idx < swap_size);
    block_read(swap_block, TO_SECTOR(idx), buffer);
}

void swap_write(size_t idx, const void *buffer) {
    ASSERT(idx < swap_size);
    block_write(swap_block, TO_SECTOR(idx), buffer);
}