#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdint.h>
#include <stdbool.h>

#include "devices/block.h"
#include "filesys/off_t.h"

#define CACHE_SECTORS 64
#define CACHE_ENABLE

void cache_init (void);

void cache_read (block_sector_t idx, off_t ofs, uint8_t *dest, size_t size);
void cache_write (block_sector_t idx, off_t ofs, 
                  const uint8_t *src, size_t size, bool toread);

void cache_prefetch (block_sector_t idx);

void cache_close (void);

#endif /* filesys/cache.h */
