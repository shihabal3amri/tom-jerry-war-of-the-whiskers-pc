// THE GUEST->HOST DISPATCH TABLE — the Stage-5 call boundary (port/ANDROID_PLAN.md §2.4).
//
// Today every guest->host transition is encoded as BYTES in guest memory: a patched
// `jmp/call rel32` whose target is a host function, or a kernel-thunk / vtable slot
// holding a host function pointer. On the 32-bit Windows host those bytes are valid and
// the engine services them with the generic host-call escape (engine_mode.cpp). On ARM
// they cannot exist — a 64-bit host address does not fit in a rel32 or a 4-byte slot —
// so the boundary becomes THIS TABLE: every registered target is keyed by the 32-bit
// value the guest actually reaches (on Windows: the host address itself; on ARM: a
// synthetic handle the patcher stores instead), and the engine, on reaching that key as
// EIP, calls a per-hook INVOKE that marshals guest state -> native call -> guest state.
//
// What registration buys, per direction of risk:
//  * The calling convention becomes DATA (conv + callee-cleaned stack bytes), which is
//    what ARM needs — and on the x86 host the tag is STATIC_ASSERTED against the hook's
//    declared C++ type, so a mis-transcribed convention is a compile error, not a
//    corrupted stack (ANDROID_PLAN risk #4).
//  * Coverage is measurable: in engine mode, any escape to an UNREGISTERED host target
//    is logged once — the to-do list for ARM is printed by the run itself.
//  * TJ_ENG_NODISPATCH=1 forces every registered hook back down the proven escape path:
//    same binary, two mechanisms, byte-identical TJ_DETLOG required (the session-25
//    same-binary A/B rule).
//
// Marshaling contract (mirrors the escape thunks bit-for-bit where the guest can see):
//  * eax:edx are written from the native call's return registers (the escape captures
//    the host's eax/edx too — MSVC guest code never reads anything else that a callee
//    may clobber; ecx is left at its call-time value, see dispatch.cpp).
//  * The guest x87 image is materialized onto the host FPU around the call
//    (FpuRestoreHost/FpuCaptureHost), exactly as EmCallThunk/EmRetThunk frstor/fnsave —
//    required so hooks' own float math and nested engine GATES see the true guest x87.
//  * ESP: pop the return address, plus the callee-cleaned bytes recorded per hook.
//    cdecl: the interpreted caller cleans its own args when it resumes.
//
// Native mode (TJ_ENGINE unset) never consults the table; registration is bookkeeping
// only and the patched bytes/slot values are byte-identical to v1.1.0.
#pragma once
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>
#include "engine/engine.h"

namespace tj::hybrid {

enum class CallConv : uint8_t { Cdecl, Stdcall, Fastcall };
// Typed    : invoke generated from the hook's C++ function-pointer type (HOOK_* macros)
// ArgBytes : kernel-table generic — stdcall with N stack-arg bytes recorded as data
// Trap     : unimplemented-import trap (prints ordinal, exits) — kernel.cpp
// RetStub  : `mov eax,imm; ret n` stub semantics — kernel.cpp arg-cleaning stubs
// Raw      : NO portable semantics — engine mode falls back to the host-call escape.
//            Every Raw entry is an explicit ARM to-do (the three naked hooks).
// Data     : kernel VAR export — the slot holds a host DATA address the guest reads.
//            Never executed; recorded so the ARM relocation sweep has the full list.
enum class HookKind : uint8_t { Typed, ArgBytes, Trap, RetStub, Raw, Data };

struct DispatchEntry;
using HookInvoke = void (*)(tj::engine::CpuState&, const DispatchEntry&);

struct DispatchEntry {
    uint32_t    key;        // the guest-visible target value (host address on x86 host)
    void*       host;       // the native function (or data) behind the key
    HookInvoke  invoke;     // null => not invocable (Raw/Data) — escape path
    const char* label;
    HookKind    kind;
    CallConv    conv;
    int16_t     argBytes;   // callee-cleaned STACK-arg bytes (excl. return address)
    uint32_t    ctx;        // Trap: ordinal; RetStub: return value
};

// What the HOOK_* macros build at a registration site.
struct Hook {
    void*       fn;
    HookInvoke  invoke;     // null for HOOK_RAW
    CallConv    conv;
    HookKind    kind;
    int16_t     stackArgBytes;
};

// ---- registration (all return the 32-bit KEY to store/patch; on the x86 Windows host
//      the key IS the host address, so patched bytes stay byte-identical to v1.1.0) ----
uint32_t DispatchRegister(const Hook& h, const char* label);
uint32_t DispatchRegisterArgBytes(void* target, const char* name, int argBytes, CallConv conv);
uint32_t DispatchRegisterTrap(void* trapCode, const char* name, uint32_t ordinal,
                              void(__cdecl* report)(int));
uint32_t DispatchRegisterRetStub(void* stubCode, const char* name, uint32_t retVal, int argBytes);
uint32_t DispatchRegisterData(void* var, const char* name);

// Engine-mode consumers (engine_mode.cpp).
bool DispatchTryInvoke(tj::engine::CpuState& s);   // true = handled (EIP advanced)
void DispatchReport();                             // one-shot summary at engine arm
void DispatchCrashDump();                          // innermost active hooks, for the VEH
void DispatchStats(uint64_t* invokes, uint32_t* misses, uint64_t* guestCalls = nullptr);

// ---- the host->guest direction (Engine::Call, guest_call.h's GuestCall) ----------
// The dispatch layer knows the live interpreted CpuState while a hook runs (pushed
// around every invoke); a null frame is pushed around ESCAPE excursions so marshaled
// guest calls are never attempted from native-on-guest-stack context (those fall back
// to the proven gate). GuestMarshalCall builds a guest frame below the current
// interpreted ESP, runs a nested interpretation to a sentinel, asserts the callee's
// stack delta against the recorded convention, and materializes the post-call x87
// back onto the host FPU (float returns ride ST0, exactly as the gate leaves them).
void DispatchPushFrame(tj::engine::CpuState* s);   // null = escape excursion marker
void DispatchPopFrame();
void DispatchSetGuestCallMode(bool marshal);       // false = TJ_ENG_GATECALL (A/B)
bool GuestMarshalReady();                          // marshal on AND a live frame
uint32_t GuestMarshalCall(uint32_t va, const uint32_t* stackW, int nStackW,
                          uint32_t ecx, uint32_t edx, int expectCleanBytes,
                          const char* what);
float GuestMarshalPopF32();                        // pop ST0 (the caller's fstp)
void  GuestMarshalPushF32(float v);                // push ST0 (a hook's float return)

// ================= typed shim generation =================
namespace dispatch_detail {

using tj::engine::CpuState;

inline uint32_t GuestLd32(uint32_t va) { return *(const uint32_t*)(uintptr_t)va; }

// One 4-byte guest stack slot / register, reinterpreted as the declared parameter type.
// Sub-4-byte integrals (uint8_t params) occupy a full slot; the value is its low bytes
// (little-endian), exactly what the native callee reads. Declared POINTER parameters
// are guest addresses: zero-extended on a 64-bit host (identity below 4 GB).
template <class T> inline T FromSlot(uint32_t raw) {
    if constexpr (std::is_pointer_v<T>) {
        return (T)(uintptr_t)raw;
    } else {
        static_assert(sizeof(T) <= 4, "hook parameters must fit one 4-byte slot (split 64-bit args)");
        T t{}; std::memcpy(&t, &raw, sizeof(T)); return t;
    }
}

// Return types we marshal: void, a ≤4-byte integral, or a pointer (which must be a
// guest-visible address — checked at runtime on 64-bit hosts). Float/64-bit-integer
// returns do not exist among the hooks today (audited session 26) — extend DELIBERATELY
// if one appears: float returns must be pushed onto the guest x87 stack.
// float is marshalable: it returns in ST0, which ShimCC pushes onto the guest x87 (and
// which x86 gets for free from the dispatch bracket's fnsave). double and every 64-bit
// return still are not -- those would need edx:eax or a second x87 slot decision.
template <class R> struct RetOk : std::bool_constant<
    std::is_pointer_v<R> || std::is_same_v<R, float> ||
    (sizeof(R) <= 4 && !std::is_floating_point_v<R>)> {};
template <> struct RetOk<void> : std::true_type {};

#if defined(_M_IX86)

// Convention is deduced from the pointer TYPE — MSVC x86 keeps cdecl/stdcall/fastcall
// as distinct function-pointer types, so the HOOK_* tag is machine-checked here.
template <class F> struct HookSig;

template <class R, class... A> struct HookSig<R(__cdecl*)(A...)> {
    static constexpr CallConv conv = CallConv::Cdecl;
    static constexpr int nargs = (int)sizeof...(A);
    static constexpr int cleanBytes = 0;                       // caller cleans
    using Ret = R;
    using U64Fn = uint64_t(__cdecl*)(A...);
    using ArgTuple = std::tuple<A...>;
    template <size_t I> static uint32_t RawArg(const CpuState& s) {
        return GuestLd32(s.r[tj::engine::ESP] + 4 + 4 * (uint32_t)I);
    }
};
template <class R, class... A> struct HookSig<R(__stdcall*)(A...)> {
    static constexpr CallConv conv = CallConv::Stdcall;
    static constexpr int nargs = (int)sizeof...(A);
    static constexpr int cleanBytes = 4 * (int)sizeof...(A);   // callee cleans all
    using Ret = R;
    using U64Fn = uint64_t(__stdcall*)(A...);
    using ArgTuple = std::tuple<A...>;
    template <size_t I> static uint32_t RawArg(const CpuState& s) {
        return GuestLd32(s.r[tj::engine::ESP] + 4 + 4 * (uint32_t)I);
    }
};
template <class R, class... A> struct HookSig<R(__fastcall*)(A...)> {
    static constexpr CallConv conv = CallConv::Fastcall;
    static constexpr int nargs = (int)sizeof...(A);
    static constexpr int cleanBytes = (int)sizeof...(A) > 2 ? 4 * ((int)sizeof...(A) - 2) : 0;
    using Ret = R;
    using U64Fn = uint64_t(__fastcall*)(A...);
    using ArgTuple = std::tuple<A...>;
    template <size_t I> static uint32_t RawArg(const CpuState& s) {
        if constexpr (I == 0) return s.r[tj::engine::ECX];
        else if constexpr (I == 1) return s.r[tj::engine::EDX];
        else return GuestLd32(s.r[tj::engine::ESP] + 4 + 4 * (uint32_t)(I - 2));
    }
};

// The generated invoke: pop return address, gather args per convention, call the host
// function through a uint64-returning cast (reads the host's edx:eax verbatim — the
// exact registers EmRetThunk captures, so the A/B against the escape path compares
// equal even for void hooks), write eax:edx, clean the callee-owned bytes, resume.
template <class F> struct Shim {
    using Sig = HookSig<F>;
    template <size_t... I>
    static void Impl(CpuState& s, const DispatchEntry& e, std::index_sequence<I...>) {
        const uint32_t ret = GuestLd32(s.r[tj::engine::ESP]);
        const uint64_t r = ((typename Sig::U64Fn)e.host)(
            FromSlot<std::tuple_element_t<I, typename Sig::ArgTuple>>(
                Sig::template RawArg<I>(s))...);
        s.r[tj::engine::EAX] = (uint32_t)r;
        s.r[tj::engine::EDX] = (uint32_t)(r >> 32);
        s.r[tj::engine::ESP] += 4 + (uint32_t)Sig::cleanBytes;
        s.eip = ret;
    }
    static void Invoke(CpuState& s, const DispatchEntry& e) {
        Impl(s, e, std::make_index_sequence<(size_t)Sig::nargs>{});
    }
};

// SigFor<CC, F>: the signature record GuestCall and MakeHook consult — on x86 it is
// HookSig<F> (conv deduced from the pointer type); on ARM it is SigCC<CC, F> (conv
// from the tag, the pointer type having lost it).
template <CallConv CC, class F> using SigFor = HookSig<F>;

#else
// ==================== AArch64 ====================
// All x86 calling conventions collapse to one C ABI here; the MSVC conv keywords
// vanish so the shared typedefs still parse, and the TAG recorded at each HOOK_*/GCALL
// site (machine-checked on the x86 build) drives the marshaling.
#ifndef __fastcall
#define __fastcall
#define __stdcall
#define __cdecl
#endif

template <CallConv CC, class F> struct SigCC;
template <CallConv CC, class R, class... A> struct SigCC<CC, R(*)(A...)> {
    static constexpr CallConv conv = CC;
    static constexpr int nargs = (int)sizeof...(A);
    static constexpr int stackArgs =
        CC == CallConv::Fastcall ? (nargs > 2 ? nargs - 2 : 0) : nargs;
    static constexpr int cleanBytes = CC == CallConv::Cdecl ? 0 : 4 * stackArgs;
    using Ret = R;
    using ArgTuple = std::tuple<A...>;
    using PlainFn = R(*)(A...);
    template <size_t I> static uint32_t RawArg(const CpuState& s) {
        if constexpr (CC == CallConv::Fastcall) {
            if constexpr (I == 0) return s.r[tj::engine::ECX];
            else if constexpr (I == 1) return s.r[tj::engine::EDX];
            else return GuestLd32(s.r[tj::engine::ESP] + 4 + 4 * (uint32_t)(I - 2));
        } else {
            return GuestLd32(s.r[tj::engine::ESP] + 4 + 4 * (uint32_t)I);
        }
    }
};

[[noreturn]] void DispatchFatal(const char* what);   // dispatch.cpp
[[noreturn]] void DispatchFatalRetPtr(const char* label, uint32_t key,
                                      unsigned long long val, uint32_t site);  // dispatch.cpp

// The ARM typed invoke: same marshaling as x86's Shim, called through the plain C ABI.
// eax gets the (≤4-byte or guest-pointer) result; edx is left untouched — on x86 the
// escape/uint64-cast capture wrote the host's real edx, but MSVC-generated guest code
// never reads a callee's edx except for 64-bit returns, which are rejected above.
template <CallConv CC, class F> struct ShimCC {
    using Sig = SigCC<CC, F>;
    template <size_t... I>
    static void Impl(CpuState& s, const DispatchEntry& e, std::index_sequence<I...>) {
        const uint32_t ret = GuestLd32(s.r[tj::engine::ESP]);
        using R = typename Sig::Ret;
        if constexpr (std::is_void_v<R>) {
            ((typename Sig::PlainFn)e.host)(
                FromSlot<std::tuple_element_t<I, typename Sig::ArgTuple>>(
                    Sig::template RawArg<I>(s))...);
        } else {
            R r = ((typename Sig::PlainFn)e.host)(
                FromSlot<std::tuple_element_t<I, typename Sig::ArgTuple>>(
                    Sig::template RawArg<I>(s))...);
            if constexpr (std::is_pointer_v<R>) {
                uintptr_t v = (uintptr_t)r;
                if (v >> 32) DispatchFatalRetPtr(e.label, e.key, (unsigned long long)v, ret);
                s.r[tj::engine::EAX] = (uint32_t)v;
            } else if constexpr (std::is_same_v<R, float>) {
                GuestMarshalPushF32(r);      // x87 return convention: the value is ST0
            } else {
                uint32_t u = 0; std::memcpy(&u, &r, sizeof(R));
                s.r[tj::engine::EAX] = u;
            }
        }
        s.r[tj::engine::ESP] += 4 + (uint32_t)Sig::cleanBytes;
        s.eip = ret;
    }
    static void Invoke(CpuState& s, const DispatchEntry& e) {
        Impl(s, e, std::make_index_sequence<(size_t)Sig::nargs>{});
    }
};

template <CallConv CC, class F> using SigFor = SigCC<CC, F>;
#endif

} // namespace dispatch_detail

template <CallConv CC, class F>
inline Hook MakeHook(F fn) {
    using Sig = dispatch_detail::SigFor<CC, F>;
#if defined(_M_IX86)
    static_assert(Sig::conv == CC,
        "HOOK_* tag does not match the hook's declared calling convention");
#endif
    static_assert(dispatch_detail::RetOk<typename Sig::Ret>::value,
        "hook return type is not marshalable (float/64-bit) — extend dispatch.h deliberately");
#if defined(_M_IX86)
    auto invoke = &dispatch_detail::Shim<F>::Invoke;
#else
    auto invoke = &dispatch_detail::ShimCC<CC, F>::Invoke;
#endif
    return Hook{ (void*)fn, invoke, CC, HookKind::Typed, (int16_t)Sig::cleanBytes };
}

// The registration markers. The tag is redundant on x86 (deduced and asserted) and
// AUTHORITATIVE on ARM (where all conventions collapse to one C ABI) — that asymmetry
// is the whole point: Windows machine-checks what ARM will trust.
#define HOOK_CDECL(f) (tj::hybrid::MakeHook<tj::hybrid::CallConv::Cdecl>(&(f)))
#define HOOK_STD(f)   (tj::hybrid::MakeHook<tj::hybrid::CallConv::Stdcall>(&(f)))
#define HOOK_FC(f)    (tj::hybrid::MakeHook<tj::hybrid::CallConv::Fastcall>(&(f)))
// A hook with no marshalable semantics (naked asm). Engine mode services it with the
// host-call escape; it CANNOT exist on ARM — every HOOK_RAW is an explicit Stage-5 to-do.
#define HOOK_RAW(f)   (tj::hybrid::Hook{ (void*)&(f), nullptr, tj::hybrid::CallConv::Cdecl, \
                                         tj::hybrid::HookKind::Raw, 0 })

} // namespace tj::hybrid
