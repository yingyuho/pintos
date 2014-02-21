#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#include "threads/synch.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include <string.h>

static struct lock fs_lock;

static void syscall_handler(struct intr_frame *);

struct file {
    struct inode *inode;        /*!< File's inode. */
    off_t pos;                  /*!< Current position. */
    bool deny_write;            /*!< Has file_deny_write() been called? */
};

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

static void check_write_array(uint8_t *ptr, int len) {
// Don't think ahout this code too hard
if (put_user(ptr, *ptr) == -1)
    thread_exit();
if (put_user(ptr + len - 1, ptr[len-1]) == -1)
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
  if (str == NULL || str >= PHYS_BASE)
    thread_exit();
  // There should be a \0 somewhere in the first 256 bytes; otherwise the
  // filename is way too long (or we've been passed an invalid
  // pointer)
  int i;
  for (i = 0; i < 256; ++i) {
    if (get_user(str+i) == 0)
      break;
    else if (get_user(str+i) == -1)
      thread_exit();
  }
  if (i == 256) {
    // The string is too long
    return false;
  }
  return true;
}

// Since we have no particular guarantee on what open() will return other
// than that it's positive and not equal to any other fd open by the file...
static int next_fd = 10;


// TODO: All filesys functions should be in critical sections (could use
// some sort of global lock for example)

static void syscall_handler(struct intr_frame *f) {
  // Take a look at the system call number; this is the first
  // thing on the caller's stack. While we're here, might as
  // well get the others.
  int32_t args[4];
  int i;
  
  // Sanity check: Stack pointer should not be below the code segment (how
  // would you even get there)
  if (f->esp < 0x08048000)
    thread_exit();

  get_user_arg(args, f->esp, 0);
  struct thread *cur = thread_current();

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
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);

    // Check that the filename is valid
    if(! check_filename(args[1])) {
      f->eax = 0;
      return;
    }

    // Try to create the file. I don't really think I need to check the
    // initial size here...
    lock_acquire(&fs_lock);
    f->eax = filesys_create(args[1], args[2]);
    lock_release(&fs_lock);
    return;
    break;
  case SYS_REMOVE:
    break;
  case SYS_OPEN:
    get_user_arg(args, f->esp, 1);

    // Assume we're going to fail :)
    f->eax = -1;

    // Check whether the provided filename lives in userland
    if ((args[1] == NULL) || (args[1] >= PHYS_BASE) ||
	(get_user((uint8_t *)args[1]) == -1) ||
	(get_user((uint8_t *)args[1] + strlen((char *)args[1]) - 1) == -1)) {
      thread_exit();
    }

    if (cur->nfiles == 128) {
      return;
    }

    // Try to open the file
    lock_acquire(&fs_lock);
    struct file *ff = filesys_open((char *)args[1]);
    lock_release(&fs_lock);
    if (ff == NULL) {
      // File couldn't be opened (for whatever reason)
      return;
    }

    // If there are 0 or 64 files open we need to allocate a new page
    if (cur->nfiles == 0) {
      cur->files[0] = (struct file_node *) palloc_get_page(0);
      // If this allocation failed, close the file and return -1
      if (cur->files[0] == NULL) {
	lock_acquire(&fs_lock);
	file_close(ff);
	lock_release(&fs_lock);
	return;
      }
    }
    if (cur->nfiles == 64) {
      cur->files[1] = (struct file_node *) palloc_get_page(0);
      // If this allocation failed, close the file and return -1
      if (cur->files[1] == NULL) {
	lock_acquire(&fs_lock);
	file_close(ff);
	lock_release(&fs_lock);
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

  case SYS_FILESIZE:
    get_user_arg(args, f->esp, 1);
    // Return -1 if fd is not valid
    f->eax = -1;
    // Find the appropriate file
    for (i = 0; i < cur->nfiles && i < 64; ++i) {
      if (cur->files[0][i].fd == args[1]) {
	lock_acquire(&fs_lock);
        f->eax = file_length(cur->files[0][i].f);
	lock_release(&fs_lock);
	return;
      }
    }

    for (i = 0; i < cur->nfiles - 64; ++i) {
      if (cur->files[1][i].fd == args[1]) {
	lock_acquire(&fs_lock);
        f->eax = file_length(cur->files[1][i].f);
	lock_release(&fs_lock);
	return;
      }
    }
    break;
  case SYS_READ:
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);
    get_user_arg(args, f->esp, 3);

    if ((args[2] == 0) || (args[2] >= PHYS_BASE))
      thread_exit();

    // Verify the buffer
    check_write_array(args[2], args[3]);

    if (args[1] == 0) { // stdin
      for(i = 0; i < args[3]; ++i) {
	((uint8_t *)args[2])[i] = input_getc();
	// how would you detect EOF though? hmm.
      }
      f->eax = i;
      return;
    }
    else {
      // Find the file
      for (i = 0; i < cur->nfiles && i < 64; ++i) {
	if (cur->files[0][i].fd == args[1]) {
	  lock_acquire(&fs_lock);
	  f->eax = file_read(cur->files[0][i].f, (void *)args[2], args[3]);
	  lock_release(&fs_lock);
	  return;
	}
      }
      
      for (i = 0; i < cur->nfiles - 64; ++i) {
	if (cur->files[1][i].fd == args[1]) {
	  lock_acquire(&fs_lock);
	  f->eax = file_read(cur->files[1][i].f, (void *)args[2], args[3]);
	  lock_release(&fs_lock);
	  return;
	}
      }
    }
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
    else {
      // Check whether the buffer is valid
      check_array(args[2], args[3]);
      
      // Find the file to write to
      for (i = 0; i < cur->nfiles && i < 64; ++i) {
	if (cur->files[0][i].fd == args[1]) {
	  // Write to this file
	  lock_acquire(&fs_lock);
	  f->eax = file_write(cur->files[0][i].f, args[2], args[3]);
	  lock_release(&fs_lock);
	  return;
	}
      }
      for (i = 0; i < cur->nfiles - 64; ++i) {
	if (cur->files[1][i].fd == args[1]) {
	  lock_acquire(&fs_lock);
	  f->eax = file_write(cur->files[1][i].f, args[2], args[3]);
	  lock_release(&fs_lock);
	  return;
	}
      }
      // Invalid fd?
      f->eax = 0;
      return;
    }
    break;

  case SYS_SEEK:
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);
    // Find the appropriate file
        for (i = 0; i < cur->nfiles && i < 64; ++i) {
      if (cur->files[0][i].fd == args[1]) {
	lock_acquire(&fs_lock);
        file_seek(cur->files[0][i].f, args[2]);
	lock_release(&fs_lock);
	return;
      }
    }

    for (i = 0; i < cur->nfiles - 64; ++i) {
      if (cur->files[1][i].fd == args[1]) {
	lock_acquire(&fs_lock);
        file_seek(cur->files[1][i].f, args[2]);
	lock_release(&fs_lock);
	return;
      }
    }
    break;
  case SYS_TELL:
    get_user_arg(args, f->esp, 1);
    // Find the appropriate file
        for (i = 0; i < cur->nfiles && i < 64; ++i) {
      if (cur->files[0][i].fd == args[1]) {
	lock_acquire(&fs_lock);
        f->eax = file_tell(cur->files[0][i].f);
	lock_release(&fs_lock);
	return;
      }
    }

    for (i = 0; i < cur->nfiles - 64; ++i) {
      if (cur->files[1][i].fd == args[1]) {
	lock_acquire(&fs_lock);
        f->eax = file_tell(cur->files[1][i].f);
	lock_release(&fs_lock);
	return;
      }
    }
    // Kill the thread :)
    thread_exit();
    break;
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
	  }
	}
	else {
	  cur->files[0][i].fd = cur->files[1][cur->nfiles-64].fd;
	  cur->files[0][i].f = cur->files[1][cur->nfiles-64].f;
	  if (cur->nfiles == 64) {
	    // deallocate files[1]
	    palloc_free_page(cur->files[1]);
	  }
	}
	return;
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
	}
	else {
	  cur->files[1][i].fd = cur->files[1][cur->nfiles-64].fd;
	  cur->files[1][i].f = cur->files[1][cur->nfiles-64].f;
	}
	return;
      }
    }

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

