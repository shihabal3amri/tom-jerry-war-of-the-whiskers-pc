// The guest->host dispatch registry (see dispatch.h for the design). Single game
// thread, like everything else on this boundary: registration happens at install time,
// lookups happen on the interpreting thread only.
#include "hybrid/dispatch.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
extern "C" {
#include "softfloat.h"      // extF80_to_f32 for the ARM float-return pop
}
#endif
#include <cstdio>

namespace tj::hybrid {

// Loud stop, portable: a boundary error must never limp on.
[[noreturn]] static void FatalStop(unsigned code) {
    fflush(stdout);
#ifdef _WIN32
    ExitProcess(code);
#else
    _Exit((int)(code & 0xFF) ? (int)(code & 0xFF) : 1);
#endif
}

namespace dispatch_detail {
#if !defined(_M_IX86)
[[noreturn]] void DispatchFatal(const char* what) {
    printf("[dispatch] FATAL: %s\n", what);
    FatalStop(0xD5);
}
// Richer variant for the >4GB return-pointer tripwire: names the hook, its key, the bad
// value, and the guest CALL SITE (the return EIP) so the offending path is identifiable
// from the log without a rebuild.
[[noreturn]] void DispatchFatalRetPtr(const char* label, uint32_t key,
                                      unsigned long long val, uint32_t site) {
    printf("[dispatch] FATAL: hook '%s' (key %08X) returned host pointer %llX (>4 GB) "
           "to guest call site %08X — relocate its result below 4 GB (GuestObjAlloc)\n",
           label ? label : "?", key, val, site);
    FatalStop(0xD5);
}
#endif
} // namespace dispatch_detail

namespace {

using tj::engine::CpuState;
using dispatch_detail::GuestLd32;

constexpr int kMaxEntries = 512;
DispatchEntry g_tab[kMaxEntries];
int           g_n = 0;
int           g_mergedDups = 0;
int           g_foldMerges = 0;

// Open-addressed key->index map (power-of-two, linear probe; ~230 live entries).
constexpr uint32_t kHashSize = 2048;                 // > 3x entries, probe stays short
int16_t g_slot[kHashSize];                            // index+1, 0 = empty
bool    g_slotInit = false;

uint32_t HashKey(uint32_t k) { k *= 0x9E3779B1u; return k >> 21; } // top 11 bits

const DispatchEntry* Find(uint32_t key) {
    if (!g_slotInit) return nullptr;
    for (uint32_t h = HashKey(key) & (kHashSize - 1); g_slot[h]; h = (h + 1) & (kHashSize - 1)) {
        const DispatchEntry& e = g_tab[g_slot[h] - 1];
        if (e.key == key) return &e;
    }
    return nullptr;
}

void Insert(int idx) {
    uint32_t h = HashKey(g_tab[idx].key) & (kHashSize - 1);
    while (g_slot[h]) h = (h + 1) & (kHashSize - 1);
    g_slot[h] = (int16_t)(idx + 1);
}

// Unregistered host targets seen by the engine (the ARM coverage list). Logged once each.
constexpr int kMaxMiss = 64;
uint32_t g_missVa[kMaxMiss];
int      g_missN = 0;
uint64_t g_invokes = 0;

// Innermost active hooks, for the VEH crash dump (a hook that faults deep inside the
// bridge is otherwise anonymous — no guest stack walk can name it).
const DispatchEntry* g_active[32];
int g_depth = 0;

// The live interpreted frame while a hook runs (null entries mark escape excursions —
// native code running ON the guest stack, where a marshaled call's frame placement
// would collide with the native frames; those contexts use the gate instead).
CpuState* g_frames[32];
int       g_frameDepth = 0;
bool      g_marshalCalls = true;      // TJ_ENG_GATECALL=1 clears (A/B kill switch)
uint64_t  g_guestCalls = 0;
#if defined(_M_IX86)
uint8_t   g_gcSentinelByte;           // nested-Run stop address (host static, never guest)
#define GC_SENTINEL ((uint32_t)(uintptr_t)&g_gcSentinelByte)
#else
// A truncated host static could collide with guest addresses and varies per ASLR run —
// nondeterminism the S5c oracle cannot tolerate. Fixed constant outside the guest
// window and the synthetic-key space.
#define GC_SENTINEL 0xF00D0001u
#endif

uint32_t AddEntry(const DispatchEntry& e) {
    if (!g_slotInit) { memset(g_slot, 0, sizeof g_slot); g_slotInit = true; }
    if (DispatchEntry* old = const_cast<DispatchEntry*>(Find(e.key))) {
        // Same target registered twice (Sh_ok0 across many ordinals, one hook patched
        // at several sites). Identical semantics merge silently; a semantic conflict
        // would corrupt the sim from inside the boundary — refuse to run.
        if (old->invoke == e.invoke && old->argBytes == e.argBytes &&
            old->kind == e.kind && old->ctx == e.ctx) { ++g_mergedDups; return e.key; }
        // COMDAT folding (/OPT:ICF here, lld ICF on ARM): two functions with identical
        // machine code share one address — first seen: Sh_MmPersistContiguousMemory ==
        // Br_SetVertexShaderInput. One body, so any invoke that provides its inputs and
        // cleans the same bytes is correct; equal argBytes is the safety condition (a
        // `ret N` byte difference would have prevented the fold). Keep the TYPED entry:
        // it passes the declared register AND stack args, a superset of ArgBytes'.
        if (old->invoke && e.invoke && old->argBytes == e.argBytes &&
            old->kind != HookKind::Data && e.kind != HookKind::Data) {
            if (old->kind == HookKind::ArgBytes && e.kind == HookKind::Typed) {
                const char* keepLabel = e.label;
                *old = e;                       // richer marshaling wins; slot index stays
                old->label = keepLabel;
            }
            ++g_foldMerges;
            return e.key;
        }
        printf("[dispatch] FATAL: key %08X registered twice with different semantics "
               "(%s vs %s)\n", e.key, old->label, e.label);
        FatalStop(0xD15BAD);
    }
    if (g_n >= kMaxEntries) {
        printf("[dispatch] FATAL: table full at %s\n", e.label);
        FatalStop(0xD15F11);
    }
    g_tab[g_n] = e;
    Insert(g_n);
    ++g_n;
    return e.key;
}

// ---- generic invokes -------------------------------------------------------------

// Kernel-table shims: convention recorded as DATA (argBytes stack dwords, stdcall
// cleanup; the four fastcall ordinals all take zero marshaled args). Args are passed
// as 4-byte values — every kernel shim parameter is a 32-bit scalar or guest pointer.
//
// ⚠⚠ THE 4-BYTE-SLOT RULE IS A HARD REQUIREMENT ON THE SHIM'S C SIGNATURE, NOT A
// DESCRIPTION. This table is UNTYPED — `host` is a void* and the call below goes through an
// all-uint32_t prototype — so nothing checks that a shim agrees. On x86 nothing had to:
// void* and uint32_t are both 4 bytes. On AArch64 they are not, and the two halves of the
// ABI disagree differently:
//   * ARGUMENTS 1-8 travel in x0-x7. A shim may declare those `void*` and get away with it,
//     because the caller's `w` write zero-extends into the full `x` the callee reads.
//   * ARGUMENT 9 ONWARD travels ON THE STACK, and there the sizes must match exactly. The
//     caller lays down a 4-byte slot per argument; a `void*` parameter reads EIGHT, so it
//     assembles its pointer from two adjacent argument slots and every later argument is
//     read at the wrong offset.
// Only two shims in this table have more than eight arguments (NtCreateFile 9,
// NtQueryDirectoryFile 10). NtQueryDirectoryFile declared its 9th `void*` and crashed Save
// Game on the device — `fileMask` arrived as 0x487a60_00000000. If you add a 9th argument to
// any shim here, IT MUST BE A uint32_t and the pointer cast belongs inside the body.
void InvokeArgBytes(CpuState& s, const DispatchEntry& e) {
    const uint32_t esp = s.r[tj::engine::ESP];
    const uint32_t ret = GuestLd32(esp);
    const int k = e.argBytes / 4;
    uint32_t a[10] = {};
    for (int i = 0; i < k && i < 10; ++i) a[i] = GuestLd32(esp + 4 + 4 * (uint32_t)i);
    using U = uint32_t;
    uint64_t r;
    switch (k) {
    case 0:  r = ((uint64_t(__stdcall*)())e.host)(); break;
    case 1:  r = ((uint64_t(__stdcall*)(U))e.host)(a[0]); break;
    case 2:  r = ((uint64_t(__stdcall*)(U,U))e.host)(a[0],a[1]); break;
    case 3:  r = ((uint64_t(__stdcall*)(U,U,U))e.host)(a[0],a[1],a[2]); break;
    case 4:  r = ((uint64_t(__stdcall*)(U,U,U,U))e.host)(a[0],a[1],a[2],a[3]); break;
    case 5:  r = ((uint64_t(__stdcall*)(U,U,U,U,U))e.host)(a[0],a[1],a[2],a[3],a[4]); break;
    case 6:  r = ((uint64_t(__stdcall*)(U,U,U,U,U,U))e.host)(a[0],a[1],a[2],a[3],a[4],a[5]); break;
    case 7:  r = ((uint64_t(__stdcall*)(U,U,U,U,U,U,U))e.host)(a[0],a[1],a[2],a[3],a[4],a[5],a[6]); break;
    case 8:  r = ((uint64_t(__stdcall*)(U,U,U,U,U,U,U,U))e.host)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); break;
    case 9:  r = ((uint64_t(__stdcall*)(U,U,U,U,U,U,U,U,U))e.host)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]); break;
    case 10: r = ((uint64_t(__stdcall*)(U,U,U,U,U,U,U,U,U,U))e.host)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9]); break;
    default:
        printf("[dispatch] FATAL: %s argBytes=%d beyond marshal limit\n", e.label, e.argBytes);
        FatalStop(0xD15A26);
        return;
    }
    s.r[tj::engine::EAX] = (uint32_t)r;
#if defined(_M_IX86)
    // x86: the uint64 cast reads the host's REAL edx — what the escape captures, so
    // the A/B compares equal. On AArch64 the high half of x0 is callee JUNK for a
    // 32-bit-returning function (and nondeterministic): leave EDX untouched, the same
    // documented choice ShimCC makes. MSVC guest code only reads a callee's edx for
    // 64-bit returns, which don't exist in this table.
    s.r[tj::engine::EDX] = (uint32_t)(r >> 32);
#endif
    s.r[tj::engine::ESP] = esp + 4 + (uint32_t)e.argBytes;
    s.eip = ret;
}

void InvokeTrap(CpuState& s, const DispatchEntry& e) {
    ((void(__cdecl*)(int))e.host)((int)e.ctx);       // prints the ordinal and exits
    (void)s;
}

// `mov eax, imm32; ret argBytes` — the kernel's arg-cleaning stubs, without the bytes.
void InvokeRetStub(CpuState& s, const DispatchEntry& e) {
    const uint32_t esp = s.r[tj::engine::ESP];
    s.r[tj::engine::EAX] = e.ctx;
    s.r[tj::engine::ESP] = esp + 4 + (uint32_t)e.argBytes;
    s.eip = GuestLd32(esp);
}

// The KEY a registration hands back for storing into guest-reachable slots/patches.
// x86 host: the host function address itself (fits, and byte-patches stay identical to
// v1.1.0). 64-bit host: a synthetic 32-bit HANDLE outside the guest exec range — the
// interpreter reaching it as EIP goes down the same escape->dispatch path. Identical
// re-registrations reuse their handle (the x86 side's dup-merge, done by identity).
uint32_t KeyFor(const DispatchEntry& proto) {
#if defined(_M_IX86)
    return (uint32_t)(uintptr_t)proto.host;
#else
    for (int i = 0; i < g_n; ++i) {
        const DispatchEntry& e = g_tab[i];
        if (e.host == proto.host && e.invoke == proto.invoke && e.kind == proto.kind &&
            e.argBytes == proto.argBytes && e.ctx == proto.ctx)
            return e.key;
    }
    return 0xE0000000u + (uint32_t)g_n * 16u;
#endif
}

} // namespace

uint32_t DispatchRegister(const Hook& h, const char* label) {
    DispatchEntry e{ 0, h.fn, h.invoke, label, h.kind, h.conv, h.stackArgBytes, 0 };
    e.key = KeyFor(e);
    return AddEntry(e);
}
uint32_t DispatchRegisterArgBytes(void* target, const char* name, int argBytes, CallConv conv) {
    DispatchEntry e = argBytes < 0
        // cdecl/varargs (DbgPrint): no fixed marshal — escape-only
        ? DispatchEntry{ 0, target, nullptr, name, HookKind::Raw, CallConv::Cdecl, -1, 0 }
        : DispatchEntry{ 0, target, &InvokeArgBytes, name, HookKind::ArgBytes, conv,
                         (int16_t)argBytes, 0 };
    e.key = KeyFor(e);
    return AddEntry(e);
}
uint32_t DispatchRegisterTrap(void* trapCode, const char* name, uint32_t ordinal,
                              void(__cdecl* report)(int)) {
    // key anchors to the generated trap CODE (what the slot holds on x86); host = the
    // reporter. On a 64-bit host there is no generated code — key by the entry itself.
    DispatchEntry e{ 0, (void*)report, &InvokeTrap, name,
                     HookKind::Trap, CallConv::Stdcall, 0, ordinal };
#if defined(_M_IX86)
    e.key = (uint32_t)(uintptr_t)trapCode;
#else
    (void)trapCode;
    e.key = KeyFor(e);
#endif
    return AddEntry(e);
}
uint32_t DispatchRegisterRetStub(void* stubCode, const char* name, uint32_t retVal, int argBytes) {
    DispatchEntry e{ 0, stubCode, &InvokeRetStub, name,
                     HookKind::RetStub, CallConv::Stdcall, (int16_t)argBytes, retVal };
#if defined(_M_IX86)
    e.key = (uint32_t)(uintptr_t)stubCode;
#else
    e.key = KeyFor(e);
#endif
    return AddEntry(e);
}
uint32_t DispatchRegisterData(void* var, const char* name) {
    // A DATA export's key IS the pointer the guest dereferences — it must be a real
    // sub-4GB address, never a synthetic handle. On a 64-bit host, a var above 4 GB is
    // the below-4GB relocation work list announcing itself.
#if !defined(_M_IX86)
    if ((uintptr_t)var >> 32)
        printf("[dispatch] DATA %s above 4 GB — guest reads WILL be wrong until relocated\n",
               name);
#endif
    return AddEntry({ (uint32_t)(uintptr_t)var, var, nullptr, name,
                      HookKind::Data, CallConv::Stdcall, 0, 0 });
}

void DispatchPushFrame(CpuState* s) {
    if (g_frameDepth < 32) g_frames[g_frameDepth] = s;
    ++g_frameDepth;
}
void DispatchPopFrame() { --g_frameDepth; }
void DispatchSetGuestCallMode(bool marshal) { g_marshalCalls = marshal; }
bool GuestMarshalReady() {
    return g_marshalCalls && g_frameDepth > 0 && g_frameDepth <= 32 &&
           g_frames[g_frameDepth - 1] != nullptr;
}

// Build a guest frame below the current interpreted ESP and run the callee through a
// nested interpretation — the ARM-shaped host->guest call. Register/flag/frame-address
// differences from the legacy gate are all in state the guest cannot legitimately read
// (dead stack, callee-saved round-trips); the x87 handling matches the gate exactly:
// capture the live host FPU as the callee's incoming state, materialize its final
// state (including an ST0 return value) back onto the host FPU afterwards.
uint32_t GuestMarshalCall(uint32_t va, const uint32_t* stackW, int nStackW,
                          uint32_t ecx, uint32_t edx, int expectCleanBytes,
                          const char* what) {
    using namespace tj::engine;
    CpuState* outer = g_frames[g_frameDepth - 1];
    ++g_guestCalls;
    Prof2Scope p2(P2_GCALL);            // TJ_ENG_PROF2: marshal overhead (nested Run
                                        // re-scopes itself to P2_INT)
    CpuState ns{};
    FpuCaptureHost(&ns.fpu);
    const uint32_t base = (outer->r[ESP] - 64u - (4u + 4u * (uint32_t)nStackW)) & ~3u;
    *(uint32_t*)(uintptr_t)base = GC_SENTINEL;
    for (int i = 0; i < nStackW; ++i)
        *(uint32_t*)(uintptr_t)(base + 4 + 4 * (uint32_t)i) = stackW[i];
    ns.r[ESP] = base;
    ns.r[ECX] = ecx;
    ns.r[EDX] = edx;
    ns.eip = va;
    ns.eflags = 0;                                     // DF clear per ABI; rest dead

    RunResult rr = Run(ns, GC_SENTINEL, 400ull * 1000 * 1000);
    if (rr.kind != RunResult::Stopped) {
        printf("[gcall] FATAL %s @%08X: run stopped kind=%d addr=%08X detail=%08X\n",
               what, va, (int)rr.kind, rr.addr, rr.detail);
        FatalStop(0xEC000000u | (uint32_t)rr.kind);
    }
    // The per-call convention assertion (plan risk #4, this direction): the interpreted
    // ret told us exactly how many bytes the callee cleaned — it must match the tag.
    if (ns.r[ESP] != base + 4u + (uint32_t)expectCleanBytes) {
        printf("[gcall] FATAL %s @%08X: callee cleaned %d bytes, tag says %d\n",
               what, va, (int)(ns.r[ESP] - base - 4u), expectCleanBytes);
        FatalStop(0xEC0000FE);
    }
    FpuRestoreHost(&ns.fpu);
    return ns.r[EAX];
}

float GuestMarshalPopF32() {
#if defined(_M_IX86)
    float ret32;                                       // ("out" is an asm mnemonic)
    __asm { fstp ret32 }                               // the native caller's pop of ST0
    return ret32;
#else
    // fstp dword, emulated on the virtual host FPU image (fpu_host.cpp): convert ST0
    // (image offset 28 — fnsave stores registers in ST order) per the CW rounding mode,
    // then pop: empty the physical register that WAS top, bump TOP, shift the
    // ST-relative register area down one slot.
    uint8_t* im = tj::engine::VirtualHostFpu()->image;
    uint16_t cw = (uint16_t)(im[0] | (im[1] << 8));
    uint16_t sw = (uint16_t)(im[4] | (im[5] << 8));
    uint16_t tw = (uint16_t)(im[8] | (im[9] << 8));
    extFloat80_t st0;
    std::memcpy(&st0.signif, im + 28, 8);
    std::memcpy(&st0.signExp, im + 36, 2);
    static const uint_fast8_t kRound[4] = {           // x87 RC -> softfloat rounding
        softfloat_round_near_even, softfloat_round_min,
        softfloat_round_max, softfloat_round_minMag };
    uint_fast8_t saved = softfloat_roundingMode;
    softfloat_roundingMode = kRound[(cw >> 10) & 3];
    float32_t f = extF80_to_f32(st0);
    softfloat_roundingMode = saved;

    unsigned top = (sw >> 11) & 7;
    tw = (uint16_t)(tw | (3u << (top * 2)));          // vacated physical reg -> empty
    top = (top + 1) & 7;
    sw = (uint16_t)((sw & ~0x3800u) | (top << 11));
    std::memmove(im + 28, im + 38, 70);               // ST1..ST7 slide into ST0..ST6
    std::memset(im + 98, 0, 10);
    im[4] = (uint8_t)sw; im[5] = (uint8_t)(sw >> 8);
    im[8] = (uint8_t)tw; im[9] = (uint8_t)(tw >> 8);

    float ret32;
    std::memcpy(&ret32, &f, 4);
    return ret32;
#endif
}

// The MIRROR of GuestMarshalPopF32: a hook that returns a float leaves it where the x87
// return convention says -- ST0 -- for the guest caller to fstp.
//
// On x86 there is nothing to do. The shim calls the hook through a uint64-returning cast,
// which never emits an fstp, so the value the callee left in ST0 is still there when
// DispatchTryInvoke's fnsave captures the host FPU as the guest's x87 state. (The junk the
// cast reads into eax:edx is written on purpose -- it is exactly what the escape path's
// EmRetThunk captures, so the dispatch-vs-escape A/B still compares equal.)
//
// On ARM the guest x87 is an image, so this is an explicit fld: decrement TOP, slide the
// ST-relative register area up one slot, and store the widened value with its tag.
void GuestMarshalPushF32(float v) {
#if defined(_M_IX86)
    (void)v;
#else
    uint8_t* im = tj::engine::VirtualHostFpu()->image;
    uint16_t sw = (uint16_t)(im[4] | (im[5] << 8));
    uint16_t tw = (uint16_t)(im[8] | (im[9] << 8));
    unsigned top = (sw >> 11) & 7;
    top = (top + 7) & 7;                                  // fld decrements TOP
    sw = (uint16_t)((sw & ~0x3800u) | (top << 11));
    float32_t f;
    std::memcpy(&f, &v, 4);
    extFloat80_t e = f32_to_extF80(f);
    tw = (uint16_t)(tw & ~(3u << (top * 2)));             // 00 = valid
    if (v == 0.0f) tw = (uint16_t)(tw | (1u << (top * 2)));   // 01 = zero
    std::memmove(im + 38, im + 28, 70);                   // ST0..ST6 -> ST1..ST7
    std::memcpy(im + 28, &e.signif, 8);
    std::memcpy(im + 36, &e.signExp, 2);
    im[4] = (uint8_t)sw; im[5] = (uint8_t)(sw >> 8);
    im[8] = (uint8_t)tw; im[9] = (uint8_t)(tw >> 8);
#endif
}

bool DispatchTryInvoke(CpuState& s) {
    const DispatchEntry* e = Find(s.eip);
    if (!e) {
        bool seen = false;
        for (int i = 0; i < g_missN; ++i) if (g_missVa[i] == s.eip) { seen = true; break; }
        if (!seen && g_missN < kMaxMiss) {
            g_missVa[g_missN++] = s.eip;
            printf("[dispatch] unregistered host target %08X (escape) — ARM coverage gap\n", s.eip);
        }
        return false;
    }
    if (!e->invoke) return false;                    // Raw (naked hooks) / Data — escape
    ++g_invokes;
    tj::engine::Prof2Scope p2(tj::engine::P2_DISP);  // TJ_ENG_PROF2: boundary + hook body
    if (g_depth < 32) g_active[g_depth] = e;
    ++g_depth;
    DispatchPushFrame(&s);              // GuestCall marshals below this frame's ESP
    // Materialize the guest x87 onto the host FPU for the duration of the native call —
    // the escape's frstor/fnsave bracket. Required for hooks that do float math and for
    // nested engine gates, which capture the HOST FPU as the guest state.
    tj::engine::FpuRestoreHost(&s.fpu);
    e->invoke(s, *e);
    tj::engine::FpuCaptureHost(&s.fpu);
    DispatchPopFrame();
    --g_depth;
    return true;
}

void DispatchStats(uint64_t* invokes, uint32_t* misses, uint64_t* guestCalls) {
    if (invokes)    *invokes = g_invokes;
    if (misses)     *misses = (uint32_t)g_missN;
    if (guestCalls) *guestCalls = g_guestCalls;
}

void DispatchReport() {
    int byKind[6] = {};
    for (int i = 0; i < g_n; ++i) ++byKind[(int)g_tab[i].kind];
    printf("[dispatch] %d entries: %d typed, %d argbytes, %d trap, %d retstub, "
           "%d raw(escape-only), %d data(ARM reloc pending); %d dup merges, %d fold merges\n",
           g_n, byKind[0], byKind[1], byKind[2], byKind[3], byKind[4], byKind[5],
           g_mergedDups, g_foldMerges);
    for (int i = 0; i < g_n; ++i)
        if (g_tab[i].kind == HookKind::Raw)
            printf("[dispatch]   RAW  %08X %s\n", g_tab[i].key, g_tab[i].label);
}

void DispatchCrashDump() {
    int d = g_depth > 32 ? 32 : g_depth;
    printf("[VEH] dispatch: %llu invokes, depth=%d, %d unregistered targets\n",
           (unsigned long long)g_invokes, g_depth, g_missN);
    for (int i = 0; i < d; ++i)
        printf("[VEH]   active[%d]=%s @%08X\n", i, g_active[i]->label, g_active[i]->key);
    fflush(stdout);
}

} // namespace tj::hybrid
