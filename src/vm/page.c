#include "vm/page.h"

#include <debug.h>
#include <stdio.h>
#include "threads/malloc.h"

void mm_init(struct mm_struct *mm)
{
  lock_init(&mm->mmap_lock_w);
}

/* Find memory segment given a virtual address */
struct vm_area_struct *mm_find(struct mm_struct * mm, uint8_t *addr)
{
  struct vm_area_struct *vma;

  if (mm->mmap == NULL)
    return NULL;

  vma = mm->mmap;

  for (; vma != NULL; vma = vma->next)
    if (vma->vm_start <= addr && addr < vma->vm_end)
      return vma;

  return NULL;
}

/* Helper functions for shadow page table */

/* User page from hash_elem */
static inline uintptr_t upage_num(const struct hash_elem *e) {
  return (uintptr_t) 
  hash_entry(e, struct vm_page_struct, elem)->upage & PTE_ADDR;
}

/* Hashing function */
static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED) {
  return hash_int(upage_num(e));
}

static bool page_less (const struct hash_elem *a, 
                       const struct hash_elem *b, 
                       void *aux UNUSED) {
  return upage_num(a) < upage_num(b);
}

/* Insert a segment into memory descriptor */
bool mm_insert_vm_area(struct mm_struct * mm, struct vm_area_struct * vm)
{
  /* Set parent */
  vm->vm_mm = mm;
  vm->pagedir = mm->pagedir;
  vm->next = NULL;

  hash_init(&vm->vm_page_table, page_hash, page_less, NULL);

  if (mm->mmap == NULL)
  {
    /* Just use it as head of list */
    mm->mmap = vm;
  }
  else
  {
    struct vm_area_struct *vma_insert = mm->mmap;

    /* Find insertion point satisfying 
       vma_insert->vm_start <= vm->vm_start <= vma_insert->next->vm_start  */
    while (vma_insert->next != NULL && vma_insert->next->vm_start < vm->vm_start)
    {
      vma_insert = vma_insert->next;
    }

    lock_acquire(&mm->mmap_lock_w);

    /* No overlapping regions */
    if (vma_insert->vm_end > vm->vm_start)
      return false;

    if (vma_insert->next != NULL)
    {
      if (vm->vm_end > vma_insert->next->vm_start)
        return false;
      else
        vm->next = vma_insert->next;
    }

    vma_insert->next = vm;

    lock_release(&mm->mmap_lock_w);
  }

  return true;
}

/* Returns true if F can be evicted */
typedef bool policy_func(struct frame_entry *f, void *aux);

struct policy_vmp {
  policy_func *policy;
  struct vm_page_struct **vmp_ptr;
};

/* FIFO polocy */
static bool policy_fifo(struct frame_entry *f, void *aux UNUSED) {
  return !(f->flags & PG_LOCKED);
}

/* Second-chance policy */
static bool policy_second_chance(struct frame_entry *f, void *aux) {
  return policy_fifo(f, aux) && !pagedir_is_accessed(f->pagedir, f->upage);
}

/* Heart of clock algorithm
 * Tells frame_pull() which frame can be pulled from the frame table, 
 * and performs synchronized data management
 * Warning: acquires a global swap lock without releasing it */
static bool evict_clock_vmp(struct frame_entry *f, 
                            void *aux) {
  struct policy_vmp *pv = aux;
  policy_func *policy = pv->policy;
  struct vm_page_struct **vmp_ptr = pv->vmp_ptr;

  struct hash_elem *e;
  size_t swap = 0;

  /* Quit if the frame should not be pulled */
  if (!policy(f, NULL))
    return false;

  uint32_t *pd = f->pagedir;
  uint8_t *upage = f->upage;

  /* Determine if swap is used and acquire a slot if needed */
  if (f->flags & (PG_CODE | PG_MMAP)) {
    (*vmp_ptr)->swap = 0;
  }
  else if ((f->flags & PG_DATA) &&
           !(f->flags & PG_DIRTY) &&
           !pagedir_is_dirty(pd, upage)) {
    (*vmp_ptr)->swap = 0;
  }
  else {
    swap = swap_get();
    swap_lock_acquire(swap);
    (*vmp_ptr)->swap = swap;
  }

  (*vmp_ptr)->upage = upage;
  (*vmp_ptr)->pte = 0;

  /* Update the shadow page table */
  e = hash_replace(&f->vma->vm_page_table, &(*vmp_ptr)->elem);

  ASSERT(e != NULL);

  /* Update the page table */
  pagedir_clear_page(pd, upage);

  /* Pass out some data */
  (*vmp_ptr) = hash_entry(e, struct vm_page_struct, elem);
  (*vmp_ptr)->swap = swap;

  return true;
}

extern struct lock fs_lock;

/* Gives a usable frame. Evict a page if needed */
void *vm_kpage(struct vm_page_struct **vmp_ptr)
{
  void *kpage = palloc_get_page(PAL_USER);
  struct frame_entry f;

  if (kpage == NULL)
  {
    /* Run clock algorithm first */
    struct policy_vmp pv = { 
      .policy = policy_second_chance, 
      .vmp_ptr = vmp_ptr 
    };

    if (!frame_pull(&f, evict_clock_vmp, &pv)) {
      pv.policy = policy_fifo;
      /* If failed, use FIFO */
      if (!frame_pull(&f, evict_clock_vmp, &pv))
        PANIC("vm_kpage: cannot pull a frame");
    }

    kpage = (void *) ((*vmp_ptr)->pte & PTE_ADDR);

    /* Should have a usable frame now */
    ASSERT((uintptr_t) kpage != 0);

    size_t swap = (*vmp_ptr)->swap;

    /* Eviction */
    if (swap != 0) {
      /* To swap */
      swap_write(swap, kpage);
      swap_lock_release(swap);
    } else if (f.flags & PG_MMAP) {
      /* To file */
      struct vm_area_struct *vma = f.vma;
      size_t offset = ((uintptr_t) f.upage - (uintptr_t) vma->vm_start) + 
                      vma->vm_file_ofs;

      int32_t write_bytes = vma->vm_file_read_bytes - offset;

      if (write_bytes > 0) {
        write_bytes = (write_bytes > PGSIZE) ? PGSIZE : write_bytes;
        lock_acquire(&fs_lock);
        file_write_at(vma->vm_file, kpage, write_bytes, offset);
        lock_release(&fs_lock);
      }
    }
  }

  return kpage;
}