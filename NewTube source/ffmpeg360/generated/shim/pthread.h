/* sb_pthread.h - the POSIX thread functions FFmpeg uses, for the Xbox 360 (implemented in sb_pthread.c).
   build_ffmpeg.py copies this to generated\shim\pthread.h, so FFmpeg's  #include <pthread.h>  finds it.
   Only declarations here (no xtl.h): the c99-to-c89 converter crashes on xtl.h. */
#ifndef SB_PTHREAD_H
#define SB_PTHREAD_H

typedef struct sb_thread *pthread_t;
typedef int pthread_attr_t;
typedef struct { void *cs; } pthread_mutex_t;           /* -> a CRITICAL_SECTION, made in pthread_mutex_init */
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { void *c; } pthread_cond_t;             /* -> our semaphore-based condition (see .c) */
typedef int pthread_condattr_t;
typedef struct { volatile long state; } pthread_once_t; /* 0 = not run, 1 = running, 2 = done */
#define PTHREAD_ONCE_INIT { 0 }
#define PTHREAD_MUTEX_INITIALIZER { 0 }                 /* ready-made mutex: created on its first lock */
#define PTHREAD_MUTEX_ERRORCHECK 2

#ifdef __cplusplus
extern "C" {
#endif

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **value);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_once(pthread_once_t *once, void (*routine)(void));
void sb_sleep_ms(int ms);           /* the 360's Sleep (not POSIX; for our own code like sbplayer.c) */

#ifdef __cplusplus
}
#endif

#endif
