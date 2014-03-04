#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdint.h>
#include "threads/pte.h"

struct mm_struct;
struct vm_area_struct;

/* Reference:
 * http://www.opensource.apple.com/source/xnu/xnu-1456.1.26/
 * osfmk/mach/vm_prot.h */
/* VM_PROT_READ <=> VM_PROT_EXECUTE */
enum vm_flag_t
{
    /* Permissions */
    VM_READ =           0x0001,
    VM_WRITE =          0x0002,
    VM_EXEC =           0x0004,

    VM_SHARED =         0x0010,

    /* The regions maps an executable file. */
    VM_EXECUTABLE =     0x0100
};

#define VM_PROT_DEFAULT (VM_PROT_READ | VM_PROT_WRITE)
#define VM_PROT_ALL (VM_PROT_READ | VM_PROT_WRITE)

struct mm_struct
{
    uint32_t *pagedir;
    struct vm_area_struct *mmap;
};

struct vm_area_struct
{
    uint32_t *vm_end;
    uint32_t *vm_start;
    uint32_t vm_page_prot;
    uint32_t vm_flags;
    struct vm_area_struct *next;
};


#endif /* vm/page.h */
