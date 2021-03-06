/*
 * Copyright (c) 2011 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * Kevin Klues <klueska@cs.berkeley.edu>
 *
 * This file is part of Parlib.
 *
 * Parlib is free software: you can redistribute it and/or modify
 * it under the terms of the Lesser GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Parlib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Lesser GNU General Public License for more details.
 *
 * See COPYING.LESSER for details on the GNU Lesser General Public License.
 * See COPYING for details on the GNU General Public License.
 */

#ifndef _UTHREAD_H
#define _UTHREAD_H

#include "arch.h"
#include "context.h"
#include "vcore.h"

/* Thread States */
#define UT_RUNNING      1
#define UT_NOT_RUNNING  2

/* Thread flags */
#define NO_INTERRUPT		0x0001

/* Externally blocked thread reasons (for uthread_has_blocked()) */
#define UTH_EXT_BLK_MUTEX         1
#define UTH_EXT_BLK_JUSTICE       2   /* whatever.  might need more options */

/* Bare necessities of a user thread.  1LSs should allocate a bigger struct and
 * cast their threads to uthreads when talking with vcore code.  Vcore/default
 * 2LS code won't touch udata or beyond. */
/* The definition of user_context is system dependant and located under
 * the proper arch.h file */
struct uthread {
    struct user_context uc;
    void (*entry_func)(void);
    void (*yield_func)(struct uthread*, void*);
    void *yield_arg;
    int flags;
    int state;
    int disable_depth;
    void *sigstack;
#ifndef PARLIB_NO_UTHREAD_TLS
    void *tls_desc;
#else
    void *dtls_data;
#endif
    struct syscall *sysc;
    uint64_t sysc_timeout;
};
typedef struct uthread uthread_t;

/* External reference to the current uthread running on this vcore */
extern __thread uthread_t *current_uthread TLS_INITIAL_EXEC;

/* 2L-Scheduler operations.  Can be 0.  Examples in pthread.c. */
typedef struct schedule_ops {
    /* Functions supporting thread ops */
    void (*sched_entry)(void);
    void (*thread_runnable)(struct uthread *);
    void (*thread_paused)(struct uthread *);
    void (*thread_blockon_sysc)(struct uthread *, void *);
    void (*thread_has_blocked)(struct uthread *, int);
    /* Functions event handling wants */
    void (*preempt_pending)(void);
    void (*spawn_thread)(uintptr_t pc_start, void *data);   /* don't run yet */
} schedule_ops_t;
extern struct schedule_ops *sched_ops;

/* Initializes the uthread library. uth is the main thread's context. */
void uthread_lib_init(struct uthread *uth);

/* Functions to make/manage uthreads.  Can be called by functions such as
 * pthread_create(), which can wrap these with their own stuff (like attrs,
 * retvals, etc). */

/* Initializes a uthread. */
void uthread_init(struct uthread *uth);

/* Cleans up a uthread that was previously initialized by a call to
 * uthread_init(). Be careful not to call this on any currently running
 * uthreads. */
void uthread_cleanup(struct uthread *uthread);

/* Function indicating an external event has blocked the uthread. */
void uthread_has_blocked(struct uthread *uthread, int flags);

/* Function indicating an external event has temporarily paused a uthread, but
 * it is ok to resume it if possible. */
void uthread_paused(struct uthread *uthread);

/* Function forcing a uthread to become runnable */
void uthread_runnable(struct uthread *uthread);

/* Function to yield a uthread - it can be made runnable again in the future */
void uthread_yield(bool save_state, void (*yield_func)(struct uthread*, void*),
                   void *yield_arg);

/* Don't allow this uthread to be interrupted by an incoming vcore
 * notification. This is the default once a uthread starts running. */
void uth_disable_notifs();

/* Allow this uthread to be interrupted by an incoming vcore notification. You
 * should call this early on in your startup code if you want to allow your
 * uthreads to be interrupted by a vcore signal. */
void uth_enable_notifs();

/* By default, all of the uthread operations are safe from interrupts.  If you
 * have other calls that you know should not be interrupted by an event, you
 * must wrap these calls in enable/disable interrupt calls.  This macro
 * provides a convenient manner of doing so. */
#define uthread_notif_safe(func) \
{ \
	uth_disable_notifs(); \
	func; \
	uth_enable_notifs(); \
}

/* Helpers, which sched_entry() can call */
void save_current_uthread(struct uthread *uthread);
void hijack_current_uthread(struct uthread *uthread);
void run_current_uthread(void) __attribute((noreturn));
void run_uthread(struct uthread *uthread) __attribute((noreturn));
void swap_uthreads(struct uthread *__old, struct uthread *__new);
void init_uthread_tf(uthread_t *uth, void (*entry)(void),
                     void *stack_bottom, uint32_t size);

#ifndef PARLIB_NO_UTHREAD_TLS
  #define uthread_set_tls_var(uthread, name, val) \
  	(*get_tls_addr(name, ((uthread_t*)(uthread))->tls_desc) = (val))

  #define uthread_get_tls_var(uthread, name) \
  	(*get_tls_addr(name, ((uthread_t*)(uthread))->tls_desc))
#else
  #define uthread_set_tls_var(uthread, name, val)
  #define uthread_get_tls_var(uthread, name) name
#endif

#endif /* _UTHREAD_H */
