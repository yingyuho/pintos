#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include "devices/block.h"

void swap_init (void);

size_t swap_get (void);

void swap_free (size_t);

void swap_read (size_t, void *);

void swap_write (size_t, const void *);

void swap_lock_acquire (size_t);

void swap_lock_release (size_t);

#endif /* vm/swap.h */