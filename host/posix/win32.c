/* (hookshot, Linux) host/posix/windows.h's functions on POSIX: see that header for what each part stands for.
   Built into the executable only (nx2/build.py), which exports the w32_* names for libhookshot_vk.so. */
#define _GNU_SOURCE
#include "windows.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <link.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <setjmp.h>
#include <execinfo.h>

/* ---- errors ------------------------------------------------------------------------------------------------- */
static _Thread_local DWORD last_error;
DWORD w32_GetLastError(void) { return last_error; }
void w32_SetLastError(DWORD e) { last_error = e; }
static DWORD from_errno(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    default: return 0x1000 + (DWORD)e;
    }
}
static BOOL fail_errno(void) { last_error = from_errno(errno); return FALSE; }

/* ---- time --------------------------------------------------------------------------------------------------- */
static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
BOOL w32_QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = mono_ns(); return TRUE; }
BOOL w32_QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
ULONGLONG w32_GetTickCount64(void) { return (ULONGLONG)(mono_ns() / 1000000); }
DWORD w32_GetTickCount(void) { return (DWORD)w32_GetTickCount64(); }
void w32_Sleep(DWORD ms) {
    if (ms == 0) { sched_yield(); return; }
    if (ms == INFINITE) for (;;) pause();
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
BOOL w32_SwitchToThread(void) { sched_yield(); return TRUE; }
#define UNIX_TO_FILETIME 116444736000000000ULL   /* 1601 to 1970 in 100 ns */
void w32_GetSystemTimeAsFileTime(FILETIME *ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t t = (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100 + UNIX_TO_FILETIME;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
static void tm_to_st(const struct tm *t, int ms, SYSTEMTIME *st) {
    st->wYear = (WORD)(t->tm_year + 1900);
    st->wMonth = (WORD)(t->tm_mon + 1);
    st->wDayOfWeek = (WORD)t->tm_wday;
    st->wDay = (WORD)t->tm_mday;
    st->wHour = (WORD)t->tm_hour;
    st->wMinute = (WORD)t->tm_min;
    st->wSecond = (WORD)t->tm_sec;
    st->wMilliseconds = (WORD)ms;
}
void w32_GetLocalTime(SYSTEMTIME *st) {
    struct timespec ts;
    struct tm t;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &t);
    tm_to_st(&t, (int)(ts.tv_nsec / 1000000), st);
}
void w32_GetSystemTime(SYSTEMTIME *st) {
    struct timespec ts;
    struct tm t;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &t);
    tm_to_st(&t, (int)(ts.tv_nsec / 1000000), st);
}
DWORD w32_GetTimeZoneInformation(TIME_ZONE_INFORMATION *tz) {
    memset(tz, 0, sizeof *tz);
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    tz->Bias = (LONG)(-t.tm_gmtoff / 60);   /* (UTC = local + bias, in minutes) */
    return t.tm_isdst > 0 ? TIME_ZONE_ID_DAYLIGHT : TIME_ZONE_ID_STANDARD;
}
BOOL w32_FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st) {
    const uint64_t t = (uint64_t)ft->dwHighDateTime << 32 | ft->dwLowDateTime;
    if (t < UNIX_TO_FILETIME) return FALSE;
    const time_t s = (time_t)((t - UNIX_TO_FILETIME) / 10000000ULL);
    struct tm tm;
    gmtime_r(&s, &tm);
    tm_to_st(&tm, (int)((t / 10000) % 1000), st);
    return TRUE;
}
BOOL w32_SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft) {
    struct tm tm = {0};
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
    const uint64_t t = (uint64_t)timegm(&tm) * 10000000ULL + (uint64_t)st->wMilliseconds * 10000ULL + UNIX_TO_FILETIME;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}
int w32_GetUserDefaultLocaleName(LPWSTR out, int cap) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    if (!l || !*l || !strcmp(l, "C") || !strcmp(l, "POSIX")) l = "en_US";
    int n = 0;
    for (; l[n] && l[n] != '.' && l[n] != '@' && n < cap - 1; n++) out[n] = l[n] == '_' ? L'-' : (wchar_t)l[n];
    out[n] = 0;
    return n + 1;
}

/* ---- futexes ------------------------------------------------------------------------------------------------ */
static int futex_wait(volatile int32_t *addr, int32_t val, const struct timespec *rel) {
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, rel, NULL, 0);
}
static void futex_wake(volatile int32_t *addr, int n) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0); }
static struct timespec rel_ms(DWORD ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; return t; }
/* the time left until a deadline, 0 if passed */
static int left_until(int64_t deadline, struct timespec *out) {
    const int64_t d = deadline - mono_ns();
    if (d <= 0) return 0;
    out->tv_sec = d / 1000000000LL;
    out->tv_nsec = d % 1000000000LL;
    return 1;
}

/* ---- SRW locks: state 0 free, -1 held exclusive, n > 0 held shared by n; writers_waiting counts sleepers ---- */
void w32_InitializeSRWLock(SRWLOCK *l) { l->state = 0, l->writers_waiting = 0; }
static void srw_sleep(SRWLOCK *l, int32_t seen) {
    __atomic_add_fetch(&l->writers_waiting, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&l->state, __ATOMIC_SEQ_CST) == seen) futex_wait(&l->state, seen, NULL);
    __atomic_sub_fetch(&l->writers_waiting, 1, __ATOMIC_SEQ_CST);
}
void w32_AcquireSRWLockExclusive(SRWLOCK *l) {
    for (int spin = 0;; spin++) {
        int32_t z = 0;
        if (__atomic_compare_exchange_n(&l->state, &z, -1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (spin < 64) { __builtin_ia32_pause(); continue; }
        srw_sleep(l, z);
    }
}
BOOLEAN w32_TryAcquireSRWLockExclusive(SRWLOCK *l) {
    int32_t z = 0;
    return __atomic_compare_exchange_n(&l->state, &z, -1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
void w32_ReleaseSRWLockExclusive(SRWLOCK *l) {
    __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&l->writers_waiting, __ATOMIC_SEQ_CST)) futex_wake(&l->state, INT_MAX);
}
void w32_AcquireSRWLockShared(SRWLOCK *l) {
    for (int spin = 0;; spin++) {
        int32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (s >= 0 && __atomic_compare_exchange_n(&l->state, &s, s + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (s >= 0) continue;   /* (another reader came in between) */
        if (spin < 64) { __builtin_ia32_pause(); continue; }
        srw_sleep(l, s);
    }
}
BOOLEAN w32_TryAcquireSRWLockShared(SRWLOCK *l) {
    int32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    while (s >= 0)
        if (__atomic_compare_exchange_n(&l->state, &s, s + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return TRUE;
    return FALSE;
}
void w32_ReleaseSRWLockShared(SRWLOCK *l) {
    if (__atomic_sub_fetch(&l->state, 1, __ATOMIC_RELEASE) == 0 && __atomic_load_n(&l->writers_waiting, __ATOMIC_SEQ_CST))
        futex_wake(&l->state, INT_MAX);
}

/* ---- condition variables: seq counts wakes, pad the sleepers ---- */
void w32_InitializeConditionVariable(CONDITION_VARIABLE *cv) { cv->seq = 0, cv->pad = 0; }
static BOOL cv_wait(CONDITION_VARIABLE *cv, DWORD ms, void (*unlock)(void *), void (*lock)(void *), void *l) {
    const int32_t seq = __atomic_load_n(&cv->seq, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&cv->pad, 1, __ATOMIC_SEQ_CST);
    unlock(l);
    int r = 0;
    if (ms == INFINITE) {
        r = futex_wait(&cv->seq, seq, NULL);
    } else {
        struct timespec t = rel_ms(ms);
        r = futex_wait(&cv->seq, seq, &t);
    }
    const int timed_out = r != 0 && errno == ETIMEDOUT;
    __atomic_sub_fetch(&cv->pad, 1, __ATOMIC_SEQ_CST);
    lock(l);
    if (timed_out) { last_error = ERROR_TIMEOUT; return FALSE; }
    return TRUE;
}
static void srw_unlock_x(void *l) { w32_ReleaseSRWLockExclusive((SRWLOCK *)l); }
static void srw_lock_x(void *l) { w32_AcquireSRWLockExclusive((SRWLOCK *)l); }
static void srw_unlock_s(void *l) { w32_ReleaseSRWLockShared((SRWLOCK *)l); }
static void srw_lock_s(void *l) { w32_AcquireSRWLockShared((SRWLOCK *)l); }
BOOL w32_SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags) {
    return flags & CONDITION_VARIABLE_LOCKMODE_SHARED ? cv_wait(cv, ms, srw_unlock_s, srw_lock_s, l)
                                                      : cv_wait(cv, ms, srw_unlock_x, srw_lock_x, l);
}
static void cs_unlock(void *cs) { w32_LeaveCriticalSection((CRITICAL_SECTION *)cs); }
static void cs_lock(void *cs) { w32_EnterCriticalSection((CRITICAL_SECTION *)cs); }
BOOL w32_SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) {
    return cv_wait(cv, ms, cs_unlock, cs_lock, cs);
}
void w32_WakeConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&cv->pad, __ATOMIC_SEQ_CST)) futex_wake(&cv->seq, 1);
}
void w32_WakeAllConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&cv->pad, __ATOMIC_SEQ_CST)) futex_wake(&cv->seq, INT_MAX);
}

/* ---- critical sections: recursive mutexes ---- */
void w32_InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    cs->impl = m;
    cs->spin = 0;
}
void w32_EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock((pthread_mutex_t *)cs->impl); }
BOOL w32_TryEnterCriticalSection(CRITICAL_SECTION *cs) { return pthread_mutex_trylock((pthread_mutex_t *)cs->impl) == 0; }
void w32_LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock((pthread_mutex_t *)cs->impl); }
void w32_DeleteCriticalSection(CRITICAL_SECTION *cs) {
    if (!cs->impl) return;
    pthread_mutex_destroy((pthread_mutex_t *)cs->impl);
    free(cs->impl);
    cs->impl = NULL;
}

/* ---- WaitOnAddress: a futex on the aligned 32-bit word that holds the address ---- */
BOOL w32_WaitOnAddress(volatile void *addr, void *compare, SIZE_T size, DWORD ms) {
    volatile int32_t *word = (volatile int32_t *)((uintptr_t)addr & ~(uintptr_t)3);
    const int64_t deadline = ms == INFINITE ? 0 : mono_ns() + (int64_t)ms * 1000000;
    for (;;) {
        const int32_t w = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        if (memcmp((const void *)addr, compare, size)) return TRUE;
        struct timespec t;
        if (ms != INFINITE && !left_until(deadline, &t)) { last_error = ERROR_TIMEOUT; return FALSE; }
        futex_wait(word, w, ms == INFINITE ? NULL : &t);
        if (memcmp((const void *)addr, compare, size)) return TRUE;
        if (ms != INFINITE && !left_until(deadline, &t)) { last_error = ERROR_TIMEOUT; return FALSE; }
    }
}
void w32_WakeByAddressSingle(void *addr) { futex_wake((volatile int32_t *)((uintptr_t)addr & ~(uintptr_t)3), INT_MAX); }
void w32_WakeByAddressAll(void *addr) { futex_wake((volatile int32_t *)((uintptr_t)addr & ~(uintptr_t)3), INT_MAX); }

/* ---- handles ----------------------------------------------------------------------------------------------- */
enum { K_EVENT = 0x45564e54, K_SEMA, K_THREAD, K_FILE, K_FIND };
typedef struct Obj {
    uint32_t kind;
    volatile int32_t refs;
} Obj;
typedef struct {   /* events and semaphores: a count under a mutex */
    Obj o;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int manual;
    LONG count, maximum;   /* (an event: 0 or 1) */
} Waitable;
typedef struct Thread {
    Obj o;
    pthread_t pt;
    volatile pid_t tid;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    Waitable *done;
    DWORD exit_code;
    char name[64];
    volatile int started;
} Thread;
typedef struct {
    Obj o;
    int fd, own;
} File;
typedef struct {
    Obj o;
    DIR *dir;
    char folder[PATH_MAX], pattern[256];
} Find;

static void obj_release(Obj *o);
static Waitable *waitable_new(int manual, LONG count, LONG maximum) {
    Waitable *w = (Waitable *)calloc(1, sizeof *w);
    w->o.kind = maximum ? K_SEMA : K_EVENT;
    w->o.refs = 1;
    pthread_mutex_init(&w->mu, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&w->cv, &a);
    pthread_condattr_destroy(&a);
    w->manual = manual;
    w->count = count;
    w->maximum = maximum;
    return w;
}
HANDLE w32_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
    (void)sa, (void)name;
    return waitable_new(manual, initial ? 1 : 0, 0);
}
HANDLE w32_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name) {
    (void)sa, (void)name;
    return waitable_new(0, initial, maximum > 0 ? maximum : 1);
}
static Waitable *as_waitable(HANDLE h) {
    Obj *o = (Obj *)h;
    return o && h != INVALID_HANDLE_VALUE && (o->kind == K_EVENT || o->kind == K_SEMA) ? (Waitable *)o : NULL;
}
BOOL w32_SetEvent(HANDLE h) {
    Waitable *w = as_waitable(h);
    if (!w) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    pthread_mutex_lock(&w->mu);
    w->count = 1;
    if (w->manual) pthread_cond_broadcast(&w->cv);
    else pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return TRUE;
}
BOOL w32_ResetEvent(HANDLE h) {
    Waitable *w = as_waitable(h);
    if (!w) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    pthread_mutex_lock(&w->mu);
    w->count = 0;
    pthread_mutex_unlock(&w->mu);
    return TRUE;
}
BOOL w32_ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) {
    Waitable *w = as_waitable(h);
    if (!w || n <= 0) { last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    pthread_mutex_lock(&w->mu);
    if (prev) *prev = w->count;
    const int ok = w->count + n <= w->maximum;
    if (ok) {
        w->count += n;
        pthread_cond_broadcast(&w->cv);
    }
    pthread_mutex_unlock(&w->mu);
    return ok;
}
static DWORD waitable_wait(Waitable *w, DWORD ms) {
    struct timespec until;
    if (ms != INFINITE) {
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec += ms / 1000;
        until.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&w->mu);
    while (w->count <= 0) {
        if (ms == 0) break;
        if (ms == INFINITE) pthread_cond_wait(&w->cv, &w->mu);
        else if (pthread_cond_timedwait(&w->cv, &w->mu, &until) == ETIMEDOUT) break;
    }
    const int got = w->count > 0;
    if (got && !w->manual) w->count--;
    pthread_mutex_unlock(&w->mu);
    return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

/* ---- threads ---- */
static _Thread_local Thread *self_thread;
static Thread *thread_new(void) {
    Thread *t = (Thread *)calloc(1, sizeof *t);
    t->o.kind = K_THREAD;
    t->o.refs = 1;
    t->done = waitable_new(1, 0, 0);
    return t;
}
static Thread *current_thread(void) {   /* (a thread CreateThread did not make gets its object on first use) */
    if (!self_thread) {
        Thread *t = thread_new();
        t->pt = pthread_self();
        t->tid = (pid_t)syscall(SYS_gettid);
        t->started = 1;
        t->o.refs = 1 << 30;   /* (never freed: its handle is the pseudo-handle's) */
        self_thread = t;
    }
    return self_thread;
}
static Thread *as_thread(HANDLE h) {
    if (h == GetCurrentThread()) return current_thread();
    Obj *o = (Obj *)h;
    return o && h != INVALID_HANDLE_VALUE && o->kind == K_THREAD ? (Thread *)o : NULL;
}
static void alt_stack(void) {   /* (a stack overflow still reaches the crash report) */
    stack_t ss;
    ss.ss_size = 256 << 10;
    ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ss.ss_flags = 0;
    if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, NULL);
}
static void *thread_start(void *p) {
    Thread *t = (Thread *)p;
    self_thread = t;
    t->tid = (pid_t)syscall(SYS_gettid);
    __atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
    alt_stack();
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
    const DWORD code = t->fn(t->arg);
    t->exit_code = code;
    w32_SetEvent(t->done);
    obj_release(&t->o);
    return NULL;
}
HANDLE w32_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id) {
    (void)sa;
    Thread *t = thread_new();
    t->fn = fn;
    t->arg = arg;
    t->o.refs = 2;   /* (the handle's and the running thread's) */
    t->exit_code = STILL_ACTIVE;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack ? (stack + 0xFFFF) & ~(SIZE_T)0xFFFF : (SIZE_T)8 << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    (void)flags;
    const int r = pthread_create(&t->pt, &a, thread_start, t);
    pthread_attr_destroy(&a);
    if (r) {
        errno = r;
        fail_errno();
        free(t);
        return NULL;
    }
    while (!__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) sched_yield();   /* (its tid, for the affinity and the name) */
    if (id) *id = (DWORD)t->tid;
    return t;
}
void w32_ExitThread(DWORD code) {
    Thread *t = current_thread();
    t->exit_code = code;
    w32_SetEvent(t->done);
    pthread_exit(NULL);
}
BOOL w32_GetExitCodeThread(HANDLE h, DWORD *code) {
    Thread *t = as_thread(h);
    if (!t) return FALSE;
    *code = w32_WaitForSingleObject(t->done, 0) == WAIT_OBJECT_0 ? t->exit_code : STILL_ACTIVE;
    return TRUE;
}
DWORD w32_GetCurrentThreadId(void) { return (DWORD)current_thread()->tid; }
DWORD w32_GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD w32_GetThreadId(HANDLE h) {
    Thread *t = as_thread(h);
    return t ? (DWORD)t->tid : 0;
}
HANDLE w32_OpenThread(DWORD access, BOOL inherit, DWORD id) {
    (void)access, (void)inherit, (void)id;
    last_error = ERROR_NOT_SUPPORTED;
    return NULL;
}
BOOL w32_SetThreadPriority(HANDLE h, int prio) {
    /* (a lower priority only: raising one takes privileges a game should not need) */
    Thread *t = as_thread(h);
    if (t && prio < 0) setpriority(PRIO_PROCESS, (id_t)t->tid, prio <= THREAD_PRIORITY_LOWEST ? 4 : 2);
    return TRUE;
}
DWORD_PTR w32_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    Thread *t = as_thread(h);
    if (!t || !mask) return 0;
    cpu_set_t old, set;
    CPU_ZERO(&old);
    CPU_ZERO(&set);
    sched_getaffinity((pid_t)t->tid, sizeof old, &old);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    if (sched_setaffinity((pid_t)t->tid, sizeof set, &set)) return 0;
    DWORD_PTR was = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &old)) was |= (DWORD_PTR)1 << i;
    return was ? was : 1;
}
BOOL w32_GetProcessAffinityMask(HANDLE h, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)h;
    cpu_set_t s;
    CPU_ZERO(&s);
    if (sched_getaffinity(0, sizeof s, &s)) return FALSE;
    DWORD_PTR m = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &s)) m |= (DWORD_PTR)1 << i;
    *proc = m;
    const int n = w32_processor_count();
    *sys = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    return TRUE;
}
HRESULT w32_SetThreadDescription(HANDLE h, PCWSTR name) {
    Thread *t = as_thread(h);
    if (!t || !name) return E_FAIL;
    w32_WideCharToMultiByte(CP_UTF8, 0, name, -1, t->name, sizeof t->name, NULL, NULL);
    char shortname[16];
    snprintf(shortname, sizeof shortname, "%s", t->name);   /* (Linux keeps 15 characters) */
    if (__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) pthread_setname_np(t->pt, shortname);
    return S_OK;
}
HRESULT w32_GetThreadDescription(HANDLE h, PWSTR *name) {
    Thread *t = as_thread(h);
    if (!t) return E_FAIL;
    *name = (PWSTR)calloc(64, sizeof(wchar_t));
    w32_MultiByteToWideChar(CP_UTF8, 0, t->name, -1, *name, 64);
    return S_OK;
}
static void ns_to_filetime(int64_t ns, FILETIME *ft) {
    const uint64_t t = (uint64_t)ns / 100;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
BOOL w32_GetThreadTimes(HANDLE h, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user) {
    Thread *t = as_thread(h);
    if (!t) return FALSE;
    clockid_t cid;
    struct timespec ts;
    if (pthread_getcpuclockid(t->pt, &cid) || clock_gettime(cid, &ts)) return FALSE;
    memset(create, 0, sizeof *create);
    memset(exit_, 0, sizeof *exit_);
    memset(kernel, 0, sizeof *kernel);
    ns_to_filetime((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec, user);   /* (user and kernel time together) */
    return TRUE;
}
DWORD w32_SuspendThread(HANDLE h) { (void)h; last_error = ERROR_NOT_SUPPORTED; return (DWORD)-1; }
DWORD w32_ResumeThread(HANDLE h) { (void)h; last_error = ERROR_NOT_SUPPORTED; return (DWORD)-1; }
BOOL w32_GetThreadContext(HANDLE h, CONTEXT *c) { (void)h, (void)c; last_error = ERROR_NOT_SUPPORTED; return FALSE; }
BOOL w32_SetThreadContext(HANDLE h, const CONTEXT *c) { (void)h, (void)c; last_error = ERROR_NOT_SUPPORTED; return FALSE; }
DWORD w32_TlsAlloc(void) {
    pthread_key_t k;
    return pthread_key_create(&k, NULL) ? TLS_OUT_OF_INDEXES : (DWORD)k;
}
LPVOID w32_TlsGetValue(DWORD i) { return pthread_getspecific((pthread_key_t)i); }
BOOL w32_TlsSetValue(DWORD i, LPVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }
BOOL w32_TlsFree(DWORD i) { return pthread_key_delete((pthread_key_t)i) == 0; }

/* ---- processors ---- */
int w32_processor_count(void) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
void w32_GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
    si->wProcessorArchitecture = 9;   /* (PROCESSOR_ARCHITECTURE_AMD64) */
    si->dwPageSize = 4096;
    si->dwAllocationGranularity = 65536;
    si->dwNumberOfProcessors = (DWORD)w32_processor_count();
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << si->dwNumberOfProcessors) - 1);
    si->lpMinimumApplicationAddress = (void *)0x10000;
    si->lpMaximumApplicationAddress = (void *)0x7FFFFFFEFFFFULL;
}
/* physical cores from sysfs: each logical processor's siblings */
BOOL w32_GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info, DWORD *len) {
    const int n = w32_processor_count() < 64 ? w32_processor_count() : 64;
    uint64_t cores[64];
    int ncores = 0;
    for (int cpu = 0; cpu < n; cpu++) {
        char path[128], text[256];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        FILE *f = fopen(path, "r");
        if (!f) return FALSE;
        const int ok = fgets(text, sizeof text, f) != NULL;
        fclose(f);
        if (!ok) return FALSE;
        uint64_t m = 0;
        for (char *p = text; *p;) {   /* "0,8" or "0-1" */
            char *end;
            long a = strtol(p, &end, 10), b = a;
            if (end == p) break;
            if (*end == '-') b = strtol(end + 1, &end, 10);
            for (long i = a; i <= b && i < 64; i++) m |= 1ULL << i;
            p = *end ? end + 1 : end;
        }
        int seen = 0;
        for (int i = 0; i < ncores; i++) seen |= cores[i] == m;
        if (!seen && ncores < 64) cores[ncores++] = m;
    }
    const DWORD need = (DWORD)(ncores * sizeof *info);
    if (*len < need) { *len = need; last_error = 122; return FALSE; }   /* (ERROR_INSUFFICIENT_BUFFER) */
    for (int i = 0; i < ncores; i++) {
        memset(&info[i], 0, sizeof info[i]);
        info[i].ProcessorMask = cores[i];
        info[i].Relationship = RelationProcessorCore;
    }
    *len = need;
    return TRUE;
}

/* ---- fibers: each its own stack; the switch saves what the System V ABI keeps across a call ---- */
typedef struct Fiber {
    void *param;        /* (first: GetFiberData) */
    void *sp;           /* the saved stack pointer while not running */
    void *stack;
    size_t stack_size;
    LPFIBER_START_ROUTINE fn;
    int is_thread;
} Fiber;
static _Thread_local Fiber *cur_fiber;
/* fiber_switch(&from->sp, to->sp): pushes the callee-saved registers, the MXCSR and the x87 control word, saves rsp,
   takes the other's and pops its */
__attribute__((naked)) static void fiber_switch(void **save, void *to) {
    __asm__ volatile(
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "subq $16, %rsp\n\t"
        "stmxcsr 0(%rsp)\n\t"
        "fnstcw 4(%rsp)\n\t"
        "movq %rsp, (%rdi)\n\t"
        "movq %rsi, %rsp\n\t"
        "ldmxcsr 0(%rsp)\n\t"
        "fldcw 4(%rsp)\n\t"
        "addq $16, %rsp\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rbx\n\t"
        "popq %rbp\n\t"
        "ret\n\t");
}
__attribute__((used, noreturn)) void w32_fiber_entry(void) {
    Fiber *f = cur_fiber;
    f->fn(f->param);
    /* (a fiber function that returns ends its thread, as on Windows) */
    w32_ExitThread(0);
}
__attribute__((naked)) static void fiber_trampoline(void) {
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call w32_fiber_entry\n\t"
                     "ud2\n\t");
}
LPVOID w32_ConvertThreadToFiberEx(LPVOID param, DWORD flags) {
    (void)flags;
    if (cur_fiber) return cur_fiber;
    Fiber *f = (Fiber *)calloc(1, sizeof *f);
    f->param = param;
    f->is_thread = 1;
    cur_fiber = f;
    return f;
}
BOOL w32_ConvertFiberToThread(void) {
    if (!cur_fiber || !cur_fiber->is_thread) return FALSE;
    free(cur_fiber);
    cur_fiber = NULL;
    return TRUE;
}
LPVOID w32_CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param) {
    (void)flags;
    size_t size = reserve > commit ? reserve : commit;
    if (size < (1 << 20)) size = 1 << 20;
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t *mem = (uint8_t *)mmap(NULL, size + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) { fail_errno(); return NULL; }
    mprotect(mem, 4096, PROT_NONE);   /* (a guard page below the stack) */
    Fiber *f = (Fiber *)calloc(1, sizeof *f);
    f->param = param;
    f->fn = fn;
    f->stack = mem;
    f->stack_size = size + 4096;
    /* the first switch to it pops 16 bytes of MXCSR / control word, six registers, then returns to the trampoline */
    uint64_t *sp = (uint64_t *)(mem + size + 4096);
    *--sp = 0;                                   /* (alignment) */
    *--sp = (uint64_t)(uintptr_t)fiber_trampoline;
    for (int i = 0; i < 6; i++) *--sp = 0;       /* rbp rbx r12 r13 r14 r15 */
    sp -= 2;
    uint32_t csr;
    uint16_t cw;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    memcpy(sp, &csr, 4);
    memcpy((uint8_t *)sp + 4, &cw, 2);
    f->sp = sp;
    return f;
}
void w32_SwitchToFiber(LPVOID fiber) {
    Fiber *to = (Fiber *)fiber, *from = cur_fiber;
    if (!from || to == from) return;
    cur_fiber = to;
    fiber_switch(&from->sp, to->sp);
}
void w32_DeleteFiber(LPVOID fiber) {
    Fiber *f = (Fiber *)fiber;
    if (!f) return;
    if (f == cur_fiber) w32_ExitThread(0);
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}
LPVOID w32_GetCurrentFiber(void) { return cur_fiber; }

/* ---- handle closing and waiting, for every kind ---- */
static void obj_release(Obj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    switch (o->kind) {
    case K_EVENT:
    case K_SEMA: {
        Waitable *w = (Waitable *)o;
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        break;
    }
    case K_THREAD:
        obj_release(&((Thread *)o)->done->o);
        break;
    case K_FILE:
        if (((File *)o)->own) close(((File *)o)->fd);
        break;
    case K_FIND:
        if (((Find *)o)->dir) closedir(((Find *)o)->dir);
        break;
    }
    o->kind = 0;
    free(o);
}
BOOL w32_CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == GetCurrentThread()) return TRUE;
    Obj *o = (Obj *)h;
    if (o->kind < K_EVENT || o->kind > K_FIND) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    obj_release(o);
    return TRUE;
}
BOOL w32_DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD options) {
    (void)sp, (void)tp, (void)access, (void)inherit, (void)options;
    Obj *o = src == GetCurrentThread() ? &current_thread()->o : (Obj *)src;
    if (!o || src == INVALID_HANDLE_VALUE) return FALSE;
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    *out = o;
    return TRUE;
}
DWORD w32_WaitForSingleObject(HANDLE h, DWORD ms) {
    if (h == GetCurrentThread()) { w32_Sleep(ms); return WAIT_TIMEOUT; }
    Obj *o = (Obj *)h;
    if (!o || h == INVALID_HANDLE_VALUE) { last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
    switch (o->kind) {
    case K_EVENT:
    case K_SEMA: return waitable_wait((Waitable *)o, ms);
    case K_THREAD: return waitable_wait(((Thread *)o)->done, ms);
    default: last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED;
    }
}
/* (several handles: polled, as nothing here waits on more than a few, and rarely) */
DWORD w32_WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    const int64_t deadline = ms == INFINITE ? INT64_MAX : mono_ns() + (int64_t)ms * 1000000;
    for (int64_t step = 0;; step++) {
        if (all) {
            DWORD k = 0;
            for (; k < n; k++)
                if (w32_WaitForSingleObject(hs[k], 0) != WAIT_OBJECT_0) break;
            if (k == n) return WAIT_OBJECT_0;
        } else {
            for (DWORD k = 0; k < n; k++)
                if (w32_WaitForSingleObject(hs[k], 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + k;
        }
        if (mono_ns() >= deadline) return WAIT_TIMEOUT;
        if (n) w32_WaitForSingleObject(hs[0], step < 100 ? 0 : 1);
        if (step < 100) sched_yield();
    }
}

/* ---- virtual memory ------------------------------------------------------------------------------------------ */
static const uint8_t *window_base;
static uint64_t window_size;
void w32_note_window(const void *base, uint64_t size) { window_base = (const uint8_t *)base, window_size = size; }
static int prot_of(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}
/* reservations made here, for MEM_RELEASE (which names only the base) */
static struct { uintptr_t base; size_t size; } reservations[1024];
static SRWLOCK reservations_lock = SRWLOCK_INIT;
LPVOID w32_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    if (!size) { last_error = ERROR_INVALID_PARAMETER; return NULL; }
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)4095, hi = ((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095;
    if (type & MEM_RESET) {
        madvise((void *)lo, hi - lo, MADV_FREE);
        return addr;
    }
    if (type & MEM_RESERVE) {
        const int prot = type & MEM_COMMIT ? prot_of(protect) : PROT_NONE;
        const size_t len = (size + 0xFFFF) & ~(size_t)0xFFFF;
        void *p = mmap(addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
        if (p == MAP_FAILED) { fail_errno(); return NULL; }
        if (addr && p != addr) { munmap(p, len); last_error = ERROR_INVALID_PARAMETER; return NULL; }
        w32_AcquireSRWLockExclusive(&reservations_lock);
        for (int i = 0; i < 1024; i++)
            if (!reservations[i].base) { reservations[i].base = (uintptr_t)p, reservations[i].size = len; break; }
        w32_ReleaseSRWLockExclusive(&reservations_lock);
        return p;
    }
    if (type & MEM_COMMIT) {
        if (!addr) return w32_VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, protect);
        if (mprotect((void *)lo, hi - lo, prot_of(protect))) { fail_errno(); return NULL; }
        return addr;
    }
    last_error = ERROR_INVALID_PARAMETER;
    return NULL;
}
BOOL w32_VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    if (type & MEM_RELEASE) {
        size_t len = 0;
        w32_AcquireSRWLockExclusive(&reservations_lock);
        for (int i = 0; i < 1024; i++)
            if (reservations[i].base == (uintptr_t)addr) { len = reservations[i].size, reservations[i].base = 0; break; }
        w32_ReleaseSRWLockExclusive(&reservations_lock);
        if (!len) { last_error = ERROR_INVALID_PARAMETER; return FALSE; }
        return munmap(addr, len) == 0;
    }
    if (type & MEM_DECOMMIT) {
        /* (the pages are given back and read as zeros again; they stay accessible, as the guest window's pages are
           on Windows, where the first touch commits them again) */
        const uintptr_t lo = ((uintptr_t)addr + 4095) & ~(uintptr_t)4095, hi = ((uintptr_t)addr + size) & ~(uintptr_t)4095;
        if (hi > lo) madvise((void *)lo, hi - lo, MADV_DONTNEED);
        return TRUE;
    }
    last_error = ERROR_INVALID_PARAMETER;
    return FALSE;
}
BOOL w32_VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, DWORD *old) {
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)4095, hi = ((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095;
    if (old) *old = PAGE_READWRITE;   /* (not tracked: the callers restore what they set themselves) */
    return mprotect((void *)lo, hi - lo, prot_of(protect)) == 0 ? TRUE : fail_errno();
}
SIZE_T w32_VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len) {
    if (len < sizeof *mbi) return 0;
    memset(mbi, 0, sizeof *mbi);
    const uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095;
    if (window_base && a >= (uintptr_t)window_base && a < (uintptr_t)window_base + window_size) {
        /* the guest window: read-write throughout (hos_main.c maps it whole; untouched pages read as zeros) */
        mbi->BaseAddress = (void *)a;
        mbi->AllocationBase = (void *)window_base;
        mbi->AllocationProtect = mbi->Protect = PAGE_READWRITE;
        mbi->RegionSize = (uintptr_t)window_base + window_size - a;
        mbi->State = MEM_COMMIT;
        mbi->Type = MEM_PRIVATE;
        return sizeof *mbi;
    }
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t prev_end = 0, next_start = UINTPTR_MAX;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, e;
        char perm[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perm) != 3) continue;
        if (a >= s && a < e) {
            mbi->BaseAddress = (void *)a;
            mbi->AllocationBase = (void *)(uintptr_t)s;
            mbi->RegionSize = (SIZE_T)(e - a);
            const int r = perm[0] == 'r', w = perm[1] == 'w', x = perm[2] == 'x';
            mbi->Protect = !r && !w && !x ? PAGE_NOACCESS
                           : x ? (w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                               : (w ? PAGE_READWRITE : PAGE_READONLY);
            mbi->AllocationProtect = mbi->Protect;
            mbi->State = mbi->Protect == PAGE_NOACCESS ? MEM_RESERVE : MEM_COMMIT;
            mbi->Type = strchr(line, '/') ? MEM_MAPPED : MEM_PRIVATE;
            found = 1;
            break;
        }
        if (e <= a) prev_end = e;
        else if (s > a && s < next_start) { next_start = s; break; }
    }
    fclose(f);
    if (!found) {   /* a gap: free up to the next mapping */
        mbi->BaseAddress = (void *)a;
        mbi->AllocationBase = NULL;
        mbi->RegionSize = (next_start == UINTPTR_MAX ? (uintptr_t)0x800000000000ULL : next_start) - a;
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
        (void)prev_end;
        if (a >= 0x800000000000ULL) return 0;
    }
    return sizeof *mbi;
}

/* ---- exceptions: the signals as Windows exceptions, through the vectored handlers then the unhandled filter ---- */
typedef struct VEH { PVECTORED_EXCEPTION_HANDLER fn; struct VEH *next; } VEH;
static VEH *volatile veh_list;
static SRWLOCK veh_lock = SRWLOCK_INIT;
static LPTOP_LEVEL_EXCEPTION_FILTER unhandled_filter;
static volatile int handlers_installed;
static void on_signal(int sig, siginfo_t *si, void *uc_);
static void install_handlers(void) {
    if (__atomic_exchange_n(&handlers_installed, 1, __ATOMIC_ACQ_REL)) return;
    alt_stack();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}
PVOID w32_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn) {
    VEH *v = (VEH *)calloc(1, sizeof *v);
    v->fn = fn;
    w32_AcquireSRWLockExclusive(&veh_lock);
    /* (the list is only ever added to while the game runs: a handler walks it without the lock, as a signal may come
       to a thread that holds it) */
    if (first || !veh_list) {
        v->next = veh_list;
        __atomic_store_n(&veh_list, v, __ATOMIC_RELEASE);
    } else {
        VEH *t = veh_list;
        while (t->next) t = t->next;
        __atomic_store_n(&t->next, v, __ATOMIC_RELEASE);
    }
    w32_ReleaseSRWLockExclusive(&veh_lock);
    install_handlers();
    return v;
}
ULONG w32_RemoveVectoredExceptionHandler(PVOID h) {
    VEH *v = (VEH *)h;
    if (v) v->fn = NULL;   /* (left in the list: a handler may be walking it) */
    return v != NULL;
}
LPTOP_LEVEL_EXCEPTION_FILTER w32_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER fn) {
    LPTOP_LEVEL_EXCEPTION_FILTER old = unhandled_filter;
    unhandled_filter = fn;
    install_handlers();
    return old;
}
static void ctx_from(CONTEXT *c, const ucontext_t *uc) {
    const greg_t *g = uc->uc_mcontext.gregs;
    memset(c, 0, sizeof *c);
    c->ContextFlags = CONTEXT_FULL;
    c->Rax = g[REG_RAX], c->Rcx = g[REG_RCX], c->Rdx = g[REG_RDX], c->Rbx = g[REG_RBX];
    c->Rsp = g[REG_RSP], c->Rbp = g[REG_RBP], c->Rsi = g[REG_RSI], c->Rdi = g[REG_RDI];
    c->R8 = g[REG_R8], c->R9 = g[REG_R9], c->R10 = g[REG_R10], c->R11 = g[REG_R11];
    c->R12 = g[REG_R12], c->R13 = g[REG_R13], c->R14 = g[REG_R14], c->R15 = g[REG_R15];
    c->Rip = g[REG_RIP];
    c->EFlags = (DWORD)g[REG_EFL];
    c->native = (void *)uc;
}
static void ctx_to(const CONTEXT *c, ucontext_t *uc) {
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = c->Rax, g[REG_RCX] = c->Rcx, g[REG_RDX] = c->Rdx, g[REG_RBX] = c->Rbx;
    g[REG_RSP] = c->Rsp, g[REG_RBP] = c->Rbp, g[REG_RSI] = c->Rsi, g[REG_RDI] = c->Rdi;
    g[REG_R8] = c->R8, g[REG_R9] = c->R9, g[REG_R10] = c->R10, g[REG_R11] = c->R11;
    g[REG_R12] = c->R12, g[REG_R13] = c->R13, g[REG_R14] = c->R14, g[REG_R15] = c->R15;
    g[REG_RIP] = c->Rip;
    g[REG_EFL] = c->EFlags;
}
/* the guard of w32_guarded on this thread, if one runs */
static _Thread_local sigjmp_buf *guard;
static _Thread_local int guard_access_only;
int w32_guarded(void (*fn)(void *), void *arg, int access_only) {
    sigjmp_buf here, *outer = guard;
    const int outer_access = guard_access_only;
    install_handlers();
    if (sigsetjmp(here, 1)) {
        guard = outer, guard_access_only = outer_access;
        return 0;
    }
    guard = &here, guard_access_only = access_only;
    fn(arg);
    guard = outer, guard_access_only = outer_access;
    return 1;
}
static void on_signal(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = (ucontext_t *)uc_;
    const int saved_errno = errno;
    if (guard && (!guard_access_only || sig == SIGSEGV || sig == SIGBUS)) siglongjmp(*guard, 1);
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    memset(&rec, 0, sizeof rec);
    ctx_from(&ctx, uc);
    rec.ExceptionAddress = (void *)ctx.Rip;
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = uc->uc_mcontext.gregs[REG_ERR] & 2 ? 1 : 0;   /* (the page fault's write bit) */
        rec.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
        break;
    case SIGILL: rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION; break;
    case SIGFPE: rec.ExceptionCode = si->si_code == FPE_INTDIV ? EXCEPTION_INT_DIVIDE_BY_ZERO : EXCEPTION_FLT_INVALID_OPERATION; break;
    default: rec.ExceptionCode = si->si_code == TRAP_BRKPT ? EXCEPTION_BREAKPOINT : EXCEPTION_SINGLE_STEP; break;
    }
    EXCEPTION_POINTERS ep = {&rec, &ctx};
    for (VEH *v = __atomic_load_n(&veh_list, __ATOMIC_ACQUIRE); v; v = __atomic_load_n(&v->next, __ATOMIC_ACQUIRE)) {
        if (v->fn && v->fn(&ep) == EXCEPTION_CONTINUE_EXECUTION) {
            ctx_to(&ctx, uc);
            errno = saved_errno;
            return;
        }
    }
    LONG r = EXCEPTION_CONTINUE_SEARCH;
    if (unhandled_filter) r = unhandled_filter(&ep);
    if (r == EXCEPTION_CONTINUE_EXECUTION) {
        ctx_to(&ctx, uc);
        errno = saved_errno;
        return;
    }
    if (r == EXCEPTION_EXECUTE_HANDLER) _exit(3);
    /* nothing took it: the signal's own action, on the instruction again */
    fflush(stderr);
    signal(sig, SIG_DFL);
    if (sig == SIGTRAP) raise(sig);
}
void w32_RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) {
    (void)flags, (void)n, (void)args;
    fprintf(stderr, "[w32] exception %#x raised\n", code);
    fflush(stderr);
    abort();
}
WORD w32_RtlCaptureStackBackTrace(DWORD skip, DWORD n, PVOID *frames, DWORD *hash) {
    void *buf[128];
    int got = backtrace(buf, 128);
    WORD k = 0;
    for (int i = (int)skip + 1; i < got && k < n; i++) frames[k++] = buf[i];
    if (hash) *hash = 0;
    return k;
}
void w32_ExitProcess(UINT code) {
    fflush(NULL);
    _exit((int)code);
}

/* ---- files ----------------------------------------------------------------------------------------------------- */
static int exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}
int w32_host_path(const char *path, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = path; *p && n + 1 < cap; p++) out[n++] = *p == '\\' ? '/' : *p;
    out[n] = 0;
    if (!n || exists(out)) return (int)n;
    /* (each folder as far as it exists, its entry found without case; the rest as written) */
    char built[PATH_MAX];
    size_t b = 0;
    const char *s = out;
    if (*s == '/') built[b++] = '/', s++;
    built[b] = 0;
    char rest[PATH_MAX];
    snprintf(rest, sizeof rest, "%s", s);
    char *save = NULL;
    for (char *part = strtok_r(rest, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        char cand[PATH_MAX];
        snprintf(cand, sizeof cand, "%s%s%s", built, b && built[b - 1] != '/' ? "/" : "", part);
        if (!exists(cand) && strcmp(part, ".") && strcmp(part, "..")) {
            DIR *d = opendir(b ? built : ".");
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL)
                    if (!strcasecmp(e->d_name, part)) {
                        snprintf(cand, sizeof cand, "%s%s%s", built, b && built[b - 1] != '/' ? "/" : "", e->d_name);
                        break;
                    }
                closedir(d);
            }
        }
        b = (size_t)snprintf(built, sizeof built, "%s", cand);
    }
    if (out[n - 1] == '/' && b + 1 < sizeof built) built[b++] = '/', built[b] = 0;
    return snprintf(out, cap, "%s", built);
}
static File *file_new(int fd, int own) {
    File *f = (File *)calloc(1, sizeof *f);
    f->o.kind = K_FILE;
    f->o.refs = 1;
    f->fd = fd;
    f->own = own;
    return f;
}
static File *as_file(HANDLE h) {
    Obj *o = (Obj *)h;
    return o && h != INVALID_HANDLE_VALUE && o->kind == K_FILE ? (File *)o : NULL;
}
int w32_fd(HANDLE h) {
    File *f = as_file(h);
    return f ? f->fd : -1;
}
HANDLE w32_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share, (void)sa, (void)tmpl;
    char p[PATH_MAX];
    w32_host_path(path, p, sizeof p);
    int o = (access & (GENERIC_WRITE | GENERIC_ALL)) ? ((access & (GENERIC_READ | GENERIC_ALL)) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: o |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: o |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: o |= O_CREAT; break;
    case TRUNCATE_EXISTING: o |= O_TRUNC; break;
    default: break;
    }
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) { last_error = ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
        o = O_RDONLY | O_DIRECTORY;
    }
    const int fd = open(p, o | O_CLOEXEC, 0644);
    if (fd < 0) {
        last_error = errno == EEXIST ? ERROR_FILE_EXISTS : from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    if (disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) last_error = 0;
    return file_new(fd, 1);
}
BOOL w32_ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, OVERLAPPED *ov) {
    File *f = as_file(h);
    if (got) *got = 0;
    if (!f) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    size_t done = 0;
    off_t at = ov ? (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset) : -1;
    while (done < n) {
        const ssize_t r = at >= 0 ? pread(f->fd, (uint8_t *)buf + done, n - done, at + (off_t)done)
                                  : read(f->fd, (uint8_t *)buf + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (got) *got = (DWORD)done;
            return fail_errno();
        }
        if (r == 0) break;
        done += (size_t)r;
    }
    if (got) *got = (DWORD)done;
    return TRUE;
}
BOOL w32_WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, OVERLAPPED *ov) {
    File *f = as_file(h);
    if (put) *put = 0;
    if (!f) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    size_t done = 0;
    off_t at = ov ? (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset) : -1;
    while (done < n) {
        const ssize_t r = at >= 0 ? pwrite(f->fd, (const uint8_t *)buf + done, n - done, at + (off_t)done)
                                  : write(f->fd, (const uint8_t *)buf + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (put) *put = (DWORD)done;
            return fail_errno();
        }
        done += (size_t)r;
    }
    if (put) *put = (DWORD)done;
    return TRUE;
}
BOOL w32_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method) {
    File *f = as_file(h);
    if (!f) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    const off_t r = lseek(f->fd, (off_t)dist.QuadPart, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return fail_errno();
    if (pos) pos->QuadPart = r;
    return TRUE;
}
DWORD w32_SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method) {
    LARGE_INTEGER d, p;
    d.QuadPart = high ? ((int64_t)*high << 32 | (uint32_t)dist) : dist;
    if (!w32_SetFilePointerEx(h, d, &p, method)) return INVALID_SET_FILE_POINTER;
    if (high) *high = (LONG)(p.QuadPart >> 32);
    return (DWORD)p.QuadPart;
}
BOOL w32_SetEndOfFile(HANDLE h) {
    File *f = as_file(h);
    if (!f) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    const off_t at = lseek(f->fd, 0, SEEK_CUR);
    return at >= 0 && ftruncate(f->fd, at) == 0 ? TRUE : fail_errno();
}
BOOL w32_GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    File *f = as_file(h);
    struct stat st;
    if (!f || fstat(f->fd, &st)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    size->QuadPart = st.st_size;
    return TRUE;
}
DWORD w32_GetFileSize(HANDLE h, DWORD *high) {
    LARGE_INTEGER s;
    if (!w32_GetFileSizeEx(h, &s)) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(s.QuadPart >> 32);
    return (DWORD)s.QuadPart;
}
BOOL w32_FlushFileBuffers(HANDLE h) {
    File *f = as_file(h);
    return f && fsync(f->fd) == 0;
}
static DWORD attributes_of(const struct stat *st, const char *name) {
    DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    if (name && name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}
static void filetime_of(const struct timespec *ts, FILETIME *ft) {
    const uint64_t t = (uint64_t)ts->tv_sec * 10000000ULL + (uint64_t)ts->tv_nsec / 100 + UNIX_TO_FILETIME;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
DWORD w32_GetFileAttributesA(LPCSTR path) {
    char p[PATH_MAX];
    struct stat st;
    w32_host_path(path, p, sizeof p);
    if (stat(p, &st)) { fail_errno(); return INVALID_FILE_ATTRIBUTES; }
    const char *base = strrchr(p, '/');
    return attributes_of(&st, base ? base + 1 : p);
}
BOOL w32_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out) {
    (void)level;
    char p[PATH_MAX];
    struct stat st;
    w32_host_path(path, p, sizeof p);
    if (stat(p, &st)) return fail_errno();
    WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)out;
    d->dwFileAttributes = attributes_of(&st, NULL);
    filetime_of(&st.st_ctim, &d->ftCreationTime);
    filetime_of(&st.st_atim, &d->ftLastAccessTime);
    filetime_of(&st.st_mtim, &d->ftLastWriteTime);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
BOOL w32_DeleteFileA(LPCSTR path) {
    char p[PATH_MAX];
    w32_host_path(path, p, sizeof p);
    return unlink(p) == 0 ? TRUE : fail_errno();
}
BOOL w32_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa) {
    (void)sa;
    char p[PATH_MAX];
    w32_host_path(path, p, sizeof p);
    if (mkdir(p, 0755) == 0) return TRUE;
    last_error = errno == EEXIST ? ERROR_ALREADY_EXISTS : from_errno(errno);
    return FALSE;
}
BOOL w32_RemoveDirectoryA(LPCSTR path) {
    char p[PATH_MAX];
    w32_host_path(path, p, sizeof p);
    return rmdir(p) == 0 ? TRUE : fail_errno();
}
BOOL w32_MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    char a[PATH_MAX], b[PATH_MAX];
    w32_host_path(from, a, sizeof a);
    w32_host_path(to, b, sizeof b);
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && exists(b)) { last_error = ERROR_ALREADY_EXISTS; return FALSE; }
    return rename(a, b) == 0 ? TRUE : fail_errno();
}
BOOL w32_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    char a[PATH_MAX], b[PATH_MAX];
    w32_host_path(from, a, sizeof a);
    w32_host_path(to, b, sizeof b);
    const int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail_errno();
    const int out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) { close(in); return fail_errno(); }
    char buf[1 << 16];
    ssize_t r;
    BOOL ok = TRUE;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)r) != r) { ok = fail_errno(); break; }
    close(in);
    close(out);
    return ok;
}
static BOOL find_next(Find *f, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(f->dir)) != NULL) {
        if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD) && strcmp(f->pattern, "*.*")) continue;
        char full[PATH_MAX + 260];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->folder, e->d_name);
        if (stat(full, &st)) continue;
        memset(fd, 0, sizeof *fd);
        fd->dwFileAttributes = attributes_of(&st, e->d_name);
        filetime_of(&st.st_ctim, &fd->ftCreationTime);
        filetime_of(&st.st_atim, &fd->ftLastAccessTime);
        filetime_of(&st.st_mtim, &fd->ftLastWriteTime);
        fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        fd->nFileSizeLow = (DWORD)st.st_size;
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return TRUE;
    }
    last_error = ERROR_NO_MORE_FILES;
    return FALSE;
}
HANDLE w32_FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    char p[PATH_MAX];
    size_t n = 0;
    for (const char *s = pattern; *s && n + 1 < sizeof p; s++) p[n++] = *s == '\\' ? '/' : *s;
    p[n] = 0;
    Find *f = (Find *)calloc(1, sizeof *f);
    f->o.kind = K_FIND;
    f->o.refs = 1;
    char *slash = strrchr(p, '/');
    if (slash) {
        *slash = 0;
        snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1);
        w32_host_path(p[0] ? p : "/", f->folder, sizeof f->folder);
    } else {
        snprintf(f->pattern, sizeof f->pattern, "%s", p);
        snprintf(f->folder, sizeof f->folder, ".");
    }
    f->dir = opendir(f->folder);
    if (!f->dir) {
        fail_errno();
        free(f);
        if (last_error == ERROR_FILE_NOT_FOUND) last_error = ERROR_PATH_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    if (!find_next(f, fd)) {
        closedir(f->dir);
        free(f);
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}
BOOL w32_FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {
    Obj *o = (Obj *)h;
    if (!o || h == INVALID_HANDLE_VALUE || o->kind != K_FIND) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return find_next((Find *)o, fd);
}
BOOL w32_FindClose(HANDLE h) { return w32_CloseHandle(h); }
HANDLE w32_GetStdHandle(DWORD which) {
    static File *std[3];
    const int i = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
    if (i < 0) return INVALID_HANDLE_VALUE;
    if (!std[i]) {
        File *f = file_new(i, 0);
        f->o.refs = 1 << 30;
        File *none = NULL;
        if (!__atomic_compare_exchange_n(&std[i], &none, f, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) free(f);
    }
    return std[i];
}
DWORD w32_GetFileType(HANDLE h) {
    File *f = as_file(h);
    struct stat st;
    if (!f || fstat(f->fd, &st)) return FILE_TYPE_UNKNOWN;
    return S_ISCHR(st.st_mode) ? FILE_TYPE_CHAR : S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode) ? FILE_TYPE_PIPE : FILE_TYPE_DISK;
}
BOOL w32_GetConsoleMode(HANDLE h, DWORD *mode) {
    File *f = as_file(h);
    if (!f || !isatty(f->fd)) return FALSE;
    if (mode) *mode = 0;
    return TRUE;
}
DWORD w32_GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file) {
    char p[PATH_MAX], r[PATH_MAX];
    w32_host_path(path, p, sizeof p);
    if (!realpath(p, r)) {
        if (p[0] == '/') snprintf(r, sizeof r, "%s", p);
        else {
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
            snprintf(r, sizeof r, "%s/%s", cwd, p);
        }
    }
    const DWORD len = (DWORD)strlen(r);
    if (len + 1 > n) return len + 1;
    memcpy(out, r, len + 1);
    if (file) {
        char *s = strrchr(out, '/');
        *file = s ? s + 1 : out;
    }
    return len;
}
DWORD w32_GetCurrentDirectoryA(DWORD n, LPSTR out) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    const DWORD len = (DWORD)strlen(cwd);
    if (len + 1 > n) return len + 1;
    memcpy(out, cwd, len + 1);
    return len;
}
DWORD w32_GetTempPathA(DWORD n, LPSTR out) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    const int len = snprintf(out, n, "%s/", t);
    return (DWORD)len;
}
DWORD w32_GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD n) {
    const char *v = getenv(name);
    if (!v) { last_error = 203; return 0; }   /* (ERROR_ENVVAR_NOT_FOUND) */
    const DWORD len = (DWORD)strlen(v);
    if (len + 1 > n) return len + 1;
    memcpy(out, v, len + 1);
    return len;
}

/* ---- modules -------------------------------------------------------------------------------------------------- */
static const char *exe_path(void) {
    static char path[PATH_MAX];
    if (!path[0]) {
        const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        path[n > 0 ? n : 0] = 0;
    }
    return path;
}
/* Windows' names for the libraries the code asks for: the ones that have a Linux counterpart, beside the exe or
   the system's; Windows' own system DLLs have none (the callers go on without them) */
HMODULE w32_LoadLibraryA(LPCSTR name) {
    if (!name) return NULL;
    char path[PATH_MAX];
    w32_host_path(name, path, sizeof path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const size_t dir_len = (size_t)(base - path);
    const size_t len = strlen(base);
    char want[PATH_MAX];
    if (len > 4 && !strcasecmp(base + len - 4, ".dll")) {
        char stem[256];
        snprintf(stem, sizeof stem, "%.*s", (int)(len - 4), base);
        static const struct { const char *dll, *so; } names[] = {
            {"SDL2", "libSDL2-2.0.so.0"}, {"vulkan-1", "libvulkan.so.1"}, {"hookshot_vk", "libhookshot_vk.so"},
            {"mkw_vk", "libhookshot_vk.so"}, {"renderdoc", "librenderdoc.so"},
        };
        const char *so = NULL;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcasecmp(stem, names[i].dll)) so = names[i].so;
        if (!so) { last_error = ERROR_FILE_NOT_FOUND; return NULL; }
        if (dir_len) snprintf(want, sizeof want, "%.*s%s", (int)dir_len, path, so);
        else if (!strncmp(so, "libhookshot", 11)) {   /* (ours: beside the executable) */
            const char *e = exe_path(), *s = strrchr(e, '/');
            snprintf(want, sizeof want, "%.*s/%s", (int)(s ? s - e : 0), e, so);
        } else snprintf(want, sizeof want, "%s", so);
    } else {
        snprintf(want, sizeof want, "%s", path);
    }
    void *h = dlopen(want, RTLD_NOW | RTLD_LOCAL);
    if (!h && dir_len) {   /* (a path beside the exe that is not there: the system's copy) */
        const char *b = strrchr(want, '/');
        h = dlopen(b ? b + 1 : want, RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) {
        if (getenv("HOS_DLOPEN_LOG")) fprintf(stderr, "[w32] %s: %s\n", want, dlerror());
        last_error = ERROR_FILE_NOT_FOUND;
    }
    return h;
}
BOOL w32_FreeLibrary(HMODULE m) { return m && dlclose(m) == 0; }
static void *self_module(void) {
    static void *self;
    if (!self) self = dlopen(NULL, RTLD_NOW);
    return self;
}
FARPROC w32_GetProcAddress(HMODULE m, LPCSTR name) {
    if (!m) return NULL;
    return (FARPROC)dlsym(m, name);
}
HMODULE w32_GetModuleHandleA(LPCSTR name) {
    if (!name) return self_module();
    last_error = ERROR_FILE_NOT_FOUND;   /* (Windows' own DLLs: none here) */
    return NULL;
}
/* (with FROM_ADDRESS: the module's load address, as Windows' HMODULE is its base) */
BOOL w32_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        Dl_info di;
        if (!dladdr((const void *)name, &di) || !di.dli_fbase) { *out = NULL; return FALSE; }
        *out = di.dli_fbase;
        return TRUE;
    }
    *out = w32_GetModuleHandleA(name);
    return *out != NULL;
}
DWORD w32_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) {
    const char *path = NULL;
    Dl_info di;
    struct link_map *lm = NULL;
    if (!m || m == self_module()) path = exe_path();
    else if (dladdr(m, &di) && di.dli_fname && di.dli_fbase == m) path = di.dli_fname;   /* (a base address) */
    else if (dlinfo(m, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_name) path = lm->l_name;   /* (a dlopen handle) */
    if (!path || !n) return 0;
    if (path[0] != '/' && dladdr(m, &di) && di.dli_fbase == m && !strcmp(path, di.dli_fname)) {}
    if (!path[0]) path = exe_path();
    const DWORD len = (DWORD)strlen(path);
    const DWORD c = len < n - 1 ? len : n - 1;
    memcpy(out, path, c);
    out[c] = 0;
    return c;
}

/* ---- strings: UTF-8 and wchar_t (UTF-32 here) ---- */
int w32_MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int n, LPWSTR dst, int cap) {
    (void)cp, (void)flags;
    const unsigned char *s = (const unsigned char *)src;
    const int len = n < 0 ? (int)strlen(src) + 1 : n;
    int k = 0;
    for (int i = 0; i < len;) {
        uint32_t c = s[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        i++;
        for (int e = 0; e < extra && i < len; e++, i++) c = c << 6 | (s[i] & 0x3F);
        if (cap) {
            if (k >= cap) return 0;
            dst[k] = (wchar_t)c;
        }
        k++;
    }
    return k;
}
int w32_WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int n, LPSTR dst, int cap, LPCSTR def, LPBOOL used) {
    (void)cp, (void)flags, (void)def;
    if (used) *used = FALSE;
    const int len = n < 0 ? (int)wcslen(src) + 1 : n;
    int k = 0;
    for (int i = 0; i < len; i++) {
        uint32_t c = (uint32_t)src[i];
        unsigned char b[4];
        int m;
        if (c < 0x80) b[0] = (unsigned char)c, m = 1;
        else if (c < 0x800) b[0] = (unsigned char)(0xC0 | c >> 6), b[1] = (unsigned char)(0x80 | (c & 0x3F)), m = 2;
        else if (c < 0x10000) b[0] = (unsigned char)(0xE0 | c >> 12), b[1] = (unsigned char)(0x80 | (c >> 6 & 0x3F)), b[2] = (unsigned char)(0x80 | (c & 0x3F)), m = 3;
        else b[0] = (unsigned char)(0xF0 | c >> 18), b[1] = (unsigned char)(0x80 | (c >> 12 & 0x3F)), b[2] = (unsigned char)(0x80 | (c >> 6 & 0x3F)), b[3] = (unsigned char)(0x80 | (c & 0x3F)), m = 4;
        if (cap) {
            if (k + m > cap) return 0;
            memcpy(dst + k, b, (size_t)m);
        }
        k += m;
    }
    return k;
}

/* ---- the window's side (host/display_sdl.c) ---- */
volatile uint8_t w32_key_down[256];
static const W32WindowOps *window_ops;
void w32_set_window_ops(const W32WindowOps *ops) { window_ops = ops; }
SHORT w32_GetAsyncKeyState(int vk) { return vk >= 0 && vk < 256 && w32_key_down[vk] ? (SHORT)0x8000 : 0; }
HWND w32_GetForegroundWindow(void) { return window_ops ? window_ops->foreground() : NULL; }
DWORD w32_GetWindowThreadProcessId(HWND w, DWORD *pid) {
    if (pid) *pid = w ? (DWORD)getpid() : 0;   /* (the only window it is asked about is the game's) */
    return w ? 1 : 0;
}
LONG w32_GetWindowLongA(HWND w, int index) {
    if (!window_ops || !w) return 0;
    return index == GWL_STYLE ? window_ops->style(w) : 0;
}
BOOL w32_PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return window_ops && w ? window_ops->post(w, msg, wp, lp) : FALSE; }
int w32_GetSystemMetrics(int index) {
    int w = 0, h = 0;
    if (window_ops && window_ops->screen) window_ops->screen(&w, &h);
    return index == SM_CXSCREEN ? w : index == SM_CYSCREEN ? h : 0;
}
BOOL w32_GetClientRect(HWND w, RECT *r) {
    memset(r, 0, sizeof *r);
    return window_ops && w ? window_ops->client(w, r) : FALSE;
}
int w32_MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type) {
    (void)w, (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "Hookshot", text ? text : "");
    return 1;
}
