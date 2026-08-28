#include "hybrid/xdk_patch.h"
#include <cstdlib>
#include "hybrid/xbe_image.h"
#include "hybrid/guest_call.h"
#include "hybrid/host_compat.h"
#include <cstdio>

namespace tj::hybrid {

// THE PATCH-SET FINGERPRINT — an AUTOMATIC, platform-independent identity for the shape of
// the sim contract. Every successful patch install XORs in Fnv32(label) mixed with its VA, so
// the value changes the moment a hook is added, removed, or moved, WITHOUT anyone having to
// remember to bump a constant. XOR because install order is not part of the contract, and the
// inputs (label text, guest VA) are identical on every platform by construction — unlike the
// module bytes, which can never match across a .dll and a .so.
//
// ⚠ WHAT IT DOES NOT CATCH: a change INSIDE a hook body — same site, same label, different
// behaviour. That is what net_lan.cpp's kSimContract constant is for. The two are hashed
// together, and this one exists so the constant is only load-bearing for the cases a machine
// genuinely cannot see.
static uint32_t g_patchFp = 0;
// TJ_LAN_PATCHDUMP=1 prints every site as it is installed, so two platforms whose
// fingerprints disagree can be DIFFED instead of guessed about — which is the whole point of
// having a fingerprint rather than a version constant.
static int PatchDumpOn() {
    static int on = -1;
    if (on < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_LAN_PATCHDUMP");
                  on = (e && *e && atoi(e)) ? 1 : 0; free(e); }
    return on;
}
static void NotePatch(uint32_t va, const char* label) {
    uint32_t h = 2166136261u;
    for (const char* c = label; c && *c; ++c) { h ^= (uint8_t)*c; h *= 16777619u; }
    for (int i = 0; i < 4; ++i) { h ^= (uint8_t)(va >> (i * 8)); h *= 16777619u; }
    g_patchFp ^= h;
    if (PatchDumpOn()) printf("[patchfp] %08x %s\n", va, label ? label : "(null)");
}
uint32_t PatchSetFingerprint() { return g_patchFp; }

static bool PatchJumpBytes(uint32_t va, const void* target, const char* label) {
    if (!InImage(va, 5)) { printf("[patch] %s: va %08x not in image\n", label, va); return false; }
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)va, 8, PAGE_EXECUTE_READWRITE, &old)) {
        printf("[patch] %s: VirtualProtect %08x failed %lu\n", label, va, GetLastError());
        return false;
    }
    uint8_t* p = (uint8_t*)(uintptr_t)va;
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(va + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect((void*)(uintptr_t)va, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    NotePatch(va, label);
    return true;
}

// Retarget an existing `call rel32` (opcode E8 at `va`) to `target`, leaving the
// instruction length and the callee's body untouched -- the hook decides whether and when
// to call the original. Used for the lockstep tick point in the game's main loop, where
// the update phase must be wrapped without disturbing the 17-byte loop around it.
static bool PatchCallBytes(uint32_t va, const void* target, const char* label) {
    if (!InImage(va, 5)) { printf("[patch] %s: va %08x not in image\n", label, va); return false; }
    uint8_t* p = (uint8_t*)(uintptr_t)va;
    if (p[0] != 0xE8) {   // refuse rather than corrupt the instruction stream
        printf("[patch] %s: %08x is not a call rel32 (opcode %02x) -- REFUSING\n", label, va, p[0]);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) {
        printf("[patch] %s: VirtualProtect %08x failed %lu\n", label, va, GetLastError());
        return false;
    }
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(va + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    NotePatch(va, label);
    return true;
}

// Typed hooks: record the dispatch entry, then patch to its KEY (on the x86 Windows
// host the key IS the host address — the written bytes are identical to v1.1.0).
bool PatchJump(uint32_t va, const Hook& h, const char* label) {
    uint32_t key = DispatchRegister(h, label);
    return PatchJumpBytes(va, (const void*)(uintptr_t)key, label);
}
bool PatchCallSite(uint32_t va, const Hook& h, const char* label) {
    uint32_t key = DispatchRegister(h, label);
    return PatchCallBytes(va, (const void*)(uintptr_t)key, label);
}

// Untyped targets: escape-only in engine mode, and a named ARM coverage gap. Any patch
// added without a HOOK_* marker lands here and announces itself at install.
bool PatchJump(uint32_t va, const void* target, const char* label) {
    printf("[dispatch] NOTE %s: raw patch target (escape-only, not ARM-portable)\n", label);
    DispatchRegister(Hook{ const_cast<void*>(target), nullptr, CallConv::Cdecl,
                           HookKind::Raw, 0 }, label);
    return PatchJumpBytes(va, target, label);
}
bool PatchCallSite(uint32_t va, const void* target, const char* label) {
    printf("[dispatch] NOTE %s: raw patch target (escape-only, not ARM-portable)\n", label);
    DispatchRegister(Hook{ const_cast<void*>(target), nullptr, CallConv::Cdecl,
                           HookKind::Raw, 0 }, label);
    return PatchCallBytes(va, target, label);
}

// ---- continuation (jmp-hook) patching ----------------------------------------------
bool PatchJmpHook(uint32_t va, bool callShaped, const void* nakedWrapper,
                  uint32_t resumeVa, void(__cdecl* fn)(tj::engine::CpuState&),
                  bool escapeSafe, const char* label) {
#if defined(_M_IX86)
    // Exactly the pre-refactor two-step: HOOK_RAW-style registration + patch to the
    // naked wrapper, then the engine jmp-hook at the wrapper's address.
    Hook h{ const_cast<void*>(nakedWrapper), nullptr, CallConv::Cdecl, HookKind::Raw, 0 };
    bool ok = callShaped ? PatchCallSite(va, h, label) : PatchJump(va, h, label);
    if (ok) EngineModeRegisterJmpHook(nakedWrapper, resumeVa, fn, escapeSafe);
    return ok;
#else
    (void)nakedWrapper;
    // Mint a synthetic key for the semantic (Raw, non-invocable: HostEscape's jmp-hook
    // check runs BEFORE the dispatch invoke, so the entry only exists to own a key and
    // appear in the coverage report).
    Hook h{ (void*)fn, nullptr, CallConv::Cdecl, HookKind::Raw, 0 };
    uint32_t key = DispatchRegister(h, label);
    bool ok = callShaped ? PatchCallBytes(va, (const void*)(uintptr_t)key, label)
                         : PatchJumpBytes(va, (const void*)(uintptr_t)key, label);
    if (ok) EngineModeRegisterJmpHook((const void*)(uintptr_t)key, resumeVa, fn, escapeSafe);
    return ok;
#endif
}

// ---- guest-visible host objects -----------------------------------------------------
uint32_t Gp32(const void* p) {
    uintptr_t u = (uintptr_t)p;
#if !defined(_M_IX86)
    if (u >> 32) {
        printf("[patch] FATAL: guest-visible object above 4 GB (%p)\n", p);
        fflush(stdout);
        ExitProcess(0x69B);
    }
#endif
    return (uint32_t)u;
}

char* GuestStrDup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)GuestObjAlloc(n, 1);
    memcpy(d, s, n);
    return d;
}

// Intern a possibly-host-image C string into the guest arena, returning a STABLE below-4GB
// copy. The guest STORES the returned pointer and reuses it across frames (e.g. FE_GetText's
// menu labels), so the SAME source pointer must always map to the SAME arena copy — a
// per-call GuestStrDup would leak the arena a string per frame. On the x86 host every pointer
// is already below 4 GB, so this is identity passthrough and the Windows path stays
// byte-identical. On ARM it relocates host string literals (a whole class of gptr bug the
// >4GB return-pointer tripwire otherwise catches only at runtime: FE_GetText's MeatMenuText
// kText[], MeatCustomText "MAX MEAT:"/kValText[] were host .rodata).
const char* GuestInternStr(const char* s) {
    if (!s) return s;
#if !defined(_M_IX86)
    if (((uintptr_t)s >> 32) == 0) return s;     // already guest-visible (arena / guest addr)
    static const char* src[256];
    static const char* dup[256];
    static int n = 0;
    for (int i = 0; i < n; ++i) if (src[i] == s) return dup[i];
    const char* d = GuestStrDup(s);
    if (n < 256) { src[n] = s; dup[n] = d; ++n; }
    else {
        // NOT "never expected" -- the Arabic pack has more strings than this cache has slots,
        // and past the last slot every call re-duplicates into the guest arena on every draw
        // until the below-4GB region is exhausted. That is a silent death; say it once, loudly,
        // so the next provider that overflows this is found by reading a log instead of by
        // bisecting a crash. (The fix is to make the provider's storage guest-visible.)
        static bool warned = false;
        if (!warned) {
            warned = true;
            printf("[patch] WARN: GuestInternStr cache FULL (256) -- every further string is\n"
                   "        re-duplicated into the guest arena PER DRAW and will exhaust it.\n"
                   "        Allocate that provider's strings below 4 GB instead.\n");
            fflush(stdout);
        }
    }
    return d;
#else
    return s;
#endif
}

void* GuestObjAlloc(size_t size, size_t align) {
    static uint8_t* chunk = nullptr;
    static size_t used = 0, cap = 0;
    if (align < 8) align = 8;
    used = (used + align - 1) & ~(align - 1);
    if (!chunk || used + size > cap) {
        cap = size > (256u << 10) ? ((size + 0xFFFFu) & ~(size_t)0xFFFFu) : (256u << 10);
        chunk = (uint8_t*)VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
        used = 0;
        if (!chunk) { printf("[patch] FATAL: guest-object arena alloc failed\n");
                      fflush(stdout); ExitProcess(0x69C); }
    }
    void* p = chunk + used;
    used += size;
    return p;
}

// ---- guest-window trampoline pad --------------------------------------------------
// One 64 KB RWX block inside the guest window, found by scanning the unreserved gap
// between the XBE image end (0x0165D0C0) and the contiguous pool (0x04000000) from the
// top down. Address value is irrelevant (trampoline VAs are runtime data); being BELOW
// 0x04000000 keeps it clear of the pool/arena section views, and being inside the
// engine's exec range makes interpreted execution ordinary guest code — no escape.
static uint8_t* g_trampPad = nullptr;
static uint32_t g_trampUsed = 0;
constexpr uint32_t kTrampPadSize = 64 * 1024;

// Claim the pad EARLY (hybrid_run, before D3D11 initializes): once the driver starts
// carving the 32-bit address space, the whole image-end..pool gap fills with 64K
// driver chunks and no in-window address survives to install time (measured).
void ReserveTrampPad() {
    if (g_trampPad) return;
    for (uint32_t base = 0x03F00000u; base >= 0x01700000u; base -= 0x10000u) {
        void* p = VirtualAlloc((void*)(uintptr_t)base, kTrampPadSize,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) {
            g_trampPad = (uint8_t*)p;
            printf("[tramp] guest-window pad at %08X (%u KB)\n", base, kTrampPadSize / 1024);
            return;
        }
    }
    printf("[tramp] NO guest-window pad address free — host RWX fallback (not ARM-portable)\n");
}

static void EnsureTrampPad() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    ReserveTrampPad();      // late fallback path (self-tests that skip hybrid_run's call)
}

uint32_t MakeGuestTramp(uint32_t va, int prologueLen, const char* label) {
    EnsureTrampPad();
    uint8_t* t;
    if (g_trampPad && g_trampUsed + 32 <= kTrampPadSize) {
        t = g_trampPad + g_trampUsed;
        g_trampUsed += 32;
    } else {
        t = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
        if (!t) { printf("[tramp] %s: alloc failed\n", label); return 0; }
        // Host address: engine mode reaches it only via the escape — record it so the
        // coverage report stays honest about the ARM gap.
        DispatchRegister(Hook{ t, nullptr, CallConv::Cdecl, HookKind::Raw, 0 }, label);
    }
    memcpy(t, (const void*)(uintptr_t)va, (size_t)prologueLen);
    // Relocate a LEADING call/jmp rel32 (net_sync's historical case; the only rel32
    // relocation any patched prologue requires — asserted by the per-site hand checks).
    const uint8_t* src = (const uint8_t*)(uintptr_t)va;
    if (prologueLen >= 5 && (src[0] == 0xE8 || src[0] == 0xE9)) {
        int32_t rel; memcpy(&rel, src + 1, 4);
        uint32_t target = va + 5 + (uint32_t)rel;
        int32_t newRel = (int32_t)(target - ((uint32_t)(uintptr_t)t + 5));
        memcpy(t + 1, &newRel, 4);
    }
    t[prologueLen] = 0xE9;
    *(int32_t*)(t + prologueLen + 1) =
        (int32_t)(va + (uint32_t)prologueLen) - (int32_t)((uintptr_t)t + (uint32_t)prologueLen + 5);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    return (uint32_t)(uintptr_t)t;
}

} // namespace tj::hybrid
