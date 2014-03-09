#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include <list.h>

#include "vm/page.h"

/* Flags for rame table entry */ 
enum frame_flags
{
    PG_LOCKED =     0x01,   /* Should not be evicted */
    PG_DIRTY =      0x02,   /* Data is modified */
    
    PG_CODE =       0x10,   /* Code segment */
    PG_DATA =       0x20,   /* Data segment */
    PG_MMAP =       0x40    /* Mapped file segment */
};

/* Frame table entry */ 
struct frame_entry
{
    uint32_t *pagedir;          /* Process page directory */
    void *upage;                /* User page address */

    struct vm_area_struct *vma; /* Parent memory area descriptor */

    size_t prev;                /* Circular queue structure */
    size_t next;

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
