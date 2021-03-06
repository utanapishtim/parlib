/* See COPYING.LESSER for copyright information. */
/* Kevin Klues <klueska@cs.berkeley.edu>	*/
/* Andrew Waterman <waterman@cs.berkeley.edu>	*/

#ifndef __PARLIB_INTERNAL_SYSCALL_H__
#define __PARLIB_INTERNAL_SYSCALL_H__

#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>

/* Only enable this for testing! */
//#define ALWAYS_BLOCK

#ifdef __GLIBC__
#define __SUPPORTED_C_LIBRARY__
#define __internal_open __open
#define __internal_read __read
#define __internal_write __write
#define __internal_fopen _IO_fopen
#define __internal_fread _IO_fread
#define __internal_fwrite _IO_fwrite
#define __internal_socket __real_socket
#define __internal_accept __real_accept
int __open(const char*, int, ...);
FILE *_IO_fopen(const char *path, const char *mode);
ssize_t __read(int, void*, size_t);
ssize_t __write(int, const void*, size_t);
size_t _IO_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t _IO_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int __real_socket(int socket_family, int socket_type, int protocol);
int __real_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
#endif

#include "../uthread.h"
#include "../event.h"
#include "parlib.h"
#include "futex.h"
#include "pthread_pool.h"
#include <sys/mman.h>

typedef struct {
  void *(*func)(void*);
  struct event_msg ev_msg;
} yield_callback_arg_t;

#ifdef ALWAYS_BLOCK
#define uthread_blocking_call(__func_nonblock, __func_block, ...) \
({ \
  typeof(__func_block(__VA_ARGS__)) ret; \
  yield_callback_arg_t arg = { NULL, {0} }; \
  int vcoreid = vcore_id(); \
  void *do_##__func(void *arg) { \
    ret = __func_block(__VA_ARGS__); \
    send_event((struct event_msg*)arg, EV_SYSCALL, vcoreid); \
    return NULL; \
  } \
  arg.func = &do_##__func; \
  uthread_yield(true, __uthread_yield_callback, &arg); \
  current_uthread->sysc_timeout = 0; \
  ret; \
})
#else
#define uthread_blocking_call(__func_nonblock, __func_block, ...) \
({ \
  typeof(__func_block(__VA_ARGS__)) ret; \
  yield_callback_arg_t arg = { NULL, {0} }; \
  int vcoreid = vcore_id(); \
  void *do_##__func(void *arg) { \
    ret = __func_block(__VA_ARGS__); \
    send_event((struct event_msg*)arg, EV_SYSCALL, vcoreid); \
    return NULL; \
  } \
  ret = __func_nonblock(__VA_ARGS__); \
  if ((ret == -1) && (errno == EWOULDBLOCK)) { \
    arg.func = &do_##__func; \
    uthread_yield(true, __uthread_yield_callback, &arg); \
  } \
  current_uthread->sysc_timeout = 0; \
  ret; \
})
#endif

static void __uthread_yield_callback(struct uthread *uthread, void *__arg) {
  yield_callback_arg_t *arg = (yield_callback_arg_t*)__arg;

  if (arg->func == NULL) {
    /* If we were able to do this syscall without blocking, then this is just a
     * simple cooperative yield, do nothing further. */
    assert(sched_ops->thread_paused);
    sched_ops->thread_paused(uthread);
  } else {
    /* Otherwise, we need to invoke the magic of our backing pthread to perform
     * the syscall as a simulated async I/O operation, and send us an event
     * when it is complete. */
    arg->ev_msg.ev_arg3 = &arg->ev_msg.sysc;
    uthread->sysc = &arg->ev_msg.sysc;

    assert(sched_ops->thread_blockon_sysc);
    sched_ops->thread_blockon_sysc(uthread, &arg->ev_msg.sysc);

    struct backing_pthread *bp = get_tls_addr(__backing_pthread, uthread->tls_desc);
    bp->syscall = arg->func;
    bp->arg = &arg->ev_msg;
    bp->futex = BACKING_THREAD_SYSCALL;
    futex_wakeup_one(&bp->futex);
  }
}

#endif // __PARLIB_INTERNAL_SYSCALL_H__
