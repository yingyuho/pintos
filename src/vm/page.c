#include "vm/page.h"

#include <debug.h>
#include "threads/malloc.h"

void mm_init(struct mm_struct *mm)
{
  lock_init(&mm->mmap_lock_w);
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

void mm_insert_vm_area(struct mm_struct * mm, struct vm_area_struct * vm)
{
  /* Set parent */
  vm->vm_mm = mm;
  vm->next = NULL;

  if (mm->mmap == NULL)
  {
    /* Just use it as head of list */
    mm->mmap = vm;
  }
  else
  {
    struct vm_area_struct *vma_insert;

    if (mm->vma_cache != NULL && mm->vma_cache->vm_end <= vm->vm_start)
      vma_insert = mm->vma_cache;
    else
      vma_insert = mm->mmap;

    /* Find insertion point */
    while (vma_insert->next != NULL && 
           vma_insert->next->vm_end <= vm->vm_start)
    {
      vma_insert = vma_insert->next;
    }

    mm->vma_cache = vma_insert;

    lock_acquire(&mm->mmap_lock_w);

    if (0 && vma_insert->vm_end        == vm->vm_start &&
        vma_insert->vm_page_prot  == vm->vm_page_prot &&
        vma_insert->vm_file       == vm->vm_file)
    {
      vma_insert->vm_end = vm->vm_end;
      free(vm);
    }
    else
    {
      if (vma_insert->next != NULL)
      {
        /* No overlapping regions */
        ASSERT(vma_insert->next->vm_start >= vm->vm_end);

        if (0 && vma_insert->next->vm_start      == vm->vm_end &&
            vma_insert->next->vm_page_prot  == vm->vm_page_prot &&
            vma_insert->next->vm_file       == vm->vm_file)
        {
          vma_insert->next->vm_start = vm->vm_start;
          free(vm);
        }
        else
        {
          vm->next = vma_insert->next;
        }
      }

      vma_insert->next = vm;
    }

    lock_release(&mm->mmap_lock_w);
  }
}
