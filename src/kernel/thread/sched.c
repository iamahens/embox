/**
 * @file
 * @brief TODO --Alina
 *
 * @date 22.04.10
 * @author Dmitry Avdyukhin
 * @author Kirill Skorodumov
 * @author Alina Kramar
 */

#include <assert.h>
#include <errno.h>

#include <embox/unit.h>
#include <lib/list.h>
#include <kernel/critical/api.h>
#include <kernel/thread/event.h>
#include <kernel/thread/sched.h>
#include <kernel/thread/sched_policy.h>
#include <kernel/thread/state.h>
#include <kernel/timer.h>
#include <hal/context.h>
#include <hal/ipl.h>

#include "types.h"

/** Interval, what scheduler_tick is called in. */
#define SCHED_TICK_INTERVAL 100

EMBOX_UNIT(unit_init, unit_fini);

/** Timer, which calls scheduler_tick. */
static sys_timer_t *tick_timer;

static void request_switch(void) {
	critical_request_dispatch(CRITICAL_SCHED_LOCK);
}

static void request_switch_if(int cond) {
	if (cond) {
		request_switch();
	}
}

int sched_init(struct thread* current, struct thread *idle) {
	int error;

#if 0
	current->state = THREAD_STATE_RUNNING;
	idle->state = THREAD_STATE_RUNNING;
#endif

	if ((error = sched_policy_init(current, idle))) {
		return error;
	}

	sched_unlock();

	return 0;
}

/**
 * Is regularly called to show that current thread to be changed.
 * @param id nothing significant
 */
static void sched_tick(sys_timer_t *timer, void *param) {
	request_switch();
}

/**
 * Switches thread to another thread and their contexts.
 */
static void sched_switch(void) {
	struct thread *current, *next;
	ipl_t ipl;

	current = sched_current();
	next = sched_policy_switch(current);

	assert(thread_state_running(next->state));

	if (next == current) {
		return;
	}

	ipl = ipl_save();
	context_switch(&current->context, &next->context);
	ipl_restore(ipl);

}

void __sched_dispatch(void) {
	assert(critical_allows(CRITICAL_SCHED_LOCK));

	sched_lock();

	sched_switch();

	sched_unlock();
}

void sched_start(struct thread *t) {
	assert(t != NULL);

	sched_lock();

#if 0
	t->state = THREAD_STATE_RUNNING;
#endif

	request_switch_if(sched_policy_start(t));

	sched_unlock();
}

void sched_stop(struct thread *t) {
	assert(t != NULL);

	sched_lock();

	request_switch_if(sched_policy_stop(t));

#if 0
	t->state = 0;
#endif

	sched_unlock();
}

int sched_sleep_locked(struct event *e) {
	struct thread *current;

	assert(e);
	assert(critical_inside(CRITICAL_SCHED_LOCK));

	current = sched_current();

	request_switch_if(sched_policy_stop(current));

	current->state = thread_state_do_sleep(current->state);

	list_add(&current->sched_list, &e->sleep_queue);

	/* Switch from the current thread. */
	sched_unlock();

	/* At this point we have been awakened and are ready to go. */
	assert(critical_allows(CRITICAL_SCHED_LOCK));
	assert(thread_state_running(current->state));

	/* Restore the locked state and return. */
	sched_lock();

	return 0;
}

int sched_sleep(struct event *e) {
	int ret;

	assert(e);
	assert(critical_allows(CRITICAL_SCHED_LOCK));

	sched_lock();

	ret = sched_sleep_locked(e);

	sched_unlock_noswitch();

	return ret;
}

static void sched_wakeup_thread(struct thread *t) {
	assert(critical_inside(CRITICAL_SCHED_LOCK));

	list_del_init(&t->sched_list);

	t->state = thread_state_do_wake(t->state);

	if (thread_state_running(t->state)) {
		request_switch_if(sched_policy_start(t));
	}
}

int sched_wake(struct event *e) {
	struct thread *t, *tmp;
	struct thread *current;

	sched_lock();

	current = sched_current();

	list_for_each_entry_safe(t, tmp, &e->sleep_queue, sched_list) {
		sched_wakeup_thread(t);
	}

	sched_unlock();

	return 0;
}

int sched_wake_one(struct event *e) {
	struct thread *t;

	sched_lock();

	t = list_entry(e->sleep_queue.next, struct thread, sched_list);
	sched_wakeup_thread(t);

	sched_unlock();

	return 0;
}

void sched_yield(void) {
	request_switch();
}

void sched_suspend(struct thread *t) {
	sched_lock();

	if (thread_state_running(t->state)) {
		request_switch_if(sched_policy_stop(t));
	}

	t->state = thread_state_do_suspend(t->state);

	sched_unlock();
}

void sched_resume(struct thread *t) {
	sched_lock();

	t->state = thread_state_do_resume(t->state);

	if (thread_state_running(t->state)) {
		request_switch_if(sched_policy_start(t));
	}

	sched_unlock();
}

int sched_change_scheduling_priority(struct thread *t, __thread_priority_t new) {
	bool need_restart;

	sched_lock();

	if (thread_state_exited(t->state)) {
		return -ESRCH;
	}

	need_restart = thread_state_running(t->state);

	request_switch_if(need_restart && sched_policy_stop(t));

	t->priority = new;

	request_switch_if(need_restart && sched_policy_start(t));

	sched_unlock();

	return 0;
}

void sched_set_priority(struct thread *t, __thread_priority_t new) {
	sched_lock();

	sched_change_scheduling_priority(t, new);
	t->initial_priority = new;

	sched_unlock();
}

static int unit_init(void) {
	if (timer_set(&tick_timer, SCHED_TICK_INTERVAL, sched_tick, NULL)) {
		return -EBUSY;
	}

	return 0;
}

static int unit_fini(void) {
	timer_close(tick_timer);

	return 0;
}
