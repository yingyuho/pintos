#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#include "filesys/file.h"
#include "filesys/filesys.h"

static void syscall_handler(struct intr_frame *);

void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
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

static void check_array(uint8_t *ptr, int len) {
  // A somewhat halfhearted check of whether an array is valid (it will work
  // for small enough arrays); just checks the first and last bytes.
  if (get_user(ptr) == -1)
    thread_exit();
  if (get_user(ptr + len - 1) == -1)
    thread_exit();
}

// Since we have no particular guarantee on what open() will return other
// than that it's positive and not equal to any other fd open by the file...
static int next_fd = 10;

static void syscall_handler(struct intr_frame *f) {
  // Take a look at the system call number; this is the first
  // thing on the caller's stack. While we're here, might as
  // well get the others.
  int32_t args[4];
  
  // Sanity check: Stack pointer should not be below the code segment (how
  // would you even get there)
  if (f->esp < 0x08048000)
    thread_exit();

  get_user_arg(args, f->esp, 0);
  

  // printf("%d %d %d %d\n", num, arg1, arg2, arg3);
  // Technically these are an enum, but C implements enums as ints...
  switch(args[0]) {

  case SYS_HALT:
    shutdown_power_off();
    break;

  case SYS_EXIT:
    get_user_arg(args, f->esp, 1);
    thread_current()->exit_status = args[1];
    thread_exit();
    break;

  case SYS_EXEC:
    break;
  case SYS_WAIT:
    break;
  case SYS_CREATE:
    break;
  case SYS_REMOVE:
    break;
  case SYS_OPEN:
    get_user_arg(args, f->esp, 1);

    // Assume we're going to fail :)
    f->eax = -1;

    // Check whether the provided filename lives in userland
    if ((args[1] == NULL) || (args[1] >= PHYS_BASE) ||
	(get_user((uint8_t *)args[1]) == -1)) {
      thread_exit();
    }

    struct thread *cur = thread_current();

    if (cur->nfiles == 128) {
      return;
    }

    // Try to open the file
    struct file *ff = filesys_open((char *)args[1]);
    if (ff == NULL) {
      // File couldn't be opened (for whatever reason)
      return;
    }

    // If there are 0 or 64 files open we need to allocate a new page
    if (cur->nfiles == 0) {
      cur->files[0] = (struct file_node *) palloc_get_page(0);
      // If this allocation failed, close the file and return -1
      if (cur->files[0] == NULL) {
	file_close(ff);
	return;
      }
    }
    if (cur->nfiles == 64) {
      cur->files[1] = (struct file_node *) palloc_get_page(0);
      // If this allocation failed, close the file and return -1
      if (cur->files[1] == NULL) {
	file_close(ff);
	return;
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
    return;
    break;

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

      // TODO: Separate into some large number of writes if necessary
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
    else { // TODO: implement (once file descriptors are working)
      // Basically, check how many bytes we have to EOF and then write 
      // up to that many bytes.
    }
    break;

  case SYS_SEEK:
    break;
  case SYS_TELL:
    break;
  case SYS_CLOSE:
    break;
  default:
    // I mean, yeah, there are other ways to implement this
    printf("unrecognized system call\n");
    thread_exit();
  }
  return;
  //  printf("system call!\n");
  //  thread_exit();
}

