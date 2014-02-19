#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

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

static void syscall_handler(struct intr_frame *f) {
  // Take a look at the system call number; this is the first
  // thing on the caller's stack. While we're here, might as
  // well get the others.
  int num = *((int *)f->esp);
  int arg1 = ((int *)f->esp)[1];
  int arg2 = ((int *)f->esp)[2];
  unsigned arg3 = ((unsigned *)f->esp)[3];
  // printf("%d %d %d %d\n", num, arg1, arg2, arg3);
  // Technically these are an enum, but C implements enums as ints...
  switch(num) {
  case SYS_HALT:
    shutdown_power_off();
    break;
  case SYS_EXIT:
    // Grab the argument off the stack and set the return value
    f->eax = arg1;
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
    break;
  case SYS_WRITE:
    // Using the second solution; we need to check that it's really valid to
    // read the buffer
    if ((arg2 < PHYS_BASE) && (get_user((uint8_t *) arg2) != -1)) {
      if (arg1 == 1) { // stdout
	putbuf((char *)arg2, arg3);
      }
      else { // TODO: implement
      }
    }
    else {
      // Well, killing the process seems like a good idea
      thread_exit();
    }
    //printf("%d %d %d %d\n", num, arg1, arg2, arg3);
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
  //  printf("system call!\n");
  //  thread_exit();
}

