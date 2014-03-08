#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include <list.h>

#include "vm/page.h"

enum frame_flags
{
    PG_LOCKED =     0x01,
    PG_CODE =       0x10,
    PG_MMAP =       0x20
};

struct frame_entry
{
    uint32_t *pagedir;
    void *upage;

    struct vm_area_struct *vma;
    struct list_elem vma_locked_elem;

    /* Info for circular list */
    /* I don't use list.h because it would be hard to realloc entries */
    size_t prev;
    size_t next;

    // uint16_t age;
    uint32_t flags;
};

void frame_init(size_t user_page_limit);

void frame_make (struct frame_entry *, 
                 struct vm_area_struct *vma, 
                 void *upage);

void frame_push (struct frame_entry *);

/*! Selector of frame to evict. */
typedef bool frame_func (struct frame_entry *, void *aux);

bool frame_pull (struct frame_entry *, frame_func *, void *aux);

void frame_for_each (frame_func *, void *aux);

void frame_remove_if (frame_func *, void *aux);

void frame_entry_pin (struct frame_entry *);
void frame_entry_unpin (struct frame_entry *);

void frame_dump (void);

#endif /* vm/frame.h */
