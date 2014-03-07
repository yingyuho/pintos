#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include <list.h>

#include "threads/palloc.h"
#include "vm/page.h"

enum frame_flags
{
    FRAME_PIN =     0x01
};

struct frame_entry
{
    uint32_t *pagedir;
    void *upage;

    struct mm_struct *mm;

    /* Info for circular list */
    /* I don't use list.h because it would be hard to realloc entries */
    size_t prev;
    size_t next;

    // uint16_t age;
    uint32_t flags;
};

void frame_init(size_t user_page_limit);

void *frame_get_page (struct mm_struct *mm, void *upage, enum palloc_flags);

void frame_free_page (uint32_t *pd, void *upage);

void frame_free_pagedir (uint32_t *pd);

struct frame_entry *frame_clock_hand (void);

struct frame_entry *frame_clock_step (void);

void frame_entry_replace (struct frame_entry *, 
                          struct mm_struct *, 
                          void *upage);

void frame_entry_pin (struct frame_entry*);
void frame_entry_unpin (struct frame_entry*);

void frame_dump (void);

// void frame_lock (void);

// void frame_unlock (void);

#endif /* vm/frame.h */
