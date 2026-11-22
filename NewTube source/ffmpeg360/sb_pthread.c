/* sb_pthread.c - POSIX threads for FFmpeg on the Xbox 360, built from the 360's own Win32-style pieces.
   Compiled DIRECTLY with cl (not through c99-to-c89): it needs xtl.h, which the converter can't read.

   - threads: CreateThread with a 1 MB stack (FFmpeg's H.264 code needs more than the default), each pinned to
     its own hardware thread 1..5 in turn (hardware thread 0 stays for the program's main loop / drawing)
   - mutex: a CRITICAL_SECTION
   - condition variable: the 360 has none (no SleepConditionVariable) -> the classic semaphore design, as in
     FFmpeg's own compat\w32pthreads.h for Windows XP: a semaphore the waiters sleep on, a waiter count, and for
     broadcast an event "all woken waiters are through" (set only by the last waiter of a broadcast)
   - once: a spin-lock on a 0 / 1 (running) / 2 (done) state */
#include <xtl.h>
#include <stdlib.h>
#include "sb_pthread.h"

struct sb_thread {
    HANDLE handle;
    void *(*start)(void *);
    void *arg;
    void *result;
};

static volatile LONG g_next_core;

static DWORD WINAPI thread_entry(void *p)
{
    struct sb_thread *t = (struct sb_thread *)p;
    t->result = t->start(t->arg);
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    struct sb_thread *t = (struct sb_thread *)malloc(sizeof(*t));
    LONG n;
    (void)attr;
    if (!t)
        return 12;                                  /* ENOMEM */
    t->start = start;
    t->arg = arg;
    t->result = NULL;
    t->handle = CreateThread(NULL, 1024 * 1024, thread_entry, t, CREATE_SUSPENDED, NULL);
    if (!t->handle) {
        free(t);
        return 11;                                  /* EAGAIN */
    }
    n = InterlockedIncrement(&g_next_core);
    XSetThreadProcessor(t->handle, 1 + (DWORD)(n - 1) % 5);   /* hardware threads 1..5 */
    ResumeThread(t->handle);
    *thread = t;
    return 0;
}

int pthread_join(pthread_t t, void **value)
{
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    if (value)
        *value = t->result;
    free(t);
    return 0;
}

/* ---- mutex ---- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    (void)attr;
    if (!cs)
        return 12;
    InitializeCriticalSection(cs);
    m->cs = cs;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (m->cs) {
        DeleteCriticalSection((CRITICAL_SECTION *)m->cs);
        free(m->cs);
        m->cs = NULL;
    }
    return 0;
}
/* a mutex made with PTHREAD_MUTEX_INITIALIZER has no CRITICAL_SECTION yet: the first lock makes one. If two
   threads get here at the same moment, both make one, but only the first to swap it in keeps it */
static CRITICAL_SECTION *get_cs(pthread_mutex_t *m)
{
    if (!m->cs) {
        CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
        InitializeCriticalSection(cs);
        if (InterlockedCompareExchangePointer(&m->cs, cs, NULL) != NULL) {
            DeleteCriticalSection(cs);          /* someone else was first */
            free(cs);
        }
    }
    return (CRITICAL_SECTION *)m->cs;
}
int pthread_mutex_lock(pthread_mutex_t *m)   { EnterCriticalSection(get_cs(m)); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { LeaveCriticalSection(get_cs(m)); return 0; }
int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->type = 0; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) { a->type = type; return 0; }

/* ---- condition variable ---- */
typedef struct {
    CRITICAL_SECTION broadcast_lock;    /* one signal/broadcast at a time */
    CRITICAL_SECTION count_lock;        /* protects waiters / is_broadcast */
    volatile int waiters;
    volatile int is_broadcast;
    HANDLE sema;                        /* waiters sleep here */
    HANDLE all_through;                 /* broadcast: set by the last woken waiter */
} cond_t;

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    cond_t *k = (cond_t *)calloc(1, sizeof(cond_t));
    (void)attr;
    if (!k)
        return 12;
    InitializeCriticalSection(&k->broadcast_lock);
    InitializeCriticalSection(&k->count_lock);
    k->sema = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
    k->all_through = CreateEvent(NULL, FALSE, FALSE, NULL);     /* auto-reset */
    if (!k->sema || !k->all_through)
        return 12;
    c->c = k;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    cond_t *k = (cond_t *)c->c;
    if (!k)
        return 0;
    CloseHandle(k->sema);
    CloseHandle(k->all_through);
    DeleteCriticalSection(&k->broadcast_lock);
    DeleteCriticalSection(&k->count_lock);
    free(k);
    c->c = NULL;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    cond_t *k = (cond_t *)c->c;
    int last_of_broadcast;

    EnterCriticalSection(&k->broadcast_lock);       /* don't join in the middle of a broadcast */
    EnterCriticalSection(&k->count_lock);
    k->waiters++;
    LeaveCriticalSection(&k->count_lock);
    LeaveCriticalSection(&k->broadcast_lock);

    pthread_mutex_unlock(m);                        /* release the caller's lock while sleeping */
    WaitForSingleObject(k->sema, INFINITE);

    EnterCriticalSection(&k->count_lock);
    k->waiters--;
    last_of_broadcast = k->is_broadcast && k->waiters == 0;
    LeaveCriticalSection(&k->count_lock);
    if (last_of_broadcast)
        SetEvent(k->all_through);

    return pthread_mutex_lock(m);                   /* take the caller's lock back before returning */
}

int pthread_cond_signal(pthread_cond_t *c)
{
    cond_t *k = (cond_t *)c->c;
    int have;
    EnterCriticalSection(&k->broadcast_lock);
    EnterCriticalSection(&k->count_lock);
    have = k->waiters;
    LeaveCriticalSection(&k->count_lock);
    if (have)
        ReleaseSemaphore(k->sema, 1, NULL);
    LeaveCriticalSection(&k->broadcast_lock);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    cond_t *k = (cond_t *)c->c;
    int have;
    EnterCriticalSection(&k->broadcast_lock);
    EnterCriticalSection(&k->count_lock);
    have = k->waiters;
    if (have)
        k->is_broadcast = 1;
    LeaveCriticalSection(&k->count_lock);
    if (have) {
        ReleaseSemaphore(k->sema, have, NULL);
        WaitForSingleObject(k->all_through, INFINITE);  /* every woken waiter has taken its turn */
        k->is_broadcast = 0;
    }
    LeaveCriticalSection(&k->broadcast_lock);
    return 0;
}

/* ---- sleep (for sbplayer.c: FFmpeg's own av_usleep only sleeps when it may include windows.h -> xtl.h) ---- */
void sb_sleep_ms(int ms)
{
    Sleep(ms);
}

/* ---- once ---- */
int pthread_once(pthread_once_t *once, void (*routine)(void))
{
    switch (InterlockedCompareExchange(&once->state, 1, 0)) {
    case 0:                                         /* first: run it */
        routine();
        InterlockedExchange(&once->state, 2);
        break;
    case 1:                                         /* another thread is running it: wait */
        while (once->state != 2) {
            __lwsync();
            Sleep(0);
        }
        break;
    }
    return 0;
}
