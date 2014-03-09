#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdint.h>
#include <debug.h>
#include <hash.h>
#include "threads/pte.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "vm/swap.h"
#include "vm/frame.h"

struct mm_struct;
struct vm_area_struct;
struct vm_operations_struct;

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

    /* The region maps an executable file. */
    VM_EXECUTABLE =     0x0100,
    /* Memory mapped file */
    VM_MMAP =           0x0200
};

#define VM_PROT_DEFAULT (VM_PROT_READ | VM_PROT_WRITE)
#define VM_PROT_ALL (VM_PROT_READ | VM_PROT_WRITE)

struct mm_struct
{
    uint32_t *pagedir;
    struct vm_area_struct *mmap;
    struct vm_area_struct *vma_cache;
    struct vm_area_struct *vma_stack;
    struct lock mmap_lock_w;
};

struct vm_page_struct
{
    void *upage;
    uint32_t pte;
    uint32_t swap;
    struct hash_elem elem;
};

struct vm_area_struct
{
    struct mm_struct *vm_mm;

    uint32_t *pagedir;
        
    uint8_t *vm_start;
    uint8_t *vm_end;
    struct vm_area_struct *next;

    uint32_t vm_page_prot;
    uint32_t vm_flags;
    
    int mmap_id;
    bool dirty;

    struct hash vm_page_table;

    struct vm_operations_struct *vm_ops;

    struct file *vm_file;
    off_t vm_file_ofs;
    uint32_t vm_file_read_bytes;
    uint32_t vm_file_zero_bytes;
};

struct vm_interval {
    uint32_t *pagedir;
    uint8_t *vm_start;
    uint8_t *vm_end;
};

/* Reference:
 * http://lxr.free-electrons.com/source/include/linux/mm.h?a=avr32#L210
 */
struct vm_fault {
    off_t page_ofs;
    void *fault_addr;
    bool user;
};

struct vm_operations_struct {
    /* Add AREA to list of memory regions */
    void (*open)(struct vm_area_struct *area);
    /* Remove AREA from list of memory regions
     * May split a region into two */
    void (*close)(struct vm_area_struct *area);
    /* Invoked by page fault handler when a process tries to access
     * a valid address but the page is not present */
    int32_t (*absent)(struct vm_area_struct *vma, struct vm_fault *vmf);
};

void mm_init (struct mm_struct *);

/* returns false if insertion failed */
bool mm_insert_vm_area (struct mm_struct *, struct vm_area_struct *);

struct vm_area_struct *mm_find (struct mm_struct *, uint8_t *);

struct hash_elem *vm_insert_page(struct vm_area_struct *, 
                                 void *upage,
                                 void *kpage);

void *vm_kpage(struct vm_page_struct **vmp_in_ptr);

#endif /* vm/page.h */
