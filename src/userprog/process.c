#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#ifdef VM
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#endif

//#define PROCESS_C_DEBUG

static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void (**eip)(void), void **esp);

/*! Starts a new thread running a user program loaded from FILENAME.  The new
    thread may be scheduled (and may even exit) before process_execute()
    returns.  Returns the new process's thread id, or TID_ERROR if the thread
    cannot be created. */
tid_t process_execute(const char *file_name) {
    char *fn_copy;
    char name[T_NAME_MAX];
    tid_t tid;

    /* Make a copy of FILE_NAME.
       Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);

    if (fn_copy == NULL)
        return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);

    /* Make sure only argv[0] become thread->name */
    strlcpy(name, file_name, sizeof name);
    if (strchr(name, ' ') != NULL)
        *strchr(name, ' ') = '\0';

    /* Create a new thread to execute FILE_NAME. */
    tid = thread_create(name, PRI_DEFAULT, start_process, fn_copy);
    if (tid == TID_ERROR)
        palloc_free_page(fn_copy); 
    return tid;
}

/*! A thread function that loads a user process and starts it running. */
static void start_process(void *file_name_) {
    char *file_name = file_name_;
    struct intr_frame if_;
    bool success;

    /* Initialize interrupt frame and load executable. */
    memset(&if_, 0, sizeof(if_));
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(file_name, &if_.eip, &if_.esp);

    thread_current()->ashes->load_success = success;

    sema_up(&thread_current()->load_done);

    /* If load failed, quit. */
    palloc_free_page(file_name);
    if (!success) 
        thread_exit();

    /* Start the user process by simulating a return from an
       interrupt, implemented by intr_exit (in
       threads/intr-stubs.S).  Because intr_exit takes all of its
       arguments on the stack in the form of a `struct intr_frame',
       we just point the stack pointer (%esp) to our stack frame
       and jump to it. */
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
    NOT_REACHED();
}

/*! Waits for thread TID to die and returns its exit status.  If it was
    terminated by the kernel (i.e. killed due to an exception), returns -1.
    If TID is invalid or if it was not a child of the calling process, or if
    process_wait() has already been successfully called for the given TID,
    returns -1 immediately, without waiting. */
int process_wait(tid_t child_tid) {
  struct thread *cur = thread_current();
  struct list_elem *e;
  struct thread_ashes *a = NULL;

  if (!list_empty(&cur->children)) {
    for (e = list_front(&cur->children); 
         e != list_tail(&cur->children); 
         e = list_next(e))
    {
      a = list_entry(e, struct thread_ashes, elem);
      if (a->tid == child_tid && !a->has_been_waited)
        break;
      else
        a = NULL;
    }
  }

  if (a == NULL)
    return -1;
  a->has_been_waited = true;
  sema_down(&a->sema);
  return a->exit_status;
}

#ifdef VM
static void swap_destructor (struct hash_elem *e, void *aux UNUSED) {
  struct vm_page_struct *vp;
  vp = hash_entry(e, struct vm_page_struct, elem);

  if (vp->swap > 0)
    swap_free(vp->swap);

  free(vp);
}
#endif /* VM */

extern struct lock fs_lock;

/*! Free the current process's resources. */
void process_exit(void) {
  struct thread *cur = thread_current();
  uint32_t *pd;

  /* Process termination message */
  printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
  
#ifdef VM
  // Write back all mmaps
  struct mm_struct *mm = &cur->mm;
  struct vm_area_struct *iter = mm->mmap;

  while (iter != NULL) {

    if (iter->vm_flags & VM_MMAP) {
      uint8_t *page;
      for (page = iter->vm_start; page < iter->vm_end; page += PGSIZE) {

        size_t offset = ((uintptr_t) page - (uintptr_t) iter->vm_start) + 
                        iter->vm_file_ofs;

        int32_t read_bytes = iter->vm_file_read_bytes - offset;

        if (read_bytes > 0 && pagedir_is_dirty(mm->pagedir, page)) {
          read_bytes = (read_bytes > PGSIZE) ? PGSIZE : read_bytes;
          lock_acquire(&fs_lock);
          file_write_at(
            iter->vm_file, 
            pagedir_get_page(mm->pagedir, page), 
            iter->vm_file_read_bytes, 
            offset);
          lock_release(&fs_lock);
        }

      }
    }

    /* Reclaim swap used by the process */
    hash_destroy(&iter->vm_page_table, swap_destructor);
    iter = iter->next;
  }
#endif /* VM */

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->PAGEDIR;

  if (pd != NULL) {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->PAGEDIR = NULL;

      pagedir_activate(NULL);
      pagedir_destroy(pd);
  }
}

/*! Sets up the CPU for running user code in the current thread.
    This function is called on every context switch. */
void process_activate(void) {
    struct thread *t = thread_current();

    /* Activate thread's page tables. */
    pagedir_activate(t->PAGEDIR);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update();
}

/*! We load ELF binaries.  The following definitions are taken
    from the ELF specification, [ELF1], more-or-less verbatim.  */

/*! ELF types.  See [ELF1] 1-2. @{ */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;
/*! @} */

/*! For use with ELF types in printf(). @{ */
#define PE32Wx PRIx32   /*!< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /*!< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /*!< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /*!< Print Elf32_Half in hexadecimal. */
/*! @} */

/*! Executable header.  See [ELF1] 1-4 to 1-8.
    This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
};

/*! Program header.  See [ELF1] 2-2 to 2-4.  There are e_phnum of these,
    starting at file offset e_phoff (see [ELF1] 1-6). */
struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

/*! Values for p_type.  See [ELF1] 2-3. @{ */
#define PT_NULL    0            /*!< Ignore. */
#define PT_LOAD    1            /*!< Loadable segment. */
#define PT_DYNAMIC 2            /*!< Dynamic linking info. */
#define PT_INTERP  3            /*!< Name of dynamic loader. */
#define PT_NOTE    4            /*!< Auxiliary info. */
#define PT_SHLIB   5            /*!< Reserved. */
#define PT_PHDR    6            /*!< Program header table. */
#define PT_STACK   0x6474e551   /*!< Stack segment. */
/*! @} */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. @{ */
#define PF_X 1          /*!< Executable. */
#define PF_W 2          /*!< Writable. */
#define PF_R 4          /*!< Readable. */
/*! @} */

static bool setup_stack(void **esp, char *exec_name, char *saveptr);
static bool validate_segment(const struct Elf32_Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

extern struct lock fs_lock;

/*! Loads an ELF executable from FILE_NAME into the current thread.  Stores the
    executable's entry point into *EIP and its initial stack pointer into *ESP.
    Returns true if successful, false otherwise. */
bool load(const char *file_name, void (**eip) (void), void **esp) {
    struct thread *t = thread_current();
    struct Elf32_Ehdr ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

#ifdef VM
    /* Initialize memory mapping */
    mm_init(&t->mm);
#endif

    /* Allocate and activate page directory. */
    t->PAGEDIR = pagedir_create();
    if (t->PAGEDIR == NULL) 
        goto done;
    process_activate();

    /* Parse the file name; first we need to actually copy the name out
       (because strtok_r modifies the string) */
    /* Limitation: This implementation requires that the command fit in one
       page. Also it means you can't have spaces in file names (which is
       sort of silly; I think the actual standards allow all sorts of
       nonsense in file names, like EOF and newlines). */
    char *saveptr;
    char *buf = (char *) palloc_get_page(0);

    char *exec_name;
    if (buf == NULL) {
      // Well, that's awkward
      printf("Out of memory\n");
      goto done;
    }
    strlcpy(buf, file_name, PGSIZE/2);

    // Parse the first thing out
    exec_name = strtok_r(buf, " ", &saveptr);
    if (exec_name == NULL) {
      // Empty command? Well, I guess we shouldn't do anything then?
      palloc_free_page(buf);
      goto done;
    }

    /* Open executable file. */
    lock_acquire(&fs_lock);
    file = filesys_open(exec_name);
    if (file)
        file_deny_write(file);
    lock_release(&fs_lock);
    if (file == NULL) {
        printf("load: %s: open failed\n", exec_name);
        goto done; 
    }

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 3 || ehdr.e_version != 1 ||
        ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", file_name);
        goto done; 
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Elf32_Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;

        file_ofs += sizeof phdr;

        switch (phdr.p_type) {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;

        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;

        case PT_LOAD:
            if (validate_segment(&phdr, file)) {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint32_t file_page = phdr.p_offset & ~PGMASK;
                uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint32_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0) {
                    /* Normal segment.
                       Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) -
                                 read_bytes);
                }
                else {
                    /* Entirely zero.
                       Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
#ifdef PROCESS_C_DEBUG
                printf("file = %x, file_page = %x, mem_page = %x, read = %x, zero = %x\n",
                  file, file_page, mem_page, read_bytes, zero_bytes);
#endif
                if (!load_segment(file, file_page, (void *) mem_page,
                                  read_bytes, zero_bytes, writable))
                    goto done;
            }
            else {
                goto done;
            }
            break;
        }
    }

    /* Set up stack. We need to pass the save pointer (it can be passed
       by value actually since we don't use it again) as well as the
       parsed file name (since that needs to be on the stack).
       Also we need to store the result of setup_stack for a bit (the comma
       operator gives the wrong return value) so we can free the buffer */
    bool stack_suc = setup_stack(esp, exec_name, saveptr);
    palloc_free_page(buf);
    if (!stack_suc)
        goto done;

    /* Start address. */
    *eip = (void (*)(void)) ehdr.e_entry;

    success = true;

done:
    /* We arrive here whether the load is successful or not. */
    //file_close(file);
    thread_current()->exec = file;
    return success;
}

/* load() helpers. */

static bool install_page(void *upage, void *kpage, bool writable);

/*! Checks whether PHDR describes a valid, loadable segment in
    FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr *phdr, struct file *file) {
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
        return false; 

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (Elf32_Off) file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false; 

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;
  
    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void *) phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void *) (phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed it then user
       code that passed a null pointer to system calls could quite likely panic
       the kernel by way of null pointer assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}

#ifdef VM

static int32_t vm_load_seg_absent(struct vm_area_struct *vma, 
                                  struct vm_fault *vmf)
{
  uint8_t *kpage;
  uint8_t *upage_in = (uint8_t *) ((uint32_t) vmf->fault_addr & ~PGMASK);

  struct frame_entry f;

  struct vm_page_struct *vmp_in = malloc(sizeof(struct vm_page_struct));

  kpage = vm_kpage(&vmp_in);

  frame_make(&f, vma, upage_in);
  vmp_in->upage = upage_in;
  vmp_in->swap = 0;

  vmp_in->pte = (uintptr_t) kpage | PTE_P | PTE_U;
  if (vma->vm_flags & VM_WRITE) vmp_in->pte |= PTE_W;

  struct hash_elem *e = hash_replace(&vma->vm_page_table, &vmp_in->elem);

  size_t swap_in = 0;

  if (e != NULL) {
    vmp_in = hash_entry(e, struct vm_page_struct, elem);
    swap_in = vmp_in->swap;
    free(vmp_in);
  }

  /* Pin the new frame for kerkel PF */
  if (!vmf->user)
    frame_entry_pin(&f);

  if (swap_in != 0)
  {
    swap_lock_acquire(swap_in);
    swap_read(swap_in, kpage);
    swap_lock_release(swap_in);
    swap_free(swap_in);
  }
  else
  {
    off_t offset = vma->vm_file_ofs + vmf->page_ofs;

    int read_bytes = vma->vm_file_read_bytes - vmf->page_ofs;
    read_bytes = (read_bytes < 0) ? 0 : read_bytes;
    read_bytes = (read_bytes > PGSIZE) ? PGSIZE : read_bytes;

    if (read_bytes)
    {
      lock_acquire(&fs_lock);
      file_read_at(vma->vm_file, kpage, read_bytes, offset);
      lock_release(&fs_lock);
    }
    memset(kpage + read_bytes, 0, PGSIZE - read_bytes);
  }

  bool success = install_page(upage_in, kpage, vma->vm_flags & VM_WRITE);

  if (success && swap_in)
    f.flags |= PG_DIRTY;

  frame_push(&f);

  return success;
}

static int32_t vm_stack_absent(struct vm_area_struct *vma UNUSED, 
                               struct vm_fault *vmf)
{
  uint8_t *kpage;
  uint8_t *upage_in = (uint8_t *) ((uint32_t) vmf->fault_addr & ~PGMASK);

  struct frame_entry f;

  struct vm_page_struct *vmp_in = malloc(sizeof(struct vm_page_struct));

  kpage = vm_kpage(&vmp_in);

  frame_make(&f, vma, upage_in);
  vmp_in->upage = upage_in;
  vmp_in->swap = 0;

  vmp_in->pte = (uintptr_t) kpage | PTE_P | PTE_W | PTE_U;

  struct hash_elem *e = hash_replace(&vma->vm_page_table, &vmp_in->elem);

  size_t swap_in = 0;

  if (e != NULL) {
    vmp_in = hash_entry(e, struct vm_page_struct, elem);
    swap_in = vmp_in->swap;
    free(vmp_in);
  }

  /* Pin the new frame for kerkel PF */
  if (!vmf->user)
    frame_entry_pin(&f);

  if (swap_in != 0)
  {
    swap_lock_acquire(swap_in);
    swap_read(swap_in, kpage);
    swap_lock_release(swap_in);
    swap_free(swap_in);
  }

  bool success = install_page(upage_in, kpage, true);

  frame_push(&f);

  return success;
}

static struct vm_operations_struct vm_stack_ops = 
  { .open = NULL, .close = NULL, .absent = vm_stack_absent };

static struct vm_operations_struct vm_load_seg_ops = 
  { .open = NULL, .close = NULL, .absent = vm_load_seg_absent };

#endif /* VM */

/*! Loads a segment starting at offset OFS in FILE at address UPAGE.  In total,
    READ_BYTES + ZERO_BYTES bytes of virtual memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

    The pages initialized by this function must be writable by the user process
    if WRITABLE is true, read-only otherwise.

    Return true if successful, false if a memory allocation error or disk read
    error occurs. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable) {
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

#ifdef VM
    struct mm_struct *mm = &thread_current()->mm;
    struct vm_area_struct *vma = malloc(sizeof(struct vm_area_struct));
    vma->vm_start = upage;
    vma->vm_end = upage + read_bytes + zero_bytes;

    vma->vm_flags = VM_READ | VM_EXEC | VM_EXECUTABLE |
                    (VM_WRITE & -(int)writable);
    vma->vm_ops = &vm_load_seg_ops;
    vma->vm_file = file;
    vma->vm_file_ofs = ofs;
    vma->vm_file_read_bytes = read_bytes;
    vma->vm_file_zero_bytes = zero_bytes;
    mm_insert_vm_area(mm, vma);
#else /* no-VM */
    while (read_bytes > 0 || zero_bytes > 0) {
        /* Calculate how to fill this page.
           We will read PAGE_READ_BYTES bytes from FILE
           and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t *kpage = palloc_get_page(PAL_USER);

        if (kpage == NULL)
            return false;

        /* Load this page. */
        lock_acquire(&fs_lock);
        int actual_read_bytes = file_read_at(file, kpage, page_read_bytes, ofs);
        lock_release(&fs_lock);

        if (actual_read_bytes != (int) page_read_bytes) {
            palloc_free_page(kpage);
            return false;
        }

        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable)) {
            palloc_free_page(kpage);
            return false; 
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }

#endif /* VM */

    return true;
}

static inline char* kpage_to_phys(char *kpage, char *ptr) {
  return PHYS_BASE + (ptr - kpage - PGSIZE); 
}

/*! Create a minimal stack by mapping a zeroed page at the top of
    user virtual memory. */
static bool setup_stack(void **esp, char *exec_name, char *saveptr) {
    char *kpage;
    char *stack, *tok;
    uint32_t argc = 0, i;
    uint8_t *upage = ((uint8_t *) PHYS_BASE) - PGSIZE;

#ifdef VM
    struct mm_struct *mm = &thread_current()->mm;
    uint32_t *pd = mm->pagedir;
    struct vm_area_struct *vma = malloc(sizeof(struct vm_area_struct));
    vma->vm_start = upage;
    vma->vm_end = PHYS_BASE;

    vma->vm_flags = VM_READ | VM_WRITE;

    vma->vm_ops = &vm_stack_ops;

    mm->vma_stack = vma;
    mm_insert_vm_area(mm, vma);

    /* Load kpage by causing page fault at upage */
    *upage = 0;
    kpage = pagedir_get_page(pd, upage);
#else
    kpage = palloc_get_page(PAL_USER | PAL_ZERO);

    if (kpage == NULL) {
      return false;
    } else if (!install_page(upage, kpage, true)) {
      palloc_free_page(kpage);
      return false;
    }
#endif

    /* For "portability" I should probably use sizeof instead of
       assuming sizes are 4 */
    stack = (char *)kpage + PGSIZE;

    /* Restore exec_name to the whole cmdline */
    if (*saveptr != '\0')
      *(saveptr - 1) = ' ';

    /* First, push the name onto the stack */
    stack -= strlen(exec_name) + 1;
    memcpy(stack, exec_name,  strlen(exec_name) + 1);

    /* Count argc */
    tok = stack;
    while(*tok != '\0')
      argc += (*tok++ != ' ' && (*tok == ' ' || *tok == '\0'));

    /* Make sure argc > 0 */
    ASSERT(argc > 0);

    /* Take note of the start of cmdline */
    tok = stack;

    /* Pad the stack out. The page is already zeroed when allocated,
       so that's okay */
    stack -= (((uint32_t)stack) % sizeof(char*));

    /* Reserve spaces for argv[i], 0 <= i <= argc */
    stack -= (argc + 1) * sizeof(char*);

    /* Fill argv[i], 0 <= i < argc */
    i = 0;
    for (tok = strtok_r(tok, " ", &saveptr); 
         tok; 
         tok = strtok_r(NULL, " ", &saveptr)) {
      if (*tok != '\0')
        ((char**)stack)[i++] = kpage_to_phys(kpage, tok);
    }

    /* Push argv */
    ((char**)stack)[-1] = kpage_to_phys(kpage, stack);
    stack -= sizeof(char*);

    /* Push argc */
    ((uint32_t*)stack)[-1] = argc;
    stack -= sizeof(uint32_t);

    /* Empty return address */
    stack -= sizeof(void*);

    *esp = (stack - (char *)kpage - PGSIZE + PHYS_BASE);

    return true;
}

/*! Adds a mapping from user virtual address UPAGE to kernel
    virtual address KPAGE to the page table.
    If WRITABLE is true, the user process may modify the page;
    otherwise, it is read-only.
    UPAGE must not already be mapped.
    KPAGE should probably be a page obtained from the user pool
    with palloc_get_page().
    Returns true on success, false if UPAGE is already mapped or
    if memory allocation fails. */
static bool install_page(void *upage, void *kpage, bool writable) {
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
       address, then map our page there. */
    return (pagedir_get_page(t->PAGEDIR, upage) == NULL &&
            pagedir_set_page(t->PAGEDIR, upage, kpage, writable));
}

