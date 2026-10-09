/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * <pthread.h> for libgcc and libstdc++ built with --enable-threads=posix.
 *
 * Types and initializers are copied from Zephyr of the NCS version in
 * ../ncs_version (include/zephyr/posix/posix_types.h, pthread.h): they are
 * compiled into libstdc++.a and must match the Zephyr ABI. Only functions
 * implemented in Zephyr lib/posix/options are declared.
 *
 * In a Zephyr build the Zephyr header is used instead: directly when
 * CONFIG_POSIX_SYSTEM_INTERFACES puts include/zephyr/posix first in the
 * include path, through the __has_include below otherwise.
 */

#ifndef _ZEPHYR_SDK_PTHREAD_H_
#define _ZEPHYR_SDK_PTHREAD_H_

#if defined(__has_include) && __has_include(<zephyr/posix/pthread.h>)

#include <zephyr/posix/pthread.h>

#else

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t pthread_t;
typedef uint32_t pthread_key_t;
typedef uint32_t pthread_mutex_t;
typedef uint32_t pthread_cond_t;

typedef struct {
	void *stack;
	unsigned int details[2];
} pthread_attr_t;

struct pthread_mutexattr {
	unsigned char type: 2;
	bool initialized: 1;
};
typedef struct pthread_mutexattr pthread_mutexattr_t;

struct pthread_condattr {
	clockid_t clock;
};
typedef struct pthread_condattr pthread_condattr_t;

struct pthread_once {
	bool flag;
};
typedef struct pthread_once pthread_once_t;

#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_CREATE_JOINABLE 0

#define PTHREAD_ONCE_INIT {0}
#define PTHREAD_MUTEX_INITIALIZER (-1)
#define PTHREAD_COND_INITIALIZER (-1)

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

int pthread_create(pthread_t *newthread, const pthread_attr_t *attr,
		   void *(*threadroutine)(void *), void *arg);
int pthread_join(pthread_t thread, void **status);
int pthread_detach(pthread_t thread);
int pthread_cancel(pthread_t pthread);
int pthread_equal(pthread_t pt1, pthread_t pt2);
pthread_t pthread_self(void);
int sched_yield(void);

int pthread_once(pthread_once_t *once, void (*initFunc)(void));

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *att);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);

int pthread_cond_init(pthread_cond_t *cv, const pthread_condattr_t *att);
int pthread_cond_destroy(pthread_cond_t *cv);
int pthread_cond_signal(pthread_cond_t *cv);
int pthread_cond_broadcast(pthread_cond_t *cv);
int pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mut);
int pthread_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *mut,
			   const struct timespec *abstime);

/*
 * Read-write locks only for the C part of gthr-posix.h. Hidden from C++ so
 * that libstdc++ configure does not find pthread_rwlock_t: std::shared_mutex
 * would use PTHREAD_RWLOCK_INITIALIZER without pthread_rwlock_destroy and
 * leak a Zephyr pool object, the condition variable version does not.
 */
#ifndef __cplusplus
typedef uint32_t pthread_rwlock_t;
typedef uint32_t pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER (-1)

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);
#endif

#ifdef __cplusplus
}
#endif

#endif

#endif /* _ZEPHYR_SDK_PTHREAD_H_ */
