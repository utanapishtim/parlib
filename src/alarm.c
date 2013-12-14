#include "alarm.h"
#include "spinlock.h"
#include "internal/assert.h"
#include "internal/time.h"
#include "internal/pthread_pool.h"
#include <unistd.h>
#include <stdlib.h>

static spinlock_t lock = SPINLOCK_INITIALIZER;

static void *__waiting_thread(void *arg)
{
	uint64_t wakeup_time, now;
	struct alarm_waiter *waiter = (struct alarm_waiter*)arg;
	spinlock_lock(&lock);
	do {
		wakeup_time = waiter->wakeup_time;
		now = time_usec();
		if (wakeup_time < now)
			break;
	    spinlock_unlock(&lock);
		usleep(wakeup_time-now);
	    spinlock_lock(&lock);
	} while(waiter->wakeup_time != wakeup_time);
	waiter->done = true;
	spinlock_unlock(&lock);

	if (!waiter->unset)
		waiter->func(waiter);

	return NULL;
}

void init_awaiter(struct alarm_waiter *waiter,
                  void (*func) (struct alarm_waiter *))
{
	waiter->func = func;
	waiter->wakeup_time = 0;
	waiter->unset = false;
	waiter->done = false;
}

void set_awaiter_rel(struct alarm_waiter *waiter, uint64_t usleep)
{
	uint64_t now = time_usec();
	spinlock_lock(&lock);
	waiter->wakeup_time = now + usleep;
	spinlock_unlock(&lock);
}

void set_awaiter_inc(struct alarm_waiter *waiter, uint64_t usleep)
{
	assert(waiter->wakeup_time);
	spinlock_lock(&lock);
	waiter->wakeup_time += usleep;
	spinlock_unlock(&lock);
}

void set_alarm(struct alarm_waiter *waiter)
{
	assert(!waiter->unset);
	pooled_pthread_start(__waiting_thread, waiter);	
}

bool unset_alarm(struct alarm_waiter *waiter)
{
	spinlock_lock(&lock);
	if (!waiter->done) {
		waiter->unset = true;
	}
	spinlock_unlock(&lock);
	return waiter->unset;
}
