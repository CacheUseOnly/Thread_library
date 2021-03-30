#include "ec440threads.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include <setjmp.h>

#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <stdnoreturn.h>
#include <stdio.h>

/* You can support more threads. At least support this many. */
#define MAX_THREADS 128

/* Your stack should be this many bytes in size */
#define THREAD_STACK_SIZE 32767

/* Number of microseconds between scheduling events */
#define SCHEDULER_INTERVAL_USECS (50 * 1000)

/* Extracted from private libc headers. These are not part of the public
 * interface for jmp_buf.
 */
#define JB_RBX 0
#define JB_RBP 1
#define JB_R12 2
#define JB_R13 3
#define JB_R14 4
#define JB_R15 5
#define JB_RSP 6
#define JB_PC 7

#define MALLOC_ERROR -1
#define THREAD_ARR_FULL -2
#define ENOTINIT -1
#define ELOCKED -2
#define EINVAL -1

/* thread_status identifies the current state of a thread. You can add, rename,
 * or delete these values. This is only a suggestion. */
enum thread_status
{
	TS_EXITED,
	TS_RUNNING,
	TS_READY,
	TS_BLOCKED		// HW3 new state
};

/* The thread control block stores information about a thread. You will
 * need one of this per thread.
 */
struct thread_control_block {
	/* TODO: add information about its stack */
	void* stack_ptr;
	/* TODO: add information about its registers */
	jmp_buf env;
	/* TODO: add a thread ID */
	unsigned tid;
	/* TODO: add information about the status (e.g., use enum thread_status) */
	enum thread_status tstatus;
	/* Add other information you need to manage this thread */
};

enum mutex_status {
	MUTEX_NOTINIT,
	MUTEX_INIT,
	MUTEX_DESTORYED
};

enum barrier_status {
	BAR_NOTINIT,
	BAR_INIT,
	BAR_DESTROYED
};

struct waitlist {
	int tid;
	struct waitlist *next;
};

struct myMutex {
	bool isLocked;
	struct waitlist* head;
	struct waitlist* tail;
	enum mutex_status status;
};

struct myBarrier {
	unsigned count;
	unsigned curr;
	struct waitlist* head;
	struct waitlist* tail;
	enum barrier_status status;
};

struct thread_control_block TCB_array[MAX_THREADS];
unsigned currThread = 0;

static void schedule(int signal);

/**
 * Your lock function should disable the timer that 
 * calls your schedule routine, and unlock should re-enable the timer.
 */
sigset_t set;
static void lock() {
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

static void unlock() {
	sigaddset(&set, SIGALRM);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/**
 * The pthread_mutex_init() function initializes a given mutex. 
 * The attr argument is unused in this assignment (we will always test it with NULL). 
 * Behavior is undefined when an already-initialized mutex is re-initialized. Always return 0.
 */
int pthread_mutex_init(pthread_mutex_t *restrict mutex,	const pthread_mutexattr_t *restrict attr) {
	struct myMutex *ptr;
	ptr = (struct myMutex*)malloc(sizeof(struct myMutex));
	ptr->isLocked = false;
	ptr->head= NULL;
	ptr->tail = NULL;
	ptr->status = MUTEX_INIT;
	mutex->__align = (long)ptr;	

	return 0;
}

/**
 * The pthread_mutex_destroy() function destroys the referenced mutex. 
 * Behavior is undefined when a mutex is destroyed while a thread is currently blocked on, 
 * or when destroying a mutex that has not been initialized. 
 * Behavior is undefined when locking or unlocking a destroyed mutex, 
 * unless it has been re-initialized by pthread_mutex_init. Return 0 on success. 
 */
int pthread_mutex_destroy(pthread_mutex_t *mutex) {
	struct myMutex* ptr = (struct myMutex*)mutex->__align;
	if (ptr->isLocked == 1) {
		return 1;
	}

	ptr->isLocked = false;
	ptr->status = MUTEX_DESTORYED;
	
	return 0;
}

/**
 * The pthread_mutex_lock() function locks a referenced mutex. 
 * If the mutex is not already locked, the current thread acquires the lock and proceeds. 
 * If the mutex is already locked, the thread blocks until the mutex is available. 
 * If multiple threads are waiting on a mutex, the order that they are awoken is undefined. 
 * Return 0 on success, or an error code otherwise.
 */
int pthread_mutex_lock(pthread_mutex_t *mutex) {
	lock();
	struct myMutex* ptr = (struct myMutex*)mutex->__align;
	if (ptr->status != MUTEX_INIT) return ENOTINIT;
	if (ptr->isLocked == true) {
		struct waitlist* wl = (struct waitlist*)malloc(sizeof(struct waitlist));
		wl->tid = pthread_self();
		wl->next = NULL;
		if (ptr->head == NULL) {
			ptr->head = wl;
			ptr->tail = wl;
		} else {
			ptr->tail->next = wl;
			ptr->tail = ptr->tail->next;
		}

		TCB_array[pthread_self()].tstatus = TS_BLOCKED;
		unlock();
		schedule(SIGALRM);
		return ELOCKED;
	}

	ptr->isLocked = true;

	unlock();
	return 0;
}

/**
 * The pthread_mutex_unlock() function unlocks a referenced mutex. 
 * If another thread is waiting on this mutex, 
 * it will be woken up so that it can continue running. 
 * Note that when that happens, 
 * the woken thread will finish acquiring the lock. 
 * Return 0 on success, or an error code otherwise.
 */
int pthread_mutex_unlock(pthread_mutex_t *mutex) {
	lock();
	struct myMutex* ptr = (struct myMutex*)mutex->__align;
	ptr->isLocked = false;
	if (ptr->head != NULL) {
		struct waitlist* temp = ptr->head;
		TCB_array[ptr->head->tid].tstatus = TS_READY;
		ptr->head = ptr->head->next;
		free(temp);
	}

	unlock();
	return 0;
}

/**
 * The pthread_barrier_init() function initializes a given barrier. 
 * The attr argument is unused in this assignment (we will always test it with NULL). 
 * The count argument specifies how many threads must enter the barrier 
 * before any threads can exit the barrier. Return 0 on success. 
 * It is an error if count is equal to zero (return EINVAL). 
 * Behavior is undefined when an already-initialized barrier is re-initialized.
 */
int pthread_barrier_init(
	pthread_barrier_t *restrict barrier,
	const pthread_barrierattr_t *restrict attr,
	unsigned count) {

	if (count == 0) {
		return EINVAL;
	}

	struct myBarrier *ptr = (struct myBarrier*)malloc(sizeof(struct myBarrier));
	ptr->count = count;
	ptr->curr = 0;
	ptr->status = BAR_INIT;
	ptr->head = NULL;
	ptr->tail = NULL;
	barrier->__align = (long)ptr;

	return 0;
}

/**
 * The pthread_barrier_wait() function enters the referenced barrier. 
 * The calling thread shall not proceed until the required number of threads 
 * (from count in pthread_barrier_init) have already entered the barrier. 
 * Other threads shall be allowed to proceed while this thread is in a barrier 
 * (unless they are also blocked for other reasons). Upon exiting a barrier, 
 * the order that the threads are awoken is undefined. 
 * Exactly one of the returned threads shall return PTHREAD_BARRIER_SERIAL_THREAD 
 * (it does not matter which one). The rest of the threads shall return 0.
 */
int pthread_barrier_wait(pthread_barrier_t *barrier) {
	struct myBarrier* ptr = (struct myBarrier*)barrier->__align;
	if (ptr->status != BAR_INIT) {
		return -2;
	}
	lock();
	ptr->curr++;
	if (ptr->curr != ptr->count) {
		struct waitlist *wl = (struct waitlist*)malloc(sizeof(struct waitlist*));
		wl->tid = pthread_self();
		wl->next = NULL;
		if (ptr->head == NULL) {
			ptr->head = wl;
			ptr->tail = wl;
		} else {
			ptr->tail->next = wl;
			ptr->tail = ptr->tail->next;
		}

		TCB_array[pthread_self()].tstatus = TS_BLOCKED;
		unlock();
		schedule(SIGALRM);
		return -3;
	} else {
		while(ptr->head != NULL) {
			struct waitlist* temp = ptr->head;
			TCB_array[ptr->head->tid].tstatus = TS_READY;
			ptr->head = ptr->head->next;
			free(temp);
		}
		ptr->head = NULL;
		ptr->tail = NULL;
		ptr->curr = 0;
	}

	unlock();
	return PTHREAD_BARRIER_SERIAL_THREAD;
}

/** 
 * The pthread_barrier_destroy() function destroys the referenced barrier. 
 * Behavior is undefined when a barrier is destroyed while a thread is waiting 
 * in the barrier or when destroying a barrier that has not been initialized. 
 * Behavior is undefined when attempting to wait in a destroyed barrier, 
 * unless it has been re-initialized by pthread_barrier_init. Return 0 on success.
 */
int pthread_barrier_destroy(pthread_barrier_t *barrier) {
	barrier = NULL;
	return 0;
}

static void schedule(int signal)
{
	if(setjmp(TCB_array[pthread_self()].env) == 0) {
		int nextThread = pthread_self(); 
		do {
			nextThread = (nextThread + 1)%MAX_THREADS; 
			if (nextThread == pthread_self()) {
				break;
			}
		} while (TCB_array[nextThread].tstatus != TS_READY);

		if (TCB_array[pthread_self()].tstatus != TS_EXITED && TCB_array[pthread_self()].tstatus != TS_BLOCKED) {
			TCB_array[pthread_self()].tstatus = TS_READY;
		}
		TCB_array[nextThread].tstatus = TS_RUNNING;
		currThread = nextThread;
		longjmp(TCB_array[nextThread].env, 1);
	}
}

static void scheduler_init()
{
	struct thread_control_block mainTCB;
	mainTCB.tid = MAX_THREADS - 1;
	mainTCB.tstatus = TS_RUNNING;
	currThread = mainTCB.tid;
	TCB_array[MAX_THREADS - 1] = mainTCB;

	struct itimerval timer;
    timer.it_value.tv_usec = SCHEDULER_INTERVAL_USECS;
    timer.it_value.tv_sec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = SCHEDULER_INTERVAL_USECS;

    struct sigaction sa;
    sa.sa_handler = schedule;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGALRM, &sa, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

int pthread_create(
	pthread_t *thread, const pthread_attr_t *attr,
	void *(*start_routine) (void *), void *arg)
{
	// Create the timer and handler for the scheduler. Create thread 0.
	static bool is_first_call = true;
	if (is_first_call)
	{
		is_first_call = false;
		scheduler_init();
	}

	static int tid = 0;
	if (tid >= (MAX_THREADS - 2)) {			// because MAX_THREADS-1 is main
		return THREAD_ARR_FULL;
	}
	*thread = tid;
	struct thread_control_block TCB;
	TCB.tid = tid;
	TCB.stack_ptr = malloc(THREAD_STACK_SIZE);
	setjmp(TCB.env);
	char* rsp = TCB.stack_ptr;
	rsp += THREAD_STACK_SIZE - 1 - sizeof(unsigned long int);
	*(unsigned long int*)rsp = (unsigned long int)pthread_exit;
	TCB.env->__jmpbuf[JB_PC] = ptr_mangle((unsigned long int)start_thunk);
	TCB.env->__jmpbuf[JB_RSP] = ptr_mangle((unsigned long int)rsp);
	TCB.env->__jmpbuf[JB_R12] = (unsigned long int)start_routine;
	TCB.env->__jmpbuf[JB_R13] = (unsigned long int)arg;
	
	TCB.tstatus = TS_READY;
	TCB_array[tid] = TCB;
	tid++;
	return 0;
}

void pthread_exit(void *value_ptr) 
{
	TCB_array[pthread_self()].tstatus = TS_EXITED;
	fflush(stdout);
	free(TCB_array[pthread_self()].stack_ptr);
	schedule(SIGALRM);
	__builtin_unreachable();
}

pthread_t pthread_self(void)
{
	return currThread;
}

/* Don't implement main in this file!
 * This is a library of functions, not an executable program. If you
 * want to run the functions in this file, create separate test programs
 * that have their own main functions.
 */
