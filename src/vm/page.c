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


static bool evict_fifo(struct frame_entry *f, void *aux) {
  void **kpage_ptr = aux;
  if (f->flags & PG_LOCKED) {
    // printf("frame locked: pd = %x, u = %x\n", f->pagedir, f->upage);
    return false;
  } else {
    *kpage_ptr = pagedir_get_page(f->pagedir, f->upage);
    return true;
  }
}

void *vm_kpage(struct vm_page_struct **vmp_in_ptr)
{
  void *kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  struct frame_entry f;

  if (kpage == NULL)
  {
    if (!frame_pull(&f, evict_fifo, &kpage))
      PANIC("vm_kpage: cannot pull a frame");

    // if (mm != f.vma->vm_mm)
      // printf("vm_kpage: mmi = %x, mmo = %x\n", 
      //   (uintptr_t) mm, (uintptr_t) f.vma->vm_mm);
    // if (mm != f.vma->vm_mm)
      // printf("vm_kpage: pdo = %x\n", (uintptr_t) f.pagedir);

    uint32_t *pd_out = f.pagedir;
    uint8_t *upage_out = f.upage;

    // if (mm != f.vma->vm_mm)
      // printf("vm_kpage: uo = %x\n", 
      //   (uintptr_t) upage_out);

    if (kpage == NULL) {
      frame_dump();
      ASSERT(kpage != NULL);
    }

    // if (mm != f.vma->vm_mm)
    //   printf("fl = %x\n", f.flags);    

    if (f.flags & PG_CODE) {
      (*vmp_in_ptr)->swap = 0;
    } else if (f.flags & PG_MMAP) {

    } 
    else {
      size_t swap_out = swap_get();
      swap_lock_acquire(swap_out);

      (*vmp_in_ptr)->swap = swap_out;

      // pagedir_set_aux(pd_out, upage_out, swap_out);
      swap_write(swap_out, kpage);
      swap_lock_release(swap_out);
    }

    (*vmp_in_ptr)->upage = upage_out;
    (*vmp_in_ptr)->pte = 0;
    struct hash_elem *e = hash_replace(&f.vma->vm_page_table, 
                                       &(*vmp_in_ptr)->elem);

    ASSERT(e != NULL);

    (*vmp_in_ptr) = hash_entry(e, struct vm_page_struct, elem);

    pagedir_clear_page(pd_out, upage_out);
  }

  return kpage;
}