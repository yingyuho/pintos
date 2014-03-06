#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"

void swap_init (void);

size_t swap_get (void);

void swap_free (size_t);

void swap_read(size_t, void *);

void swap_write(size_t, const void *);

#endif /* vm/swap.h */