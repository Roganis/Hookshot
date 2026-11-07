/* (hookshot, Linux) the thread snapshot the profiler walks: none here (the profiler's sampling is Windows-only) */
#pragma once
#include "windows.h"
#define TH32CS_SNAPTHREAD 0x00000004
typedef struct { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32;
#define CreateToolhelp32Snapshot(f, p) INVALID_HANDLE_VALUE
#define Thread32First(h, e) FALSE
#define Thread32Next(h, e) FALSE
