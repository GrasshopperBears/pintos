/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

void recover_priority(struct lock *lock);
void add_donate_list(struct lock* lock);
void update_donate_list(struct lock* lock);
struct list_elem* find_donate_elem_by_lock(struct list* list, struct lock* lock);
bool donate_elem_compare (const struct list_elem *e1, const struct list_elem *e2, void *aux UNUSED);

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	// if (sema->value > 1)
	// 	printf("SEMA VALUE: %d\n", sema->value);
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;
	struct int_list_elem* stack_node;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();

	while (sema->value == 0) {
		list_insert_ordered(&sema->waiters, &thread_current ()->elem, thread_compare, NULL); // modify
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;
	struct thread * t;

	ASSERT (sema != NULL);
	old_level = intr_disable ();

	if (!list_empty (&sema->waiters)) {
		list_sort(&sema->waiters, thread_compare, NULL); // Before unblock thread, some threads in waiters list could be changed their priority.
		t = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
		thread_unblock (t);
	}

	sema->value++;
	if (sema->value > 1)
		printf("SEMA VALUE: %d\n", sema->value);
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
	lock->donated = false;
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	bool success = false;
	struct donate_elem* el;
	enum intr_level old_level;
	struct semaphore* sema = &lock->semaphore;
	int prev;

	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));
	
	// old_level = intr_disable ();
	while (!success) {
		// if (strcmp("main", thread_current()->name))
		// 	printf("ENTER\n");
		if (lock->holder != NULL && !strcmp(thread_current()->name, "acquire2")) {
			// printf("LOCK addr of 2: %p\n", lock);
			// printf("CURRENT LOCK NAME of 2: %s\n", lock->holder->name);
			// printf("CURRENT LOCK VALUE of 2: %d\n", (&lock->semaphore)->value);
		} else if (lock->holder != NULL && !strcmp(thread_current()->name, "acquire1")) {
			// printf("LOCK addr of 1: %p\n", lock);
			// printf("CURRENT LOCK VALUE of 1: %d\n", (&lock->semaphore)->value);
		}
		success = sema_try_down(&lock->semaphore);
		if (success) {
			lock->holder = thread_current ();
			break;
		}
		// printf("FAILED SEMA DOWN\n");
		// printf("original: %d, to: %d\n", lock->holder->priority, thread_current()->priority);
		old_level = intr_disable ();
		list_push_front(&sema->waiters, &thread_current ()->elem);
		if (lock->holder->priority < thread_current()->priority) {
			printf("START DONATION\n");
			// printf("CURRENT LOCK NAME: %s\n", lock->holder->name);

			// printf("SIZE IN WHILE: %d\n", list_size(&lock->holder->donation_list));
			// printf("EMPTY: %d\n", list_size(&lock->holder->donation_list));
			prev = lock->holder->priority;
			lock->holder->priority = thread_current()->priority;
			if (!lock->donated) {
				lock->original_priority = prev;
				lock->donated = true;
				el->lock = lock;
				printf("HI\n");
				el->priority_after_donation = thread_current()->priority;
				list_push_front(&lock->holder->donation_list, &el->elem);
				// printf("SIZE IN WHILE: %d\n", list_size(&lock->holder->donation_list));
				// printf("PUSHED\n");
			} else {	
				update_donate_list(lock);
			}
			// printf("%s to prior %d\n", lock->holder->name, lock->holder->priority);
			// printf("(ACQ) %s to prior %d\n", lock->holder->name, lock->holder->priority);
		}
		
		// printf("SEMA WAITERS EMPTY?: %d\n", list_empty(&sema->waiters));
		// printf("SEMA WAITERS FIRST ELEM NAME?: %s\n", list_entry(list_begin(&sema->waiters), struct thread, elem)->name);
		
		// list_insert_ordered(&sema->waiters, &thread_current ()->elem, thread_compare, NULL);
		// printf("INSERTED\n");
		thread_block();
		intr_set_level(old_level);
	}
	// lock->holder = thread_current ();
	// intr_set_level(old_level);
	// if (strcmp("main", thread_current()->name))
	// 	printf("%s GOT THE LOCK\n", thread_current()->name);
}

void
add_donate_list(struct lock* lock) {
	struct thread* lock_holder = lock->holder;
	struct donate_elem* el;

	el->lock = lock;
	el->priority_after_donation = thread_current()->priority;
	list_push_front(&lock_holder->donation_list, &el->elem);
	printf("LOCK addr of add: %p\n", lock);
	// printf("PUSHED PRI: %d\n", el.priority_after_donation);
	// printf("EMPTY: %d\n", list_empty(lock_holder->donation_list));
	// printf("VERIFY: %d\n", list_entry(list_begin(&lock_holder->donation_list), struct donate_elem, elem)->priority_after_donation);
	// printf("PUSHED LOCK HOLDER NAME: %p\n", list_entry(list_front(&lock_holder->donation_list), struct donate_elem, elem)->lock);
	printf("SIZE AFTER ADD: %d\n", list_size(&lock_holder->donation_list));
	printf("ADDED\n");
}

void
update_donate_list(struct lock* lock) {
	struct thread* lock_holder = lock->holder;
	struct list_elem *found_el;
	struct donate_elem *el;

	ASSERT(!list_empty(&lock_holder->donation_list));

	printf("LOCK addr of update: %p\n", lock);
	// printf("SIZE WHEN UPDATE: %d\n", list_size(&lock_holder->donation_list));
	found_el = find_donate_elem_by_lock(&lock_holder->donation_list, lock);
	el = list_entry(found_el, struct donate_elem, elem);
	el->priority_after_donation = thread_current()->priority;
	printf("UPDATED\n");
}

struct list_elem* find_donate_elem_by_lock(struct list* list, struct lock* lock) {
	struct list_elem *el = list_begin(list);
	struct donate_elem* donate_el;
	printf("STARTED\n");
	printf("LOCK addr of find: %p\n", lock);
	// printf("SIZE: %d\n", list_size(&list));

	// printf("ENTERED\n");
	while (el != NULL) {
		donate_el = list_entry(el, struct donate_elem, elem);
		// printf("LOCK HOLDER: %s\n", donate_el->lock->holder->name);
		if (donate_el->lock == lock) {
			
			printf("FOUND\n");
			// printf("%d\n", donate_el->priority_after_donation);
			return el;
		}
		else
			printf("NOT FOUND\n");
		// printf("%d\n", donate_el->priority_after_donation);
		el = el->next;
	}
	printf("NOT FOUND\n");
	return NULL;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	enum intr_level old_level;
	struct semaphore* sema = &lock->semaphore;
	struct thread * t;

	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));
	ASSERT (sema != NULL);


	lock->holder = NULL;
	sema_up(sema);

	old_level = intr_disable ();
	if (lock->donated) {
		// printf("RECOVERING...\n");
		recover_priority(lock);
		lock->donated = false;
	} else {
		// printf("NOT RECOVER\n");
	}

	// sema->value++;
	
	
	// printf("%s RELAESE THE %x\n", thread_current()->name, lock);
	if (!list_empty (&sema->waiters)) {
		// printf("WAKE UP!\n");
		list_sort(&sema->waiters, thread_compare, NULL); // Before unblock thread, some threads in waiters list could be changed their priority.
		t = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
		// printf("%s thread unblocked\n", t->name);
		thread_unblock (t);
	}
	
	thread_kick();
	intr_set_level (old_level);
}

void
recover_priority(struct lock *lock) {
	struct thread* t = thread_current();
	struct list_elem* el;
	struct donate_elem* donate_el;
	// old_level = intr_disable ();

	if (lock->donated) {
		el = find_donate_elem_by_lock(&t->donation_list, lock);
		list_remove(el);
		lock->donated = false;
		t->priority = lock->original_priority;
		printf("CEHCK 2: %d\n", t->priority);
	}
	if (!list_empty(&t->donation_list)) {
		list_sort(&t->donation_list, donate_elem_compare, NULL);
		donate_el = list_entry(list_begin(&t->donation_list), struct donate_elem, elem);
		if (donate_el->priority_after_donation > t->priority) {
			t->priority = donate_el->priority_after_donation;
			printf("CEHCK 3: %d\n", t->priority);
		}
		// printf("(REL) %s to prior %d\n", t->name, t->priority);
	}
	// intr_set_level (old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

bool 
semaphore_compare (const struct list_elem *e1, const struct list_elem *e2, void *aux UNUSED);

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	list_insert_ordered(&cond->waiters, &waiter.elem, semaphore_compare, NULL);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
		list_sort(&cond->waiters, semaphore_compare, NULL); //modify
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

/* Compare priority of two thread t1 and t2.
   parameter e1 and e2 are struct list_elim which point semaphore_elem.
   and each semaphore_elem contain semaphore. then, each semaphore contains waiter list.
   t1 and t2 are the first element (have the highest priority) of the waiter list. */

bool 
semaphore_compare (const struct list_elem *e1, const struct list_elem *e2, void *aux UNUSED) {
	
	struct semaphore_elem *se1 = list_entry(e1, struct semaphore_elem, elem);
	struct semaphore_elem *se2 = list_entry(e2, struct semaphore_elem, elem);

	struct semaphore s1 = se1->semaphore;
	struct semaphore s2 = se2->semaphore;

	struct list waiters1 = s1.waiters;
	struct list waiters2 = s2.waiters;

	struct list_elem *begin1 = list_begin(&waiters1);
	struct list_elem *begin2 = list_begin(&waiters2);

	struct thread *t1 = list_entry(begin1, struct thread, elem);
	struct thread *t2 = list_entry(begin2, struct thread, elem);

	if (t1->priority > t2->priority)
		return true;
	else
		return false;
}

bool
donate_elem_compare (const struct list_elem *e1, const struct list_elem *e2, void *aux UNUSED) {
	struct donate_elem* el1 = list_entry(e1, struct donate_elem, elem);
	struct donate_elem* el2 = list_entry(e2, struct donate_elem, elem);

	if (el1->priority_after_donation > el2->priority_after_donation)
		return true;
	else
		return false;
}
