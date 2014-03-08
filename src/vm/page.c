#include "vm/page.h"

#include <debug.h>
#include "threads/malloc.h"

static inline uintptr_t upage_num(const struct hash_elem *e) {
  return (uintptr_t) 
  hash_entry(e, struct vm_page_struct, elem)->upage & PTE_ADDR;
}

static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED) {
  return hash_int(upage_num(e));
}

static bool page_less (const struct hash_elem *a, 
                       const struct hash_elem *b, 
                       void *aux UNUSED) {
  return upage_num(a) < upage_num(b);
}

void mm_init(struct mm_struct *mm)
{
  lock_init(&mm->mmap_lock_w);
  lock_init(&mm->mmap_lock_pf);
}

struct vm_area_struct *mm_find(struct mm_struct * mm, uint8_t *addr)
{
  struct vm_area_struct *vma;

  if (mm->mmap == NULL)
    return NULL;

  if (mm->vma_cache != NULL && mm->vma_cache->vm_start <= addr)
    vma = mm->vma_cache;
  else
    vma = mm->mmap;

  for (; vma != NULL; vma = vma->next)
    if (vma->vm_start <= addr && addr < vma->vm_end)
      return (mm->vma_cache = vma);

  return NULL;
}

bool mm_insert_vm_area(struct mm_struct * mm, struct vm_area_struct * vm)
{
  /* Set parent */
  vm->vm_mm = mm;
  vm->pagedir = mm->pagedir;
  vm->next = NULL;
  // list_init(&vm->vm_locked_list);
  hash_init(&vm->vm_page_table, page_hash, page_less, NULL);

  if (mm->mmap == NULL)
  {
    /* Just use it as head of list */
    mm->mmap = vm;
  }
  else
  {
    struct vm_area_struct *vma_insert;

    if (mm->vma_cache != NULL && mm->vma_cache->vm_start <= vm->vm_start)
      vma_insert = mm->vma_cache;
    else
      vma_insert = mm->mmap;

    /* Find insertion point satisfying 
       vma_insert->vm_start <= vm->vm_start <= vma_insert->next->vm_start  */
    while (vma_insert->next != NULL && vma_insert->next->vm_start < vm->vm_start)
    {
      vma_insert = vma_insert->next;
    }

    mm->vma_cache = vma_insert;

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

static bool evict_fifo_vmp(struct frame_entry *f, void *aux) {
  struct vm_page_struct **vmp_ptr = aux;
  struct hash_elem *e;
  size_t swap = 0;

  if (f->flags & PG_LOCKED)
    return false;

  uint32_t *pd = f->pagedir;
  uint8_t *upage = f->upage;

  if (f->flags & (PG_CODE | PG_MMAP)) {
    (*vmp_ptr)->swap = 0;
  } 
  else {
    swap = swap_get();
    swap_lock_acquire(swap);
    (*vmp_ptr)->swap = swap;
    // printf("acq s = %x\n", swap);
  }

  (*vmp_ptr)->upage = upage;
  (*vmp_ptr)->pte = 0;

  e = hash_replace(&f->vma->vm_page_table, &(*vmp_ptr)->elem);

  ASSERT(e != NULL);

  pagedir_clear_page(pd, upage);

  (*vmp_ptr) = hash_entry(e, struct vm_page_struct, elem);
  (*vmp_ptr)->swap = swap;

  return true;
}

void *vm_kpage(struct vm_page_struct **vmp_ptr)
{
  void *kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  struct frame_entry f;

  if (kpage == NULL)
  {
    if (!frame_pull(&f, evict_fifo_vmp, vmp_ptr))
      PANIC("vm_kpage: cannot pull a frame");

    kpage = (*vmp_ptr)->pte & PTE_ADDR;

    ASSERT((uintptr_t) kpage != 0);

    size_t swap = (*vmp_ptr)->swap;

    if (swap != 0) {
      swap_write(swap, kpage);
      swap_lock_release(swap);
      // printf("rel s = %x\n", swap);
    } else if (f.flags & PG_MMAP) {
      // TODO
    }
  }

  return kpage;
}