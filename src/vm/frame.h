#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include <list.h>

#include "threads/palloc.h"

enum frame_flags
{
    FRAME_USER = 001              /* User page. */
};

struct frame_entry
{
	uint32_t *pagedir;
    void *upage;

    /* Info for circular list */
    /* I don't use list.h because it would be hard to realloc entries */
    size_t prev;
    size_t next;

    // uint16_t age;
    // uint16_t flags;
};

void frame_init(size_t user_page_limit);

void *frame_get_page (uint32_t *pd, void *upage, enum palloc_flags);

void frame_free_page (uint32_t *pd, void *upage);

void frame_free_pagedir (uint32_t *pd);

struct frame_entry *frame_clock_hand (void);

void frame_clock_advance (void);

void frame_entry_replace (struct frame_entry *, uint32_t *pd, void *upage);

#endif /* vm/frame.h */
