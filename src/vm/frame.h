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
    void *page;

    /* Info for circular list */
    /* I don't use list.h because it would be hard to realloc entries */
    size_t prev;
    size_t next;

    uint16_t age;
    uint16_t flags;
};

void frame_init(void);

void *frame_get_page (enum palloc_flags);

void frame_free_page (void *);

#endif /* vm/frame.h */
