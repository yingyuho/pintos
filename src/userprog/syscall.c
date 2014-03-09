#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "devices/input.h"

int process_wait(tid_t);
void shutdown_power_off(void) NO_RETURN;

#include "threads/synch.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include <string.h>

#ifdef VM
#include <round.h>
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#endif

struct lock fs_lock;

static void syscall_handler(struct intr_frame *);

// // Copied from file.c
// struct file {
//     struct inode *inode;        /*!< File's inode. */
//     off_t pos;                  /*!< Current position. */
//     bool deny_write;            /*!< Has file_deny_write() been called? */
// };

void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
    lock_init(&fs_lock);
}

/*! Reads a byte at user virtual address UADDR.
    UADDR must be below PHYS_BASE.
    Returns the byte value if successful, -1 if a segfault occurred. */
static int get_user(const uint8_t *uaddr) {
    int result;
    asm ("movl $1f, %0; movzbl %1, %0; 1:"
         : "=&a" (result) : "m" (*uaddr));
    return result;
}

/*! Writes BYTE to user address UDST.
    UDST must be below PHYS_BASE.
    Returns true if successful, false if a segfault occurred. */
static bool put_user (uint8_t *udst, uint8_t byte) {
    int error_code;
    asm ("movl $1f, %0; movb %b2, %1; 1:"
         : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != -1;
}

static inline bool get_user_word(uint8_t *dest, const uint8_t *src) {
    int i;
    if (src + 4 > (uint8_t*)PHYS_BASE) { return false; }
    i = get_user(src++);
    if (i < 0) { return false; } else { *dest++ = (uint8_t)i; }
    i = get_user(src++);
    if (i < 0) { return false; } else { *dest++ = (uint8_t)i; }
    i = get_user(src++);
    if (i < 0) { return false; } else { *dest++ = (uint8_t)i; }
    i = get_user(src++);
    if (i < 0) { return false; } else { *dest++ = (uint8_t)i; }
    return true;
}

static inline void get_user_arg(int32_t *dest, const uint32_t *src, uint32_t offset) {
    if (!get_user_word((uint8_t*)&dest[offset], (uint8_t*)&src[offset]))
        thread_exit();
}

static int wait_load(tid_t tid) {
  struct thread *cur = thread_current();
  struct list_elem *e;
  struct thread_ashes *a = NULL;

  if (tid == TID_ERROR)
    return -1;

  if (!list_empty(&cur->children)) {
    for (e = list_back(&cur->children); 
         e != list_head(&cur->children); 
         e = list_prev(e))
    {
      a = list_entry(e, struct thread_ashes, elem);
      if (a->tid == tid)
        break;
      else
        a = NULL;
    }
  }

  if (a == NULL)
    return -1;

  sema_down(&a->thread->load_done);
  return a->load_success ? tid : -1;
}

static void check_write_array(uint8_t *ptr, int len) {
  // Don't think ahout this code too hard
  if (!put_user(ptr, *ptr))
      thread_exit();
  if (!put_user(ptr + len - 1, ptr[len-1]))
      thread_exit();
}

static void check_array(uint8_t *ptr, int len) {
  // A somewhat halfhearted check of whether an array is valid (it will work
  // for small enough arrays); just checks the first and last bytes.
  if (get_user(ptr) == -1)
    thread_exit();
  if (get_user(ptr + len - 1) == -1)
    thread_exit();
}

// returns false if the filename is too long (>255 for now)
static bool check_filename(char *str) {
  if (str == NULL || str >= (char *) PHYS_BASE)
    thread_exit();
  // There should be a \0 somewhere in the first 256 bytes; otherwise the
  // filename is way too long (or we've been passed an invalid
  // pointer)
  uint32_t i;
  int c;
  for (i = 0; i < 256; ++i) {
    c = get_user((uint8_t *)(str+i));
    if (c == 0)
      break;
    else if (c == -1)
      thread_exit();
  }
  if (i == 256) {
    // The string is too long
    return false;
  }
  return true;
}

#ifdef VM
static bool pin_func(struct frame_entry *f, void *aux) {
  struct vm_interval *v = aux;

  if (v->pagedir == f->pagedir && 
          v->vm_start <= (uint8_t *) f->upage &&
          (uint8_t *) f->upage < v->vm_end)
  {
    frame_entry_pin(f);
  }

  return true;
}

static bool unpin_func(struct frame_entry *f, void *aux) {
  uint32_t *pd = aux;
  if (f->pagedir == pd)
    frame_entry_unpin(f);
  return true;
}

static void pin_frames(uint32_t *pd, uint8_t *start, size_t len)
{
  struct vm_interval vm_intv = 
  { 
    .pagedir = pd,
    .vm_start = (uint8_t *) ROUND_DOWN((uintptr_t) start, PGSIZE), 
    .vm_end = (uint8_t *) ROUND_UP((uintptr_t) start + len, PGSIZE)
  };

  frame_for_each(pin_func, &vm_intv);

  uint8_t *ptr;
  for (ptr = vm_intv.vm_start; ptr < vm_intv.vm_end; ptr += PGSIZE)
    if(get_user(ptr) < 0) {
      printf("pin_frames: failed, s = %x, p = %x, e = %x\n",
        (uintptr_t) vm_intv.vm_start, 
        (uintptr_t) ptr, 
        (uintptr_t) vm_intv.vm_end);
      thread_exit();
    }
}

#else
static void pin_frames(uint32_t *pd, uint8_t *start, size_t len) {}
#endif

// Since we have no particular guarantee on what open() will return other
// than that it's positive and not equal to any other fd open by the file...
static int next_fd = 10;

#ifdef VM
static int mmap (int fd, void *addr);
static void munmap (int mapping);
#endif

static struct file *find_file(int fd);

static void syscall_handler(struct intr_frame *f) {
  // Take a look at the system call number; this is the first
  // thing on the caller's stack. While we're here, might as
  // well get the others.
  int32_t args[4];
  int i;
  struct file *file;

  // Sanity check: Stack pointer should not be below the code segment (how
  // would you even get there)
  if (f->esp < (void *)0x08048000)
    thread_exit();

  get_user_arg(args, f->esp, 0);
  struct thread *cur = thread_current();

  // Technically these are an enum, but C implements enums as ints...
  switch(args[0]) {

  case SYS_HALT:
    shutdown_power_off();
    goto done;

  case SYS_EXIT:
    get_user_arg(args, f->esp, 1);
    thread_current()->exit_status = args[1];
    thread_current()->ashes->exit_status = args[1];
    thread_exit();
    goto done;

  case SYS_EXEC:
    get_user_arg(args, f->esp, 1);
    f->eax = -1;
    if (get_user((uint8_t*)args[1]) < 0) { thread_exit(); }
    pin_frames(cur->PAGEDIR, (void *) args[1], 
      strlen((const char *) args[1]));
    //len = strlen((const char*)args[1]) + 1;
    //buf = malloc(len);
    //strlcpy(buf, (const char*)args[1], len);
    tid_t tid = process_execute((const char*)args[1]);
    f->eax = wait_load(tid);
    if ((int32_t)f->eax == -1)
      process_wait(tid);
        
    //free(buf);
    goto done;

  case SYS_WAIT:
    get_user_arg(args, f->esp, 1);
    f->eax = process_wait(args[1]);
    break;
  case SYS_CREATE:
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);

    // Check that the filename is valid
    if(!check_filename((char *)args[1])) {
      f->eax = 0;
      goto done;
    }
    pin_frames(cur->PAGEDIR, (void *) args[1], 1);

    // Try to create the file. I don't really think I need to check the
    // initial size here...
    lock_acquire(&fs_lock);
    f->eax = filesys_create((char *) args[1], (off_t) args[2]);
    lock_release(&fs_lock);

    goto done;

  case SYS_REMOVE:
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);

    // Check that the filename is valid
    if(!check_filename((char *)args[1])) {
      f->eax = 0;
      goto done;
    }
    pin_frames(cur->PAGEDIR, (void *) args[1], 1);

    lock_acquire(&fs_lock);
    f->eax = filesys_remove((char *) args[1]);
    lock_release(&fs_lock);

    goto done;

  case SYS_OPEN:
    get_user_arg(args, f->esp, 1);

    // Assume we're going to fail :)
    f->eax = -1;

    // Check whether the provided filename lives in userland
    if (((char *) args[1] == NULL) || ((char *) args[1] >= (char *) PHYS_BASE) ||
        (get_user((uint8_t *)args[1]) == -1) ||
        (get_user((uint8_t *)args[1] + strlen((char *)args[1]) - 1) == -1)) {
      thread_exit();
    }

    pin_frames(cur->PAGEDIR, (void *) args[1], 1);

    if (cur->nfiles == 128) {
      goto done;
    }

    // Try to open the file
    lock_acquire(&fs_lock);
    struct file *ff = filesys_open((char *)args[1]);
    lock_release(&fs_lock);
    if (ff == NULL) {
      // File couldn't be opened (for whatever reason)
      goto done;
    }

    // If there are 0 or 64 files open we need to allocate a new page
    if (cur->nfiles == 0) {
      cur->files[0] = (struct file_node *) palloc_get_page(0);
      // If this allocation failed, close the file and return -1
      if (cur->files[0] == NULL) {
        lock_acquire(&fs_lock);
        file_close(ff);
        lock_release(&fs_lock);
        goto done;
      }
    }
    if (cur->nfiles == 64) {
      cur->files[1] = (struct file_node *) palloc_get_page(0);
      // If this allocation failed, close the file and return -1
      if (cur->files[1] == NULL) {
        lock_acquire(&fs_lock);
        file_close(ff);
        lock_release(&fs_lock);
        goto done;
      }
    }
    
    // At this point we're going to succeed, so set the return value.

    // This can be compiled into an atomic instruction... not that it matters,
    // because multiple threads using the same file descriptor is fine.
    // We only need that the file descriptor is unique for a given thread.
    f->eax = next_fd++;

    if (cur->nfiles < 64) {
      cur->files[0][cur->nfiles].fd = f->eax;
      cur->files[0][cur->nfiles].f = ff;
    }
    else {
      cur->files[1][cur->nfiles - 64].fd = f->eax;
      cur->files[1][cur->nfiles - 64].f = ff;
    }
    
    cur->nfiles++;
    goto done;
    break;

  case SYS_FILESIZE:
    get_user_arg(args, f->esp, 1);
    // Return -1 if fd is not valid
    f->eax = -1;

    file = find_file(args[1]);

    if (file != NULL) {
      lock_acquire(&fs_lock);
      f->eax = file_length(file);
      lock_release(&fs_lock);
    }

    goto done;

  case SYS_READ:
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);
    get_user_arg(args, f->esp, 3);

    if (((void *)args[2] == NULL) || ((void *)args[2] >= PHYS_BASE))
      thread_exit();

    // Verify the buffer
    check_array((void *) args[2], args[3]);
    check_write_array((void *) args[2], args[3]);
    pin_frames(cur->PAGEDIR, (void *) args[2], args[3]);

    if (args[1] == 0) { // stdin
      for(i = 0; i < args[3]; ++i) {
        ((uint8_t *)args[2])[i] = input_getc();
        // how would you detect EOF though? hmm.
      }
      f->eax = i;
      goto done;
    }
    else {
      // Find the file
      file = find_file(args[1]);

      if (file != NULL) {
        lock_acquire(&fs_lock);
        f->eax = file_read(file, (void *)args[2], args[3]);
        lock_release(&fs_lock);
      }
      else
        f->eax = -1;
    }

    goto done;

  case SYS_WRITE:
    // Using the second solution; we need to check that it's really valid to
    // read the buffer
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);
    get_user_arg(args, f->esp, 3);

    // A fairly halfhearted check of whether the buffer is valid; every time
    // we write some bytes, check whether the first and last bytes are valid

    if (args[1] == 1) { // stdout
      // We're writing to console so we'll always write everything
      f->eax = args[3];

      while ((uint32_t)args[3] > 256) {
        // Check whether the first and last bytes are valid
        if (get_user((uint8_t *)args[2]) == -1)
          thread_exit();
        if (get_user((uint8_t *)args[2] + 255) == -1)
          thread_exit();
        putbuf((char *)args[2], 256);
        ((uint32_t *)args)[3] -= 256;
        ((char **)args)[2] += 256;
      }
      if (get_user((uint8_t *)args[2]) == -1)
        thread_exit();
      if (get_user((uint8_t *)args[2] + (uint32_t)args[3] - 1) == -1)
        thread_exit();
      putbuf((char *)args[2], (uint32_t)args[3]);
    }
    else {
      // Check whether the buffer is valid
      check_array((void *) args[2], args[3]);
      pin_frames(cur->PAGEDIR, (void *) args[2], args[3]);

      file = find_file(args[1]);

      if (file != NULL) {
        lock_acquire(&fs_lock);
        f->eax = file_write(file, (void *)args[2], args[3]);
        lock_release(&fs_lock);
      }
      else
        f->eax = 0;
    }

    goto done;

  case SYS_SEEK:
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);

    file = find_file(args[1]);

    if (file != NULL)
      file_seek(file, args[2]);

    goto done;

  case SYS_TELL:
    get_user_arg(args, f->esp, 1);

    file = find_file(args[1]);

    if (file != NULL) {
      lock_acquire(&fs_lock);
      f->eax = file_tell(file);
      lock_release(&fs_lock);
    }
    else
      f->eax = -1;
   
    goto done;

  case SYS_CLOSE:
    // Close doesn't actually return anything. Retrieve the fd, then
    // iterate through the table of files and remove the appropriate entry
    // if found.
    get_user_arg(args, f->esp, 1);
    for (i = 0; i < cur->nfiles && i < 64; ++i) {
      if (cur->files[0][i].fd == args[1]) {
        // Close this file
        lock_acquire(&fs_lock);
        file_close(cur->files[0][i].f);
        lock_release(&fs_lock);
        // Move the last entry in the table here
        cur->nfiles--;
        if (cur->nfiles < 64) {
          cur->files[0][i].fd = cur->files[0][cur->nfiles].fd;
          cur->files[0][i].f = cur->files[0][cur->nfiles].f;
          if (cur->nfiles == 0) {
            // deallocate files[0]
            palloc_free_page(cur->files[0]);
            cur->files[0] = NULL;
          }
        }
        else {
          cur->files[0][i].fd = cur->files[1][cur->nfiles-64].fd;
          cur->files[0][i].f = cur->files[1][cur->nfiles-64].f;
          if (cur->nfiles == 64) {
            // deallocate files[1]
            palloc_free_page(cur->files[1]);
            cur->files[1] = NULL;
          }
        }
        goto done;
      }
    }

    for (i = 0; i < cur->nfiles - 64; ++i) {
      if (cur->files[1][i].fd == args[1]) {
        lock_acquire(&fs_lock);
        file_close(cur->files[1][i].f);
        lock_release(&fs_lock);
        // Move the last entry in the table here
        cur->nfiles--;
        if (cur->nfiles == 64) {
          palloc_free_page(cur->files[1]);
          cur->files[1] = NULL;
        }
        else {
          cur->files[1][i].fd = cur->files[1][cur->nfiles-64].fd;
          cur->files[1][i].f = cur->files[1][cur->nfiles-64].f;
        }
        goto done;
      }
    }

    break;

    f->eax = filesys_create((char *) args[1], (off_t) args[2]);
#ifdef VM
  /* mapid_t mmap (int fd, void *addr) */
  /* Maps the file open as fd into the process's virtual address space. 
     The entire file is mapped into consecutive virtual pages starting at addr. */
  case SYS_MMAP:
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);

    f->eax = mmap(args[1], (void *) args[2] );

    goto done;

  /* void munmap (mapid_t mapping) */
  /* Unmaps the mapping designated by mapping, which must be a mapping ID returned 
     by a previous call to mmap by the same process that has not yet been unmapped. */
  case SYS_MUNMAP:
    get_user_arg(args, f->esp, 1);

    munmap(args[1]);

    goto done;
#endif /* VM */
  default:
    // I mean, yeah, there are other ways to implement this
    printf("unrecognized system call\n");
    thread_exit();
  }
done:
#ifdef VM
  frame_for_each(unpin_func, cur->PAGEDIR);
#endif /* VM */
  return;
}

static struct file *find_file(int fd) {
  int i;
  struct thread *cur = thread_current();
  for (i = 0; i < cur->nfiles && i < 64; ++i) {
    if (cur->files[0][i].fd == fd) {
      return cur->files[0][i].f;
    }
  }

  for (i = 0; i < cur->nfiles - 64; ++i) {
    if (cur->files[1][i].fd == fd) {
      return cur->files[1][i].f;
    }
  }

  return NULL;
}

#ifdef VM

static int32_t vm_mmap_absent(struct vm_area_struct *vma UNUSED, 
                              struct vm_fault *vmf)
{
  uint8_t *kpage;
  uint8_t *upage_in = (uint8_t *) ((uint32_t) vmf->fault_addr & ~PGMASK);
  struct mm_struct *mm = vma->vm_mm;
  uint32_t *pd_in = mm->pagedir;

  struct frame_entry f;

  struct vm_page_struct *vmp_in = malloc(sizeof(struct vm_page_struct));

  kpage = vm_kpage(&vmp_in);

  frame_make(&f, vma, upage_in);
  vmp_in->upage = upage_in;
  vmp_in->swap = 0;

  vmp_in->pte = (uintptr_t) kpage | PTE_P | PTE_U | PTE_W;

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
  }

  uint32_t *pd = thread_current()->PAGEDIR;

  bool success = (pagedir_get_page(pd, upage_in) == NULL &&
                  pagedir_set_page(pd, upage_in, kpage, true));

  frame_push(&f);

  return success;
}

static struct vm_operations_struct vm_mmap_ops = 
  { .open = NULL, .close = NULL, .absent = vm_mmap_absent };

static int mmap (int fd, void *addr) {

  static int id = 2;
  if (id > 1000000000) {
    // You know, just in case someone decides to open a bazillion files -.-
    id = 2;
  }

  /* Fail for STDIN, STDOUT and negative FD */
  /* Fail if ADDR is not page-aligned */
  if (fd < 2 || ((uintptr_t) addr % PGSIZE != 0) || (addr == 0x0))
    return -1;

  struct file *f = find_file(fd);

  if (f == NULL)
    return -1;

  f = file_reopen(f);

  int read_bytes;

  lock_acquire(&fs_lock);
  read_bytes = file_length(f);
  lock_release(&fs_lock);

  if (read_bytes <= 0)
    return -1;

  /* Try to allocate memory area */
  struct mm_struct *mm = &thread_current()->mm;
  struct vm_area_struct *vma = malloc(sizeof(struct vm_area_struct));

  size_t all_bytes = ROUND_UP(read_bytes, PGSIZE);
  size_t zero_bytes = all_bytes - read_bytes;

  vma->vm_start = (uint8_t *) addr;
  vma->vm_end = vma->vm_start + all_bytes;

  vma->vm_flags = VM_READ | VM_MMAP | VM_WRITE;
  vma->vm_ops = &vm_mmap_ops;
  vma->vm_file = f;
  vma->vm_file_ofs = 0;
  vma->vm_file_read_bytes = read_bytes;
  vma->vm_file_zero_bytes = zero_bytes;
  vma->mmap_id = id;

  if (mm_insert_vm_area(mm, vma)) {
    return id++;
  } else {
    free(vma);
    return -1;
  }

  return -1;
}

static void munmap (int mapping) {
  // Go through the mmap list, checking whether it was actually mapped
  struct mm_struct *mm = &thread_current()->mm;
  struct vm_area_struct *iter = mm->mmap;
  struct vm_area_struct *prev = NULL;
  while (iter != NULL) {
    if (iter->mmap_id == mapping) {
      if (iter->vm_flags & VM_MMAP) {
	if (prev != NULL)
	  prev->next = iter->next;
	else
	  mm->mmap = iter->next;
	void *page;
	int nbytes = iter->vm_file_read_bytes;
        for (page = iter->vm_start; page < iter->vm_end; page += PGSIZE, nbytes -= PGSIZE) {
	  lock_acquire(&fs_lock);
	  if (pagedir_is_dirty(mm->pagedir, page))
	    file_write_at(iter->vm_file, pagedir_get_page(mm->pagedir, page), nbytes>PGSIZE?PGSIZE:nbytes, iter->vm_file_ofs + (page - (void*)iter->vm_start));
	  lock_release(&fs_lock);
	}
	free(iter);
	return;
      }      
      else {
	/* Well, that's not good */
	printf("Tried to unmap non-mmaped area!\n");
	thread_exit();
      }
    }
    prev = iter;
    iter = iter->next;
  }
}
#endif /* VM */
