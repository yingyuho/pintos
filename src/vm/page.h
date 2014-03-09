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

/* Flags for memory area descriptor
 *
 * Reference:
 * http://www.opensource.apple.com/source/xnu/xnu-1456.1.26/osfmk/mach/vm_prot.h
 */
enum vm_flag_t
{
    VM_READ =           0x0001,     /* Permissions */
    VM_WRITE =          0x0002,
    VM_EXEC =           0x0004,

    VM_SHARED =         0x0010,     /* Shared by processes */
    
    VM_EXECUTABLE =     0x0100,     /* Maps an executable file. */
    VM_MMAP =           0x0200      /* Memory mapped file */
};

#define VM_PROT_DEFAULT (VM_PROT_READ | VM_PROT_WRITE)
#define VM_PROT_ALL (VM_PROT_READ | VM_PROT_WRITE)

/* Memory descriptor */
struct mm_struct
{
    uint32_t *pagedir;
    struct vm_area_struct *mmap;        /* The first memory segment */
    struct vm_area_struct *vma_stack;   /* The stack segment */
    struct lock mmap_lock_w;            /* Lock for modifying list structure */
};

/* Shadow page table entry owned by memory area descriptor */
struct vm_page_struct
{
    void *upage;            /* User page number (hash key) */
    uint32_t pte;           /* Page table entry */
    uint32_t swap;          /* Swap slot */
    struct hash_elem elem;
};

/* Memory area descriptor */
struct vm_area_struct
{
    struct mm_struct *vm_mm;        /* Parent memory descriptor */

    uint32_t *pagedir;              /* Process page directory */
        
    uint8_t *vm_start;              /* Start and end addresses */
    uint8_t *vm_end;
    struct vm_area_struct *next;    /* Next segment */

    uint32_t vm_flags;

    int mmap_id;                    /* ID of memory mapped file */
    bool dirty;

    struct hash vm_page_table;      /* Shadow page table */

    struct vm_operations_struct *vm_ops;    /* Some function pointers */

    struct file *vm_file;           /* Mapped file */
    off_t vm_file_ofs;              /* File offset at vm_start */
    uint32_t vm_file_read_bytes;
    uint32_t vm_file_zero_bytes;
};

/* Lightweight struct for a virtual memory segment */
struct vm_interval {
    uint32_t *pagedir;
    uint8_t *vm_start;
    uint8_t *vm_end;
};

/* Data passed around PF handlers 
 *
 * Reference:
 * http://lxr.free-electrons.com/source/include/linux/mm.h?a=avr32#L210
 */
struct vm_fault {
    off_t page_ofs;         /* Page offset in the memory segment */
    void *fault_addr;       /* Faulting address */
    bool user;              /* From user or kernel */
};

struct vm_operations_struct {
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
