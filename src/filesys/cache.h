#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdint.h>

#include "devices/block.h"

#define CACHE_SECTORS 64

void cache_init (void);


#endif /* filesys/cache.h */
