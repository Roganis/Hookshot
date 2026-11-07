/* (hookshot, Linux) The part of Win32 the runtime (host/) and the Vulkan back end (gpu/vk/) use, on POSIX.

   Only on the include path of non-Windows builds (nx2/build.py): every file keeps its #include <windows.h>, and this
   answers it. The functions are w32_* (host/posix/win32.c, host/posix/gdi.c), named through macros so that nothing of
   the process (Mesa, SDL, FFmpeg) can mistake them for its own; the executable exports them (build.py's dynamic list)
   and libhookshot_vk.so uses the same ones.

   What does not translate is answered as Windows would answer a call that failed (the profiler's thread sampling,
   hardware breakpoints, waitable timers, the console), and the callers already take that path. What does:
     handles      events, threads, files, fibers: one object kind each behind HANDLE, CloseHandle for all
     locks        SRWLOCK and CONDITION_VARIABLE on futexes (zero-initialized, as on Windows), CRITICAL_SECTION
     memory       VirtualAlloc / Free / Protect / Query on mmap, mprotect and madvise (/proc/self/maps for Query)
     exceptions   vectored handlers and the unhandled filter, from SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGTRAP
     files        CreateFileA and friends, backslashes taken as slashes, a missing path looked up without case
     modules      LoadLibraryA / GetProcAddress on dlopen ("SDL2.dll" is libSDL2-2.0.so.0), the exe's own path
     keyboard     GetAsyncKeyState from the window's key state (host/display_sdl.c keeps it in VK codes)
     windows      the few calls the menu makes go to the window's thread (host/display_sdl.c) */
#ifndef HOOKSHOT_POSIX_WINDOWS_H
#define HOOKSHOT_POSIX_WINDOWS_H
#ifdef _WIN32
#error "host/posix is for builds that are not Windows"
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <strings.h>
#include <sched.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- __declspec: the few kinds used, as attributes ---- */
#define __declspec(x) W32_DECLSPEC_##x
#define W32_DECLSPEC_noinline __attribute__((noinline))
#define W32_DECLSPEC_noreturn __attribute__((noreturn))
#define W32_DECLSPEC_dllexport __attribute__((visibility("default")))
#define W32_DECLSPEC_dllimport
#define W32_DECLSPEC_thread __thread
#define W32_DECLSPEC_selectany __attribute__((weak))
#define W32_DECLSPEC_align(n) __attribute__((aligned(n)))

/* ---- what __try / __except did: fn(arg) under a guard; 0 when it faulted (access_only: only an access violation is
   caught, as GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION; else any fault, as __except (1)) ---- */
int w32_guarded(void (*fn)(void *), void *arg, int access_only);

/* ---- the Microsoft C library's names ---- */
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _fileno fileno
#define _chsize_s(fd, n) ftruncate((fd), (off_t)(n))
#define _mkdir(p) mkdir((p), 0755)
#define _getpid getpid
#define _unlink unlink
#define _access access
#define _putenv putenv
extern char **environ;
#define _environ environ
#define _atoi64 atoll
#define _mkgmtime timegm
/* (the Microsoft order: the result first; 0 when done) */
#include <time.h>
static inline int w32_localtime_s(struct tm *out, const time_t *t) { return localtime_r(t, out) ? 0 : 22; }
static inline int w32_gmtime_s(struct tm *out, const time_t *t) { return gmtime_r(t, out) ? 0 : 22; }
#define localtime_s w32_localtime_s
#define gmtime_s w32_gmtime_s

/* ---- types ---- */
typedef int BOOL;
typedef unsigned char BYTE, BOOLEAN, UCHAR;
typedef uint16_t WORD, USHORT;
typedef uint32_t DWORD, UINT, ULONG;
typedef int32_t LONG, INT;   /* (Windows' long is 32 bits) */
typedef int64_t LONG64, LONGLONG, INT64, LONG_PTR, INT_PTR, SSIZE_T;
typedef uint64_t ULONG64, DWORD64, ULONGLONG, UINT64, DWORD_PTR, ULONG_PTR, UINT_PTR, SIZE_T;
typedef int16_t SHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef float FLOAT;
typedef void VOID, *PVOID, *LPVOID, *HANDLE, *HMODULE, *HINSTANCE, *HWND, *HDC, *HGDIOBJ, *HBITMAP, *HFONT, *HBRUSH,
    *FARPROC_PTR, *HKEY, *HMONITOR, *HICON, *HCURSOR;
typedef const void *LPCVOID;
typedef char *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
typedef wchar_t *LPWSTR, *PWSTR;
typedef const wchar_t *LPCWSTR, *PCWSTR;
typedef DWORD *LPDWORD, *PDWORD;
typedef LONG *PLONG;
typedef BOOL *LPBOOL;
typedef ULONG_PTR *PULONG_PTR;
typedef intptr_t (*FARPROC)(void);
typedef LONG HRESULT;
typedef DWORD COLORREF;
typedef int64_t LRESULT;
typedef uint64_t WPARAM;
typedef int64_t LPARAM;
typedef union { struct { DWORD LowPart; LONG HighPart; }; struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; }; ULONGLONG QuadPart; } ULARGE_INTEGER;
typedef LARGE_INTEGER *PLARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

#define WINAPI
#define CALLBACK
#define APIENTRY
#define WINAPIV
#define __stdcall
#define __cdecl
#define TRUE 1
#define FALSE 0
#define MAX_PATH 4096
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xffff))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define _strdup strdup
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define __forceinline inline __attribute__((always_inline))
#define __debugbreak() __builtin_trap()
#ifndef min
#ifndef __cplusplus
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#endif

/* ---- errors ---- */
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_HANDLE_EOF 38
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_ALREADY_EXISTS 183
#define ERROR_NOT_ALL_ASSIGNED 1300
#define ERROR_TIMEOUT 1460
#define GetLastError w32_GetLastError
#define SetLastError w32_SetLastError
DWORD w32_GetLastError(void);
void w32_SetLastError(DWORD e);

/* ---- atomics (the Interlocked family on any width: the callers' types decide, as Windows' overloads would) ---- */
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64 InterlockedIncrement
#define InterlockedDecrement64 InterlockedDecrement
#define InterlockedIncrement16 InterlockedIncrement
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64 InterlockedExchange
#define InterlockedExchange8 InterlockedExchange
#define InterlockedExchangePointer InterlockedExchange
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64 InterlockedExchangeAdd
#define InterlockedAdd(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64 InterlockedAdd
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr64 InterlockedOr
#define InterlockedOr8 InterlockedOr
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd64 InterlockedAnd
#define InterlockedXor(p, v) __atomic_fetch_xor((p), (v), __ATOMIC_SEQ_CST)
/* (dest, exchange, comparand): the value before */
#define InterlockedCompareExchange(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedCompareExchange64 InterlockedCompareExchange
#define InterlockedCompareExchange16 InterlockedCompareExchange
#define InterlockedCompareExchangePointer InterlockedCompareExchange
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() __builtin_ia32_pause()
#define _mm_pause_w32() __builtin_ia32_pause()

/* ---- time ---- */
#define QueryPerformanceCounter w32_QueryPerformanceCounter
#define QueryPerformanceFrequency w32_QueryPerformanceFrequency
#define GetTickCount w32_GetTickCount
#define GetTickCount64 w32_GetTickCount64
#define Sleep w32_Sleep
#define SleepEx(ms, a) (w32_Sleep(ms), 0)
#define SwitchToThread w32_SwitchToThread
#define GetSystemTimeAsFileTime w32_GetSystemTimeAsFileTime
#define GetSystemTimePreciseAsFileTime w32_GetSystemTimeAsFileTime
#define timeBeginPeriod(x) 0
#define timeEndPeriod(x) 0
#define timeGetTime() ((DWORD)w32_GetTickCount64())
BOOL w32_QueryPerformanceCounter(LARGE_INTEGER *c);
BOOL w32_QueryPerformanceFrequency(LARGE_INTEGER *f);
DWORD w32_GetTickCount(void);
ULONGLONG w32_GetTickCount64(void);
void w32_Sleep(DWORD ms);
BOOL w32_SwitchToThread(void);
void w32_GetSystemTimeAsFileTime(FILETIME *ft);

/* ---- locks ---- */
typedef struct { volatile int32_t state, writers_waiting; } SRWLOCK, *PSRWLOCK;   /* state: 0 free, -1 held exclusive, n shared */
typedef struct { volatile int32_t seq, pad; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define SRWLOCK_INIT {0, 0}
#define CONDITION_VARIABLE_INIT {0, 0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 1
typedef struct { void *impl; int32_t spin; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
typedef struct { volatile int32_t state, pad; void *ptr; } INIT_ONCE, *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0, 0, 0}
#define InitializeSRWLock w32_InitializeSRWLock
#define AcquireSRWLockExclusive w32_AcquireSRWLockExclusive
#define ReleaseSRWLockExclusive w32_ReleaseSRWLockExclusive
#define AcquireSRWLockShared w32_AcquireSRWLockShared
#define ReleaseSRWLockShared w32_ReleaseSRWLockShared
#define TryAcquireSRWLockExclusive w32_TryAcquireSRWLockExclusive
#define TryAcquireSRWLockShared w32_TryAcquireSRWLockShared
#define InitializeConditionVariable w32_InitializeConditionVariable
#define SleepConditionVariableSRW w32_SleepConditionVariableSRW
#define SleepConditionVariableCS w32_SleepConditionVariableCS
#define WakeConditionVariable w32_WakeConditionVariable
#define WakeAllConditionVariable w32_WakeAllConditionVariable
#define InitializeCriticalSection w32_InitializeCriticalSection
#define InitializeCriticalSectionAndSpinCount(cs, n) (w32_InitializeCriticalSection(cs), TRUE)
#define InitializeCriticalSectionEx(cs, n, f) (w32_InitializeCriticalSection(cs), TRUE)
#define EnterCriticalSection w32_EnterCriticalSection
#define TryEnterCriticalSection w32_TryEnterCriticalSection
#define LeaveCriticalSection w32_LeaveCriticalSection
#define DeleteCriticalSection w32_DeleteCriticalSection
#define WaitOnAddress w32_WaitOnAddress
#define WakeByAddressSingle w32_WakeByAddressSingle
#define WakeByAddressAll w32_WakeByAddressAll
void w32_InitializeSRWLock(SRWLOCK *l);
void w32_AcquireSRWLockExclusive(SRWLOCK *l);
void w32_ReleaseSRWLockExclusive(SRWLOCK *l);
void w32_AcquireSRWLockShared(SRWLOCK *l);
void w32_ReleaseSRWLockShared(SRWLOCK *l);
BOOLEAN w32_TryAcquireSRWLockExclusive(SRWLOCK *l);
BOOLEAN w32_TryAcquireSRWLockShared(SRWLOCK *l);
void w32_InitializeConditionVariable(CONDITION_VARIABLE *cv);
BOOL w32_SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags);
BOOL w32_SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms);
void w32_WakeConditionVariable(CONDITION_VARIABLE *cv);
void w32_WakeAllConditionVariable(CONDITION_VARIABLE *cv);
void w32_InitializeCriticalSection(CRITICAL_SECTION *cs);
void w32_EnterCriticalSection(CRITICAL_SECTION *cs);
BOOL w32_TryEnterCriticalSection(CRITICAL_SECTION *cs);
void w32_LeaveCriticalSection(CRITICAL_SECTION *cs);
void w32_DeleteCriticalSection(CRITICAL_SECTION *cs);
BOOL w32_WaitOnAddress(volatile void *addr, void *compare, SIZE_T size, DWORD ms);
void w32_WakeByAddressSingle(void *addr);
void w32_WakeByAddressAll(void *addr);

/* ---- handles: events, waits ---- */
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS 64
#define CREATE_EVENT_MANUAL_RESET 1
#define CREATE_EVENT_INITIAL_SET 2
#define EVENT_ALL_ACCESS 0x1F0003
#define SYNCHRONIZE 0x00100000
#define TIMER_ALL_ACCESS 0x1F0003
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define CreateEventA w32_CreateEventA
#define CreateEventW(sa, m, i, n) w32_CreateEventA((sa), (m), (i), NULL)
#define CreateEventExA(sa, n, f, a) w32_CreateEventA((sa), ((f) & CREATE_EVENT_MANUAL_RESET) != 0, ((f) & CREATE_EVENT_INITIAL_SET) != 0, NULL)
#define CreateEventExW CreateEventExA
#define SetEvent w32_SetEvent
#define ResetEvent w32_ResetEvent
#define WaitForSingleObject w32_WaitForSingleObject
#define WaitForSingleObjectEx(h, ms, a) w32_WaitForSingleObject((h), (ms))
#define WaitForMultipleObjects w32_WaitForMultipleObjects
#define CloseHandle w32_CloseHandle
#define DuplicateHandle w32_DuplicateHandle
#define CreateSemaphoreA w32_CreateSemaphoreA
#define CreateSemaphoreW(sa, i, m, n) w32_CreateSemaphoreA((sa), (i), (m), NULL)
#define ReleaseSemaphore w32_ReleaseSemaphore
/* (no waitable timers: the callers sleep another way when creation fails) */
#define CreateWaitableTimerExW(a, b, c, d) ((HANDLE)NULL)
#define CreateWaitableTimerW(a, b, c) ((HANDLE)NULL)
#define CreateWaitableTimerA(a, b, c) ((HANDLE)NULL)
#define SetWaitableTimer(...) FALSE
#define SetWaitableTimerEx(...) FALSE
HANDLE w32_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
BOOL w32_SetEvent(HANDLE h);
BOOL w32_ResetEvent(HANDLE h);
DWORD w32_WaitForSingleObject(HANDLE h, DWORD ms);
DWORD w32_WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
BOOL w32_CloseHandle(HANDLE h);
BOOL w32_DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD options);
HANDLE w32_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name);
BOOL w32_ReleaseSemaphore(HANDLE h, LONG n, LONG *prev);
#define DUPLICATE_SAME_ACCESS 2

/* ---- threads ---- */
typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define CREATE_SUSPENDED 0x4
#define THREAD_PRIORITY_IDLE -15
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_ALL_ACCESS 0x1FFFFF
#define THREAD_SUSPEND_RESUME 0x2
#define THREAD_GET_CONTEXT 0x8
#define THREAD_SET_CONTEXT 0x10
#define THREAD_QUERY_INFORMATION 0x40
#define THREAD_QUERY_LIMITED_INFORMATION 0x800
#define CreateThread w32_CreateThread
#define ExitThread w32_ExitThread
#define GetCurrentThread() ((HANDLE)(intptr_t)-2)
#define GetCurrentProcess() ((HANDLE)(intptr_t)-1)
#define GetCurrentThreadId w32_GetCurrentThreadId
#define GetCurrentProcessId w32_GetCurrentProcessId
#define GetThreadId w32_GetThreadId
#define OpenThread w32_OpenThread
#define SetThreadPriority w32_SetThreadPriority
#define GetThreadPriority(h) THREAD_PRIORITY_NORMAL
#define SetThreadAffinityMask w32_SetThreadAffinityMask
#define GetProcessAffinityMask w32_GetProcessAffinityMask
#define SetThreadDescription w32_SetThreadDescription
#define GetThreadDescription w32_GetThreadDescription
#define SetThreadIdealProcessor(h, n) ((DWORD)0)
#define SetThreadPriorityBoost(h, d) TRUE
#define SetPriorityClass(h, c) TRUE
#define GetThreadTimes w32_GetThreadTimes
#define SuspendThread w32_SuspendThread
#define ResumeThread w32_ResumeThread
#define GetExitCodeThread w32_GetExitCodeThread
#define TlsAlloc w32_TlsAlloc
#define TlsGetValue w32_TlsGetValue
#define TlsSetValue w32_TlsSetValue
#define TlsFree w32_TlsFree
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
#define STILL_ACTIVE 259
HANDLE w32_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id);
void w32_ExitThread(DWORD code) __attribute__((noreturn));
DWORD w32_GetCurrentThreadId(void);
DWORD w32_GetCurrentProcessId(void);
DWORD w32_GetThreadId(HANDLE h);
HANDLE w32_OpenThread(DWORD access, BOOL inherit, DWORD id);
BOOL w32_SetThreadPriority(HANDLE h, int prio);
DWORD_PTR w32_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
BOOL w32_GetProcessAffinityMask(HANDLE h, DWORD_PTR *proc, DWORD_PTR *sys);
HRESULT w32_SetThreadDescription(HANDLE h, PCWSTR name);
HRESULT w32_GetThreadDescription(HANDLE h, PWSTR *name);
BOOL w32_GetThreadTimes(HANDLE h, FILETIME *create, FILETIME *exit, FILETIME *kernel, FILETIME *user);
DWORD w32_SuspendThread(HANDLE h);
DWORD w32_ResumeThread(HANDLE h);
BOOL w32_GetExitCodeThread(HANDLE h, DWORD *code);
DWORD w32_TlsAlloc(void);
LPVOID w32_TlsGetValue(DWORD i);
BOOL w32_TlsSetValue(DWORD i, LPVOID v);
BOOL w32_TlsFree(DWORD i);

/* ---- processors ---- */
typedef enum { RelationProcessorCore, RelationNumaNode, RelationCache, RelationProcessorPackage, RelationGroup, RelationAll = 0xffff } LOGICAL_PROCESSOR_RELATIONSHIP;
typedef struct {
    ULONG_PTR ProcessorMask;
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    union { struct { BYTE Flags; } ProcessorCore; struct { DWORD NodeNumber; } NumaNode; ULONGLONG Reserved[2]; };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION;
typedef struct {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
#define GetLogicalProcessorInformation w32_GetLogicalProcessorInformation
#define GetSystemInfo w32_GetSystemInfo
#define GetActiveProcessorCount(g) ((DWORD)w32_processor_count())
#define GetCurrentProcessorNumber() ((DWORD)sched_getcpu())
#define ALL_PROCESSOR_GROUPS 0xffff
BOOL w32_GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info, DWORD *len);
void w32_GetSystemInfo(SYSTEM_INFO *si);
int w32_processor_count(void);

/* ---- fibers ---- */
#define FIBER_FLAG_FLOAT_SWITCH 1
typedef void(WINAPI *LPFIBER_START_ROUTINE)(LPVOID);
#define ConvertThreadToFiber(p) w32_ConvertThreadToFiberEx((p), 0)
#define ConvertThreadToFiberEx w32_ConvertThreadToFiberEx
#define ConvertFiberToThread w32_ConvertFiberToThread
#define CreateFiber(s, f, p) w32_CreateFiberEx((s), (s), 0, (f), (p))
#define CreateFiberEx w32_CreateFiberEx
#define SwitchToFiber w32_SwitchToFiber
#define DeleteFiber w32_DeleteFiber
#define GetCurrentFiber w32_GetCurrentFiber
#define GetFiberData() (*(void **)w32_GetCurrentFiber())
LPVOID w32_ConvertThreadToFiberEx(LPVOID param, DWORD flags);
BOOL w32_ConvertFiberToThread(void);
LPVOID w32_CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param);
void w32_SwitchToFiber(LPVOID fiber);
void w32_DeleteFiber(LPVOID fiber);
LPVOID w32_GetCurrentFiber(void);

/* ---- virtual memory ---- */
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_RESET 0x00080000
#define MEM_TOP_DOWN 0x00100000
#define MEM_WRITE_WATCH 0x00200000
#define MEM_IMAGE 0x01000000
#define MEM_RESET_UNDO 0x01000000
#define MEM_LARGE_PAGES 0x20000000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
typedef struct {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
#define VirtualAlloc w32_VirtualAlloc
#define VirtualFree w32_VirtualFree
#define VirtualProtect w32_VirtualProtect
#define VirtualQuery w32_VirtualQuery
#define VirtualLock(p, n) TRUE
#define VirtualUnlock(p, n) TRUE
#define FlushInstructionCache(p, a, n) TRUE
#define GetLargePageMinimum() ((SIZE_T)0)
#define GetWriteWatch(...) ((UINT)1)
#define ResetWriteWatch(...) ((UINT)1)
#define WRITE_WATCH_FLAG_RESET 1
LPVOID w32_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL w32_VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL w32_VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, DWORD *old);
SIZE_T w32_VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);
/* (the guest window: VirtualQuery answers it without reading /proc, host/hos_main.c tells where it is) */
void w32_note_window(const void *base, uint64_t size);

/* ---- exceptions ---- */
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_DATATYPE_MISALIGNMENT 0x80000002u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_INT_OVERFLOW 0xC0000095u
#define EXCEPTION_FLT_DIVIDE_BY_ZERO 0xC000008Eu
#define EXCEPTION_FLT_INVALID_OPERATION 0xC0000090u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_PRIV_INSTRUCTION 0xC0000096u
#define EXCEPTION_IN_PAGE_ERROR 0xC0000006u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
/* (the registers a handler reads and may change: the general ones, the flags, the debug registers as written) */
typedef struct {
    DWORD ContextFlags, MxCsr;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD EFlags;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
    void *native;   /* (the signal's ucontext_t) */
} CONTEXT, *PCONTEXT;
#define CONTEXT_AMD64 0x00100000
#define CONTEXT_CONTROL (CONTEXT_AMD64 | 0x1)
#define CONTEXT_INTEGER (CONTEXT_AMD64 | 0x2)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_AMD64 | 0x10)
#define CONTEXT_FULL (CONTEXT_AMD64 | 0xB)
#define CONTEXT_ALL (CONTEXT_AMD64 | 0x1F)
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
typedef LONG(WINAPI *PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
typedef LONG(WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
typedef LPTOP_LEVEL_EXCEPTION_FILTER PTOP_LEVEL_EXCEPTION_FILTER;
#define AddVectoredExceptionHandler w32_AddVectoredExceptionHandler
#define RemoveVectoredExceptionHandler w32_RemoveVectoredExceptionHandler
#define SetUnhandledExceptionFilter w32_SetUnhandledExceptionFilter
#define GetThreadContext w32_GetThreadContext
#define SetThreadContext w32_SetThreadContext
#define RaiseException w32_RaiseException
#define SetErrorMode(m) 0u
#define SEM_FAILCRITICALERRORS 1
#define SEM_NOGPFAULTERRORBOX 2
PVOID w32_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn);
ULONG w32_RemoveVectoredExceptionHandler(PVOID h);
LPTOP_LEVEL_EXCEPTION_FILTER w32_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER fn);
BOOL w32_GetThreadContext(HANDLE h, CONTEXT *c);
BOOL w32_SetThreadContext(HANDLE h, const CONTEXT *c);
void w32_RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args);
/* (unwinding: the host backtraces of crash reports take the Linux path) */
typedef struct { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
#define RtlLookupFunctionEntry(pc, base, h) ((PRUNTIME_FUNCTION)NULL)
#define RtlVirtualUnwind(...) ((void *)NULL)
#define RtlCaptureStackBackTrace w32_RtlCaptureStackBackTrace
#define UNW_FLAG_NHANDLER 0
WORD w32_RtlCaptureStackBackTrace(DWORD skip, DWORD n, PVOID *frames, DWORD *hash);
#define ExitProcess w32_ExitProcess
#define TerminateProcess(h, code) (w32_ExitProcess(code), TRUE)
void w32_ExitProcess(UINT code) __attribute__((noreturn));
#define IsDebuggerPresent() FALSE
#define OutputDebugStringA(s) ((void)(s))
#define DebugBreak() __builtin_trap()

/* ---- files ---- */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_ALL 0x10000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_COPY_ALLOWED 2
#define MOVEFILE_WRITE_THROUGH 8
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef struct { void *Internal, *InternalHigh; DWORD Offset, OffsetHigh; HANDLE hEvent; } OVERLAPPED, *LPOVERLAPPED;
#define CreateFileA w32_CreateFileA
#define ReadFile w32_ReadFile
#define WriteFile w32_WriteFile
#define SetFilePointerEx w32_SetFilePointerEx
#define SetFilePointer w32_SetFilePointer
#define SetEndOfFile w32_SetEndOfFile
#define GetFileSizeEx w32_GetFileSizeEx
#define GetFileSize w32_GetFileSize
#define FlushFileBuffers w32_FlushFileBuffers
#define GetFileAttributesA w32_GetFileAttributesA
#define GetFileAttributesExA w32_GetFileAttributesExA
#define DeleteFileA w32_DeleteFileA
#define CreateDirectoryA w32_CreateDirectoryA
#define RemoveDirectoryA w32_RemoveDirectoryA
#define MoveFileExA w32_MoveFileExA
#define MoveFileA(a, b) w32_MoveFileExA((a), (b), 0)
#define CopyFileA w32_CopyFileA
#define FindFirstFileA w32_FindFirstFileA
#define FindNextFileA w32_FindNextFileA
#define FindClose w32_FindClose
#define GetStdHandle w32_GetStdHandle
#define GetFileType w32_GetFileType
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_DISK 1
#define FILE_TYPE_CHAR 2
#define FILE_TYPE_PIPE 3
#define SetCurrentDirectoryA(p) (chdir(p) == 0)
DWORD w32_GetFileType(HANDLE h);
#define GetConsoleMode w32_GetConsoleMode
#define SetConsoleMode(h, m) TRUE
#define SetConsoleOutputCP(cp) TRUE
#define AllocConsole() FALSE
#define FreeConsole() TRUE
#define SetConsoleTitleA(t) TRUE
#define AttachConsole(p) FALSE
#define ATTACH_PARENT_PROCESS ((DWORD)-1)
#define GetFullPathNameA w32_GetFullPathNameA
#define GetCurrentDirectoryA w32_GetCurrentDirectoryA
#define GetTempPathA w32_GetTempPathA
#define GetEnvironmentVariableA w32_GetEnvironmentVariableA
#define SetEnvironmentVariableA(n, v) ((v) ? setenv((n), (v), 1) == 0 : unsetenv(n) == 0)
HANDLE w32_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL w32_ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, OVERLAPPED *ov);
BOOL w32_WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, OVERLAPPED *ov);
BOOL w32_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method);
DWORD w32_SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method);
BOOL w32_SetEndOfFile(HANDLE h);
BOOL w32_GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
DWORD w32_GetFileSize(HANDLE h, DWORD *high);
BOOL w32_FlushFileBuffers(HANDLE h);
DWORD w32_GetFileAttributesA(LPCSTR path);
BOOL w32_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out);
BOOL w32_DeleteFileA(LPCSTR path);
BOOL w32_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL w32_RemoveDirectoryA(LPCSTR path);
BOOL w32_MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL w32_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
HANDLE w32_FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);
BOOL w32_FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL w32_FindClose(HANDLE h);
HANDLE w32_GetStdHandle(DWORD which);
BOOL w32_GetConsoleMode(HANDLE h, DWORD *mode);
DWORD w32_GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file);
DWORD w32_GetCurrentDirectoryA(DWORD n, LPSTR out);
DWORD w32_GetTempPathA(DWORD n, LPSTR out);
DWORD w32_GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD n);
/* the host path of a Windows-style one: '\' as '/', and where it does not exist as written, the entry of each folder
   that matches without case (Windows' file names are; the game's romfs paths and its folders' names can differ) */
int w32_host_path(const char *path, char *out, size_t cap);
int w32_fd(HANDLE h);   /* (a file handle's descriptor, -1 for anything else) */

/* ---- modules ---- */
#define GET_MODULE_HANDLE_EX_FLAG_PIN 1
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define LoadLibraryA w32_LoadLibraryA
#define LoadLibraryW(n) ((HMODULE)NULL)
#define LoadLibraryExA(n, f, fl) w32_LoadLibraryA(n)
#define FreeLibrary w32_FreeLibrary
#define GetProcAddress w32_GetProcAddress
#define GetModuleHandleA w32_GetModuleHandleA
#define GetModuleHandleW(n) w32_GetModuleHandleA(NULL)
#define GetModuleHandleExA w32_GetModuleHandleExA
#define GetModuleFileNameA w32_GetModuleFileNameA
HMODULE w32_LoadLibraryA(LPCSTR name);
BOOL w32_FreeLibrary(HMODULE m);
FARPROC w32_GetProcAddress(HMODULE m, LPCSTR name);
HMODULE w32_GetModuleHandleA(LPCSTR name);
BOOL w32_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out);
DWORD w32_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n);

/* ---- strings ---- */
#define CP_ACP 0
#define CP_UTF8 65001
#define MultiByteToWideChar w32_MultiByteToWideChar
#define WideCharToMultiByte w32_WideCharToMultiByte
#define LocalFree(p) (free(p), (HANDLE)NULL)
int w32_MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int n, LPWSTR dst, int cap);
int w32_WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int n, LPSTR dst, int cap, LPCSTR def, LPBOOL used);

/* ---- time of day ---- */
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;
typedef struct { LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias; WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; LONG DaylightBias; } TIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN 0
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2
#define GetLocalTime w32_GetLocalTime
#define GetSystemTime w32_GetSystemTime
#define GetTimeZoneInformation w32_GetTimeZoneInformation
#define FileTimeToSystemTime w32_FileTimeToSystemTime
#define SystemTimeToFileTime w32_SystemTimeToFileTime
void w32_GetLocalTime(SYSTEMTIME *st);
void w32_GetSystemTime(SYSTEMTIME *st);
DWORD w32_GetTimeZoneInformation(TIME_ZONE_INFORMATION *tz);
BOOL w32_FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st);
BOOL w32_SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft);

/* ---- the user's locale ---- */
#define LOCALE_NAME_MAX_LENGTH 85
#define GetUserDefaultLocaleName w32_GetUserDefaultLocaleName
#define GetUserDefaultUILanguage() ((WORD)0x409)
#define GetUserDefaultLangID() ((WORD)0x409)
int w32_GetUserDefaultLocaleName(LPWSTR out, int cap);

/* ---- the window: what host/display_sdl.c answers ---- */
#define WM_APP 0x8000
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define WS_CAPTION 0x00C00000L
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_NOACTIVATE 0x08000000L
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SW_SHOW 5
#define SW_SHOWNORMAL 1
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define GetForegroundWindow w32_GetForegroundWindow
#define GetWindowThreadProcessId w32_GetWindowThreadProcessId
#define GetWindowLongA w32_GetWindowLongA
#define GetWindowLongPtrA w32_GetWindowLongA
#define PostMessageA w32_PostMessageA
#define GetSystemMetrics w32_GetSystemMetrics
#define GetClientRect w32_GetClientRect
#define GetClassNameA(w, s, n) ((s)[0] = 0, 0)
#define GetWindowTextA(w, s, n) ((s)[0] = 0, 0)
#define MessageBoxA w32_MessageBoxA
#define ShellExecuteA(...) ((HINSTANCE)NULL)
#define GetAsyncKeyState w32_GetAsyncKeyState
#define GetKeyState w32_GetAsyncKeyState
HWND w32_GetForegroundWindow(void);
DWORD w32_GetWindowThreadProcessId(HWND w, DWORD *pid);
LONG w32_GetWindowLongA(HWND w, int index);
BOOL w32_PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
int w32_GetSystemMetrics(int index);
BOOL w32_GetClientRect(HWND w, RECT *r);
int w32_MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type);
SHORT w32_GetAsyncKeyState(int vk);
/* host/display_sdl.c's side: the keys down (by VK code) and the window's answers */
extern volatile uint8_t w32_key_down[256];
typedef struct {
    HWND (*foreground)(void);                     /* the game's window when it has the keyboard focus, else NULL */
    LONG (*style)(HWND w);                        /* WS_CAPTION unless fullscreen */
    BOOL (*post)(HWND w, UINT msg, WPARAM wp, LPARAM lp);
    BOOL (*client)(HWND w, RECT *r);
    void (*screen)(int *w, int *h);
} W32WindowOps;
void w32_set_window_ops(const W32WindowOps *ops);

/* virtual-key codes */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE

/* ---- GDI: the menu's text (host/posix/gdi.c, FreeType) ---- */
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define TRANSPARENT 1
#define OPAQUE 2
#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_TOP 0
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define FW_NORMAL 400
#define FW_MEDIUM 500
#define FW_SEMIBOLD 600
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define NONANTIALIASED_QUALITY 3
#define ANTIALIASED_QUALITY 4
#define CLEARTYPE_QUALITY 5
#define DEFAULT_PITCH 0
#define CreateCompatibleDC w32_CreateCompatibleDC
#define CreateDIBSection w32_CreateDIBSection
#define SelectObject w32_SelectObject
#define DeleteObject w32_DeleteObject
#define DeleteDC w32_DeleteDC
#define GdiFlush() TRUE
#define CreateFontA w32_CreateFontA
#define CreateSolidBrush w32_CreateSolidBrush
#define FillRect w32_FillRect
#define SetBkMode w32_SetBkMode
#define SetTextColor w32_SetTextColor
#define SetTextAlign w32_SetTextAlign
#define TextOutA w32_TextOutA
#define GetTextExtentPoint32A w32_GetTextExtentPoint32A
HDC w32_CreateCompatibleDC(HDC dc);
HBITMAP w32_CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset);
HGDIOBJ w32_SelectObject(HDC dc, HGDIOBJ o);
BOOL w32_DeleteObject(HGDIOBJ o);
BOOL w32_DeleteDC(HDC dc);
HFONT w32_CreateFontA(int height, int width, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike,
                      DWORD charset, DWORD out_prec, DWORD clip_prec, DWORD quality, DWORD pitch, LPCSTR face);
HBRUSH w32_CreateSolidBrush(COLORREF c);
int w32_FillRect(HDC dc, const RECT *r, HBRUSH b);
int w32_SetBkMode(HDC dc, int mode);
COLORREF w32_SetTextColor(HDC dc, COLORREF c);
UINT w32_SetTextAlign(HDC dc, UINT align);
BOOL w32_TextOutA(HDC dc, int x, int y, LPCSTR s, int n);
BOOL w32_GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, SIZE *out);

#ifdef __cplusplus
}
#endif
#endif
