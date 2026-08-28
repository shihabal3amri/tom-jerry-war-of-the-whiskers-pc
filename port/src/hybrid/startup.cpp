// Native startup chain for the hybrid bring-up. Instead of running the Xbox CRT/XAPI
// bootstrap (which reads the Xbox KPCR via fs: and mounts drives), we replicate only
// what the game needs, then run the original C/C++ initializer tables and hand off to
// the game's main. Every stage runs under SEH so a fault reports the exact original
// function (symbolized) that blocked us -- the crash-by-crash bring-up loop.
#include "hybrid/xbe_image.h"
#include "hybrid/symbolize.h"
#include "hybrid/guest_call.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdint>

namespace tj::hybrid {

bool CreateGameHeap();   // crt_glue.cpp
void InstallCrtGlue();   // crt_glue.cpp
void ApplyFsFixups();    // fs_fixup.cpp
int  InstallD3D8Bridge();// d3d8_bridge.cpp
void SnapshotDSoundGameFns(); // dsound_bridge.cpp (must run BEFORE InstallD3D8Bridge)
int  InstallDSoundBridge();   // dsound_bridge.cpp (must run AFTER InstallD3D8Bridge)
int  InstallFeMenu();         // fe_menu.cpp (VIDEO screen re-enable + RESOLUTION row)
int  InstallNetSync();        // net_sync.cpp (phase-2 LAN: determinism instrumentation)
int  InstallLanMatch();       // lan_match.cpp (launch parity, FIGHT SETTINGS, ROUNDS, names)
int  InstallLanUi();          // lan_ui.cpp    (main-menu row, browser, lobby, text modal)
int  InstallAudioUi();        // audio_ui.cpp  (two independent music/effects sliders)
int  InstallArabic();         // arabic.cpp    (Arabic glyphs + strings; TJ_ARABIC=1)
int  InstallMeatRush();       // meat_rush.cpp (MEAT RUSH: the meat is the only item)
int  InstallMeatUi();         // meat_ui.cpp   (the MAX MEAT rule on FIGHT SETTINGS)
int  InstallMeatMenu();       // meat_menu.cpp (MULTIPLAYER row + the submenu on screen 14)
int  InstallProf();           // prof.cpp     (TJ_PROF: Stage-0 EIP sampler, Android plan)
bool RunGameMainEngine(uint32_t vaGameMain);  // engine_mode.cpp (TJ_ENGINE=1, Stage 3)
void EngineModeCrashDump();                   // engine_mode.cpp (VEH FATAL context)

// Original init entry points (see re: __rtinit/__cinit walk the CRT init tables).
static const uint32_t VA_rtinit = 0x000761fb;   // __xi table (heap/fpu pre-init)
static const uint32_t VA_cinit  = 0x000761a3;   // __xc C++ ctors + __xp
static const uint32_t VA_gameMain = 0x00019490; // game main: init + frame loop
static const uint32_t VA_gameInit = 0x000191f0; // game init (called once by main)

typedef void (__cdecl* VoidFn)(void);

// One guest void() call, host-appropriate: native on the x86 host (the pre-engine
// startup init is deliberately native there — TJ_ENGINE arms later), marshaled through
// Engine::Call on ARM (the engine is armed from process start; EngineModeArmBoot).
static void CallGuestVoid(uint32_t va) {
#if defined(_M_IX86)
    ((VoidFn)(uintptr_t)va)();
#else
    GCALL0(Cdecl, VoidFn, va);
#endif
}

#ifdef _WIN32
// Heuristic stack trace: scan the stack for values that point just past a `call`
// instruction in the XBE image -- those are return addresses. Prints a plausible call
// chain, symbolized. Good enough to locate the caller during bring-up.
static bool LooksLikeRet(uint32_t v) {
    if (v < 0x11000 || v >= 0x0165d0c0) return false;
    __try {
        uint8_t* p = (uint8_t*)(uintptr_t)v;
        if (p[-5] == 0xE8) return true;                 // call rel32
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true; // call r/m (2-byte)
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true; // call [reg+disp8]
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true; // call [disp32]
        if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true; // call [reg*s+disp32]
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
// Symbolize any address: XBE code by function name, else owning module + offset.
static const char* SymAny(uint32_t at) {
    static char buf[176];
    if (at >= 0x11000 && at < 0x0165d0c0) return Symbolize(at);
    HMODULE m = nullptr; char mn[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)at, &m) && m) {
        GetModuleFileNameA(m, mn, MAX_PATH);
        const char* b = strrchr(mn, '\\'); b = b ? b+1 : mn;
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s+0x%x", b, at - (uint32_t)(uintptr_t)m);
    } else _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08x", at);
    return buf;
}

// Walk the EBP frame chain (reliable for the game's standard-prologue functions; FPO
// frames break the chain, at which point we stop). Prints return addresses symbolized.
static void StackTrace(uint32_t ebp, int maxFrames) {
    printf("[VEH] call stack (ebp chain):\n");
    for (int i = 0; i < maxFrames; ++i) {
        if (ebp < 0x10000 || (ebp & 3)) break;
        uint32_t next, ret;
        __try { next = *(uint32_t*)(uintptr_t)ebp; ret = *(uint32_t*)(uintptr_t)(ebp + 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (ret == 0) break;
        printf("    #%-2d %s\n", i, SymAny(ret));
        if (next <= ebp) break;   // frames grow upward
        ebp = next;
    }
    fflush(stdout);
}

// The game installs its own fs:[0] SEH frames, which intercept faults before our
// __except. A vectored handler runs first, so it reliably reports the real fault site.
static bool g_vehStop = true;
static LONG CALLBACK HybridVeh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Ignore benign/continuable codes the CRT raises intentionally (C++ EH, MSVC name).
    if (code == 0x40010006 /*DBG_PRINTEXCEPTION*/ || code == 0x406D1388 /*SetThreadName*/)
        return EXCEPTION_CONTINUE_SEARCH;
    // Reentrancy guard: the register-dump below probes memory under __try; that probe's own
    // AV lands here first (vectored runs before SEH). Pass it through so the __except eats it.
    static DWORD inVeh = 0;
    if (inVeh == GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
    inVeh = GetCurrentThreadId();
    struct VehScope { DWORD* p; ~VehScope() { *p = 0; } } vehScope{ &inVeh };
    uint32_t at = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uint32_t op = ep->ExceptionRecord->NumberParameters > 0 ? (uint32_t)ep->ExceptionRecord->ExceptionInformation[0] : 0;
    uint32_t adr= ep->ExceptionRecord->NumberParameters > 1 ? (uint32_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    if (code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH; // C++ throw: let game handle
    // Only genuine faults are fatal. Codes like STATUS_INVALID_HANDLE (0xC0000008) are
    // often deliberately raised and handled by the game/XAPI SEH (handle-validity probes),
    // so log them as first-chance and let the existing handler run.
    bool fatal = (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                  code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
                  code == EXCEPTION_INT_DIVIDE_BY_ZERO);
    // Symbolize: XBE code by name, otherwise the owning module.
    char loc[160];
    if (at >= 0x11000 && at < 0x0165d0c0) { strncpy_s(loc, Symbolize(at), _TRUNCATE); }
    else {
        HMODULE m = nullptr; char mn[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)at, &m) && m) {
            GetModuleFileNameA(m, mn, MAX_PATH);
            const char* b = strrchr(mn, '\\'); b = b ? b+1 : mn;
            _snprintf_s(loc, sizeof(loc), _TRUNCATE, "%s+0x%x", b, at - (uint32_t)(uintptr_t)m);
        } else _snprintf_s(loc, sizeof(loc), _TRUNCATE, "0x%08x", at);
    }
    printf("\n[VEH] %s code=0x%08lx at %s", fatal ? "FATAL" : "first-chance", code, loc);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        printf("  (%s of 0x%08x)", op==1?"write":op==8?"exec":"read", adr);
    printf("\n"); fflush(stdout);
    static int firstChanceTraces = 0;
    if (fatal) {
        CONTEXT* c = ep->ContextRecord;
        printf("[VEH] eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
               c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        auto dump = [](const char* label, uint32_t a) {
            if (a < 0x10000) { printf("  %s=%08x (low)\n", label, a); return; }
            __try {
                printf("  %s @%08x:", label, a);
                for (int i = -4; i < 12; ++i) printf(" %08x", *(uint32_t*)(uintptr_t)(a + i*4));
                printf("\n");
            } __except (EXCEPTION_EXECUTE_HANDLER) { printf("  %s @%08x <unreadable>\n", label, a); }
        };
        dump("esi", c->Esi); dump("edi", c->Edi); dump("esp", c->Esp);
        EngineModeCrashDump();
    }
    if (fatal || firstChanceTraces++ < 3)
        StackTrace((uint32_t)ep->ContextRecord->Ebp, 16);
    if (fatal && g_vehStop) { fflush(stdout); ExitProcess(0xFA010000u | (at & 0xFFFF)); }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void InstallVeh() { static bool done=false; if(!done){done=true; AddVectoredExceptionHandler(1, HybridVeh);} }

// Run fn() under SEH; on fault, print the symbolized faulting address. Returns true
// if it completed without an exception.
static bool GuardedCall(const char* label, VoidFn fn) {
    printf("[startup] -> %s (0x%p)\n", label, (void*)fn);
    fflush(stdout);
    __try {
        fn();
        printf("[startup] <- %s returned OK\n", label);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        EXCEPTION_POINTERS* ep = nullptr;
        // GetExceptionInformation is only valid in the filter; capture via a helper.
        DWORD code = GetExceptionCode();
        printf("[startup] !! %s FAULTED code=0x%08lx\n", label, code);
        return false;
    }
}

// Variant that captures the faulting address (filter runs before unwind).
static uint32_t g_faultAddr, g_faultCode, g_faultInfo0, g_faultInfo1;
static int FaultFilter(EXCEPTION_POINTERS* ep) {
    g_faultCode = ep->ExceptionRecord->ExceptionCode;
    g_faultAddr = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    g_faultInfo0 = ep->ExceptionRecord->NumberParameters > 0 ? (uint32_t)ep->ExceptionRecord->ExceptionInformation[0] : 0;
    g_faultInfo1 = ep->ExceptionRecord->NumberParameters > 1 ? (uint32_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool GuardedCallAddr(const char* label, uint32_t va) {
    VoidFn fn = (VoidFn)(uintptr_t)va;
    printf("[startup] -> %s @ %s\n", label, Symbolize(va));
    fflush(stdout);
    __try {
        fn();
        printf("[startup] <- %s returned OK\n", label);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        const char* kind = g_faultCode == EXCEPTION_ACCESS_VIOLATION ?
            (g_faultInfo0 ? "write" : "read") : "";
        printf("[startup] !! %s FAULT code=0x%08x at %s", label, g_faultCode, Symbolize(g_faultAddr));
        if (g_faultCode == EXCEPTION_ACCESS_VIOLATION)
            printf("  (%s of 0x%08x)", kind, g_faultInfo1);
        printf("\n");
        fflush(stdout);
        return false;
    }
}

#else // !_WIN32 -----------------------------------------------------------------------

// ARM: no VEH, no SEH — a guest problem is reported by the ENGINE (RunResult::Fault
// through the marshal's loud FATAL), and a host fault crashes into headless_main's
// signal dump. The guarded call is a marshaled Engine::Call from process start.
static bool g_vehStop = true;         // referenced by the shared milestone functions
static void InstallVeh() {}
static bool GuardedCallAddr(const char* label, uint32_t va) {
    printf("[startup] -> %s @ %s\n", label, Symbolize(va));
    fflush(stdout);
    CallGuestVoid(va);              // a failure inside is a loud engine FATAL, not a return
    printf("[startup] <- %s returned OK\n", label);
    return true;
}

#endif // _WIN32

// Replicate __cinit's table walks with per-entry logging, so a faulting C++ ctor is
// pinpointed exactly (the VEH prints the address; we print which init entry it was).
static void RunInitTable(const char* which, uint32_t lo, uint32_t hi) {
    for (uint32_t p = lo; p < hi; p += 4) {
        uint32_t fn = *(uint32_t*)(uintptr_t)p;
        if (fn == 0 || fn == 0xFFFFFFFF) continue;
        printf("[cinit] %s[%u] -> %s\n", which, (p - lo) / 4, Symbolize(fn));
        fflush(stdout);
        CallGuestVoid(fn);
    }
}

static void RunCinitVerbose() {
    // __cinit prologue: optional pre-init hook at [0x18270c].
    uint32_t hook = *(uint32_t*)(uintptr_t)0x0018270c;
    if (hook) { printf("[cinit] pre-init hook -> %s\n", Symbolize(hook)); fflush(stdout); CallGuestVoid(hook); }
    // ORDER MATTERS (mirror original __cinit): the 0xf574c table runs first -- it
    // contains ___onexitinit which builds the atexit table the C++ ctors then use.
    RunInitTable("__xp", 0x000f574c, 0x000f5760); // onexit init + CRT pre-ctors
    RunInitTable("__xc", 0x000f5710, 0x000f5748); // C++ constructors
}

// Mimic the XDK CRT __controlfp: default x87 precision/rounding the game expects.
static void SetupFpu() {
#if defined(_M_IX86)
    unsigned cw = 0x027F; // round-nearest, double-extended precision, all exceptions masked
    __asm { fninit }
    __asm { fldcw cw }
#else
    // The "host FPU" is the virtual fnsave image (fpu_host.cpp): same fninit + fldcw
    // state, byte for byte. EngineModeArmBoot did this too; idempotent by construction.
    uint8_t* im = tj::engine::VirtualHostFpu()->image;
    memset(im, 0, 108);
    im[0] = 0x7F; im[1] = 0x02;   // CW 0x027F
    im[8] = 0xFF; im[9] = 0xFF;   // TW all-empty
#endif
}

// Milestone 2: run the CRT initializer tables (static constructors). Returns count of
// stages that completed.
int RunStartupInit() {
    printf("\n=== Milestone 2: native startup -> CRT init tables ===\n");
    LoadSymbols();
    InstallVeh();
    SetupFpu();
    ApplyFsFixups();
    EngineModeRefreshFsBase();   // ARM boots armed: fs:[...] must resolve to the KPCR
                                 // BEFORE the first marshaled init call runs. Windows:
                                 // harmless, the engine body re-asserts it later.
    printf("[startup] creating game heap...\n"); fflush(stdout);
    if (!CreateGameHeap()) { printf("[startup] heap creation FAILED\n"); return 0; }
    InstallCrtGlue();
    int ok = 0;
    if (GuardedCallAddr("__rtinit (__xi)", VA_rtinit)) ++ok;
    printf("[startup] -> __cinit (verbose walk)\n"); fflush(stdout);
    RunCinitVerbose();
    printf("[startup] <- __cinit completed\n");
    ++ok;
    printf("[startup] init stages completed: %d/2\n", ok);
    return ok;
}

// Milestone 3: run the game's one-time init (0x191f0, called by main before its frame
// loop). This is where the real engine init happens -- display, assets, front end.
bool RunGameMain() {
    printf("\n=== Milestone 3: game init (0x191f0) ===\n");
    g_vehStop = true;
    printf("[startup] installing D3D8->D3D11 bridge...\n");
    SnapshotDSoundGameFns();   // capture entries the D3D bridge's audio stubs overwrite
    InstallD3D8Bridge();
    InstallDSoundBridge();     // XAudio2 shims re-patch over the plain DSOUND stubs
    return GuardedCallAddr("game init", VA_gameInit);
}

// Milestone 4: run the game's real entry (init + frame loop). Never returns normally --
// the loop runs the front-end state machine (update FUN_000179c0 + render FUN_00017500)
// and presents via the bridged Swap. We run it for the session and screenshot the window.
bool RunGameLoop() {
    printf("\n=== Milestone 4: game main + frame loop (0x19490) ===\n");
    g_vehStop = true;
    printf("[startup] installing D3D8->D3D11 bridge...\n");
    SnapshotDSoundGameFns();   // capture entries the D3D bridge's audio stubs overwrite
    InstallD3D8Bridge();
    InstallDSoundBridge();     // XAudio2 shims re-patch over the plain DSOUND stubs
    InstallLanUi();            // phase-2 LAN screens (must precede the menu row it labels)
    InstallFeMenu();           // options-menu VIDEO row + native RESOLUTION option
    InstallAudioUi();          // two independent audio sliders (frontend + pause badge)
    InstallArabic();           // Arabic pack (glyph art + shaped strings); no-op unless asked
    InstallLanMatch();         // phase-2 LAN: launch parity, FIGHT SETTINGS, ROUNDS, names
    InstallMeatRush();         // MEAT RUSH: item scheduler steering (before the match)
    InstallMeatUi();           // MEAT RUSH: the MAX MEAT rule
    InstallMeatMenu();         // MEAT RUSH: the MULTIPLAYER menu (before the menu Build hook)
    InstallNetSync();          // phase-2 LAN: main-loop phase brackets + RNG counters
    InstallProf();             // TJ_PROF only; must be on the game thread (samples it)
#if defined(_M_IX86)
    char eng[8] = {0};
    if (GetEnvironmentVariableA("TJ_ENGINE", eng, 8) && eng[0] == '1')
        return RunGameMainEngine(VA_gameMain);   // Stage 3: interpret the whole game
    return GuardedCallAddr("game main", VA_gameMain);
#else
    return RunGameMainEngine(VA_gameMain);       // ARM: there is only the engine
#endif
}

} // namespace tj::hybrid
