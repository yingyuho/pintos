#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed_point.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/*! Random value for struct thread's `magic' member.
    Used to detect stack overflow.  See the big comment at the top
    of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

// from file.c
struct file {
    struct inode *inode;        /*!< File's inode. */
    off_t pos;                  /*!< Current position. */
    bool deny_write;            /*!< Has file_deny_write() been called? */
};


/*! List of processes in THREAD_READY state, that is, processes
    that are ready to run but not actually running. */
static struct list ready_list;

/*! List of all processes.  Processes are added to this list
    when they are first scheduled and removed when they exit. */
static struct list all_list;

/*! Idle thread. */
static struct thread *idle_thread;

/*! Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;
//static struct thread_ashes initial_thread_ashes;

/*! Lock used by allocate_tid(). */
static struct lock tid_lock;

/*! Stack frame for kernel_thread(). */
struct kernel_thread_frame {
    void *eip;                  /*!< Return address. */
    thread_func *function;      /*!< Function to call. */
    void *aux;                  /*!< Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;    /*!< # of timer ticks spent idle. */
static long long kernel_ticks;  /*!< # of timer ticks in kernel threads. */
static long long user_ticks;    /*!< # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /*!< # of timer ticks to give each thread. */
static unsigned thread_ticks;   /*!< # of timer ticks since last yield. */

/* Multilevel feedback queue scheduling */
static fp_t load_avg;

#define LOAD_AVG_OLD fp_div_fp(59, 60)
#define LOAD_AVG_NEW fp_div_fp( 1, 60)

static void set_priority(int);
static void tick_mlfqs(void);
static int auto_priority(struct thread *t);

/*! If false (default), use round-robin scheduler.
    If true, use multi-level feedback queue scheduler.
    Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *running_thread(void);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static bool is_thread(struct thread *) UNUSED;
static void *alloc_frame(struct thread *, size_t size);
static void schedule(void);
void thread_schedule_tail(struct thread *prev);
static tid_t allocate_tid(void);

/*! Initializes the threading system by transforming the code
    that's currently running into a thread.  This can't work in
    general and it is possible in this case only because loader.S
    was careful to put the bottom of the stack at a page boundary.

    Also initializes the run queue and the tid lock.

    After calling this function, be sure to initialize the page allocator
    before trying to create any threads with thread_create().

    It is not safe to call thread_current() until this function finishes. */
void thread_init(void) {
    //struct thread_ashes *a;
    ASSERT(intr_get_level() == INTR_OFF);

    lock_init(&tid_lock);
    list_init(&ready_list);
    list_init(&all_list);

    /* Set up a thread structure for the running thread. */
    initial_thread = running_thread();
    init_thread(initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid();
    initial_thread->exit_status = -1;

    /* Init ashes */
#if 0
    a = initial_thread->ashes = &initial_thread_ashes;
    a->tid = initial_thread->tid;
    a->exit_status = -1;
    a->thread = initial_thread;
    sema_init(&a->sema, 0);
#endif
    list_init(&initial_thread->children);

    /* Initialize MLFQS variable(s) */
    //load_avg = fp_from_int(0);
}

/*! Starts preemptive thread scheduling by enabling interrupts.
    Also creates the idle thread. */
void thread_start(void) {
    /* Create the idle thread. */
    struct semaphore idle_started;
    sema_init(&idle_started, 0);
    thread_create("idle", PRI_MIN, idle, &idle_started);
    
    /* Set up locks for the initial thread */
    if (!thread_mlfqs) {
      initial_thread->locks = malloc(sizeof(struct list));
      list_init(initial_thread->locks);
    }
    else {
      load_avg = fp_from_int(0);
    }

    /* Start preemptive thread scheduling. */
    intr_enable();

    /* Wait for the idle thread to initialize idle_thread. */
    sema_down(&idle_started);
}

int auto_priority(struct thread *t) {
  int p = -fp_round(fp_add_int(fp_div_int(t->recent_cpu, 4), t->nice * 2 - PRI_MAX));
    if (p < PRI_MIN)
	p = PRI_MIN;
    if (p > PRI_MAX)
        p = PRI_MAX;

    return p;
}

/*! Called by the timer interrupt handler at each timer tick.
    Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
    struct thread *t = thread_current();

    /* Update statistics. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    if (thread_mlfqs)
        tick_mlfqs();

    /* Enforce preemption. */
    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return();
}

void tick_mlfqs(void) {
    struct thread *t0 = thread_current();
    struct thread *t1;
    struct list_elem *e;

    int32_t ready_running_threads;
    // damping factor for recent_cpu per second
    fp_t cpu_damp;

    /* Do once per 4 ticks */
    /* Update priorities */
    if (timer_ticks() % 4 == 0) {
        t0->priority = auto_priority(t0);

        if (!list_empty(&ready_list)) {
            for (e = list_front(&ready_list); 
                 e != list_tail(&ready_list); 
                 e = list_next(e)) {
                t1 = list_entry(e, struct thread, elem);
                t1->priority = auto_priority(t1);
                //reinsert(t1);
		/* Sorting the entire list is faster (O(n log n) instead
		   of O(n^2) */
            }
	    list_sort(&ready_list, pri_less_func, 0);
            t1 = list_entry(list_front(&ready_list), struct thread, elem);

            if (t0->priority < t1->priority)
                intr_yield_on_return();
        }
	
    }

    /* recent CPU time += 1 for the running thread per tick */
    if (t0 != idle_thread)
        t0->recent_cpu = fp_add_int(t0->recent_cpu, 1);

    /* Do once per second */
    if (timer_ticks() % TIMER_FREQ == 0) {
        /* Count threads that are running or ready to run */
        ready_running_threads = (t0 != idle_thread) + list_size(&ready_list);

        cpu_damp = fp_div_fp(2 * load_avg, fp_add_int(2 * load_avg, 1));

        /* Damp recent CPU time for running and ready threads */

        t0->recent_cpu = fp_add_int(fp_mul_fp(cpu_damp, t0->recent_cpu), t0->nice);

        if (!list_empty(&ready_list)) {
            for (e = list_front(&ready_list); 
                 e != list_tail(&ready_list); 
                 e = list_next(e)) {
                t1 = list_entry(e, struct thread, elem);
                t1->recent_cpu = fp_add_int(fp_mul_fp(cpu_damp, t1->recent_cpu), t1->nice);
            }
        }

        /* Update current system load average */
        load_avg = fp_add_fp(fp_mul_fp(LOAD_AVG_OLD, load_avg), 
                            fp_mul_int(LOAD_AVG_NEW, ready_running_threads));
    }


}

/*! Prints thread statistics. */
void thread_print_stats(void) {
    printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
           idle_ticks, kernel_ticks, user_ticks);
}

/*! Creates a new kernel thread named NAME with the given initial PRIORITY,
    which executes FUNCTION passing AUX as the argument, and adds it to the
    ready queue.  Returns the thread identifier for the new thread, or
    TID_ERROR if creation fails.

    If thread_start() has been called, then the new thread may be scheduled
    before thread_create() returns.  It could even exit before thread_create()
    returns.  Contrariwise, the original thread may run for any amount of time
    before the new thread is scheduled.  Use a semaphore or some other form of
    synchronization if you need to ensure ordering.

    The code provided sets the new thread's `priority' member to PRIORITY, but
    no actual priority scheduling is implemented.  Priority scheduling is the
    goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority, thread_func *function,
                    void *aux) {
    struct thread *t;
    struct thread_ashes *a;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;

    ASSERT(function != NULL);

    /* Allocate thread. */
    t = palloc_get_page(PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* Initialize thread. */
    t->recent_cpu = fp_from_int(0);
    t->nice = 0;
    if (thread_mlfqs)
        init_thread(t, name, auto_priority(t));
    else {
        init_thread(t, name, priority);
	t->locks = malloc(sizeof(struct list));
	list_init(t->locks);
    }
    tid = t->tid = allocate_tid();

    t->exit_status = -1;

    /* Init ashes */
    a = t->ashes = malloc(sizeof(struct thread_ashes));
//printf("a=%lu\n", a);
    a->tid = tid;
    a->has_been_waited = false;
    a->load_success = false;
    a->exit_status = -1;
    a->thread = t;

    sema_init(&a->sema, 0);
//printf("c=%lu\n", &thread_current()->children);
    list_init(&t->children);
    list_push_back(&thread_current()->children, &a->elem);

    sema_init(&t->load_done, 0);

    /* Stack frame for kernel_thread(). */
    kf = alloc_frame(t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    /* Stack frame for switch_entry(). */
    ef = alloc_frame(t, sizeof *ef);
    ef->eip = (void (*) (void)) kernel_thread;

    /* Stack frame for switch_threads(). */
    sf = alloc_frame(t, sizeof *sf);
    sf->eip = switch_entry;
    sf->ebp = 0;

    /* Add to run queue. */
    thread_unblock(t);
    
    // Yield if the new thread has higher priority
    if (thread_current()->priority < priority)
      thread_yield();

    return tid;
}

/*! Puts the current thread to sleep.  It will not be scheduled
    again until awoken by thread_unblock().

    This function must be called with interrupts turned off.  It is usually a
    better idea to use one of the synchronization primitives in synch.h. */
void thread_block(void) {
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);

    thread_current()->status = THREAD_BLOCKED;
    schedule();
}

/*! Transitions a blocked thread T to the ready-to-run state.  This is an
    error if T is not blocked.  (Use thread_yield() to make the running
    thread ready.)

    This function does not preempt the running thread.  This can be important:
    if the caller had disabled interrupts itself, it may expect that it can
    atomically unblock a thread and update other data. */
void thread_unblock(struct thread *t) {
    enum intr_level old_level;

    ASSERT(is_thread(t));

    old_level = intr_disable();
    ASSERT(t->status == THREAD_BLOCKED);
    list_insert_ordered(&ready_list, &t->elem, pri_less_func, 0);
    t->status = THREAD_READY;
    intr_set_level(old_level);
}

/* Yields if the current thread has lower priority than the first thread in
   the ready queue and not in interrupt context */
void maybe_yield() {
  if ((! intr_context()) && (! list_empty(&ready_list)))
    if (thread_current()->priority < list_entry(list_begin(&ready_list),
				struct thread, elem)-> priority)
      thread_yield();
}

// Reschedule a thread (due to changed priority)
// Should only be called with interrupts disabled
// (usually a good idea while working
// with lists, especially static or global ones)
void reinsert(struct thread *t) {
  list_remove(&t->elem);
  list_insert_ordered(&ready_list, &t->elem, pri_less_func, 0);
}

/*! Returns the name of the running thread. */
const char * thread_name(void) {
    return thread_current()->name;
}

/*! Returns the running thread.
    This is running_thread() plus a couple of sanity checks.
    See the big comment at the top of thread.h for details. */
struct thread * thread_current(void) {
    struct thread *t = running_thread();

    /* Make sure T is really a thread.
       If either of these assertions fire, then your thread may
       have overflowed its stack.  Each thread has less than 4 kB
       of stack, so a few big automatic arrays or moderate
       recursion can cause stack overflow. */
    ASSERT(is_thread(t));
    ASSERT(t->status == THREAD_RUNNING);

    return t;
}

/*! Returns the running thread's tid. */
tid_t thread_tid(void) {
    return thread_current()->tid;
}

extern struct lock fs_lock;

/*! Deschedules the current thread and destroys it.  Never
    returns to the caller. */
void thread_exit(void) {
    struct list_elem *e;
    struct thread_ashes *a;
    struct thread *cur = thread_current();
    int i;
    ASSERT(!intr_context());

#ifdef USERPROG
    // Clean up all open file descriptors, including its own
    for (i = 0; i < cur->nfiles && i < 64; ++i) {
      lock_acquire(&fs_lock);
      file_close(cur->files[0][i].f);
      lock_release(&fs_lock);
    }
    if (cur->nfiles)
      palloc_free_page(cur->files[0]);

    for (i = 0; i < cur->nfiles - 64; ++i) {
      lock_acquire(&fs_lock);
      file_close(cur->files[1][i].f);
      lock_release(&fs_lock);
    }
    if (cur->nfiles > 64)
      palloc_free_page(cur->files[1]);

    file_close(cur->exec);
    process_exit();
#endif

    /* Up ashes' semaphore */
    if (cur->ashes)
        sema_up(&cur->ashes->sema);

    /* Remove thread from all threads list, set our status to dying,
       and schedule another process.  That process will destroy us
       when it calls thread_schedule_tail(). */
    intr_disable();
    list_remove(&cur->allelem);
    cur->status = THREAD_DYING;
    schedule();
    NOT_REACHED();
}

/*! Yields the CPU.  The current thread is not put to sleep and
    may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
    struct thread *cur = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();
    if (cur != idle_thread) {
      list_insert_ordered(&ready_list, &cur->elem, pri_less_func, 0);
    }
    cur->status = THREAD_READY;
    schedule();
    intr_set_level(old_level);
}

/*! Invoke function 'func' on all threads, passing along 'aux'.
    This function must be called with interrupts off. */
void thread_foreach(thread_action_func *func, void *aux) {
    struct list_elem *e;

    ASSERT(intr_get_level() == INTR_OFF);

    for (e = list_begin(&all_list); e != list_end(&all_list);
         e = list_next(e)) {
        struct thread *t = list_entry(e, struct thread, allelem);
        func(t, aux);
    }
}

/* Helper function to get donated priority from held locks */
void get_donated_priority(struct thread *t) {
  struct list_elem *e;
  for ( e = list_begin(t->locks); 
	    e != list_end(t->locks); e = list_next(e)) {
	struct lock *l = list_entry(e, struct lock, elem);
	// Check the "priority" of the lock, if any
	struct list *z = &l->semaphore.waiters;
	if (! list_empty(z)) {	  
	  int i = intr_disable();
	  int p = list_entry(list_begin(z), struct thread, elem)->cur_pri;
	  // We do need to avoid a race condition here though (another thread
	  // can increase our cur_pri, and then we'll set it to the wrong
	  // value).
	  if (p > t->cur_pri)
	    t->cur_pri = p;
	  intr_set_level(i);
	}
      }
}

/*! Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) {
  // Do nothing in MLFQS (multilevel feedback queue scheduler) mode
  if (!thread_mlfqs)
    set_priority(new_priority);
}

void set_priority(int new_priority) {
  int old_priority = thread_current()->priority;

  thread_current()->priority = new_priority;
  if (old_priority < new_priority) {
    if (thread_current()->cur_pri < thread_current()->priority)
      thread_current()->cur_pri = thread_current()->priority;
  }
  // If priority decreased, check whether we still have the highest priority
  if (old_priority > new_priority) {
    // TODO: actually recalculate cur_pri if necessary
    // This is easy if cur_pri was larger than old_priority (it stays the same)
    // and otherwise we have to iterate through held locks to check their
    // donations.
    
    // Of course, we don't have to do that iteration atomically, because the
    // list can't change and lock priority can only increase
    // (It's not really a race condition; no matter when that increase happens
    // we get the same result in the end)
    if (thread_current()->cur_pri == old_priority) {
      thread_current()->cur_pri = new_priority;
      // Look through locks to find the largest element
      struct list_elem *e;
      for ( e = list_begin(thread_current()->locks); 
	    e != list_end(thread_current()->locks); e = list_next(e)) {
	struct lock *l = list_entry(e, struct lock, elem);
	// Check the "priority" of the lock, if any
	struct list *z = &l->semaphore.waiters;
	if (! list_empty(z)) {
	  int i = intr_disable();
	  int p = list_entry(list_begin(z), struct thread, elem)->cur_pri;
	  // We do need to avoid a race condition here though (another thread
	  // can increase our cur_pri; p being increased doesn't matter,
	  // because that will also update our cur_pri if needed.
	  if (p > thread_current()->cur_pri)
	    thread_current()->cur_pri = p;
	  intr_set_level(i);
	}
      }
    }
    maybe_yield();
  }
}

int get_thread_priority(struct thread *t) {
  if (thread_mlfqs)
    return t->priority;
  return t->cur_pri;
}

/*! Returns the current thread's priority. */
int thread_get_priority(void) {
  if (thread_mlfqs)
    return thread_current()->priority;
  return thread_current()->cur_pri;
}

/*! Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice) {
    struct thread *t = thread_current();
    if (nice > NICE_MAX)
        nice = NICE_MAX;
    if (nice < NICE_MIN)
        nice = NICE_MIN;
    t->nice = nice;
    t->priority = auto_priority(t);
    maybe_yield();
}

/*! Returns the current thread's nice value. */
int thread_get_nice(void) {
    return thread_current()->nice;
}

/*! Returns 100 times the system load average. */
int thread_get_load_avg(void) {
    return fp_round(fp_mul_int(load_avg, 100));
}

/*! Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
    return fp_round(fp_mul_int(thread_current()->recent_cpu, 100));
}

/*! Idle thread.  Executes when no other thread is ready to run.

    The idle thread is initially put on the ready list by thread_start().
    It will be scheduled once initially, at which point it initializes
    idle_thread, "up"s the semaphore passed to it to enable thread_start()
    to continue, and immediately blocks.  After that, the idle thread never
    appears in the ready list.  It is returned by next_thread_to_run() as a
    special case when the ready list is empty. */
static void idle(void *idle_started_ UNUSED) {
    struct semaphore *idle_started = idle_started_;
    idle_thread = thread_current();
    sema_up(idle_started);

    for (;;) {
        /* Let someone else run. */
        intr_disable();
        thread_block();

        /* Re-enable interrupts and wait for the next one.

           The `sti' instruction disables interrupts until the completion of
           the next instruction, so these two instructions are executed
           atomically.  This atomicity is important; otherwise, an interrupt
           could be handled between re-enabling interrupts and waiting for the
           next one to occur, wasting as much as one clock tick worth of time.

           See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
           7.11.1 "HLT Instruction". */
        asm volatile ("sti; hlt" : : : "memory");
    }
}

/*! Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func *function, void *aux) {
    ASSERT(function != NULL);

    intr_enable();       /* The scheduler runs with interrupts off. */
    function(aux);       /* Execute the thread function. */
    thread_exit();       /* If function() returns, kill the thread. */
}

/*! Returns the running thread. */
struct thread * running_thread(void) {
    uint32_t *esp;

    /* Copy the CPU's stack pointer into `esp', and then round that
       down to the start of a page.  Because `struct thread' is
       always at the beginning of a page and the stack pointer is
       somewhere in the middle, this locates the curent thread. */
    asm ("mov %%esp, %0" : "=g" (esp));
    return pg_round_down(esp);
}

/*! Returns true if T appears to point to a valid thread. */
static bool is_thread(struct thread *t) {
    return t != NULL && t->magic == THREAD_MAGIC;
}

/*! Does basic initialization of T as a blocked thread named NAME. */
static void init_thread(struct thread *t, const char *name, int priority) {
    enum intr_level old_level;

    ASSERT(t != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT(name != NULL);

    memset(t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;

    strlcpy(t->name, name, sizeof t->name);
    if (strchr(t->name, ' ') != NULL)
        *strchr(t->name, ' ') = '\0';

    t->stack = (uint8_t *) t + PGSIZE;
    t->priority = priority;
    t->cur_pri = priority;
    t->magic = THREAD_MAGIC;
    old_level = intr_disable();
    list_push_back(&all_list, &t->allelem);
    intr_set_level(old_level);
}

/*! Allocates a SIZE-byte frame at the top of thread T's stack and
    returns a pointer to the frame's base. */
static void * alloc_frame(struct thread *t, size_t size) {
    /* Stack data is always allocated in word-size units. */
    ASSERT(is_thread(t));
    ASSERT(size % sizeof(uint32_t) == 0);

    t->stack -= size;
    return t->stack;
}

/*! Chooses and returns the next thread to be scheduled.  Should return a
    thread from the run queue, unless the run queue is empty.  (If the running
    thread can continue running, then it will be in the run queue.)  If the
    run queue is empty, return idle_thread. */
static struct thread * next_thread_to_run(void) {
    if (list_empty(&ready_list))
      return idle_thread;
    else
      return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/*! Completes a thread switch by activating the new thread's page tables, and,
    if the previous thread is dying, destroying it.

    At this function's invocation, we just switched from thread PREV, the new
    thread is already running, and interrupts are still disabled.  This
    function is normally invoked by thread_schedule() as its final action
    before returning, but the first time a thread is scheduled it is called by
    switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is complete.  In
   practice that means that printf()s should be added at the end of the
   function.

   After this function and its caller returns, the thread switch is complete. */
void thread_schedule_tail(struct thread *prev) {
    struct thread *cur = running_thread();
    struct list_elem *e;
  
    ASSERT(intr_get_level() == INTR_OFF);

    /* Mark us as running. */
    cur->status = THREAD_RUNNING;

    /* Start new time slice. */
    thread_ticks = 0;

#ifdef USERPROG
    /* Activate the new address space. */
    process_activate();
#endif

    /* If the thread we switched from is dying, destroy its struct thread.
       This must happen late so that thread_exit() doesn't pull out the rug
       under itself.  (We don't free initial_thread because its memory was
       not obtained via palloc().) */
    if (prev != NULL && prev->status == THREAD_DYING &&
        prev != initial_thread) {
        ASSERT(prev != cur);
	if (!thread_mlfqs)
	  free(prev->locks);
#if 0
    if (!list_empty(&prev->children) && 
        prev != initial_thread)
    {
        for (e = list_front(&prev->children); 
             e != list_tail(&prev->children); 
             e = list_next(e))
        {
            printf("parent %s\n", prev->name);
            free(list_entry(e, struct thread_ashes, elem));
        }
    }
#endif
        palloc_free_page(prev);
    }
}

/*! Schedules a new process.  At entry, interrupts must be off and the running
    process's state must have been changed from running to some other state.
    This function finds another thread to run and switches to it.

    It's not safe to call printf() until thread_schedule_tail() has
    completed. */
static void schedule(void) {
    struct thread *cur = running_thread();
    struct thread *next = next_thread_to_run();
    struct thread *prev = NULL;

    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(cur->status != THREAD_RUNNING);
    ASSERT(is_thread(next));

    if (cur != next)
        prev = switch_threads(cur, next);
    thread_schedule_tail(prev);
}

/*! Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire(&tid_lock);
    tid = next_tid++;
    lock_release(&tid_lock);

    return tid;
}

struct lock_elem {
  struct lock *l;
  struct list_elem elem;
};



/*! Offset of `stack' member within `struct thread'.
    Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

