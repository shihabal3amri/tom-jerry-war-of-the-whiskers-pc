// The hybrid layer's host-platform seam (Stage 5, port/ANDROID_PLAN.md §4).
//
// On Windows this header IS <windows.h> — every TU that used to include windows.h
// includes this instead, and the Windows build compiles the exact same tokens it
// always did (nothing is wrapped, renamed, or re-declared; v1.1.0 byte-identity is
// untouched by construction).
//
// On a POSIX host (the aarch64 Android cross) it provides the SMALL Win32 subset the
// portable hybrid TUs actually use, implemented in host_compat.cpp:
//   * virtual memory  — VirtualAlloc/Free/Protect over mmap. ⚠ Address-less
//     allocations come from a BELOW-4GB probe cursor: on this layer every anonymous
//     allocation is potentially guest-visible (KPCR, kernel VARs, guest stack), and a
//     silent above-4GB placement is the truncated-pointer class of bug §2.1 exists to
//     prevent. Fixed-address requests probe with MAP_FIXED_NOREPLACE, never clobber.
//   * sections        — CreateFileMappingW/MapViewOfFileEx over memfd + MAP_SHARED
//     (the contiguous pool / low arena dual views: identity + 0x80000000 alias).
//   * time            — QueryPerformanceCounter/Frequency, GetTickCount, FILETIME.
//   * threads/TLS/CS  — CreateThread/WaitForSingleObject over pthreads; recursive
//     pthread mutex behind CRITICAL_SECTION; TlsAlloc family.
//   * misc            — GetEnvironmentVariableA, ExitProcess, Get/SetLastError,
//     FlushInstructionCache (no-op: guest code is only ever INTERPRETED on ARM).
//   * MSVC secure CRT — strncpy_s/_snprintf_s/fopen_s/_dupenv_s/sscanf_s/... the
//     hybrid uses throughout, as thin standard-C wrappers.
//   * SEH             — __try/__except compile away to `if (true) { } else { }`.
//     Guest faults cannot occur under interpretation without the engine reporting
//     them first (RunResult::Fault), and the game's deliberate raises are already
//     neutralized by the RtlRaiseException stub; a native host fault is a real bug
//     and crashes loudly (the SIGSEGV dump in headless_main). No silent recovery.
//
// File I/O (CreateFileA/FindFirstFileA/...) is included for file_io.cpp, with two
// POSIX-only behaviors the Windows build never needs: '\\' -> '/' separator
// normalization and per-component case-insensitive fallback resolution (the game
// asks for mixed-case paths; Android filesystems are case-sensitive).
#pragma once

#if defined(_WIN32)

#include <windows.h>

#else // ---------------------------------------------------------- POSIX (aarch64) ----

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

// ---- basic types / constants -------------------------------------------------------
using BOOL = int;
using DWORD = uint32_t;
using LONG = int32_t;
using LONGLONG = int64_t;
using ULONG = uint32_t;
using UINT = unsigned int;
using SIZE_T = size_t;
using LPVOID = void*;
using PVOID = void*;
using HANDLE = void*;
using HMODULE = void*;
using LPCSTR = const char*;
using LPSTR = char*;
using errno_t = int;
#define WINAPI
#define CALLBACK
#define __stdcall_compat
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE 0xFFFFFFFFu

union LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; } u;
    int64_t QuadPart;
};
union ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; } u;
    uint64_t QuadPart;
};
struct FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; };

// ---- SEH compiles away (see header comment) ----------------------------------------
#define __try if (true)
#define __except(filter) else if (0)
#define EXCEPTION_EXECUTE_HANDLER 1
inline DWORD GetExceptionCode() { return 0; }
inline void* GetExceptionInformation() { return nullptr; }

// ---- virtual memory ----------------------------------------------------------------
#define MEM_COMMIT   0x1000u
#define MEM_FREE     0x10000u  // VirtualQuery: nothing mapped at this address
#define MEM_RESERVE  0x2000u
#define MEM_RELEASE  0x8000u
#define PAGE_NOACCESS          0x01u
#define PAGE_GUARD             0x100u   // never set by the shim's VirtualQuery, but callers
                                        // test for it (arabic.cpp's Readable) and must compile
#define PAGE_READONLY          0x02u
#define PAGE_READWRITE         0x04u
#define PAGE_EXECUTE_READ      0x20u
#define PAGE_EXECUTE_READWRITE 0x40u

struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress; void* AllocationBase; DWORD AllocationProtect;
    SIZE_T RegionSize; DWORD State; DWORD Protect; DWORD Type;
};

void* VirtualAlloc(void* addr, size_t size, DWORD type, DWORD protect);
BOOL  VirtualFree(void* addr, size_t size, DWORD type);
BOOL  VirtualProtect(void* addr, size_t size, DWORD prot, DWORD* oldProt);
SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* mbi, size_t len);

// Sections (memfd): the HANDLE is (fd+1) cast to pointer so null stays "failed".
HANDLE CreateFileMappingW(HANDLE file, void* attrs, DWORD protect,
                          DWORD sizeHigh, DWORD sizeLow, const wchar_t* name);
#define FILE_MAP_ALL_ACCESS 0xF001Fu
void* MapViewOfFileEx(HANDLE sec, DWORD access, DWORD offHigh, DWORD offLow,
                      size_t size, void* baseAddr);

inline HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
inline BOOL FlushInstructionCache(HANDLE, const void*, size_t) { return TRUE; }
inline HMODULE GetModuleHandleA(const char*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }

// ---- process / env / errors --------------------------------------------------------
[[noreturn]] void ExitProcess(UINT code);
DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD cap);
DWORD GetLastError();
void  SetLastError(DWORD e);
DWORD GetCurrentThreadId();

// ---- time --------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out);
DWORD GetTickCount();
void GetSystemTimeAsFileTime(FILETIME* ft);
void Sleep(DWORD ms);

// ---- threads / events (handles carry a kind header; see host_compat.cpp) ------------
using ThreadProc = DWORD (*)(void*);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
HANDLE CreateThread(void* attrs, size_t stackSize, ThreadProc proc, void* arg,
                    DWORD flags, DWORD* outTid);
// Events: single guest thread + synchronous I/O — a signaled flag is the whole
// semantics needed (file_io signals its own events after already-completed I/O).
HANDLE CreateEventA(void* attrs, BOOL manualReset, BOOL initialState, const char* name);
BOOL  SetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL  CloseHandle(HANDLE h);

// ---- TLS (single guest thread; a handful of slots) ----------------------------------
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
DWORD TlsAlloc();
void* TlsGetValue(DWORD idx);
BOOL  TlsSetValue(DWORD idx, void* v);

// ---- critical sections (recursive mutex) --------------------------------------------
struct CRITICAL_SECTION { void* impl; };
void InitializeCriticalSection(CRITICAL_SECTION* cs);
void EnterCriticalSection(CRITICAL_SECTION* cs);
void LeaveCriticalSection(CRITICAL_SECTION* cs);
void DeleteCriticalSection(CRITICAL_SECTION* cs);

// ---- file I/O (file_io.cpp + small helpers elsewhere) -------------------------------
#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ  0x1u
#define FILE_SHARE_WRITE 0x2u
#define CREATE_NEW    1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS   4
#define FILE_ATTRIBUTE_NORMAL    0x80u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define TRUNCATE_EXISTING 5
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define ERROR_ALREADY_EXISTS 183u

HANDLE CreateFileA(const char* path, DWORD access, DWORD share, void* sec,
                   DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void* buf, DWORD len, DWORD* got, void* ovl);
BOOL WriteFile(HANDLE h, const void* buf, DWORD len, DWORD* put, void* ovl);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newPos, DWORD method);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
DWORD GetFileAttributesA(const char* path);
BOOL CreateDirectoryA(const char* path, void* sec);
BOOL CopyFileA(const char* from, const char* to, BOOL failIfExists);
BOOL DeleteFileA(const char* path);
DWORD GetModuleFileNameA(HMODULE m, char* out, DWORD cap);
DWORD GetFullPathNameA(const char* in, DWORD cap, char* out, char** filePart);
BOOL GetDiskFreeSpaceExA(const char* path, ULARGE_INTEGER* avail, ULARGE_INTEGER* total,
                         ULARGE_INTEGER* freeTotal);

struct WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwReserved0, dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
};
HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd);
BOOL FindClose(HANDLE h);

// Fixed-address mmap with a safety ladder: MAP_FIXED_NOREPLACE where the kernel (or
// qemu-user, for the WSL leg) supports it; otherwise verify the whole range is
// UNMAPPED via mincore and use plain MAP_FIXED — never a silent clobber. Returns
// nullptr on failure or occupancy. `fd` < 0 = anonymous.
void* HostMapFixed(void* addr, size_t size, int prot, bool shared, int fd, long long off);

// Resolve a Windows-shaped path ('\\' separators, mixed case) to an existing POSIX
// path when possible: separators normalized, then per-component case-insensitive
// lookup for components that don't exist verbatim. Always writes a usable path
// (missing components pass through unchanged, so creates still work).
void HostResolvePath(const char* in, char* out, size_t cap);

// ---- MSVC secure-CRT shims ----------------------------------------------------------
#define _TRUNCATE ((size_t)-1)

errno_t fopen_s(FILE** f, const char* path, const char* mode);
errno_t _dupenv_s(char** out, size_t* len, const char* name);

int _vsnprintf_s_impl(char* buf, size_t cap, const char* fmt, va_list ap);
inline int _snprintf_s(char* buf, size_t cap, size_t /*maxCount = _TRUNCATE*/,
                       const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = _vsnprintf_s_impl(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
template <size_t N>
inline int _snprintf_s(char (&buf)[N], size_t /*maxCount*/, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = _vsnprintf_s_impl(buf, N, fmt, ap);
    va_end(ap);
    return r;
}
template <size_t N>
inline int sprintf_s(char (&buf)[N], const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = _vsnprintf_s_impl(buf, N, fmt, ap);
    va_end(ap);
    return r;
}

inline int _vsnprintf_s(char* buf, size_t cap, size_t /*maxCount*/, const char* fmt,
                        va_list ap) {
    return _vsnprintf_s_impl(buf, cap, fmt, ap);
}
template <size_t N>
inline int _vsnprintf_s(char (&buf)[N], size_t /*maxCount*/, const char* fmt, va_list ap) {
    return _vsnprintf_s_impl(buf, N, fmt, ap);
}

// INI persistence (tomjerry.ini): the two profile calls the settings screens use.
UINT GetPrivateProfileIntA(const char* section, const char* key, int def, const char* path);
BOOL WritePrivateProfileStringA(const char* section, const char* key, const char* value,
                                const char* path);

errno_t strncpy_s_impl(char* dst, size_t cap, const char* src, size_t count);
inline errno_t strncpy_s(char* dst, size_t cap, const char* src, size_t count) {
    return strncpy_s_impl(dst, cap, src, count);
}
template <size_t N>
inline errno_t strncpy_s(char (&dst)[N], const char* src, size_t count) {
    return strncpy_s_impl(dst, N, src, count);
}
inline errno_t strcpy_s(char* dst, size_t cap, const char* src) {
    return strncpy_s_impl(dst, cap, src, _TRUNCATE);
}
template <size_t N>
inline errno_t strcpy_s(char (&dst)[N], const char* src) {
    return strncpy_s_impl(dst, N, src, _TRUNCATE);
}
errno_t strcat_s_impl(char* dst, size_t cap, const char* src);
inline errno_t strcat_s(char* dst, size_t cap, const char* src) {
    return strcat_s_impl(dst, cap, src);
}
template <size_t N>
inline errno_t strcat_s(char (&dst)[N], const char* src) {
    return strcat_s_impl(dst, N, src);
}
// sscanf_s: identical to sscanf for the numeric-only formats the hybrid uses (%u/%d/%x);
// no %s/%c buffer-size arguments exist at any call site (they would differ).
#define sscanf_s sscanf
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

#endif // !_WIN32
