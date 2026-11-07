/* (hookshot, Linux) the process memory counters the texture cache logs: from /proc/self/statm */
#pragma once
#include "windows.h"
typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
        QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, PROCESS_MEMORY_COUNTERS;
static inline BOOL K32GetProcessMemoryInfo(HANDLE h, PROCESS_MEMORY_COUNTERS *pm, DWORD cb) {
    (void)h;
    unsigned long long size = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return FALSE;
    const int got = fscanf(f, "%llu %llu", &size, &resident);
    fclose(f);
    if (got != 2) return FALSE;
    memset(pm, 0, cb);
    pm->cb = cb;
    pm->WorkingSetSize = pm->PeakWorkingSetSize = (SIZE_T)resident * 4096;
    pm->PagefileUsage = pm->PeakPagefileUsage = (SIZE_T)resident * 4096;
    if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) ((PROCESS_MEMORY_COUNTERS_EX *)pm)->PrivateUsage = (SIZE_T)resident * 4096;
    return TRUE;
}
#define GetProcessMemoryInfo K32GetProcessMemoryInfo
