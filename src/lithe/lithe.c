/*
 * Lithe implementation.
 *
 * Notes:
 *
 * Trampoline Thread/Stack:
 *
 * The trampoline thread is created in vcore context and thus shares
 * the same tls as the vcore it is executed on.  To jump to the trampoline
 * simply call run_uthread() passing the trampoline as the parameter.
 *
 *
 * Parents, children, and grandchildren:
 *
 * There is an unfortunate race with unregistering and yielding that
 * has (for now) forced us into a particular
 * implementation. Specifically, when a child scheduler unregisters
 * there might be a parent scheduler that is simultaneously trying to
 * enter. Really there are two options, (1) when a hard thread tries
 * to enter a child scheduler it can first lock the parent and look
 * through all of its schedulers for the child, or (2) a parent
 * scheduler can always enter a child scheduler provided that its
 * parent field is set correctly AND the child is still registered.
 * The advantage of (1) is that we can free child schedulers when they
 * unregister (if a parent is invoking enter then the child will no
 * longer be a member of the parents children and so the pointer will
 * just be invalid). The disadvantage of (1), however, is that every
 * enter requires acquiring the spinlock and looking through the
 * schedulers! When doing experiements we found this to be very
 * costly. The advantage of (2) is that we don't need to look through
 * the children when we enter. The disadvantage of (2), however, is
 * that we need to keep every child scheduler around so that we can
 * check whether or not that child is still registered! It has been
 * reported that this creates a lot of schedulers that never get
 * cleaned up. The practical impacts of this are still unknown
 * however. A hybrid solution is probably the best here. Something
 * that looks through the children without acquiring the lock (since
 * most of the time the scheduler will be found in the children), and
 * frees a child when it unregisters.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <asm/unistd.h>

#include <ht/atomic.h>
#include <spinlock.h>

#include <sys/mman.h>
#include <sys/resource.h>

#include <lithe/lithe.h>
#include <lithe/fatal.h>

#ifndef __linux__
#error "expecting __linux__ to be defined (for now, Lithe only runs on Linux)"
#endif

/* Struct to keep track of each of the 2nd-level schedulers managed by lithe */
struct lithe_sched {
  /* Lock for managing scheduler. */
  int lock;

  /* State of scheduler. */
  enum { 
    REGISTERED,
    UNREGISTERING,
    UNREGISTERED 
  } state;

  /* Number of vcores currently owned by this scheduler. */
  int vcores;

  /* Scheduler's parent scheduler. */
  lithe_sched_t *parent;

  /* Scheduler's parent task. */
  lithe_task_t *parent_task;

  /* Scheduler's children schedulers (necessary to insure calling enter is
   * valid). */
  lithe_sched_t *children;

  /* Linked list of sibling's (list head is parent->children). */ 
  lithe_sched_t *next, *prev;

  /* Scheduler's functions. */
  const lithe_sched_funcs_t *funcs;

  /* Scheduler's 'this' pointer. */
  void *this;
};

/* Hooks from uthread code into lithe */
static uthread_t*   lithe_init(void);
static void         lithe_vcore_entry(void);
static uthread_t*   lithe_thread_create(void (*func)(void), void *udata);
static void         lithe_thread_runnable(uthread_t *uthread);
static void         lithe_thread_yield(uthread_t *uthread);
static void         lithe_thread_exit(uthread_t *uthread);
static unsigned int lithe_vcores_wanted(void);

/* Unique function pointer table required by uthread code */
struct schedule_ops lithe_sched_ops = {
  .sched_init       = lithe_init,
  .sched_entry      = lithe_vcore_entry,
  .thread_create    = lithe_thread_create,
  .thread_runnable  = lithe_thread_runnable,
  .thread_yield     = lithe_thread_yield,
  .thread_exit      = lithe_thread_exit,
  .vcores_wanted    = lithe_vcores_wanted,
  .preempt_pending  = NULL, /* lithe_preempt_pending, */
  .spawn_thread     = NULL, /* lithe_spawn_thread, */
};
/* Publish these schedule_ops, overriding the weak defaults setup in uthread */
struct schedule_ops *sched_ops __attribute__((weak)) = &lithe_sched_ops;

/* Lithe's base scheduler functions */
static void base_enter(void *this);
static void base_yield(void *this, lithe_sched_t *sched);
static void base_reg(void *this, lithe_sched_t *sched);
static void base_unreg(void *this, lithe_sched_t *sched);
static int base_request(void *this, lithe_sched_t *sched, int k);
static void base_unblock(void *this, lithe_task_t *task);

static const lithe_sched_funcs_t base_funcs = {
  .enter   = base_enter,
  .yield   = base_yield,
  .reg     = base_reg,
  .unreg   = base_unreg,
  .request = base_request,
  .unblock = base_unblock,
};

/* Base scheduler itself */
static lithe_sched_t base = { 
  .lock         = UNLOCKED,
  .state        = REGISTERED,
  .vcores       = 0,
  .parent       = NULL,
  .parent_task  = NULL,
  .children     = NULL,
  .next         = NULL,
  .prev         = NULL,
  .funcs        = &base_funcs,
  .this         = (void *) 0xBADC0DE,
};

/* Reference to the task running the main thread.  Whenever we create a new
 * trampoline task, use this task as our parent, even if creating from vcore
 * context */
static lithe_task_t *main_task = NULL;

/* Root scheduler, i.e. the child scheduler of base. */
static lithe_sched_t *root = NULL;

static __thread struct {
  /* Task used when transitioning between schedulers. */
  lithe_task_t *trampoline;

  /* State variable indicating whether we should yield the vcore we are
   * currently executing when vcore_entry is called again */
  bool yield_vcore;

} lithe_tls = {NULL, false};
#define trampoline    (lithe_tls.trampoline)
#define yield_vcore   (lithe_tls.yield_vcore)
#define current_task  ((lithe_task_t*)current_uthread)
#define current_sched (current_task->sched)

void vcore_ready()
{
  /* Once the vcore subsystem is up and running, initialize the uthread
   * library, which will, in turn, eventually call lithe_init() for us */
  uthread_init();
}

static uthread_t* lithe_init()
{
  /* Create a lithe task for the main thread to run in */
  main_task = (lithe_task_t*)calloc(1, sizeof(lithe_task_t));
  assert(main_task);

  /* Set the scheduler associated with the task to be the base scheduler */
  main_task->sched = &base;

  /* Return a reference to the main task back to the uthread library. We will
   * resume this task once lithe_vcore_entry() is called from the uthread
   * library */
  return (uthread_t*)(main_task);
}

static void __lithe_sched_reenter()
{
  assert(current_task == trampoline);

  /* Enter current scheduler. */
  current_sched->funcs->enter(current_sched->this);
  fatal("lithe: returned from enter");
}

static void __attribute__((noreturn)) lithe_vcore_entry()
{
  /* Make sure we are in vcore context */
  assert(in_vcore_context());

  /* If we are supposed to vacate this vcore and give it back to the system, do
   * so, cleaning up any lithe specific state in the process. This occurs as a
   * result of base_yield() being called, and the trampoline task exiting
   * cleanly */
  if(yield_vcore) {
    __sync_fetch_and_add(&base.vcores, -1);
    memset(&lithe_tls, 0, sizeof(lithe_tls));
    vcore_yield();
  }

  /* Otherwise, we need a trampoline.  If we are entering this vcore for the
   * first time, we need to create one and set up both the vcore state and the
   * trampoline state appropriately */
  if(trampoline == NULL) {
	/* Create a new blank slate trampoline */
    trampoline = (lithe_task_t*)uthread_create(NULL, NULL);
    assert(trampoline);
    /* Make the trampoline self aware */
    uthread_set_tls_var(trampoline, trampoline, trampoline);
    /* Up our vcore count for the base scheduler */
    __sync_fetch_and_add(&base.vcores, 1);
  }

  /* If the thread local variable 'current_uthread' is set, then just resume
   * it. This will happen in one of 2 cases: 1) It is the first, i.e. main
   * thread, or 2) The current vcore was taken over to run a signal handler
   * from the kernel, and is now being restarted */
  if(current_uthread) {
    trampoline->sched = current_sched;
    uthread_set_tls_var(&current_task->uth, trampoline, trampoline);
    run_current_uthread();
  }

  /* Otherwise... */
  /* Set the current trampoline scheduler to be the base scheduler */
  trampoline->sched = &base;
  /* Set the entry function of the trampoline to enter the scheduler */
  init_uthread_entry(&trampoline->uth, __lithe_sched_reenter, 0);
  /* Jump to the trampoline */
  run_uthread(&trampoline->uth);
}

static int __allocate_lithe_task_stack(lithe_task_stack_t **stack)
{
  /* Set things up to create a 4 page stack */
  int pagesize = getpagesize();
  size_t size = pagesize * 4;

  /* Build the protection and flags for the mmap call to allocate the stack */
  const int protection = (PROT_READ | PROT_WRITE | PROT_EXEC);
  const int flags = (MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT);

  /* mmap the stack in */
  void *sp = mmap(NULL, size, protection, flags, -1, 0);
  if (sp == MAP_FAILED) 
    return -1;

  /* Disallow all memory access to the last page. */
  if (mprotect(sp, pagesize, PROT_NONE) != 0)
    return -1;

  /* Set the paramters properly before returning */
  (*stack)->sp = sp;
  (*stack)->size = size;
  return 0;
}

static void __free_lithe_task_stack(lithe_task_stack_t *stack)
{
  munmap(stack->sp, stack->size);
}

static uthread_t* lithe_thread_create(void (*func)(void), void *udata)
{
  /* Create a new lithe task */
  lithe_task_t *t = (lithe_task_t*)calloc(1, sizeof(lithe_task_t));
  assert(t);

  /* Get a reference to the lithe stack task if there is one */
  lithe_task_stack_t *stack = (lithe_task_stack_t*)udata;
  /* If not, create one.. */
  if(stack == NULL) {
	stack = &(t->stack);
    assert(__allocate_lithe_task_stack(&stack) == 0);
    t->dynamic_stack = true;
  }
  else t->stack = *stack;

  /* Initialize the newly created uthread with the stack */
  init_uthread_stack(&t->uth, stack->sp, stack->size); 
  /* Initialize the start function for the new thread if there is one */
  if(func != NULL)
    init_uthread_entry(&t->uth, func, 0);

  /* Return the new thread */
  return (struct uthread*)t;
}

static void lithe_thread_runnable(struct uthread *uthread)
{
}

static void lithe_thread_yield(struct uthread *uthread)
{
}

static void lithe_thread_exit(struct uthread *uthread)
{
  lithe_task_t *t = (lithe_task_t*)uthread;
  if(t->dynamic_stack)
    __free_lithe_task_stack(&t->stack);
}

static unsigned int lithe_vcores_wanted(void)
{
  return 0;
}

void base_enter(void *this)
{
  lithe_sched_t *sched = root;
  if(sched != NULL)
    lithe_sched_enter(sched);

  /* Leave base, yield back the vcore */
  base_yield(NULL, NULL);
}

void base_yield(void *this, lithe_sched_t *sched)
{
  /* Cleanup trampoline and yield the vcore. */
  vcore_set_tls_var(vcore_id(), yield_vcore, true);
  uthread_exit();
}

void base_reg(void *this, lithe_sched_t *sched)
{
  assert(root == NULL);
  root = sched;
}

void base_unreg(void *this, lithe_sched_t *sched)
{
  assert(root == sched);
  root = NULL;
}

int base_request(void *this, lithe_sched_t *sched, int k)
{
  assert(root == sched);
  return vcore_request(k);
}

void base_unblock(void *this, lithe_task_t *task)
{
  fatal("unimplemented");
}

void *lithe_sched_current()
{
  return current_sched->this;
}

int lithe_sched_enter(lithe_sched_t *child)
{
  assert(current_task == trampoline);

  lithe_sched_t *parent = current_sched;

  if (child == NULL)
    fatal("lithe: cannot enter NULL child");

  if (child->parent != parent)
    fatal("lithe: cannot enter child; child is not descendant of parent");

  /* TODO: Check if child is descendant of parent? */

  /*
   * Optimistically try and join child. We need to try and increment
   * the childrens number of threads before we check the childs state
   * because that is the order of writes that an unregistering child
   * will take.
   */
  if (child->state == REGISTERED) {
    /* Leave parent, join child. */
    assert(child != &base);
    current_sched = child;
    __sync_fetch_and_add(&(child->vcores), 1);

    if (child->state != REGISTERED) {
      /* Leave child, join parent. */
      __sync_fetch_and_add(&(child->vcores), -1);
      current_sched = parent;

      /* Signal unregistering hard thread. */
      if (child->state == UNREGISTERING) {
        if (child->vcores == 0) {
          child->state = UNREGISTERED;
        }
      }

      /* Stay in parent. */
      parent->funcs->yield(parent->this, child);
      fatal("lithe: returned from yield");
    }
  } else {
    /* Stay in parent. */
    parent->funcs->yield(parent->this, child);
    fatal("lithe: returned from yield");
  }

  /* Enter child. */
  __lithe_sched_reenter();
  fatal("lithe: returned from enter");
}

int lithe_sched_yield()
{
  assert(current_task == trampoline);

  lithe_sched_t *parent = current_sched->parent;
  lithe_sched_t *child = current_sched;

  /* Leave child, join parent. */
  __sync_fetch_and_add(&(child->vcores), -1);
  current_sched = parent;

  /* Signal unregistering hard thread. */
  if (child->state == UNREGISTERING) {
    if (child->vcores == 0) {
      child->state = UNREGISTERED;
    }
  }

  /* Yield to parent. */
  parent->funcs->yield(parent->this, child);
  fatal("lithe: returned from yield");
}

void lithe_sched_reenter()
{
  /* If we are already on the trampoline, simply enter the scheduler */
  if(current_task == trampoline)
    __lithe_sched_reenter();
  /* Otherwise, jump to the trampoline to reenter the scheduler */
  else {
    init_uthread_entry(&trampoline->uth, __lithe_sched_reenter, 0);
    run_uthread(&trampoline->uth);
  }
}

int lithe_sched_register(const lithe_sched_funcs_t *funcs,
            void *this,
            lithe_task_t *start_task)
{
  if (funcs == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (start_task == NULL) {
    errno = EINVAL;
    return -1;
  }

  /* We should never be on the trampoline. */
  if (current_task == trampoline) {
    errno = EPERM;
    return -1;
  }

  /* 
   * N.B. In some instances, Lithe code may get called from a non-hard
   * thread (i.e., doing a pthread_create).  The problem with allowing
   * something like this is dealing with multiple threads trying to
   * simultaneosly change the root scheduler (the child scheduler of
   * base).  Really we would need one base scheduler for each thread.
   */
  if (current_task == NULL) {
    errno = EPERM;
    return -1;
  }

  lithe_sched_t *parent = current_sched;
  lithe_sched_t *child = (lithe_sched_t *) malloc(sizeof(lithe_sched_t));

  if (child == NULL) {
    errno = ENOMEM;
    return -1;
  }

  /* Check if parent scheduler is still valid. */
  spinlock_lock(&parent->lock);
  { 
   if (parent->state != REGISTERED) {
      spinlock_unlock(&parent->lock);
      errno = EPERM;
      return -1;
    }
  }
  spinlock_unlock(&parent->lock);

  /* Set-up child scheduler. */
  spinlock_init(&child->lock);
  child->state = REGISTERED;
  child->vcores = 0;
  child->parent = parent;
  child->parent_task = current_task;
  child->children = NULL;
  child->next = NULL;
  child->prev = NULL;
  child->funcs = funcs;
  child->this = this;

  /* Add child to parents list of children. */
  spinlock_lock(&parent->lock);
  {
    child->next = parent->children;
    child->prev = NULL;

    if (parent->children != NULL) {
      parent->children->prev = child;
    }

    parent->children = child;
  }
  spinlock_unlock(&parent->lock);


  /* Inform parent. */
  parent->funcs->reg(parent->this, child);

  /* Leave parent, join child. */
  __sync_fetch_and_add(&(child->vcores), 1);
  assert(start_task);
  start_task->sched = child;
  trampoline->sched = child;
  uthread_set_tls_var(&start_task->uth, trampoline, trampoline);
  swap_uthreads(&child->parent_task->uth, &start_task->uth);
  return 0;
}

int lithe_sched_unregister()
{
  lithe_sched_t *parent = current_sched->parent;
  lithe_sched_t *child = current_sched;

  /*
   * Wait until all grandchildren have unregistered. The lock protects
   * against some other scheduler trying to register with child
   * (another grandchild) as well as our write to child state.
   */
  spinlock_lock(&child->lock);
  {
    lithe_sched_t *sched = child->children;

    while (sched != NULL) {
      while (sched->state != UNREGISTERED) {
        rdfence();
        atomic_delay();
      }
      sched = sched->next;
    }

    child->state = UNREGISTERING;
  }
  spinlock_unlock(&child->lock);

  /* Leave child, join parent. */
  __sync_fetch_and_add(&(child->vcores), -1);

  if (child->vcores == 0) {
    child->state = UNREGISTERED;
  }

  /* Inform parent. */
  parent->funcs->unreg(parent->this, child);

  /* Wait until all threads have left child. */
  while (child->state != UNREGISTERED) {
    /* TODO: Consider 'monitor' and 'mwait' or even MCS. */
    rdfence();
    atomic_delay();
  }

  /* Free grandchildren schedulers. */
  lithe_sched_t *sched = child->children;

  while (sched != NULL) {
    lithe_sched_t *next = sched->next;
    free(sched);
    sched = next;
  }

  /* Return control. */
  trampoline->sched = child->parent_task->sched;
  uthread_set_tls_var(&child->parent_task->uth, trampoline, trampoline);
  run_uthread(&child->parent_task->uth);
  return -1;
}


int lithe_sched_request(int k)
{
  lithe_sched_t *parent = current_sched->parent;
  lithe_sched_t *child = current_sched;

  return parent->funcs->request(parent->this, child, k);
}

lithe_task_t *lithe_task_create(void (*func) (void *), void *arg, 
                                lithe_task_stack_t *stack)
{
  assert(func != NULL);

  lithe_task_t *task = (lithe_task_t*)uthread_create(NULL, stack);
  if(task == NULL)
    return NULL;

  init_uthread_entry(&task->uth, func, 1, arg); 
  return task;
}

int lithe_task_destroy(lithe_task_t *task)
{
  assert(current_task != task);

  if (task == NULL) {
    errno = EINVAL;
    return -1;
  }
  uthread_destroy((uthread_t*)task);
  return 0;
}

lithe_task_t *lithe_task_self()
{
  return current_task;
}

int lithe_task_run(lithe_task_t *task)
{
  if (task == NULL) {
    errno = EINVAL;
    return -1;
  }

  /* TODO: Must we be on the trampoline? */
  if (current_task != trampoline) {
    errno = EPERM;
    return -1;
  }

  task->sched = current_sched;
  trampoline->sched = task->sched;
  uthread_set_tls_var(&task->uth, trampoline, trampoline);
  run_uthread(&task->uth);
  return -1;
}

int lithe_task_block(void (*func) (lithe_task_t *, void *), void *arg)
{
  if (func == NULL) {
    errno = EINVAL;
    return -1;
  }

  /* Confirm we aren't blocking using trampoline. */
  if (current_task == trampoline) {
    errno = EPERM;
    return -1;
  }

  /* Confirm there is a task to block (i.e. no task during 'request'). */
  if (current_task == NULL) {
    errno = EPERM;
    return -1;
  }

  lithe_task_t *task = current_task;
  init_uthread_entry(&trampoline->uth, func, 2, task, arg);
  swap_uthreads(&task->uth, &trampoline->uth);
  return 0;
}

int lithe_task_unblock(lithe_task_t *task)
{
  if (task == NULL) {
    errno = EINVAL;
    return -1;
  }

  current_sched->funcs->unblock(current_sched->this, task);
  return 0;
}

