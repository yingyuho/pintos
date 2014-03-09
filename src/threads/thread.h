/*! \file thread.h
 *
 * Declarations for the kernel threading functionality in PintOS.
 */

#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"

#include "filesys/file.h"

#ifdef VM
#include "vm/page.h"
#endif

struct thread;

#ifdef USERPROG
struct thread_ashes;
#endif

/*! States in a thread's life cycle. */
enum thread_status {
    THREAD_RUNNING,     /*!< Running thread. */
    THREAD_READY,       /*!< Not running but ready to run. */
    THREAD_BLOCKED,     /*!< Waiting for an event to trigger. */
    THREAD_DYING        /*!< About to be destroyed. */
};

/*! Thread identifier type.
    You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /*!< Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /*!< Lowest priority. */
#define PRI_DEFAULT 31                  /*!< Default priority. */
#define PRI_MAX 63                      /*!< Highest priority. */

/* Thread nicenesses. */
#define NICE_MIN -20                    /*!< Lowest niceness. */
#define NICE_DEFAULT 0                  /*!< Default niceness. */
#define NICE_MAX 20                     /*!< Highest niceness. */

#define T_NAME_MAX 16

/*! A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

\verbatim
        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+
\endverbatim

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion.

   The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list.
*/
struct thread {
    /*! Owned by thread.c. */
    /**@{*/
    tid_t tid;                          /*!< Thread identifier. */
    enum thread_status status;          /*!< Thread state. */
    int32_t exit_status;
    char name[T_NAME_MAX];                      /*!< Name (for debugging purposes). */
    uint8_t *stack;                     /*!< Saved stack pointer. */
    int priority;                       /*!< Priority. */
    int nice;                       /*!< Niceness. */
    int cur_pri; /* Current priority; at least as large as priority */
    int recent_cpu; /* Amount of CPU time used recently */
    struct list_elem allelem;           /*!< List element for all threads list. */

    struct list *locks; /* Currently held locks */
    struct lock *bllock; /* Lock currently blocked on, if any */
    struct semaphore *blsema; /* Ditto, but semaphore. The reason we still need
                                 the lock is to implement nesting */
    /**@}*/

    /*! Shared between thread.c and synch.c. */
    /**@{*/
    struct list_elem elem;              /*!< List element. */
    /**@}*/

#ifdef USERPROG
    /*! Owned by userprog/process.c. */
    /**@{*/
#ifdef VM
    void *esp;                          /* Holds ESP for PF handler */
    struct mm_struct mm;                /* Memory descriptor */
    #define PAGEDIR mm.pagedir
#else
    uint32_t *pagedir;                  /*!< Page directory. */
    #define PAGEDIR pagedir
#endif
    /**@{*/

    struct file_node *files[2]; // lists of open files
    // The reason for having two of them is so we can avoid allocating pages
    // until actually necessary
    int nfiles; // number of open files

    struct file *exec; // executable

    struct thread_ashes *ashes;
    struct list children; /* List of ashes */
    struct semaphore load_done;
    //bool load_success;
#endif /* USERPROG */

    /*! Owned by thread.c. */
    /**@{*/
    unsigned magic;                     /* Detects stack overflow. */
    /**@}*/
};

/*! If false (default), use round-robin scheduler.
    If true, use multi-level feedback queue scheduler.
    Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

void maybe_yield(void);
void reinsert(struct thread*);
void get_donated_priority(struct thread *);
int get_thread_priority(struct thread *);

struct thread *thread_current (void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

/*! Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(struct thread *t, void *aux);

void thread_foreach(thread_action_func *, void *);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

struct file_node {
  int fd; // file descriptor for this node
  struct file *f; //file
};

#ifdef USERPROG
/* thread_ashes */

struct thread_ashes {
    tid_t tid;
    bool has_been_waited;
    bool load_success;
    int32_t exit_status;
    struct semaphore sema;
    struct thread *thread;
    struct list_elem elem;
};
#endif

#endif /* threads/thread.h */

