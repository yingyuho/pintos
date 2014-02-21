#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "devices/shutdown.h"

int process_wait(tid_t);
void shutdown_power_off(void) NO_RETURN;

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

static int wait_load(tid_t tid) {
  struct thread *cur = thread_current();
  struct list_elem *e;
  struct thread_ashes *a = NULL;

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
  return a->thread->load_success ? tid : -1;
}

static void syscall_handler(struct intr_frame *f) {
  // Take a look at the system call number; this is the first
  // thing on the caller's stack. While we're here, might as
  // well get the others.
  int32_t args[4];
  //char *buf;
  //int len;

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
    thread_current()->ashes->exit_status = args[1];
    thread_exit();
    break;

  case SYS_EXEC:
    get_user_arg(args, f->esp, 1);
    if (get_user((uint8_t*)args[1]) < 0) { thread_exit(); }
    //len = strlen((const char*)args[1]) + 1;
    //buf = malloc(len);
    //strlcpy(buf, (const char*)args[1], len);
    f->eax = wait_load(process_execute((const char*)args[1]));
    //free(buf);
    break;
  case SYS_WAIT:
    get_user_arg(args, f->esp, 1);
    f->eax = process_wait(args[1]);
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
    get_user_arg(args, f->esp, 1);
    get_user_arg(args, f->esp, 2);
    get_user_arg(args, f->esp, 3);

    if (args[1] == 1) { // stdout
      putbuf((char *)args[2], (uint32_t)args[3]);
    }
    else { // TODO: implement
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

  //  printf("system call!\n");
  //  thread_exit();
}

