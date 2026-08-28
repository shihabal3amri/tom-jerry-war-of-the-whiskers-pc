// POSIX implementation of the Win32 subset in host_compat.h (aarch64 headless build
// only — the Windows build never compiles this file). See the header for the design;
// the one policy decision that matters lives here: ADDRESS-LESS ALLOCATIONS ARE
// BELOW 4 GB, ALWAYS. Anything allocated through this layer can end up guest-visible
// (KPCR base burned into guest instruction displacements, kernel VAR addresses in
// thunk slots, the guest stack itself), and an above-4GB placement truncates silently
// when stored through a 32-bit slot — the exact failure §2.1's gptr rule exists for.
#if !defined(_WIN32)
#include "hybrid/host_compat.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <ctype.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// memfd_create appears in bionic/glibc headers late; the syscall is ancient.
#include <sys/syscall.h>
static int HostMemfd(const char* name) {
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, 0);
#else
    (void)name; errno = ENOSYS; return -1;
#endif
}

// ---- fixed-address mapping ladder ----------------------------------------------------
void* HostMapFixed(void* addr, size_t size, int prot, bool shared, int fd, long long off) {
    int base = (shared ? MAP_SHARED : MAP_PRIVATE) | (fd < 0 ? MAP_ANONYMOUS : 0);
    void* p = mmap(addr, size, prot, base | MAP_FIXED_NOREPLACE, fd, (off_t)off);
    if (p == addr) return p;
    if (p != MAP_FAILED) { munmap(p, size); return nullptr; }   // NOREPLACE honored: occupied
    // TJ_MMAP_TRUST_FIXED=1: the qemu-user leg (WSL S5c rig). qemu pre-reserves the
    // WHOLE guest address space PROT_NONE, so both NOREPLACE and mincore read every
    // address as occupied — plain MAP_FIXED is how a guest carves from that
    // reservation, and at these fixed layout addresses nothing real can be clobbered.
    // NEVER set this on a real device; the Android kernel honors NOREPLACE.
    static int trust = -1;
    if (trust < 0) { const char* e = getenv("TJ_MMAP_TRUST_FIXED"); trust = e && *e == '1'; }
    if (!trust) {
        // NOREPLACE unsupported (old kernels): verify the range is unmapped page by
        // page, then MAP_FIXED. mincore == -1/ENOMEM means "no mapping at addr".
        for (uintptr_t a = (uintptr_t)addr; a < (uintptr_t)addr + size; a += 0x1000) {
            unsigned char vec = 0;
            if (mincore((void*)a, 0x1000, &vec) == 0) return nullptr;  // something lives here
            if (errno != ENOMEM) return nullptr;
        }
    }
    p = mmap(addr, size, prot, base | MAP_FIXED, fd, (off_t)off);
    return p == addr ? p : nullptr;
}

// ---- allocation registry (VirtualFree needs sizes; Win32 callers pass 0) ------------
namespace {
struct AllocRec { void* p; size_t size; };
constexpr int kMaxAllocs = 4096;
AllocRec g_allocs[kMaxAllocs];
int g_nAllocs = 0;
pthread_mutex_t g_allocLock = PTHREAD_MUTEX_INITIALIZER;

void RememberAlloc(void* p, size_t size) {
    pthread_mutex_lock(&g_allocLock);
    if (g_nAllocs < kMaxAllocs) g_allocs[g_nAllocs++] = { p, size };
    pthread_mutex_unlock(&g_allocLock);
}
size_t ForgetAlloc(void* p) {
    size_t sz = 0;
    pthread_mutex_lock(&g_allocLock);
    for (int i = 0; i < g_nAllocs; ++i)
        if (g_allocs[i].p == p) { sz = g_allocs[i].size; g_allocs[i] = g_allocs[--g_nAllocs]; break; }
    pthread_mutex_unlock(&g_allocLock);
    return sz;
}

// Below-4GB probe cursor for address-less allocations. Starts above the guest
// window/pool/arena complex (which ends at 0x10000000) and stays clear of the
// 0x80000000 GPU-alias region. 64 KB granularity like Win32.
uintptr_t g_lowCursor = 0x40000000u;
constexpr uintptr_t kLowLimit = 0x7F000000u;

void* LowAlloc(size_t size) {
    size_t rounded = (size + 0xFFFFu) & ~(size_t)0xFFFFu;
    pthread_mutex_lock(&g_allocLock);
    uintptr_t at = g_lowCursor;
    void* got = nullptr;
    while (at + rounded <= kLowLimit) {
        void* p = HostMapFixed((void*)at, rounded, PROT_READ | PROT_WRITE,
                               /*shared=*/false, -1, 0);
        if (p) { got = p; g_lowCursor = at + rounded; break; }
        at += 0x10000u;
    }
    pthread_mutex_unlock(&g_allocLock);
    if (!got) {
        fprintf(stderr, "[compat] FATAL: below-4GB allocation of %zu bytes failed\n", size);
        fflush(nullptr);
        _Exit(0xA4);
    }
    return got;
}
} // namespace

// ---- virtual memory ------------------------------------------------------------------
void* VirtualAlloc(void* addr, size_t size, DWORD type, DWORD protect) {
    (void)type;
    int prot = PROT_READ | PROT_WRITE;              // exec irrelevant: guest is interpreted
    (void)protect;
    if (!addr) {
        void* p = LowAlloc(size);
        RememberAlloc(p, (size + 0xFFFFu) & ~(size_t)0xFFFFu);
        return p;
    }
    size_t rounded = (size + 0xFFFu) & ~(size_t)0xFFFu;
    void* p = HostMapFixed(addr, rounded, prot, /*shared=*/false, -1, 0);
    if (!p) return nullptr;
    RememberAlloc(p, rounded);
    return p;
}

BOOL VirtualFree(void* addr, size_t size, DWORD type) {
    if (!addr) return FALSE;
    if (type & MEM_RELEASE) {
        size_t sz = ForgetAlloc(addr);
        if (!sz) return FALSE;                       // not ours (e.g. a hold that never existed)
        return munmap(addr, sz) == 0 ? TRUE : FALSE;
    }
    (void)size;
    return TRUE;
}

BOOL VirtualProtect(void* addr, size_t size, DWORD prot, DWORD* oldProt) {
    // Everything this layer maps is RW already and guest code is interpreted, so
    // protection changes are bookkeeping. Report the previous protection as RW.
    (void)addr; (void)size; (void)prot;
    if (oldProt) *oldProt = PAGE_READWRITE;
    return TRUE;
}

// ⚠ THIS USED TO LIE, AND THE LIE WAS EXPENSIVE. It zeroed the struct and returned success,
// so State came back 0 -- never MEM_COMMIT -- and every caller that asks "is this address
// readable?" got NO for anything it could not answer another way. arabic.cpp's Readable()
// fast-paths only the pool (0x04000000..0x10000000); on Android the master pointer, the font
// set and every font live in the guest IMAGE window (0x00010000..0x0165E000), which is BELOW
// that. So ClassifyFont never recognised a font, BuildArabicFont never ran, and the accent
// overlay was never silenced -- Arabic on the phone drew with stray marks over the letters
// while Windows was clean. Nothing logged, because every one of those functions returns
// before its first printf.
//
// mincore is the honest answer and host_compat already relies on it in HostMapFixed: 0 means
// the page is mapped, -1/ENOMEM means it is not. One syscall per page, and callers probe a
// couple of pages at a time.
SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* mbi, size_t len) {
    if (!mbi || len < sizeof(*mbi)) return 0;
    memset(mbi, 0, sizeof(*mbi));
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)0xFFF;
    unsigned char vec = 0;
    bool mapped = mincore((void*)page, 0x1000, &vec) == 0;
    mbi->BaseAddress = (void*)page;
    mbi->RegionSize  = 0x1000;
    mbi->State       = mapped ? MEM_COMMIT : MEM_FREE;
    mbi->Protect     = mapped ? PAGE_READWRITE : PAGE_NOACCESS;
    return sizeof(*mbi);
}

HANDLE CreateFileMappingW(HANDLE file, void*, DWORD, DWORD sizeHigh, DWORD sizeLow,
                          const wchar_t*) {
    if (file != INVALID_HANDLE_VALUE) return nullptr;   // only pagefile sections are used
    int fd = HostMemfd("tj_section");
    if (fd < 0) return nullptr;
    uint64_t size = ((uint64_t)sizeHigh << 32) | sizeLow;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return nullptr; }
    return (HANDLE)(intptr_t)(fd + 1);
}

void* MapViewOfFileEx(HANDLE sec, DWORD, DWORD offHigh, DWORD offLow,
                      size_t size, void* baseAddr) {
    int fd = (int)(intptr_t)sec - 1;
    off_t off = (off_t)(((uint64_t)offHigh << 32) | offLow);
    if (!size) {
        struct stat st{};
        if (fstat(fd, &st) != 0) return nullptr;
        size = (size_t)st.st_size;
    }
    if (baseAddr)
        return HostMapFixed(baseAddr, size, PROT_READ | PROT_WRITE, /*shared=*/true,
                            fd, (long long)off);
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    return p == MAP_FAILED ? nullptr : p;
}

// ---- process / env / errors ----------------------------------------------------------
[[noreturn]] void ExitProcess(UINT code) { fflush(nullptr); _Exit((int)code); }

DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD cap) {
    const char* v = getenv(name);
    if (!v) return 0;
    size_t n = strlen(v);
    if (buf && cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(buf, v, c);
        buf[c] = 0;
    }
    return (DWORD)n;
}

static __thread DWORD g_lastError = 0;
DWORD GetLastError() { return g_lastError; }
void SetLastError(DWORD e) { g_lastError = e; }
DWORD GetCurrentThreadId() { return (DWORD)(uintptr_t)pthread_self(); }

// ---- time ------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out->QuadPart = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    out->QuadPart = 1000000000ll;
    return TRUE;
}
DWORD GetTickCount() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
void GetSystemTimeAsFileTime(FILETIME* ft) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    // Unix epoch -> Windows epoch (1601): 11644473600 seconds, in 100ns units.
    uint64_t t = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull
               + (uint64_t)ts.tv_nsec / 100u;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
void Sleep(DWORD ms) {
    timespec ts{ (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000l };
    nanosleep(&ts, nullptr);
}

// ---- handles (threads / events / files share a kind header) -----------------------------
namespace {
enum : uint32_t { kHandleThread = 0x7411AD01u, kHandleEvent = 0x7411AD02u,
                  kHandleFile = 0x7411AD03u };
struct HostHandle { uint32_t kind; };
struct ThreadWrap : HostHandle {
    pthread_t th;
    ThreadProc proc;
    void* arg;
};
struct EventWrap : HostHandle {
    bool signaled;
};
struct FileWrap : HostHandle {
    int fd;
};
void* ThreadTrampoline(void* p) {
    ThreadWrap* w = (ThreadWrap*)p;
    w->proc(w->arg);
    return nullptr;
}
} // namespace

HANDLE CreateThread(void*, size_t stackSize, ThreadProc proc, void* arg,
                    DWORD, DWORD* outTid) {
    ThreadWrap* w = new ThreadWrap{ {kHandleThread}, {}, proc, arg };
    pthread_attr_t at;
    pthread_attr_init(&at);
    if (stackSize) pthread_attr_setstacksize(&at, stackSize < (1u << 20) ? (1u << 20) : stackSize);
    int rc = pthread_create(&w->th, &at, &ThreadTrampoline, w);
    pthread_attr_destroy(&at);
    if (rc != 0) { delete w; g_lastError = (DWORD)rc; return nullptr; }
    if (outTid) *outTid = 0;
    return (HANDLE)w;
}
HANDLE CreateEventA(void*, BOOL, BOOL initialState, const char*) {
    return (HANDLE)new EventWrap{ {kHandleEvent}, initialState != 0 };
}
BOOL SetEvent(HANDLE h) {
    HostHandle* hh = (HostHandle*)h;
    if (!hh || hh->kind != kHandleEvent) return FALSE;
    ((EventWrap*)hh)->signaled = true;
    return TRUE;
}
DWORD WaitForSingleObject(HANDLE h, DWORD) {
    HostHandle* hh = (HostHandle*)h;
    if (!hh) return 0xFFFFFFFFu;
    if (hh->kind == kHandleThread) { pthread_join(((ThreadWrap*)hh)->th, nullptr); return 0; }
    if (hh->kind == kHandleEvent) return ((EventWrap*)hh)->signaled ? 0 : 0x102 /*TIMEOUT*/;
    return 0;
}
BOOL CloseHandle(HANDLE h) {
    HostHandle* hh = (HostHandle*)h;
    if (!hh || h == INVALID_HANDLE_VALUE) return FALSE;
    switch (hh->kind) {
    case kHandleThread: delete (ThreadWrap*)hh; return TRUE;
    case kHandleEvent:  delete (EventWrap*)hh; return TRUE;
    case kHandleFile: {
        int fd = ((FileWrap*)hh)->fd;
        delete (FileWrap*)hh;
        return close(fd) == 0 ? TRUE : FALSE;
    }
    default: return FALSE;
    }
}

// ---- TLS -------------------------------------------------------------------------------
namespace {
constexpr int kTlsSlots = 16;
pthread_key_t g_tlsKeys[kTlsSlots];
bool g_tlsUsed[kTlsSlots] = {};
} // namespace
DWORD TlsAlloc() {
    for (int i = 0; i < kTlsSlots; ++i)
        if (!g_tlsUsed[i]) {
            if (pthread_key_create(&g_tlsKeys[i], nullptr) != 0) return TLS_OUT_OF_INDEXES;
            g_tlsUsed[i] = true;
            return (DWORD)i;
        }
    return TLS_OUT_OF_INDEXES;
}
void* TlsGetValue(DWORD idx) {
    if (idx >= kTlsSlots || !g_tlsUsed[idx]) return nullptr;
    return pthread_getspecific(g_tlsKeys[idx]);
}
BOOL TlsSetValue(DWORD idx, void* v) {
    if (idx >= kTlsSlots || !g_tlsUsed[idx]) return FALSE;
    return pthread_setspecific(g_tlsKeys[idx], v) == 0 ? TRUE : FALSE;
}

// ---- critical sections -------------------------------------------------------------------
void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutex_t* m = new pthread_mutex_t;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &at);
    pthread_mutexattr_destroy(&at);
    cs->impl = m;
}
void EnterCriticalSection(CRITICAL_SECTION* cs) {
    if (!cs->impl) InitializeCriticalSection(cs);    // single-threaded init in practice
    pthread_mutex_lock((pthread_mutex_t*)cs->impl);
}
void LeaveCriticalSection(CRITICAL_SECTION* cs) {
    if (cs->impl) pthread_mutex_unlock((pthread_mutex_t*)cs->impl);
}
void DeleteCriticalSection(CRITICAL_SECTION* cs) {
    if (cs->impl) {
        pthread_mutex_destroy((pthread_mutex_t*)cs->impl);
        delete (pthread_mutex_t*)cs->impl;
        cs->impl = nullptr;
    }
}

// ---- path resolution ----------------------------------------------------------------------
// Normalize separators, then resolve each missing component case-insensitively against
// the directory that holds it. The output is always usable: components that resolve
// keep their on-disk case; components that don't (a file about to be created) pass
// through as given.
void HostResolvePath(const char* in, char* out, size_t cap) {
    char norm[1024];
    size_t n = strlen(in);
    if (n >= sizeof(norm)) n = sizeof(norm) - 1;
    for (size_t i = 0; i < n; ++i) norm[i] = in[i] == '\\' ? '/' : in[i];
    norm[n] = 0;

    struct stat st{};
    if (stat(norm, &st) == 0 || n == 0) {            // exists verbatim (the common case)
        snprintf(out, cap, "%s", norm);
        return;
    }

    // Walk component by component, fixing case where needed.
    char cur[1024];
    size_t curLen = 0;
    cur[0] = 0;
    const char* p = norm;
    if (*p == '/') { cur[curLen++] = '/'; cur[curLen] = 0; ++p; }
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t compLen = slash ? (size_t)(slash - p) : strlen(p);
        char comp[512];
        if (compLen >= sizeof(comp)) compLen = sizeof(comp) - 1;
        memcpy(comp, p, compLen);
        comp[compLen] = 0;

        char cand[1024];
        snprintf(cand, sizeof(cand), "%.*s%s%s", (int)curLen, cur,
                 (curLen && cur[curLen - 1] != '/') ? "/" : "", comp);
        if (stat(cand, &st) != 0) {
            // case-insensitive lookup in the parent
            const char* dirPath = curLen ? cur : ".";
            DIR* d = opendir(dirPath);
            if (d) {
                dirent* e;
                while ((e = readdir(d)) != nullptr) {
                    if (strcasecmp(e->d_name, comp) == 0) {
                        snprintf(cand, sizeof(cand), "%.*s%s%s", (int)curLen, cur,
                                 (curLen && cur[curLen - 1] != '/') ? "/" : "", e->d_name);
                        break;
                    }
                }
                closedir(d);
            }
        }
        snprintf(cur, sizeof(cur), "%s", cand);
        curLen = strlen(cur);
        p = slash ? slash + 1 : p + strlen(p);
    }
    snprintf(out, cap, "%s", cur);
}

// ---- file I/O --------------------------------------------------------------------------------
static HANDLE FdToHandle(int fd) { return (HANDLE)new FileWrap{ {kHandleFile}, fd }; }
static int HandleToFd(HANDLE h) {
    HostHandle* hh = (HostHandle*)h;
    return (hh && hh->kind == kHandleFile) ? ((FileWrap*)hh)->fd : -1;
}

HANDLE CreateFileA(const char* path, DWORD access, DWORD, void*, DWORD disp, DWORD, HANDLE) {
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    int flags = 0;
    bool rd = (access & GENERIC_READ) != 0, wr = (access & GENERIC_WRITE) != 0;
    flags = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case CREATE_NEW:    flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS:   flags |= O_CREAT; break;
    default: break;
    }
    int fd = open(rp, flags, 0644);
    if (fd < 0) { g_lastError = (DWORD)errno; return INVALID_HANDLE_VALUE; }
    return FdToHandle(fd);
}
BOOL ReadFile(HANDLE h, void* buf, DWORD len, DWORD* got, void*) {
    ssize_t r = read(HandleToFd(h), buf, len);
    if (r < 0) { if (got) *got = 0; return FALSE; }
    if (got) *got = (DWORD)r;
    return TRUE;
}
BOOL WriteFile(HANDLE h, const void* buf, DWORD len, DWORD* put, void*) {
    ssize_t r = write(HandleToFd(h), buf, len);
    if (r < 0) { if (put) *put = 0; return FALSE; }
    if (put) *put = (DWORD)r;
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newPos, DWORD method) {
    int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t r = lseek(HandleToFd(h), (off_t)dist.QuadPart, whence);
    if (r < 0) return FALSE;
    if (newPos) newPos->QuadPart = (int64_t)r;
    return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    struct stat st{};
    if (fstat(HandleToFd(h), &st) != 0) return FALSE;
    size->QuadPart = (int64_t)st.st_size;
    return TRUE;
}
DWORD GetFileAttributesA(const char* path) {
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    struct stat st{};
    if (stat(rp, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL CreateDirectoryA(const char* path, void*) {
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    if (mkdir(rp, 0755) == 0) return TRUE;
    g_lastError = errno == EEXIST ? ERROR_ALREADY_EXISTS : (DWORD)errno;
    return FALSE;
}
BOOL CopyFileA(const char* from, const char* to, BOOL failIfExists) {
    char rf[1024], rt[1024];
    HostResolvePath(from, rf, sizeof(rf));
    HostResolvePath(to, rt, sizeof(rt));
    if (failIfExists) {
        struct stat st{};
        if (stat(rt, &st) == 0) return FALSE;
    }
    int a = open(rf, O_RDONLY);
    if (a < 0) return FALSE;
    int b = open(rt, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (b < 0) { close(a); return FALSE; }
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(a, buf, sizeof(buf))) > 0)
        if (write(b, buf, (size_t)n) != n) { ok = false; break; }
    if (n < 0) ok = false;
    close(a);
    close(b);
    return ok ? TRUE : FALSE;
}
BOOL DeleteFileA(const char* path) {
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    return unlink(rp) == 0 ? TRUE : FALSE;
}
DWORD GetModuleFileNameA(HMODULE, char* out, DWORD cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { if (cap) out[0] = 0; return 0; }
    out[n] = 0;
    return (DWORD)n;
}
DWORD GetFullPathNameA(const char* in, DWORD cap, char* out, char** filePart) {
    if (filePart) *filePart = nullptr;
    char norm[1024];
    HostResolvePath(in, norm, sizeof(norm));
    if (norm[0] == '/') {
        snprintf(out, cap, "%s", norm);
    } else {
        char cwd[768];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        snprintf(out, cap, "%s/%s", cwd, norm);
    }
    return (DWORD)strlen(out);
}
#include <sys/statvfs.h>
BOOL GetDiskFreeSpaceExA(const char* path, ULARGE_INTEGER* avail, ULARGE_INTEGER* total,
                         ULARGE_INTEGER* freeTotal) {
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    struct statvfs vs{};
    if (statvfs(rp, &vs) != 0) return FALSE;
    uint64_t frsize = vs.f_frsize ? vs.f_frsize : vs.f_bsize;
    if (avail)     avail->QuadPart = (uint64_t)vs.f_bavail * frsize;
    if (total)     total->QuadPart = (uint64_t)vs.f_blocks * frsize;
    if (freeTotal) freeTotal->QuadPart = (uint64_t)vs.f_bfree * frsize;
    return TRUE;
}

// ---- FindFirstFile over readdir + fnmatch-lite ------------------------------------------------
namespace {
struct FindState {
    DIR* dir;
    char pat[512];             // pattern component (may contain * / ?)
    char dirPath[1024];
};
bool GlobMatch(const char* pat, const char* s) {
    // Case-insensitive * and ? matching (all any Find pattern here uses).
    while (*pat) {
        if (*pat == '*') {
            ++pat;
            if (!*pat) return true;
            for (const char* t = s; ; ++t) {
                if (GlobMatch(pat, t)) return true;
                if (!*t) return false;
            }
        }
        if (!*s) return false;
        if (*pat != '?' &&
            tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return false;
        ++pat; ++s;
    }
    return !*s;
}
bool FindEmit(FindState* fs, WIN32_FIND_DATAA* fd) {
    dirent* e;
    while ((e = readdir(fs->dir)) != nullptr) {
        if (!GlobMatch(fs->pat, e->d_name)) continue;
        memset(fd, 0, sizeof(*fd));
        snprintf(fd->cFileName, sizeof(fd->cFileName), "%s", e->d_name);
        char full[1200];
        snprintf(full, sizeof(full), "%s/%s", fs->dirPath, e->d_name);
        struct stat st{};
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            else fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeLow = (DWORD)(st.st_size & 0xFFFFFFFFu);
            fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
            // st_mtime -> FILETIME (the save-list UI reads write times relatively)
            uint64_t t = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
            fd->ftLastWriteTime.dwLowDateTime = (DWORD)t;
            fd->ftLastWriteTime.dwHighDateTime = (DWORD)(t >> 32);
        }
        return true;
    }
    return false;
}
} // namespace

HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* fd) {
    char norm[1024];
    HostResolvePath(pattern, norm, sizeof(norm));   // resolves the DIRECTORY part's case
    const char* slash = strrchr(norm, '/');
    FindState* fs = new FindState{};
    if (slash) {
        snprintf(fs->dirPath, sizeof(fs->dirPath), "%.*s", (int)(slash - norm), norm);
        snprintf(fs->pat, sizeof(fs->pat), "%s", slash + 1);
    } else {
        snprintf(fs->dirPath, sizeof(fs->dirPath), ".");
        snprintf(fs->pat, sizeof(fs->pat), "%s", norm);
    }
    fs->dir = opendir(fs->dirPath);
    if (!fs->dir || !FindEmit(fs, fd)) {
        if (fs->dir) closedir(fs->dir);
        delete fs;
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)fs;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd) {
    return FindEmit((FindState*)h, fd) ? TRUE : FALSE;
}
BOOL FindClose(HANDLE h) {
    FindState* fs = (FindState*)h;
    if (fs->dir) closedir(fs->dir);
    delete fs;
    return TRUE;
}

// ---- MSVC secure-CRT shims --------------------------------------------------------------------
errno_t fopen_s(FILE** f, const char* path, const char* mode) {
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    *f = fopen(rp, mode);
    return *f ? 0 : errno;
}
errno_t _dupenv_s(char** out, size_t* len, const char* name) {
    const char* v = getenv(name);
    if (!v) { *out = nullptr; if (len) *len = 0; return 0; }
    *out = strdup(v);
    if (len) *len = strlen(v) + 1;
    return 0;
}
int _vsnprintf_s_impl(char* buf, size_t cap, const char* fmt, va_list ap) {
    int r = vsnprintf(buf, cap, fmt, ap);
    return r < 0 || (size_t)r >= cap ? (int)cap - 1 : r;
}
errno_t strncpy_s_impl(char* dst, size_t cap, const char* src, size_t count) {
    if (!dst || !cap) return 22;
    if (!src) { dst[0] = 0; return 22; }
    size_t n = strlen(src);
    if (count != _TRUNCATE && n > count) n = count;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}
errno_t strcat_s_impl(char* dst, size_t cap, const char* src) {
    size_t d = strlen(dst);
    if (d >= cap) return 22;
    return strncpy_s_impl(dst + d, cap - d, src, _TRUNCATE);
}

// ---- INI persistence (GetPrivateProfileIntA / WritePrivateProfileStringA) -----------
// A minimal faithful [section] key=value store: read parses; write rewrites the file
// with the one key replaced or appended, preserving everything else verbatim.
namespace {
bool IniFindValue(const char* path, const char* section, const char* key,
                  char* out, size_t cap) {
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    FILE* f = fopen(rp, "r");
    if (!f) return false;
    char line[512];
    bool inSec = false, found = false;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[') {
            char* e = strchr(p, ']');
            inSec = e && strncasecmp(p + 1, section, (size_t)(e - p - 1)) == 0 &&
                    strlen(section) == (size_t)(e - p - 1);
            continue;
        }
        if (!inSec) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        size_t kl = (size_t)(eq - p);
        while (kl && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) --kl;
        if (kl != strlen(key) || strncasecmp(p, key, kl) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r' || v[vl - 1] == ' ')) --vl;
        if (vl >= cap) vl = cap - 1;
        memcpy(out, v, vl);
        out[vl] = 0;
        found = true;
        break;
    }
    fclose(f);
    return found;
}
} // namespace

UINT GetPrivateProfileIntA(const char* section, const char* key, int def, const char* path) {
    char v[64];
    if (!IniFindValue(path, section, key, v, sizeof(v))) return (UINT)def;
    return (UINT)strtol(v, nullptr, 10);
}

BOOL WritePrivateProfileStringA(const char* section, const char* key, const char* value,
                                const char* path) {
    // WIN32 NULL SEMANTICS, which this shim did not implement and which crashed the phone:
    //   section == NULL  -> "flush the cached copy of the ini". Windows callers use the
    //                       triple-null call as a commit barrier; there is no cache here, so
    //                       it is a no-op that must simply SUCCEED.
    //   key     == NULL  -> delete the whole section.
    //   value   == NULL  -> delete the key (already handled by the splice below).
    // Without the first line, the parse loop reached strlen(section) on the ini's very first
    // '[' line and dereferenced null. ArabicSave() makes exactly that call right after
    // writing the language, so EVERY switch to Arabic killed the process on ARM -- while on
    // Windows the same line is a documented no-op.
    if (!section) return TRUE;
    char rp[1024];
    HostResolvePath(path, rp, sizeof(rp));
    // Read the whole file, splice the key.
    char* body = nullptr;
    size_t bodyLen = 0;
    if (FILE* f = fopen(rp, "r")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        body = (char*)malloc((size_t)n + 1);
        bodyLen = fread(body, 1, (size_t)n, f);
        body[bodyLen] = 0;
        fclose(f);
    }
    FILE* o = fopen(rp, "w");
    if (!o) { free(body); return FALSE; }
    bool wroteKey = false, inSec = false, sawSec = false;
    char line[512];
    const char* p = body ? body : "";
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) + 1 : strlen(p);
        if (ll >= sizeof(line)) ll = sizeof(line) - 1;
        memcpy(line, p, ll);
        line[ll] = 0;
        p += ll;
        const char* t = line;
        while (*t == ' ' || *t == '\t') ++t;
        if (*t == '[') {
            if (inSec && !wroteKey && value) {           // leaving our section: append key
                fprintf(o, "%s=%s\n", key, value);
                wroteKey = true;
            }
            const char* e = strchr(t, ']');
            inSec = e && strlen(section) == (size_t)(e - t - 1) &&
                    strncasecmp(t + 1, section, (size_t)(e - t - 1)) == 0;
            if (inSec && !key) { sawSec = true; continue; }   // key == NULL: drop the section
            if (inSec) sawSec = true;
            fputs(line, o);
            continue;
        }
        if (inSec && !key) continue;                     // ...and everything inside it
        if (inSec) {
            const char* eq = strchr(t, '=');
            if (eq) {
                size_t kl = (size_t)(eq - t);
                while (kl && (t[kl - 1] == ' ' || t[kl - 1] == '\t')) --kl;
                if (kl == strlen(key) && strncasecmp(t, key, kl) == 0) {
                    if (value && !wroteKey) fprintf(o, "%s=%s\n", key, value);
                    wroteKey = true;                     // (null value = delete the key)
                    continue;
                }
            }
        }
        fputs(line, o);
    }
    if (!wroteKey && value && key) {
        if (!sawSec) fprintf(o, "[%s]\n", section);
        else if (inSec) { /* section was last: fall through and append */ }
        fprintf(o, "%s=%s\n", key, value);
    }
    fclose(o);
    free(body);
    return TRUE;
}

#endif // !_WIN32
