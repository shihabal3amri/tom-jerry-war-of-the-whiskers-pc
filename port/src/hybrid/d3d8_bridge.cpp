// D3D8 -> D3D11 bridge for the hybrid bring-up. The Xbox D3D8 library (statically
// linked in the "D3D" section) programs the NV2A GPU directly and cannot run on PC, so
// we redirect the game's graphics through our native tj::gfx::Device (D3D11).
//
// This file first provides the native display surface (a Win32 window + D3D11 device)
// that the bridged device-creation path returns. The per-call method bridging (clear /
// draw / present / textures) is wired in as the crash-by-crash bring-up reaches it,
// guided by the D3D8 device-model RE.
#include "hybrid/xdk_patch.h"
#include "hybrid/lan_match.h"   // LanFeMgr(), for the arena-select log
#include "hybrid/guest_call.h"
#include "hybrid/dsound_stubs.h"
#include "hybrid/net_lan.h"
#include "hybrid/lan_ui.h"
#include "hybrid/arabic.h"
#include "runtime/gfx/d3d8.h"
#include "runtime/assets/xmf_texture.h"
#include "android/perf_hint.h"
#include "runtime/input/xinput_pad.h"
#include "hybrid/host_compat.h"
#ifdef _WIN32
#include <psapi.h>
#else
#include <sys/mman.h>   // mincore: the IsReadable probe
#endif
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <unordered_map>

extern "C" uint32_t Device_NullSrvBinds();   // d3d8.cpp (dark-scene canary: stale handle binds)

namespace tj::hybrid {

static HWND        g_wnd = nullptr;
static tj::gfx::Device g_dev;
static bool        g_devReady = false;
static void        StartInputThread();   // defined with the input bridge below

#ifndef _WIN32
// The Android app hands us the ANativeWindow before boot; EnsureDisplay passes it to the
// GLES3 Device (owns EGL). Null on the qemu/headless leg -> the null recorder device.
static void* g_platformWindow = nullptr;
void SetPlatformWindow(void* w) { g_platformWindow = w; }
#endif

// Display mode: 0 = windowed, 1 = borderless (window covers the monitor, DXGI
// stretches the fixed-size backbuffer), 2 = exclusive DXGI fullscreen (real mode
// switch to the game resolution). DXGI's own Alt+Enter handling is disabled at device
// creation (MakeWindowAssociation) -- the bridge owns all transitions.
enum { DISP_WINDOWED = 0, DISP_BORDERLESS = 1, DISP_FULLSCREEN = 2 };
static int  g_dispMode = DISP_WINDOWED;
static int  g_backW = 640, g_backH = 480;                   // current backbuffer size
void SetBackbufferSize(int w, int h) { g_backW = w; g_backH = h; }
int  GetDisplayModeKind() { return g_dispMode; }
bool SetDisplayModeKind(int kind);                          // fwd (needs g_dev/g_devReady)

#ifdef _WIN32
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Closing the window quits the game -- there is nothing to unwind through (the
    // loader's low image is overwritten), so a clean ExitProcess is the correct exit.
    if (m == WM_CLOSE) { printf("[d3d8] window closed -- exiting\n"); fflush(stdout); ExitProcess(0); }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN && !(l & (1 << 30))) {
        // Alt+Enter: windowed <-> borderless (exclusive fullscreen drops to windowed).
        SetDisplayModeKind(g_dispMode == DISP_WINDOWED ? DISP_BORDERLESS : DISP_WINDOWED);
        return 0;
    }
    if (m == WM_SYSCHAR && w == VK_RETURN) return 0;   // eat the Alt+Enter beep
    // LAN name / password / address entry: the modal accepts the keyboard at the same time
    // as the pad grid. Characters are queued here (UI thread) and drained on the game
    // thread; MergeKeyboard suppresses the menu bindings while capture is active so typing
    // cannot also move the cursor.
    if (tj::hybrid::LanTextCaptureActive()) {
        if (m == WM_CHAR) { LanTextCapturePush((int)w); return 0; }
        if (m == WM_KEYDOWN && (w == VK_ESCAPE || w == VK_BACK || w == VK_RETURN)) {
            LanTextCapturePush(w == VK_ESCAPE ? 27 : w == VK_BACK ? '\b' : '\r');
            return 0;
        }
    }
    return DefWindowProcA(h, m, w, l);
}

// The window's hInstance MUST be this DLL's module -- NOT the loader exe: MapXbeImage
// overwrites the loader's PE header at 0x10000, so user32 rejects it as an hInstance.
static HINSTANCE SelfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&WndProc, &h);
    return (HINSTANCE)h;
}

// The window lives on a DEDICATED UI THREAD with a real GetMessage pump. Without one the
// window goes "Not Responding" (ghosted: frozen frame, no input, kill prompt) within
// seconds -- messages CANNOT be pumped on the game thread either, because dispatching
// there walks the game's Xbox-style fs:[0] SEH chain and faults inside combase. The
// window must be CREATED on the pumping thread (messages are delivered to the creator).
static HANDLE g_uiReady = nullptr;
static int    g_uiW = 640, g_uiH = 480;
static DWORD WINAPI UiThreadMain(LPVOID) {
    HINSTANCE hInst = SelfModule();
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "TJHybridWnd";
    RegisterClassA(&wc);
    RECT r = {0, 0, g_uiW, g_uiH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("TJHybridWnd", "Tom & Jerry: War of the Whiskers",
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                          nullptr, nullptr, wc.hInstance, nullptr);
    SetEvent(g_uiReady);
    if (!g_wnd) return 1;
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

#endif // _WIN32

// Create the native window (on its UI thread) + D3D11 device. Idempotent.
// Headless (ARM): no window, no UI thread — the null recorder device only.
bool EnsureDisplay(int width, int height, int sampleCount) {
    if (g_devReady) return true;
    printf("[d3d8] EnsureDisplay enter\n");
#ifndef _WIN32
    (void)sampleCount;
    StartInputThread();     // initializes the pad lock (no poll thread headless)
    tj::gfx::PresentParams hp;
    hp.backWidth = width; hp.backHeight = height; hp.vsync = true; hp.sampleCount = 1;
    // g_platformWindow: the ANativeWindow on Android (real GLES3 device); null on the
    // qemu/headless leg (the null recorder). Same call, backend chosen by which gfx TU links.
    if (!g_dev.Create(reinterpret_cast<HWND>(g_platformWindow), hp)) {
        printf("[d3d8] Device::Create failed (window=%p)\n", g_platformWindow); return false; }
    SetBackbufferSize(width, height);
    g_devReady = true;
    printf("[d3d8] display ready: %dx%d (window=%p)\n", width, height, g_platformWindow);
    return true;
#else
    StartInputThread();     // pristine-process moment: XInput must lazily init NOW
    g_uiW = width; g_uiH = height;
    g_uiReady = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, UiThreadMain, nullptr, 0, nullptr);
    if (t) { WaitForSingleObject(g_uiReady, 10000); CloseHandle(t); }
    if (!g_wnd) { printf("[d3d8] CreateWindow failed %lu\n", GetLastError()); return false; }
    printf("[d3d8] window %p (UI thread), creating D3D11 device...\n", (void*)g_wnd);
    tj::gfx::PresentParams pp;
    pp.backWidth = width; pp.backHeight = height; pp.vsync = true; pp.sampleCount = sampleCount;
    // TJ_FAST runs unthrottled for test automation -- vsync would cap it at the refresh
    // rate whenever the window is visible (occluded windows skip the wait, which made
    // fast-run timing depend on window z-order).
    { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_FAST");
      if (e && atoi(e)) pp.vsync = false; free(e); }
    if (!g_dev.Create(g_wnd, pp)) { printf("[d3d8] Device::Create failed\n"); return false; }
    extern void SetBackbufferSize(int w, int h);   // below (forward: g_backW/g_backH)
    SetBackbufferSize(width, height);
    g_devReady = true;
    printf("[d3d8] native display ready: %dx%d MSAAx%d (hwnd=%p)\n", width, height, sampleCount, (void*)g_wnd);
    return true;
#endif // _WIN32
}

void PumpMessages() {
#ifdef _WIN32
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
#endif
}

tj::gfx::Device* BridgeDevice() { return g_devReady ? &g_dev : nullptr; }

// Apply a display-mode kind (windowed/borderless/exclusive fullscreen). Reentrant-safe
// per mode; keeps the backbuffer at the game resolution (g_backW/H) in every mode --
// windowed sizes the window to it, borderless stretches it over the monitor, exclusive
// mode-switches the display to it.
bool SetDisplayModeKind(int kind) {
#ifndef _WIN32
    // Headless: there is no window and no mode to switch (and the Android build has no
    // resolution settings by decision anyway — ANDROID_PLAN §2.7).
    return kind == g_dispMode;
#else
    if (!g_devReady || !g_wnd) return false;
    if (kind == g_dispMode) return true;
    // leave exclusive first (DXGI requires the windowed transition before restyling)
    if (g_dispMode == DISP_FULLSCREEN) g_dev.SetFullscreenExclusive(false, 0, 0);
    switch (kind) {
    case DISP_WINDOWED: {
        RECT r = { 0, 0, g_backW, g_backH };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowLongA(g_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_NOTOPMOST, 60, 60, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED);
        g_dev.ResizeBackbuffer(g_backW, g_backH);        // undo any fullscreen mode size
        break;
    }
    case DISP_BORDERLESS: {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoA(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(g_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
        break;
    }
    case DISP_FULLSCREEN:
        if (!g_dev.SetFullscreenExclusive(true, g_backW, g_backH)) {
            printf("[d3d8] exclusive fullscreen refused -- falling back to borderless\n");
            g_dispMode = DISP_WINDOWED;                  // force the restyle below
            return SetDisplayModeKind(DISP_BORDERLESS);
        }
        break;
    default: return false;
    }
    g_dispMode = kind;
    printf("[d3d8] display mode -> %s\n",
           kind == DISP_WINDOWED ? "windowed" : kind == DISP_BORDERLESS ? "borderless" : "fullscreen");
    return true;
#endif // _WIN32
}

// Live resolution change (the in-game RESOLUTION row). Works in every display mode:
// windowed resizes the window, borderless keeps the monitor-covering window and only
// resizes the backbuffer (DXGI stretches), exclusive fullscreen re-targets the display
// mode itself. The transition-capture textures are size-stale afterwards; Br_Swap
// recreates them on the next frame via the size check.
bool ResizeDisplay(int w, int h) {
#ifndef _WIN32
    if (!g_devReady) return false;
    g_backW = w; g_backH = h;
    return g_dev.ResizeBackbuffer(w, h);
#else
    if (!g_devReady || !g_wnd) return false;
    g_backW = w; g_backH = h;
    switch (g_dispMode) {
    case DISP_WINDOWED: {
        RECT r = { 0, 0, w, h };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return g_dev.ResizeBackbuffer(w, h);
    }
    case DISP_BORDERLESS:
        return g_dev.ResizeBackbuffer(w, h);
    case DISP_FULLSCREEN:
        // re-enter at the new mode (ResizeTarget + buffer resize)
        return g_dev.SetFullscreenExclusive(true, w, h);
    }
    return false;
#endif // _WIN32
}

// The Xbox exposes unified memory to the GPU through a write-combined alias based at
// 0x80000000 (0x80000000 | physical). D3D8/XPP and the game's renderer write push
// buffers, GPU objects, textures and vertex data there directly (e.g. 0x80000020,
// 0x802a0000). Reserve that aperture as plain RW memory so those writes land somewhere
// valid -- a lightweight stand-in for NV2A memory. Must run before D3D11/DXGI grab
// address space. Returns true if the aperture (or a fallback) is committed.
static void* g_gpuAperture = nullptr;
bool ReserveGpuAperture() {
    if (g_gpuAperture) return true;
    // Reserve 0x80000000..0x84000000 for stray GPU-object writes. The 0x84000000+ alias of
    // the contiguous pool (kernel SetupContiguousPool) lives just above and must stay free.
    g_gpuAperture = VirtualAlloc((void*)0x80000000, 0x04000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_gpuAperture) {
        printf("[d3d8] GPU aperture reserve @0x80000000 FAILED err=%lu\n", GetLastError());
        return false;
    }
    printf("[d3d8] GPU aperture reserved 0x80000000..0x84000000\n");
    return true;
}

// ===========================================================================
// D3D8 -> D3D11 entry-point bridge. The Xbox D3D8 device is a singleton read from
// [0xa6758]; all methods are direct calls (no vtable), so we patch each function's
// entry with a jmp to a native thunk (matching calling convention for stack cleanup).
// Globals the game reads: device gate 0x15ca804, D3D obj 0x15ca800, present params
// struct 0x15c9098. D3D8 internal g_pDevice 0xa6758.
// ===========================================================================
// The Xbox CDevice is the FIXED global at 0xa6760 (in the mapped D3D section); g_pDevice
// (0xa6758) holds that address. Device methods read absolute [0xa6760]/[0xa6764] as the
// pushbuffer PUT/LIMIT, so we MUST use this real location, not a private object.
static const uint32_t kDevice = 0xa6760;
static uint8_t* g_pushbuf = nullptr;             // scratch pushbuffer (tokens we ignore)
static const uint32_t kPushSize = 32u << 20;
static uint32_t g_drawCalls = 0;                 // DrawVerticesUP calls since last Clear (diagnostic)
static int      g_frame = 0;                     // frames presented (advanced in Br_Swap)
static uint32_t g_dbgUiDraws = 0, g_dbg3dDraws = 0, g_dbgFvfDraws = 0, g_dbgSkips = 0;  // per-frame path counters
static uint32_t g_frmTexCreate = 0, g_frmTexUpdate = 0;   // per-frame texture create/update (perf)
static uint32_t g_texEvict = 0;          // cache evictions per window (thrash canary)
// --- phase timers (session 12 perf hunt) --------------------------------------
// The 400-frame log reports ms/f split by phase so slow-motion is attributed by
// MEASUREMENT, not guesswork (the texture-churn fixes cut GPU/memory cost hugely but
// left frame time at ~19 ms -- the cost is elsewhere). tex is a SUBSET of pb+draw
// (ResolveTexture is called from inside them); "rest" = total - pb - draw - swap =
// the game's own x86 simulation + everything the bridge doesn't time.
static int64_t g_tPb = 0, g_tDraw = 0, g_tTex = 0, g_tSwap = 0;
// The tex phase splits into two very different jobs and only one of them is ours to
// make cheaper: DECODE (Xbox format -> RGBA, on this CPU) and UPLOAD (handing the
// result to the backend). Beach spends 3-5 ms/f here; without the split, optimising it
// would be guesswork about which half.
static int64_t g_tTexDec = 0, g_tTexUpl = 0;
static uint64_t g_texDecPix = 0;        // pixels decoded per window (the work, not the calls)
static double g_frmMax = 0.0;               // worst single frame in the 400-frame window
static uint32_t g_frmOver17 = 0, g_frmOver20 = 0, g_frmOver33 = 0;
struct PhaseTimer {
    int64_t* acc; LARGE_INTEGER t0;
    explicit PhaseTimer(int64_t* a) : acc(a) { QueryPerformanceCounter(&t0); }
    ~PhaseTimer() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); *acc += t1.QuadPart - t0.QuadPart; }
};

static void SetGlobal32(uint32_t va, uint32_t val) {
    DWORD old; VirtualProtect((void*)(uintptr_t)va, 4, PAGE_EXECUTE_READWRITE, &old);
    *(uint32_t*)(uintptr_t)va = val; VirtualProtect((void*)(uintptr_t)va, 4, old, &old);
}

// Set up the CDevice object at 0xa6760 with the fields the D3D8 helpers/state functions
// read: a big scratch pushbuffer (so SetRenderState/SetTexture never trip MakeSpace, which
// would spin on the absent GPU), the render-target surface pointer (for Get2DSurfaceDesc)
// and the backbuffer format. Draw/Clear/Swap are bridged, so most other fields stay zero.
static void SetupDevice() {
    if (!g_pushbuf) g_pushbuf = (uint8_t*)VirtualAlloc(nullptr, kPushSize, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    uint8_t* dev = (uint8_t*)(uintptr_t)kDevice;
    DWORD old; VirtualProtect(dev, 0x2490, PAGE_EXECUTE_READWRITE, &old);
    memset(dev, 0, 0x2490);
    *(uint32_t*)(dev + 0x00)   = (uint32_t)(uintptr_t)g_pushbuf;                 // pushbuffer PUT
    *(uint32_t*)(dev + 0x04)   = (uint32_t)(uintptr_t)(g_pushbuf + kPushSize - 0x10000); // LIMIT
    *(uint32_t*)(dev + 0x1a04) = kDevice + 0x1a14;                              // RT surface ptr
    *(uint32_t*)(dev + 0x1a18) = 0xFFFFFFFF;                                    // RT surface Data marker
    *(uint32_t*)(dev + 0x195c) = 0x07;                                          // backbuffer format
    *(uint32_t*)(dev + 0x790)  = kDevice + 0x2000;                              // shader-state ptr (zeroed -> checks skip)
    SetGlobal32(0xa6758, kDevice);      // g_pDevice
    SetGlobal32(0x15ca804, kDevice);    // game device gate
    SetGlobal32(0x15ca800, kDevice);    // IDirect3D8 object (non-null)
    printf("[d3d8] device @0x%x set up (pushbuf %p..%p)\n", kDevice, (void*)g_pushbuf,
           (void*)(g_pushbuf + kPushSize));
}
static void ResetPushbuffer() { *(uint32_t*)(uintptr_t)(kDevice + 0) = (uint32_t)(uintptr_t)g_pushbuf; }

// Direct3D_CreateDevice(Adapter, DevType, hFocus, Behavior, pPresentParams, ppDevice) ret 0x18
static int32_t __stdcall Br_CreateDevice(uint32_t adapter, uint32_t devType, uint32_t hFocus,
        uint32_t behavior, void* pp, void** ppDevice) {
    (void)adapter; (void)devType; (void)hFocus; (void)behavior;
    // present params: +0 width, +4 height (Xbox D3DPRESENT_PARAMETERS), +0x10 MSAA
    int w = 640, h = 480, samples = 1;
    if (pp) { w = *(uint32_t*)pp; h = *(uint32_t*)((char*)pp + 4); if (w <= 0 || w > 4096) { w = 640; h = 480; } }
    printf("[d3d8] Direct3D_CreateDevice %dx%d\n", w, h);
    EnsureDisplay(w, h, samples);
    if (ppDevice) *ppDevice = (void*)(uintptr_t)kDevice;
    return 0; // D3D_OK
}

static void SetGlobal16(uint32_t va, uint16_t val) {
    DWORD old; VirtualProtect((void*)(uintptr_t)va, 2, PAGE_READWRITE, &old);
    *(uint16_t*)(uintptr_t)va = val; VirtualProtect((void*)(uintptr_t)va, 2, old, &old);
}

// Native replacement for SYS_Init3DEnvironment (0x7c6f0). Instead of the Xbox D3D8
// adapter-mode enumeration + Direct3D_CreateDevice (which programs the NV2A), create our
// D3D11 display and populate exactly the globals downstream game code reads (mapped by
// RE): the D3DPRESENT_PARAMETERS struct at 0x15c9098 and the device/mode globals.
static int __cdecl Br_Init3DEnvironment(void* modeStruct, int flags) {
    (void)flags;
    int w = 640, h = 480;
    if (modeStruct) {
        uint16_t rw = *(uint16_t*)modeStruct, rh = *(uint16_t*)((char*)modeStruct + 2);
        if (rw >= 320 && rw <= 4096 && rh >= 240 && rh <= 4096) { w = rw; h = rh; }
    }
    printf("[d3d8] SYS_Init3DEnvironment (native) %dx%d\n", w, h);
    EnsureDisplay(w, h, 1);
    SetupDevice();
    // D3DPRESENT_PARAMETERS @ 0x15c9098 (fields per RE)
    SetGlobal32(0x15c9090, 0x80000001);       // presentation interval (copy just before struct)
    SetGlobal32(0x15c9098, (uint32_t)w);      // +0x00 BackBufferWidth
    SetGlobal32(0x15c909c, (uint32_t)h);      // +0x04 BackBufferHeight
    SetGlobal32(0x15c90a0, 0x00000006);       // +0x08 BackBufferFormat (X8R8G8B8)
    SetGlobal32(0x15c90a4, 1);                // +0x0C BackBufferCount
    SetGlobal32(0x15c90a8, 0x11);            // +0x10 MultiSampleType (none)
    SetGlobal32(0x15c90ac, 1);               // +0x14 SwapEffect DISCARD
    SetGlobal32(0x15c90b8, 1);               // +0x20 EnableAutoDepthStencil
    SetGlobal32(0x15c90bc, 0x2e);            // +0x24 AutoDepthStencilFormat (D24S8)
    SetGlobal32(0x15c90c8, 0x80000001);      // +0x30 PresentationInterval
    // device + mode globals (device pointers set by SetupDevice())
    SetGlobal32(0x15ca7bc, 0);               // not device-lost
    SetGlobal32(0x15ca7cc, (uint32_t)(w | (h << 16)));
    SetGlobal16(0x15ca7d0, 0x20);            // bpp
    SetGlobal32(0x15ca7d8, 0x11);            // MSAA copy
    if (g_devReady) { g_dev.Clear(0x7, 0xFF000000u, 1.0f, 0); g_dev.BeginScene(); }
    return 1; // success
}

// Device state-binding setters. During init the engine clears bindings (null args); at
// render time these will bind real resources. No-op for now -- our D3D11 device manages
// its own binding; the important draw/clear/present are bridged separately.
// Current texture resource bound to stage 0, and a cache: resource ptr -> D3D11 handle.
static void* g_curTexRes = nullptr;
// Cache key includes the Data pointer: animated effect textures (fire etc.) swap their
// pixel pointer per frame -- a key without it serves the first frame forever.
// LRU with eviction: a long session (multiple arenas + damage states + menus) exceeds
// any fixed cap, and the old overflow behavior -- create-but-don't-cache -- re-created
// textures EVERY DRAW once full: GPU memory climbed, the game went slow-motion, then
// crashed (user-reported). Evicted entries release their D3D11 texture.
struct TexCacheEnt { void* res; uint32_t data, fmtd; const void* pal;
                     tj::gfx::TextureHandle h; uint32_t lastUse;
                     uint64_t srcHash;          // content hash of the SOURCE bytes
                     uint32_t updN, lastUpd; }; // in-place update count + last update frame
static TexCacheEnt g_texCache[2048]; static int g_texCacheN = 0;
// Fast 64-bit FNV-1a over the raw source pixels. The perf fix for the user-reported
// intermittent slow-motion: heavy scenes re-registered/re-pointed textures every frame
// (up to ~63/frame measured) and each one paid a full decode + UpdateSubresource +
// GenerateMips even when the CONTENT was identical. Hashing the source (~1-2 GB/s) and
// skipping unchanged uploads collapses that to near-zero.
static uint64_t HashSrc(const uint8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    uint32_t n8 = n >> 3;
    const uint64_t* q = (const uint64_t*)p;
    for (uint32_t i = 0; i < n8; ++i) { h ^= q[i]; h *= 1099511628211ull; }
    for (uint32_t i = n8 << 3; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static uint32_t g_frmTexSkip = 0;    // per-frame unchanged-content skips (log: skip=)
// res -> slot index, so the per-draw lookup is O(1). A LINEAR scan over up to 1024
// entries x hundreds of draws/frame made in-match frame time climb as the cache filled
// across maps (6.9 -> 20 ms) = the "stacking" slow motion. One entry per resource: a
// changed data/format/palette re-decodes into the same slot (see ResolveTexture).
static std::unordered_map<const void*, int> g_texIndex;
static void* g_curPalRes = nullptr;   // stage-0 palette resource (P8 textures)

// True if [p, p+len) is committed readable memory (VirtualQuery walk). Resource Data
// pointers can transiently point at reserved-but-uncommitted heap while a load is in
// flight -- decoding those crashed the bridge, so probe before trusting.
static bool IsReadable(uint32_t p, uint32_t len) {
    uint32_t end = p + len;
    if (end < p) return false;                       // address overflow
    // FAST PATH (perf): the whole game-memory window -- contiguous pool 0x04000000 +
    // low arena up to 0x10000000 -- is backed by pagefile SECTIONS mapped in full at
    // startup (kernel.cpp SetupContiguousPool/ReserveLowArena), so every address inside
    // it is committed by construction and needs no probe. This is not a micro-tweak:
    // VirtualQuery is a syscall and the vertex paths called IsReadable ~2x per draw
    // (~900 syscalls/frame at 450 draws) on pointers that are ALWAYS in this window.
    if (p >= 0x04000000u && end <= 0x10000000u) return true;
#ifdef _WIN32
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)(uintptr_t)p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        p = (uint32_t)(uintptr_t)mbi.BaseAddress + (uint32_t)mbi.RegionSize;
    }
    return true;
#else
    // POSIX equivalent of the VirtualQuery walk: mincore fails with ENOMEM on any
    // unmapped page — same "is this committed" answer, page by page.
    uintptr_t a = p & ~(uintptr_t)0xFFF;
    while (a < end) {
        unsigned char vec = 0;
        if (mincore((void*)a, 0x1000, &vec) != 0) return false;
        a += 0x1000;
    }
    return true;
#endif
}

static int FmtBpp(int fmt);   // defined with the LockRect bridge below

// Bytes of source pixel data for a w*h mip-0 surface in Xbox format `fmt`.
static uint32_t SrcPixelBytes(int fmt, int w, int h) {
    switch (fmt) {
        case 0x0C: return (uint32_t)((w<4?4:w)/4 * ((h<4?4:h)/4) * 8);   // DXT1
        case 0x0E: case 0x0F: return (uint32_t)((w<4?4:w)/4 * ((h<4?4:h)/4) * 16); // DXT3/5
        default:   return (uint32_t)w * h * (uint32_t)FmtBpp(fmt) / 8;
    }
}

// Info about the most recently resolved texture: linear surfaces are sampled with
// UNNORMALIZED texel-space coordinates on NV2A, so draws need a 1/w,1/h UV scale.
static bool g_lastTexLinear = false;
static int  g_lastTexW = 1, g_lastTexH = 1;

// --- Render-to-texture (the game's per-pass RT redirect) ------------------------------
// The engine's pass table (records @0x01623B00, mode dword +0xA0) redirects whole render
// passes into "temp" textures: motion-blur smears (tmP1-3 quads in the MB_<CHAR> scenes
// sample them during knockbacks; headers live in .data BSS @0x95EA40+) and level-select
// previews. FUN_00082660 binds a texture as the render target, FUN_00082620 restores the
// saved main target. We patch those and keep a map D3D8-texture-header -> live D3D11
// render texture; ResolveTexture returns the live texture for anything in the map.
struct RtTexEnt { void* hdr; tj::gfx::TextureHandle h; int w, hgt; };
static RtTexEnt g_rtTex[16]; static int g_rtTexN = 0;
static void*    g_rtActive = nullptr;      // texture header currently bound as RT

static RtTexEnt* RtFind(void* hdr) {
    for (int i = 0; i < g_rtTexN; ++i) if (g_rtTex[i].hdr == hdr) return &g_rtTex[i];
    return nullptr;
}
// Bind `texHdr` (a D3DPixelContainer) as the draw target, creating the native render
// texture on first use (dims from the header's format dword; RT textures are swizzled,
// so the log2 fields are valid).
static bool RtBind(void* texHdr) {
    if (!texHdr || !g_devReady) return false;
    RtTexEnt* e = RtFind(texHdr);
    if (!e) {
        uint32_t fmtd = *(uint32_t*)((char*)texHdr + 0xC);
        int w = 1 << ((fmtd >> 20) & 0xF), h = 1 << ((fmtd >> 24) & 0xF);
        uint32_t sizef = *(uint32_t*)((char*)texHdr + 0x10);
        if (sizef) { w = (int)(sizef & 0xFFF) + 1; h = (int)((sizef >> 12) & 0xFFF) + 1; }
        if (w < 1 || w > 2048 || h < 1 || h > 2048 || g_rtTexN >= 16) return false;
        tj::gfx::TextureHandle th = g_dev.CreateRenderTexture(w, h);
        if (th < 0) return false;
        g_rtTex[g_rtTexN] = { texHdr, th, w, h };
        e = &g_rtTex[g_rtTexN++];
        printf("[d3d8] render texture %dx%d for hdr %p (fmt %08x)\n", w, h, texHdr, fmtd);
    }
    g_dev.SetRenderTexture(e->h);
    g_rtActive = texHdr;
    return true;
}
// FUN_00082660(pTexSlot, pDepthSlot) cdecl: begin rendering into pTexSlot->tex (+4).
// Faithful side effects: colormask renderstate/shadow 0 (the quad-shadow flush restores
// it mid-pass, exactly as on hardware) + RTT-active flag.
static void __cdecl Br_RttBegin(void* texSlot, void* depthSlot) {
    (void)depthSlot;
    void* texHdr = texSlot ? (void*)(uintptr_t)*(uint32_t*)((char*)texSlot + 4) : nullptr;
    static int calls = 0;
    if (calls < 12) { ++calls;
        printf("[rtt] begin f=%d slot=%p hdr=%p\n", g_frame, texSlot, texHdr); }
    if (!RtBind(texHdr)) return;
    SetGlobal32(0xA65CC, 0);
    SetGlobal32(0x1625910, 1);
}
// FUN_000826e0(pContainer) cdecl: rebind a container as the target, keeping the saved
// depth (pass mode 2). The argument is the pixel container itself (not a slot).
static void __cdecl Br_RttContinue(void* texHdr) {
    if (!RtBind(texHdr)) return;
    SetGlobal32(0x1625910, 1);
}
// FUN_00082620() cdecl: restore the main render target if a redirect is active.
static void __cdecl Br_RttEnd() {
    if (*(volatile uint32_t*)(uintptr_t)0x1625910 == 0) return;
    if (g_rtActive && g_devReady) g_dev.SetRenderTargetBackbuffer();
    g_rtActive = nullptr;
    SetGlobal32(0xA65CC, 0x01010101);
    SetGlobal32(0x1625910, 0);
}

// --- The engine's runtime "temp" texture headers (game .data BSS @0x95EA2C+, 0x14 B
// each). These are general-purpose dynamic textures the game points at different
// content over time:
//   FRONTEND: the logo/title screens COMPOSITE REAL LOADED ARTWORK through them (the
//     WAR-of-the-WHISKERS logo pieces sample 0x95EA2C/0x95EA68 with art the game loads
//     into pool buffers) -- they must decode NORMALLY there, or the startup logo
//     vanishes (session-10 regression, user-reported).
//   IN-MATCH: the motion-blur smear strips (tmP1-3 in the MB_<CHAR> scenes) sample
//     them, but the capture passes that would fill them are DISABLED in the retail
//     build (match init writes the enable at [master+0x1C918], then unconditionally
//     zeroes it). On hardware they sample stale/zero memory (invisible at the smears'
//     low alphas); our pool RECYCLES that memory into live data, which decoded as the
//     opaque-white knockback shapes. In matches, resolve them transparent.
// If frontend content is unreadable (load in flight), fall back to the latched
// transition snapshot (0x95EA2C) / transparent rather than flashing white.
static const uint32_t kTempTexBase = 0x95EA2C, kTempTexEnd = 0x95EA2C + 6 * 0x14;
static const uint32_t kTransitionTexHdr = 0x95EA2C;
static tj::gfx::TextureHandle g_transitionCap = tj::gfx::kNoTexture;  // latched snapshot
static tj::gfx::TextureHandle g_transparentTex = tj::gfx::kNoTexture;
static tj::gfx::TextureHandle TransparentTex() {
    if (g_transparentTex < 0) {
        uint32_t clear = 0;
        g_transparentTex = g_dev.CreateTexture(&clear, 1, 1);
    }
    return g_transparentTex;
}

// Decode into a REUSED buffer, not a fresh vector: in a busy arena this runs 44-72 times per
// frame, and the vector API charged each call a heap allocation, a prefill of every texel that
// the decode then overwrote, and an alpha scan for a flag only the native mesh demo reads.
// Same pixels -- tex_test proves bit-identity over 22 formats x 10 sizes (220/220) and
// measures 3.0x overall, 3-13x on the swizzled formats the game actually ships. The buffer is
// shared by both texture paths: they run on the game thread, one decode at a time, and the
// result is consumed (uploaded) before the next call.
static bool DecodeScratch(int fmt, const uint8_t* pix, size_t avail, const uint32_t* pal,
                          int w, int h, uint32_t** out) {
    static uint32_t* buf = nullptr; static size_t cap = 0;
    size_t need = (size_t)w * (size_t)h;
    if (need > cap) {
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        size_t want = (need + 0xFFFF) & ~(size_t)0xFFFF;
        buf = (uint32_t*)VirtualAlloc(nullptr, want * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        cap = buf ? want : 0;
    }
    *out = buf;
    if (!buf) return false;
    // TJ_TEXFAST=0 reverts to the allocating vector decoder IN THE SAME BINARY, so the device
    // A/B is one build measured two ways -- the rule this project has used for every
    // mechanism swap (dispatch, GCALL, raw hooks, the JIT tiers).
    static int fast = -1;
    if (fast < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_TEXFAST");
                    fast = (e && *e == '0') ? 0 : 1; free(e); }
    PhaseTimer _pd(&g_tTexDec);
    bool ok;
    if (fast) {
        ok = tj::assets::DecodeXboxTextureInto(fmt, pix, avail, pal, w, h, buf);
    } else {
        tj::assets::DecodedTexture d = tj::assets::DecodeXboxTexture(fmt, pix, avail, pal, w, h);
        ok = d.ok && !d.rgba.empty();
        if (ok) memcpy(buf, d.rgba.data(), (size_t)w * h * 4);
    }
    if (ok) g_texDecPix += (uint64_t)w * (uint64_t)h;
    return ok;
}

// Resolve (decode + upload, cached) the bound resource to a D3D11 texture handle.
static const bool g_arabicProbe = [] {
    char v[8] = { 0 };
    return GetEnvironmentVariableA("TJ_ARABIC_PROBE", v, sizeof v) && v[0] == '1';
}();
// The 512x512 replacement glyph sheet, created once per resource that carries the retail
// one. arabic.cpp owns the pixels (retail art decoded out of the guest's own texture by
// this same decoder, Arabic strip below it); the handle is cached here beside the device.
// A replacement controller glyph, cached beside the device like the sheet.
static tj::gfx::TextureHandle ArabicSpriteTex(uint32_t resVA, int idx) {
    static uint32_t cRes[8] = { 0 };
    static tj::gfx::TextureHandle cTex[8];
    static int cN = 0;
    for (int i = 0; i < cN; ++i) if (cRes[i] == resVA) return cTex[i];
    int w = 0, h = 0;
    const uint32_t* px = ArabicSpritePixels(idx, &w, &h);
    if (!px) return tj::gfx::kNoTexture;
    tj::gfx::TextureHandle t = g_dev.CreateTexture(px, w, h);
    if (t >= 0 && cN < 8) { cRes[cN] = resVA; cTex[cN] = t; ++cN; }
    g_lastTexW = w; g_lastTexH = h;
    return t;
}
static tj::gfx::TextureHandle ArabicSubstitute(uint32_t resVA) {
    // ⚠ SAME CAP AS arabic.cpp's slot table. When this was 4 and that was 16, a fifth sheet
    // address missed here on every bind and re-created a 512x1024 texture plus its mip chain
    // each time, with nothing reclaiming them.
    static uint32_t cRes[kArabicAtlasSlots] = { 0 };
    static tj::gfx::TextureHandle cTex[kArabicAtlasSlots];
    static int cN = 0;
    static int cW = 0, cH = 0;
    for (int i = 0; i < cN; ++i)
        if (cRes[i] == resVA) { g_lastTexW = cW; g_lastTexH = cH; return cTex[i]; }
    int aw = 0, ah = 0;
    const uint32_t* px = ArabicAtlasPixels(resVA, &aw, &ah);
    if (!px) return tj::gfx::kNoTexture;
    tj::gfx::TextureHandle t = g_dev.CreateTexture(px, aw, ah);
    if (t >= 0 && cN < kArabicAtlasSlots) { cRes[cN] = resVA; cTex[cN] = t; ++cN; }
    cW = aw; cH = ah;
    g_lastTexW = aw; g_lastTexH = ah;
    return t;
}
static tj::gfx::TextureHandle ResolveTexture(void* res) {
    PhaseTimer _pt(&g_tTex);
    g_lastTexLinear = false; g_lastTexW = g_lastTexH = 1;
    if (!res) return tj::gfx::kNoTexture;
    // Live render textures (motion-blur smears, preview panels) resolve to the native
    // texture that was rendered into -- their pool "pixels" are never CPU-written.
    if (RtTexEnt* rt = RtFind(res)) return rt->h;
    uint32_t resVA = (uint32_t)(uintptr_t)res;
    // ARABIC: the glyph sheet is replaced wholesale by a larger one (arabic.cpp), because
    // the retail 256x256 has no free space left. Intercepted here rather than decoded,
    // since the resource's own format dword has been retold it is 512x512 -- which is what
    // makes the engine divide glyph rectangles by the new width.
    if (g_arabicProbe) {                 // TJ_ARABIC_PROBE=1: what resources actually arrive
        static uint32_t seen[64]; static int seenN = 0;
        bool dup = false;
        for (int i = 0; i < seenN; ++i) if (seen[i] == resVA) dup = true;
        if (!dup && seenN < 64) {
            seen[seenN++] = resVA;
            uint32_t f = *(uint32_t*)((char*)res + 0x0C);
            uint32_t dp = *(uint32_t*)((char*)res + 0x04);
            printf("[ar-probe] res=%08X data=%08X first=%08X fmtd=%08X %dx%d fmt=%02X\n",
                   resVA, dp, (dp >= 0x1000 && dp < 0x10000000) ? *(uint32_t*)(uintptr_t)dp : 0,
                   f, 1 << ((f >> 20) & 0xF), 1 << ((f >> 24) & 0xF), (f >> 8) & 0xFF);
        }
    }
    if (ArabicIsFontAtlas(resVA)) {
        ArabicReassertSheet(resVA);   // a FONT.PS2 reload restores the retail 256x256 dword,
                                      // and this early-out is what would make that permanent
        tj::gfx::TextureHandle t = ArabicSubstitute(resVA);
        if (t >= 0) return t;
    }
    if (int sp = ArabicSpriteFor(resVA); sp >= 0) {
        tj::gfx::TextureHandle t = ArabicSpriteTex(resVA, sp);
        if (t >= 0) return t;
    }
    const bool tempTex = resVA >= kTempTexBase && resVA < kTempTexEnd;
    if (tempTex && *(volatile uint8_t*)(uintptr_t)0x184661 == 7)   // in-match: smear strips
        return TransparentTex();
    uint32_t common = *(uint32_t*)((char*)res + 0x00);
    if ((common & 0x00070000) != 0x00040000) return tj::gfx::kNoTexture; // not a texture
    uint32_t data  = *(uint32_t*)((char*)res + 0x04);
    uint32_t fmtd  = *(uint32_t*)((char*)res + 0x0C);
    uint32_t sizef = *(uint32_t*)((char*)res + 0x10);
    int fmt = (fmtd >> 8) & 0xFF;
    int w, h; uint32_t pitch = 0;
    if (sizef) {
        // LINEAR surface: dims/pitch live in the Size dword, NOT the format log2 fields
        // (those read 0 -> the old code decoded every linear texture as a 1x1 solid color).
        w = (int)(sizef & 0xFFF) + 1;
        h = (int)((sizef >> 12) & 0xFFF) + 1;
        pitch = ((sizef >> 24) + 1) << 6;
    } else {
        w = 1 << ((fmtd >> 20) & 0xF);
        h = 1 << ((fmtd >> 24) & 0xF);
    }
    if (w < 1 || w > 2048 || h < 1 || h > 2048) return tj::gfx::kNoTexture;
    g_lastTexLinear = sizef != 0; g_lastTexW = w; g_lastTexH = h;
    // palette for P8: resource +8 -> palette resource -> its Data (256 ARGB); if the
    // resource doesn't carry one, fall back to the stage-0 palette the game bound via
    // SetPalette (the in-game material path binds palettes separately per stage).
    const uint32_t* pal = nullptr;
    if (fmt == 0x0B) {
        uint32_t prs[2] = { *(uint32_t*)((char*)res + 0x08), (uint32_t)(uintptr_t)g_curPalRes };
        for (uint32_t pr : prs) {
            if (!pr || !IsReadable(pr, 8)) continue;
            uint32_t pd = *(uint32_t*)((char*)(uintptr_t)pr + 4);
            if (pd & 0x80000000) pd &= 0x0FFFFFFF;
            if (pd >= 0x1000 && pd < 0x10000000 && IsReadable(pd, 256*4)) { pal = (const uint32_t*)(uintptr_t)pd; break; }
        }
    }
    // One cache entry per resource, indexed O(1) by res. Unchanged (same data/fmt/pal) =
    // fast return; a changed data/fmt/pal re-decodes into the SAME texture in place
    // (animated water/flipbooks: no per-frame allocation, no linear scan).
    int existSlot = -1;
    { auto it = g_texIndex.find(res);
      if (it != g_texIndex.end()) {
          int i = it->second;
          g_texCache[i].lastUse = (uint32_t)g_frame;
          if (g_texCache[i].fmtd == fmtd && g_texCache[i].pal == pal && g_texCache[i].data == data)
              return g_texCache[i].h;                          // unchanged: fast path
          existSlot = i;                                       // changed: re-decode below
      } }
    uint32_t srcBytes = pitch ? pitch * (uint32_t)h : SrcPixelBytes(fmt, w, h);
    bool inRange = data >= 0x1000 && data < 0x10000000;
    bool readable = inRange && IsReadable(data, srcBytes);
    const uint8_t* pix = readable ? (const uint8_t*)(uintptr_t)data : nullptr;
    if (!pix) {
        // Unreadable/absent pixels: skip this frame, DON'T cache -- the data may arrive later.
        static void* warned[64]; static int warnedN = 0;
        bool seen = false;
        for (int i = 0; i < warnedN; ++i) if (warned[i] == res) { seen = true; break; }
        if (!seen && warnedN < 64) { warned[warnedN++] = res;
            printf("[d3d8] ResolveTexture SKIP res=%p data=%08x fmt=%02x %dx%d need=%u %s\n",
                   res, data, fmt, w, h, SrcPixelBytes(fmt, w, h),
                   inRange ? "unreadable" : "out-of-range"); }
        if (tempTex)
            return (resVA == kTransitionTexHdr && g_transitionCap >= 0) ? g_transitionCap
                                                                        : TransparentTex();
        return tj::gfx::kNoTexture;
    }
    // Content dirty-check BEFORE any decode work: same format/palette and identical
    // source bytes (only the Data pointer moved) = nothing to do. This is the hot path
    // for the re-register/ping-pong storms that caused in-match slow-motion.
    uint64_t srcHash = 0; bool haveHash = false;
    if (existSlot >= 0 && g_texCache[existSlot].h >= 0 &&
        g_texCache[existSlot].fmtd == fmtd && g_texCache[existSlot].pal == pal) {
        srcHash = HashSrc(pix, srcBytes); haveHash = true;
        if (srcHash == g_texCache[existSlot].srcHash) {
            g_texCache[existSlot].data = data;
            ++g_frmTexSkip;
            return g_texCache[existSlot].h;
        }
    }
    // ARABIC: the glyph sheet is recognised by its own bytes -- the file is loaded once per
    // resource container, so this catches every copy, and it cannot pick the neighbouring
    // FONG sheet the way a pointer walk can. Registering retells the resource its size,
    // which is what the engine divides glyph rectangles by from the next frame on.
    if (ArabicMatchSheet(fmt, w, h, pix, srcBytes)) {
        ArabicRegisterSheet(resVA);
        tj::gfx::TextureHandle t = ArabicSubstitute(resVA);
        if (t >= 0) return t;
    }
    // The controller face buttons, same identify-by-content rule (arabic.cpp).
    if (int sp = -1; ArabicMatchSprite(fmt, w, h, pix, srcBytes, &sp)) {
        ArabicRegisterSprite(resVA, sp);
        tj::gfx::TextureHandle t = ArabicSpriteTex(resVA, sp);
        if (t >= 0) return t;
    }
    const uint8_t* rawPix = pix;      // hash target: always the PRE-repack source bytes
    // Linear rows are 64-byte aligned; the decoder expects tight rows -> repack.
    static uint8_t* repack = nullptr; static uint32_t repackCap = 0;
    uint32_t tight = (uint32_t)w * (uint32_t)FmtBpp(fmt) / 8;
    if (pitch && tight && pitch != tight) {
        uint32_t need2 = tight * (uint32_t)h;
        if (need2 > repackCap) {
            if (repack) VirtualFree(repack, 0, MEM_RELEASE);
            repackCap = (need2 + 0xFFFFF) & ~0xFFFFFu;
            repack = (uint8_t*)VirtualAlloc(nullptr, repackCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        if (repack) {
            for (int y = 0; y < h; ++y) memcpy(repack + (size_t)y * tight, pix + (size_t)y * pitch, tight);
            pix = repack;
        }
    }
    // Decode into a REUSED buffer, not a fresh vector: in a busy arena this path runs 44-72
    // times per frame, and the vector API charged each one a heap allocation, a prefill of
    // every texel that the decode then overwrote, and an alpha scan for a flag only the
    // native mesh demo reads. Same pixels -- tex_test proves bit-identity over 22 formats x
    // 10 sizes (220/220) and measures 3.0x overall, 3-13x on the swizzled formats the game
    // actually ships.
    uint32_t* decBuf = nullptr;
    bool decoded = DecodeScratch(fmt, pix, (size_t)w * h * 4, pal, w, h, &decBuf);
    if (!decoded) {
        // Decode FAILED (format the decoder doesn't handle, or P8 with no palette) -->
        // this surface draws white. Log once per resource so missing textures are
        // findable (the SKIP path above only catches unreadable DATA, not bad formats).
        static void* warned[128]; static int warnedN = 0;
        bool seen = false;
        for (int i = 0; i < warnedN; ++i) if (warned[i] == res) { seen = true; break; }
        if (!seen && warnedN < 128) { warned[warnedN++] = res;
            printf("[d3d8] ResolveTexture DECODE-FAIL res=%p fmt=%02x %dx%d pal=%p linear=%d\n",
                   res, fmt, w, h, (void*)pal, (int)g_lastTexLinear); }
    }
    // Animated update: re-upload into the existing texture (no new allocation). If the
    // dimensions changed (rare -- pool res reuse) UpdateTexture fails -> fall through to
    // recreate in the same slot.
    if (!haveHash && decoded) { srcHash = HashSrc(rawPix, srcBytes); haveHash = true; }
    if (existSlot >= 0 && decoded && g_texCache[existSlot].h >= 0) {
        TexCacheEnt& en = g_texCache[existSlot];
        // Amortized mips for rapid updaters: a texture updating every frame regenerates
        // its mip chain only every 4th update (distant content lags <=4 frames --
        // invisible for water/fire), an occasional update still gets fresh mips.
        bool rapid = en.lastUpd && (uint32_t)g_frame - en.lastUpd <= 30;
        bool mips = !rapid || ((en.updN + 1) & 3) == 0;
        bool updOk; { PhaseTimer _pu(&g_tTexUpl);
                      updOk = g_dev.UpdateTexture(en.h, decBuf, w, h, mips); }
        if (updOk) {
            en.data = data; en.srcHash = srcHash; ++en.updN; en.lastUpd = (uint32_t)g_frame;
            ++g_frmTexUpdate;
            return en.h;
        }
    }
    // Recycling pool (session 12): Acquire reuses a released same-size texture via
    // UpdateSubresource, or falls through to CreateTexture on a pool miss. The old
    // handle is Released AFTER the Acquire below, so Acquire can never hand back the
    // texture this cache entry still references.
    tj::gfx::TextureHandle handle = tj::gfx::kNoTexture;
    if (decoded) { ++g_frmTexCreate; PhaseTimer _pa(&g_tTexUpl);
                   handle = g_dev.AcquireTexture(decBuf, w, h); }
    int slot;
    if (existSlot >= 0) {                        // reuse the entry (dims changed / had no tex)
        slot = existSlot;
        if (g_texCache[slot].h >= 0) g_dev.ReleaseTexture(g_texCache[slot].h);
    } else if (g_texCacheN < 2048) slot = g_texCacheN++;
    else {
        // EVICTION. Counted because a FULL cache whose working set no longer fits degrades
        // into evict-and-recreate every frame -- a regime that persists until a level load
        // rebuilds the cache, which is exactly the shape of "it went slow and stayed slow
        // until I restarted the map". The scan itself is O(2048) per eviction on top.
        ++g_texEvict;
        slot = 0;                                // evict least-recently-used
        for (int i = 1; i < g_texCacheN; ++i)
            if (g_texCache[i].lastUse < g_texCache[slot].lastUse) slot = i;
        if (g_texCache[slot].h >= 0) g_dev.ReleaseTexture(g_texCache[slot].h);
        g_texIndex.erase(g_texCache[slot].res);  // drop the evicted res from the index
    }
    g_texCache[slot] = { res, data, fmtd, pal, handle, (uint32_t)g_frame, srcHash, 0, 0 };
    g_texIndex[res] = slot;
    return handle;
}
// All 4 stages are shadowed: the Shiny floor material binds stages 0-3 (base, mask and
// two env/sheen layers); everything else uses stage 0 only.
static void* g_stageTexRes[4] = {};
static void* g_stagePalRes[4] = {};
static void __stdcall Br_SetTexture(uint32_t stage, void* tex) {
    if (stage < 4) g_stageTexRes[stage] = tex;
    if (stage == 0) g_curTexRes = tex;
}
static void __stdcall Br_SetPalette(uint32_t stage, void* pal) {
    if (stage < 4) g_stagePalRes[stage] = pal;
    if (stage == 0) g_curPalRes = pal;
}

// --- 3D mesh path: stream/index state ---------------------------------------
// SetStreamSource(Stream, pVB, Stride) stdcall ret 0xC -- mesh path uses stream 0 only.
// SetIndices(pIB, BaseVertexIndex) stdcall ret 8 -- the REAL D3D8 writes IB->Data into the
// global D3D__IndexData @0xA62B4 and the game then computes Draw pointers as
// [0xA62B4] + startWord*2, so our bridge must keep that global populated too.
static void*    g_curVB = nullptr;       // D3DVertexBuffer* (Data @ +4)
static uint32_t g_curVBStride = 0;
static uint32_t g_baseVertex = 0;
static void __stdcall Br_SetStreamSource(uint32_t stream, void* vb, uint32_t stride) {
    if (stream != 0) return;
    g_curVB = vb; g_curVBStride = stride;
}
static void __stdcall Br_SetIndices(void* ib, uint32_t baseVtx) {
    g_baseVertex = baseVtx;
    uint32_t data = ib ? *(uint32_t*)((char*)ib + 4) : 0;
    SetGlobal32(0xA62B4, data);                       // D3D__IndexData (game reads this)
    *(uint32_t*)(uintptr_t)(kDevice + 0x1C) = baseVtx;
}

// ===========================================================================
// Shader + draw bridge. The Xbox binds real NV2A vertex/pixel microcode; we can't run
// that, so SetVertexShader/SetPixelShader become no-ops (we record the handle) and the
// actual drawing is done natively in DrawVerticesUP using a semantic-replacement pipeline
// (transform by the captured c[100..103] matrix, sample the bound texture, alpha-blend).
// ===========================================================================
static float    g_vsc[304][4];         // shadowed vertex-shader constants (c[0..303])
static float    g_psc[16][4] = { {1,1,1,1}, {1,1,1,1} };  // pixel-shader constants; c1 = sprite tint
static uint32_t g_curVS = 0, g_curPS = 0;
static tj::gfx::TextureHandle g_curTex = tj::gfx::kNoTexture;

// D3DDevice_SetPixelShaderConstant(Register, pConstantData, ConstantCount) ret 0xC.
// The 2D UI writes each sprite's RGBA tint (incl. the screen-fade overlay alpha) to ps c1;
// stubbing this drew every sprite untinted opaque white (the "white screen" bug).
static void __stdcall Br_SetPSConst(uint32_t reg, const float* data, uint32_t count) {
    if (!data) return;
    for (uint32_t i = 0; i < count && reg + i < 16; ++i)
        memcpy(g_psc[reg + i], data + i * 4, 16);
}
static uint32_t PackTint(const float* c) {   // (r,g,b,a) floats -> 0xAARRGGBB
    auto b = [](float f) -> uint32_t { int v = (int)(f * 255.0f + 0.5f); return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    return (b(c[3]) << 24) | (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
}

static void __stdcall Br_SetVertexShader(uint32_t handle) { g_curVS = handle; }
static void __stdcall Br_SetPixelShader(uint32_t handle)  { g_curPS = handle; }

// Created-shader registry: each CreateVertexShader call gets a DISTINCT odd fake handle and
// we parse its D3DVSD declaration into a stream-0 component layout, so the draw bridge can
// fetch position/uv/color from the vertex data of any technique. Decl dwords (Xbox D3DVSD):
//   0x2000000s               stream select s
//   0x40000000|(type<<16)|r  bind next component (type 0x32=FLOAT3, 0x22=FLOAT2,
//                            0x40=D3DCOLOR, 0x45=SHORT4, 0x35=SHORT3, 0x34=PBYTE3) to reg r
//   0x50000000|(dwords<<16)  skip <dwords> dwords of pad
//   0xFFFFFFFF               end
struct ShaderRec {
    uint32_t handle; uint32_t fn;                 // fn = microcode VA (identifies technique)
    int stride, posOff, uvOff, colorOff;
    int uv2Off;                                   // second FLOAT2 (particle billboard corner)
    int normOff;                                  // second FLOAT3 (vertex normal)
    int boneRegOff, boneWtOff;                    // skinning: 4 x s16 user-reg, 4 x u8 weight
};
static ShaderRec g_shaderRecs[192]; static int g_shaderRecN = 0;
static int VsdTypeSize(uint32_t t) {
    switch (t) { case 0x32: return 12; case 0x22: return 8; case 0x40: return 4;
                 case 0x45: return 8;  case 0x35: return 6; case 0x34: return 3; default: return 4; }
}
static int32_t __stdcall Br_CreateVertexShader(const uint32_t* decl, uint32_t func, uint32_t* pHandle, uint32_t usage) {
    (void)usage;
    ShaderRec r = {}; r.fn = func;
    r.posOff = r.uvOff = r.colorOff = r.uv2Off = r.normOff = r.boneRegOff = r.boneWtOff = -1;
    int off = 0, stream = 0, prevType = 0;
    for (int i = 0; decl && i < 64 && decl[i] != 0xFFFFFFFF; ++i) {
        uint32_t d = decl[i];
        if ((d & 0xF0000000) == 0x20000000) { stream = d & 0xF; continue; }
        if (stream != 0) continue;
        if ((d & 0xF0000000) == 0x50000000) { off += ((d >> 16) & 0xFFF) * 4; continue; }
        if ((d & 0xF0000000) == 0x40000000) {
            uint32_t type = (d >> 16) & 0xFF;
            if (type == 0x32 && r.posOff < 0) r.posOff = off;
            else if (type == 0x32 && r.normOff < 0) r.normOff = off; // vertex normal
            else if (type == 0x22 && r.uvOff  < 0) r.uvOff  = off;
            else if (type == 0x22 && r.uv2Off < 0) r.uv2Off = off;   // billboard corner
            else if (type == 0x45 && r.boneRegOff < 0) r.boneRegOff = off;   // SHORT4 bone regs
            // D3DCOLOR is both diffuse (XMF token 7) and skin weights (token 14, which
            // directly follows the SHORT4 bone indices) -- classify by what precedes it.
            else if (type == 0x40 && prevType == 0x45 && r.boneWtOff < 0) r.boneWtOff = off;
            else if (type == 0x40 && r.colorOff < 0) r.colorOff = off;
            off += VsdTypeSize(type); prevType = (int)type;
        }
    }
    r.stride = off;
    // Dedupe: every level/FE load re-registers the same technique shaders. Without
    // this the registry filled after ~7 loads and later CreateVertexShader calls went
    // unregistered -- FindShader missed and their draws silently vanished.
    for (int i = 0; i < g_shaderRecN; ++i) {
        const ShaderRec& e = g_shaderRecs[i];
        if (e.fn == r.fn && e.stride == r.stride && e.posOff == r.posOff &&
            e.uvOff == r.uvOff && e.colorOff == r.colorOff && e.uv2Off == r.uv2Off &&
            e.normOff == r.normOff && e.boneRegOff == r.boneRegOff &&
            e.boneWtOff == r.boneWtOff) {
            if (pHandle) *pHandle = e.handle;
            return 0;
        }
    }
    r.handle = 0x101 + (uint32_t)g_shaderRecN * 2;    // odd => "created shader"
    if (g_shaderRecN < 192) g_shaderRecs[g_shaderRecN++] = r;
    else printf("[d3d8] WARN shader registry full (fn=%08x stride=%d)\n", r.fn, r.stride);
    if (pHandle) *pHandle = r.handle;
    return 0;
}
static const ShaderRec* FindShader(uint32_t handle) {
    for (int i = 0; i < g_shaderRecN; ++i)
        if (g_shaderRecs[i].handle == handle) return &g_shaderRecs[i];
    return nullptr;
}
// The 2D UI techniques (HUD.XBase @0x17e5f0, HUDFlat.XBase @0x17e980) transform pos*2-1
// via c105; everything else transforms by the captured c100..103 = transpose(WVP).
// Identify them by microcode pointer read live from the technique structs (+0x110).
static bool IsUiShader(const ShaderRec* r) {
    if (!r) return true;
    uint32_t hudFn  = *(uint32_t*)(uintptr_t)(0x17e5f0 + 0x110);
    uint32_t flatFn = *(uint32_t*)(uintptr_t)(0x17e980 + 0x110);
    return r->fn == hudFn || r->fn == flatFn;
}
// SpecialEffects2D (particle billboards): all 4 quad verts share the center position;
// the expansion lives in the v8 corner attribute + constant scales.
static bool IsParticleShader(const ShaderRec* r) {
    return r && r->fn == *(uint32_t*)(uintptr_t)(0x17ED10 + 0x110);
}
static int32_t __stdcall Br_CreatePixelShader(uint32_t func, uint32_t* pHandle) {
    (void)func; if (pHandle) *pHandle = 0x103; return 0;
}
// __fastcall(ecx=StartRegister, edx=pConstantData) -- 4 constants (16 floats), fixed.
static void __fastcall Br_SetVSConst4(uint32_t reg, const float* data) {
    if (data && reg + 4 <= 304) memcpy(g_vsc[reg], data, 16 * sizeof(float));
}
static void __fastcall Br_SetVSConst1(uint32_t reg, const float* data) {
    if (data && reg < 304) memcpy(g_vsc[reg], data, 4 * sizeof(float));
}
// SetVertexShaderConstantNotInline(ecx=reg, edx=pData, stack: count) ret 4 -- used for
// bone palettes and bulk uploads; capture into the same shadow.
static uint32_t g_dbgBoneUploads = 0;   // NotInline writes into the bone-palette range
// NOTE: the count argument is in DWORDS (floats), not vec4s -- a bone-palette upload is
// count=0xC (3 vec4 rows). Treating it as vec4s over-copied 4x and stomped neighbors.
static void __fastcall Br_SetVSConstN(uint32_t reg, const float* data, uint32_t countDwords) {
    if (reg < 96) ++g_dbgBoneUploads;
    if (!data || reg >= 304) return;
    uint32_t maxDw = (304 - reg) * 4;
    if (countDwords > maxDw) countDwords = maxDw;
    memcpy(g_vsc[reg], data, countDwords * 4);
}
// LoadVertexShader/SelectVertexShader/SetVertexShaderInput operate on real Xbox shader
// objects; our created-shader handles are fakes (semantic replacement), so dereferencing
// them in the real code AVs. No-op all three.
static void __stdcall Br_LoadVertexShader(uint32_t handle, uint32_t address) { (void)handle; (void)address; }
static void __stdcall Br_SelectVertexShader(uint32_t handle, uint32_t address) { (void)handle; (void)address; }
static int32_t __stdcall Br_SetVertexShaderInput(uint32_t handle, uint32_t count, void* inputs) {
    (void)handle; (void)count; (void)inputs; return 0;
}

// Apply the captured transform c[100..103] (rows of M) to a point: clip = M * (pos,1),
// via per-component dp4 (matches the game's transposed-upload convention).
static void TransformPoint(const float* pos, float out[4]) {
    float p[4] = { pos[0], pos[1], pos[2], 1.0f };
    for (int r = 0; r < 4; ++r) {
        const float* c = g_vsc[100 + r];
        out[r] = c[0]*p[0] + c[1]*p[1] + c[2]*p[2] + c[3]*p[3];
    }
}

// Triangulate `n` sequential vertex positions into a triangle list by NV2A primitive type
// (5=TRILIST 6=TRISTRIP 7=TRIFAN 8=QUADLIST 9=QUADSTRIP 10=POLYGON). Returns index count.
static int TriangulateToList(uint32_t primType, uint32_t n, uint16_t* ib, int cap) {
    int ni = 0;
    switch (primType) {
    case 5:
        for (uint32_t i = 0; i + 3 <= n && ni + 3 <= cap; i += 3) { ib[ni++]=(uint16_t)i; ib[ni++]=(uint16_t)(i+1); ib[ni++]=(uint16_t)(i+2); }
        break;
    case 6: case 9:
        for (uint32_t i = 0; i + 2 < n && ni + 3 <= cap; ++i) {
            if (i & 1) { ib[ni++]=(uint16_t)(i+1); ib[ni++]=(uint16_t)i; ib[ni++]=(uint16_t)(i+2); }
            else       { ib[ni++]=(uint16_t)i; ib[ni++]=(uint16_t)(i+1); ib[ni++]=(uint16_t)(i+2); }
        }
        break;
    case 8:
        for (uint32_t q = 0; q + 4 <= n && ni + 6 <= cap; q += 4) {
            ib[ni++]=(uint16_t)q; ib[ni++]=(uint16_t)(q+1); ib[ni++]=(uint16_t)(q+2);
            ib[ni++]=(uint16_t)q; ib[ni++]=(uint16_t)(q+2); ib[ni++]=(uint16_t)(q+3);
        }
        break;
    default: // 7 fan, 10 polygon
        for (uint32_t i = 1; i + 1 < n && ni + 3 <= cap; ++i) { ib[ni++]=0; ib[ni++]=(uint16_t)i; ib[ni++]=(uint16_t)(i+1); }
        break;
    }
    return ni;
}

// --- 3D draw core ------------------------------------------------------------
// Game/GPU address -> CPU pointer: the WC alias sets bit 31 (0x80000000|phys) and
// D3DResource_Register masks to 28 bits, so a 28-bit mask recovers the identity-mapped
// pool/arena address for any aliased/masked form.
static const uint8_t* GameMem(uint32_t addr) {
    if (addr & 0x80000000) addr &= 0x0FFFFFFF;
    return (const uint8_t*)(uintptr_t)addr;
}
static tj::gfx::VertexPTC* g_meshVB = nullptr;
static uint16_t*           g_meshIB = nullptr;
static const int kMeshMaxV = 65536, kMeshMaxI = 65536 * 3;

// The mesh-path draw: fetch vertices via the bound created-shader's parsed decl (from the
// stream-0 VB, or upData for UP draws), add the c126 per-part offset, transform by the
// captured c100..103 = transpose(WVP), depth-tested. indices==null -> sequential from
// startVertex; SetIndices' base vertex applies to indexed VB draws only.
static void Draw3D(uint32_t prim, const uint16_t* indices, uint32_t indexCount,
                   uint32_t startVertex, const uint8_t* upData, uint32_t upStride) {
    PhaseTimer _pt(&g_tDraw);
    // Diagnostic (TJ_DRAWLOG=frame): log every Draw3D call + drop reason for 2 frames,
    // plus every 2D-particle batch on any frame (see below).
    static int dlogFrom = -2;
    if (dlogFrom == -2) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_DRAWLOG"); dlogFrom = e ? atoi(e) : -1; free(e); }
    const bool dlog = dlogFrom >= 0 && g_frame >= dlogFrom && g_frame < dlogFrom + 2;
    if (!g_devReady || indexCount < 3 || indexCount > (uint32_t)kMeshMaxV) { ++g_dbgSkips; return; }
    const ShaderRec* sr = FindShader(g_curVS);
    if (!sr || sr->posOff < 0) { ++g_dbgSkips;
        if (dlog) printf("[dlog] f=%d DROP no-decl prim=%u n=%u vs=%08x\n", g_frame, prim, indexCount, g_curVS);
        return; }
    const uint8_t* vbase; uint32_t stride;
    if (upData) { vbase = upData; stride = upStride ? upStride : (uint32_t)sr->stride; }
    else {
        if (!g_curVB) return;
        vbase = GameMem(*(uint32_t*)((char*)g_curVB + 4));
        stride = g_curVBStride ? g_curVBStride : (uint32_t)sr->stride;
    }
    if (!vbase || !stride) return;
    if (!g_meshVB) {
        g_meshVB = (tj::gfx::VertexPTC*)VirtualAlloc(nullptr, sizeof(tj::gfx::VertexPTC) * kMeshMaxV,
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_meshIB = (uint16_t*)VirtualAlloc(nullptr, sizeof(uint16_t) * kMeshMaxI,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_meshVB || !g_meshIB) return;
    }
    uint32_t base = (indices && !upData) ? g_baseVertex : 0;
    // bounds-probe the source ranges before trusting game pointers
    uint32_t maxVi = 0;
    if (indices) {
        if (!IsReadable((uint32_t)(uintptr_t)indices, indexCount * 2)) {
            if (dlog) printf("[dlog] f=%d DROP ib-unread prim=%u n=%u\n", g_frame, prim, indexCount);
            return; }
        for (uint32_t k = 0; k < indexCount; ++k) if (indices[k] > maxVi) maxVi = indices[k];
        maxVi += base;
    } else maxVi = startVertex + indexCount - 1;
    if (!IsReadable((uint32_t)(uintptr_t)vbase, (maxVi + 1) * stride)) {
        if (dlog) printf("[dlog] f=%d DROP vb-unread prim=%u n=%u vb=%p stride=%u maxVi=%u\n",
                         g_frame, prim, indexCount, vbase, stride, maxVi);
        return; }
    if (dlog) {
        const uint8_t* v0 = vbase + (size_t)(indices ? indices[0] + base : startVertex) * stride;
        const float* p0 = (const float*)(v0 + sr->posOff);
        // NDC bbox + vertex alpha range: locate each draw on screen and spot fade meshes.
        float nx0 = 1e9f, ny0 = 1e9f, nx1 = -1e9f, ny1 = -1e9f;
        uint32_t aMin = 255, aMax = 0;
        for (uint32_t k = 0; k < indexCount; ++k) {
            uint32_t vi = indices ? (uint32_t)indices[k] + base : startVertex + k;
            const uint8_t* v = vbase + (size_t)vi * stride;
            const float* p = (const float*)(v + sr->posOff);
            float in[3] = { p[0] - g_vsc[126][0], p[1] - g_vsc[126][1], p[2] - g_vsc[126][2] };
            float clip[4];
            for (int r = 0; r < 4; ++r) {
                const float* c = g_vsc[100 + r];
                clip[r] = c[0]*in[0] + c[1]*in[1] + c[2]*in[2] + c[3];
            }
            if (clip[3] > 0.001f) {
                float x = clip[0] / clip[3], y = clip[1] / clip[3];
                if (x < nx0) nx0 = x; if (x > nx1) nx1 = x;
                if (y < ny0) ny0 = y; if (y > ny1) ny1 = y;
            }
            if (sr->colorOff >= 0) {
                uint32_t a = *(const uint32_t*)(v + sr->colorOff) >> 24;
                if (a < aMin) aMin = a; if (a > aMax) aMax = a;
            }
        }
        printf("[dlog] f=%d prim=%u n=%u base=%u maxVi=%u stride=%u fn=%08x vb=%p v0=(%.1f,%.1f,%.1f) c126=(%.1f,%.1f,%.1f) tex=%p cw=%08x zw=%u dst=%03x ndc=(%.2f,%.2f..%.2f,%.2f) a=%u..%u\n",
               g_frame, prim, indexCount, base, maxVi, stride, sr->fn, vbase, p0[0], p0[1], p0[2],
               g_vsc[126][0], g_vsc[126][1], g_vsc[126][2], g_curTexRes,
               *(volatile uint32_t*)(uintptr_t)0xA65CC, *(volatile uint32_t*)(uintptr_t)0xA65C0,
               *(volatile uint32_t*)(uintptr_t)0xA65BC, nx0, ny0, nx1, ny1, aMin, aMax);
    }
    // Resolve the texture up front: linear textures need texel-space UV normalization.
    // Particle UVs are already normalized by the game -- never rescale them.
    const bool particle = IsParticleShader(sr);
    // With TJ_DRAWLOG set, also log every 2D-particle batch (any frame): emitter draws are
    // sporadic (burner fire, dust) and rarely land inside the 2-frame window.
    if (dlogFrom >= 0 && particle) {
        const float* p0 = (const float*)(vbase + (size_t)startVertex * stride + sr->posOff);
        printf("[dlog] f=%d PARTICLE n=%u v0=(%.1f,%.1f,%.1f) tex=%p\n",
               g_frame, indexCount, p0[0], p0[1], p0[2], g_curTexRes);
    }
    tj::gfx::TextureHandle tex3d = ResolveTexture(g_curTexRes);
    float uScl = 1.0f, vScl = 1.0f;
    if (g_lastTexLinear && !particle) { uScl = 1.0f / (float)g_lastTexW; vScl = 1.0f / (float)g_lastTexH; }
    // TEMP diagnostic (TJ_MBLOG=1): hunt the motion-blur ghost draws (MB_<CHAR>.xmf,
    // EmissiveV, small strips, stride 24) -- log every bucket-path EmissiveV draw with
    // texture + vertex-color detail so ghost frames can be correlated with captures.
    static int mbLog = -1;
    if (mbLog < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_MBLOG"); mbLog = e && atoi(e) ? 1 : 0; free(e); }
    if (mbLog && !upData) {
        uint32_t emissiveFn = *(uint32_t*)(uintptr_t)(0x17c970 + 6*0x390 + 0x110);
        if (sr->fn == emissiveFn && indexCount <= 300 && stride == 24) {
            const uint8_t* v0 = vbase + (size_t)((indices ? indices[0] + base : startVertex)) * stride;
            const float* p0 = (const float*)(v0 + sr->posOff);
            uint32_t col0 = sr->colorOff >= 0 ? *(const uint32_t*)(v0 + sr->colorOff) : 0xDEADBEEF;
            uint32_t rd[5] = {};
            if (g_curTexRes && IsReadable((uint32_t)(uintptr_t)g_curTexRes, 20)) memcpy(rd, g_curTexRes, 20);
            printf("[mb] f=%d n=%u vb=%p v0=(%.1f,%.1f,%.1f) col=%08x tex=%p hdr=%08x/%08x/%08x/%08x/%08x ok=%d c126=(%.1f,%.1f,%.1f)\n",
                   g_frame, indexCount, vbase, p0[0], p0[1], p0[2], col0, g_curTexRes,
                   rd[0], rd[1], rd[2], rd[3], rd[4], tex3d != tj::gfx::kNoTexture,
                   g_vsc[126][0], g_vsc[126][1], g_vsc[126][2]);
        }
    }
    // c126 = per-part position offset for mesh techniques; the particle microcode never
    // reads it (a stale mesh offset would displace every particle).
    const float ox = particle ? 0.0f : g_vsc[126][0];
    const float oy = particle ? 0.0f : g_vsc[126][1];
    const float oz = particle ? 0.0f : g_vsc[126][2];
    const bool skinned = sr->boneRegOff >= 0 && sr->boneWtOff >= 0;
    // Shiny / BumpyShiny technique (by microcode ptr): the floor's 4-stage sheen also
    // arrives through the bucket path (damage-state floor swaps). Same CPU env-UV math
    // as the pushbuffer path; here c126 is the prop bbox center and c106 the (object-
    // space) eye, both provided by the game's per-draw constant uploads.
    static uint32_t shinyFn = 0, bumpyFn = 0;
    if (!shinyFn) { shinyFn = *(uint32_t*)(uintptr_t)(0x17f0a0 + 0x110);
                    bumpyFn = *(uint32_t*)(uintptr_t)(0x17f430 + 0x110); }
    const bool isShiny = (sr->fn == shinyFn || sr->fn == bumpyFn) && sr->normOff >= 0 && !skinned;
    static tj::gfx::VertexPT2C* g_shinyVB = nullptr;
    if (isShiny && !g_shinyVB)
        g_shinyVB = (tj::gfx::VertexPT2C*)VirtualAlloc(nullptr, sizeof(tj::gfx::VertexPT2C) * kMeshMaxV,
                                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    // Particle (SpecialEffects2D) transform = c100..103 like every 3D technique -- ALWAYS.
    // (An old heuristic fell back to c146..149 whenever they were nonzero, from a microcode
    // decode that claimed that block. In-game the CPU-skinning bone palette occupies the
    // hw-reg range that includes c146, so the heuristic saw bone-matrix rows, "chose" 146,
    // and every 2D particle -- burner FIRE, dust -- transformed to garbage clip space with
    // w==0 and vanished off-screen. The frontend has no bone uploads, which is why the
    // particle path looked verified there.)
    static int skinLog = 0;
    if (skinned && skinLog < 3) { ++skinLog;
        const uint8_t* v0 = vbase + (size_t)((indices ? indices[0] + base : startVertex)) * stride;
        const int16_t* br = (const int16_t*)(v0 + sr->boneRegOff);
        const float* pp = (const float*)(v0 + sr->posOff);
        int hw0 = (int)br[0] + 96;
        printf("[skin] stride=%u pos=%d reg=%d wt=%d uv=%d col=%d | v0 pos=%.2f,%.2f,%.2f regs=%d,%d,%d,%d wts=%u,%u,%u,%u\n",
               stride, sr->posOff, sr->boneRegOff, sr->boneWtOff, sr->uvOff, sr->colorOff,
               pp[0], pp[1], pp[2], br[0], br[1], br[2], br[3],
               v0[sr->boneWtOff], v0[sr->boneWtOff+1], v0[sr->boneWtOff+2], v0[sr->boneWtOff+3]);
        if (hw0 >= 0 && hw0 < 302)
            printf("[skin] c[%d]: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", hw0,
                   g_vsc[hw0][0], g_vsc[hw0][1], g_vsc[hw0][2], g_vsc[hw0][3],
                   g_vsc[hw0+1][0], g_vsc[hw0+1][1], g_vsc[hw0+1][2], g_vsc[hw0+1][3],
                   g_vsc[hw0+2][0], g_vsc[hw0+2][1], g_vsc[hw0+2][2], g_vsc[hw0+2][3]);
    }
    for (uint32_t k = 0; k < indexCount; ++k) {
        uint32_t vi = indices ? (uint32_t)indices[k] + base : startVertex + k;
        const uint8_t* v = vbase + (size_t)vi * stride;
        tj::gfx::VertexPTC& o = g_meshVB[k];
        const float* p = (const float*)(v + sr->posOff);
        float px = p[0], py = p[1], pz = p[2];
        float nx = 0, ny = 0, nz = 0;
        const bool haveNorm = sr->normOff >= 0;
        if (haveNorm) { const float* np = (const float*)(v + sr->normOff); nx = np[0]; ny = np[1]; nz = np[2]; }
        if (skinned) {
            // CPU skinning from the live bone palette the game uploads to the vs constants:
            // each s16 in the vertex is a USER register (hw slot = reg+96) holding a 4x3
            // matrix as 3 dp4 row vec4s; weights are u8/255; 0xCDCD = unused slot.
            // The NORMAL skins through the same rows (3x3 part, no translation).
            float sx = 0, sy = 0, sz = 0, tw = 0;
            float mx = 0, my = 0, mz = 0;
            for (int b = 0; b < 4; ++b) {
                uint16_t raw = *(const uint16_t*)(v + sr->boneRegOff + 2 * b);
                uint8_t w8 = v[sr->boneWtOff + b];
                if (raw == 0xCDCD || !w8) continue;
                int hw = (int)(int16_t)raw + 96;
                if (hw < 0 || hw + 2 >= 304) continue;
                const float* r0 = g_vsc[hw], *r1 = g_vsc[hw + 1], *r2 = g_vsc[hw + 2];
                float wgt = w8 * (1.0f / 255.0f); tw += wgt;
                if (r0[0] == 0 && r0[1] == 0 && r0[2] == 0 && r1[1] == 0) {   // not yet uploaded
                    sx += wgt * px; sy += wgt * py; sz += wgt * pz;           // identity fallback
                    mx += wgt * nx; my += wgt * ny; mz += wgt * nz;
                } else {
                    sx += wgt * (r0[0]*px + r0[1]*py + r0[2]*pz + r0[3]);
                    sy += wgt * (r1[0]*px + r1[1]*py + r1[2]*pz + r1[3]);
                    sz += wgt * (r2[0]*px + r2[1]*py + r2[2]*pz + r2[3]);
                    mx += wgt * (r0[0]*nx + r0[1]*ny + r0[2]*nz);
                    my += wgt * (r1[0]*nx + r1[1]*ny + r1[2]*nz);
                    mz += wgt * (r2[0]*nx + r2[1]*ny + r2[2]*nz);
                }
            }
            if (tw > 0.05f) {
                float inv = 1.0f / tw;
                px = sx * inv; py = sy * inv; pz = sz * inv;
                nx = mx * inv; ny = my * inv; nz = mz * inv;
            }
        }
        if (particle) {
            // Billboard: transform the shared center to clip space by c100..103 = WVP^T,
            // add the per-vertex corner offset scaled by c116.x/-c117.y, then pre-divide
            // (the draw uses an identity transform).
            float clip[4];
            for (int r = 0; r < 4; ++r) {
                const float* c = g_vsc[100 + r];
                clip[r] = c[0]*px + c[1]*py + c[2]*pz + c[3];
            }
            if (sr->uv2Off >= 0) {
                const float* corner = (const float*)(v + sr->uv2Off);
                clip[0] += corner[0] * g_vsc[116][0];
                clip[1] += corner[1] * -g_vsc[117][1];
            }
            float w = clip[3] != 0.0f ? clip[3] : 1.0f;
            o.x = clip[0] / w; o.y = clip[1] / w; o.z = clip[2] / w;
        } else {
            // The mesh microcode computes v0 MINUS c126 (c126 = the prop's bbox CENTER;
            // dynamic-prop matrices map center-relative coords to world).
            o.x = px - ox; o.y = py - oy; o.z = pz - oz;
        }
        if (sr->uvOff >= 0) { const float* uv = (const float*)(v + sr->uvOff); o.u = uv[0] * uScl; o.v = uv[1] * vScl; }
        else { o.u = 0.0f; o.v = 0.0f; }
        if (isShiny && g_shinyVB) {
            // Mirror this vertex into the shiny buffer with the CPU env UV
            // (reflect(eye-p, N) dotted with c108/c109, /e3 only when nonzero).
            tj::gfx::VertexPT2C& s = g_shinyVB[k];
            s.x = px - ox; s.y = py - oy; s.z = pz - oz;
            s.u0 = o.u; s.v0 = o.v;
            float vx = g_vsc[106][0] - s.x, vy = g_vsc[106][1] - s.y, vz = g_vsc[106][2] - s.z;
            float vl = vx*vx + vy*vy + vz*vz;   // normalize (see PB path note above)
            if (vl > 1e-12f) { float inv = 1.0f / sqrtf(vl); vx *= inv; vy *= inv; vz *= inv; }
            float ndv = nx*vx + ny*vy + nz*vz;
            float rx = 2.0f*ndv*nx - vx, ry = 2.0f*ndv*ny - vy, rz = 2.0f*ndv*nz - vz;
            float e0 = rx*g_vsc[108][0] + ry*g_vsc[108][1] + rz*g_vsc[108][2];
            float e1 = rx*g_vsc[109][0] + ry*g_vsc[109][1] + rz*g_vsc[109][2];
            float e3 = rx*g_vsc[111][0] + ry*g_vsc[111][1] + rz*g_vsc[111][2];
            if (e3 > 1e-6f || e3 < -1e-6f) { e0 /= e3; e1 /= e3; }
            s.u1 = e0 * 0.5f + 0.5f; s.v1 = e1 * 0.5f + 0.5f;
        }
        if (skinned && haveNorm) {
            // Skinning-technique vertex lighting (microcode @tech5+0x110):
            //   oD0.rgb = max(dp3(N,c136),0)*c137 + max(dp3(N,c138),0)*c139 + c135
            // This is what shades characters (dark away-facing edges = the "cel" look) and
            // carries status tints (anger-red rides the light/ambient colors). If the light
            // constants were never uploaded (all zero), keep fullbright.
            const float* L1 = g_vsc[136]; const float* C1 = g_vsc[137];
            const float* L2 = g_vsc[138]; const float* C2 = g_vsc[139];
            const float* AMB = g_vsc[135];
            bool lit = C1[0] != 0 || C1[1] != 0 || C1[2] != 0 || AMB[0] != 0 || AMB[1] != 0 ||
                       AMB[2] != 0 || C2[0] != 0 || C2[1] != 0 || C2[2] != 0;
            if (lit) {
                float d1 = nx*L1[0] + ny*L1[1] + nz*L1[2]; if (d1 < 0) d1 = 0;
                float d2 = nx*L2[0] + ny*L2[1] + nz*L2[2]; if (d2 < 0) d2 = 0;
                float cr = d1*C1[0] + d2*C2[0] + AMB[0];
                float cg = d1*C1[1] + d2*C2[1] + AMB[1];
                float cb = d1*C1[2] + d2*C2[2] + AMB[2];
                auto q = [](float f) -> uint32_t { int i = (int)(f * 255.0f + 0.5f); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); };
                o.color = 0xFF000000u | (q(cr) << 16) | (q(cg) << 8) | q(cb);
            } else o.color = 0xFFFFFFFFu;
        } else {
            o.color = sr->colorOff >= 0 ? *(const uint32_t*)(v + sr->colorOff) : 0xFFFFFFFFu;
            // Technique 3 (the "flicker" variant the game switches parts to while dimming):
            // vertex ALPHA is a BRIGHTNESS factor (normal = 0xFE), not translucency.
            static uint32_t tech3fn = 0;
            if (!tech3fn) tech3fn = *(uint32_t*)(uintptr_t)(0x17c970 + 3*0x390 + 0x110);
            if (sr->fn == tech3fn && sr->colorOff >= 0) {
                uint32_t c = o.color;
                uint32_t a = c >> 24;
                uint32_t f = a >= 127 ? 256 : a * 2;    // 0xFE -> full, dimmer below
                uint32_t rr = ((c >> 16 & 0xFF) * f) >> 8, gg = ((c >> 8 & 0xFF) * f) >> 8, bb = ((c & 0xFF) * f) >> 8;
                o.color = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
            }
        }
        if (isShiny && g_shinyVB) g_shinyVB[k].color = o.color;
    }
    int ni = TriangulateToList(prim, indexCount, g_meshIB, kMeshMaxI);
    if (ni < 3) return;
    // c100..103 hold transpose(WVP) (columns); rebuild the row-major WVP for the shader.
    // Particle vertices are already clip-space-divided -> identity transform.
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (!particle)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[r * 4 + c] = g_vsc[100 + c][r];
    // Honor the game's render-state shadows (kept up to date by inline game code even with
    // the D3D entries patched): COLOR_MASK @0xA65CC == 0 means a stencil-shadow-volume
    // pass -- without stencil emulation those draws must be SKIPPED entirely (they painted
    // the "floating gray slab" artifacts). Z-write @0xA65C0 per material. Blending: the
    // game keeps ALPHABLENDENABLE on with (SRC_ALPHA, INV_SRC_ALPHA) for every shipped
    // material -- standard alpha is the single correct mode.
    if (*(volatile uint32_t*)(uintptr_t)0xA65CC == 0) {
        if (dlog) printf("[dlog] f=%d DROP cmask prim=%u n=%u fn=%08x\n", g_frame, prim, indexCount, sr->fn);
        return; }
    uint32_t zwrite = *(volatile uint32_t*)(uintptr_t)0xA65C0;
    // GL_ONE dst factor (@0xA65BC) = ADDITIVE blending (flame cards, glows).
    uint32_t dstB   = *(volatile uint32_t*)(uintptr_t)0xA65BC;
    // Stage-0 UV address modes from the deferred texture-state shadows (Xbox
    // D3DTADDRESS: 3 = CLAMP, 1 = WRAP, 2 = MIRROR->wrap approximation).
    uint32_t au = *(volatile uint32_t*)(uintptr_t)0xA62C0;
    uint32_t av = *(volatile uint32_t*)(uintptr_t)0xA62C4;
    ++g_drawCalls; ++g_dbg3dDraws;
    g_dev.SetTransform(m);
    g_dev.SetBlendMode(dstB == 1 ? tj::gfx::Device::BLEND_ADD : tj::gfx::Device::BLEND_ALPHA,
                       zwrite != 0);
    g_dev.SetUvClamp(au == 3, av == 3);
    if (isShiny && g_shinyVB && g_stageTexRes[2]) {
        // 4-stage sheen draw; fall back to the plain textured draw if the extra stage
        // textures don't resolve (never drop the floor).
        tj::gfx::TextureHandle t1 = ResolveTexture(g_stageTexRes[1]);
        tj::gfx::TextureHandle t2 = ResolveTexture(g_stageTexRes[2]);
        tj::gfx::TextureHandle t3 = ResolveTexture(g_stageTexRes[3]);
        if (t2 >= 0) {
            static int shinyLog = -1;
            if (shinyLog < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_SHINYLOG");
                                shinyLog = e && atoi(e) ? 1 : 0; free(e); }
            if (shinyLog && g_frame % 60 == 0) {
                float e0min = 1e9f, e0max = -1e9f, e1min = 1e9f, e1max = -1e9f;
                for (uint32_t k = 0; k < indexCount; ++k) {
                    if (g_shinyVB[k].u1 < e0min) e0min = g_shinyVB[k].u1;
                    if (g_shinyVB[k].u1 > e0max) e0max = g_shinyVB[k].u1;
                    if (g_shinyVB[k].v1 < e1min) e1min = g_shinyVB[k].v1;
                    if (g_shinyVB[k].v1 > e1max) e1max = g_shinyVB[k].v1;
                }
                printf("[shiny] BK f=%d n=%u res=%p/%p/%p/%p env=(%.2f..%.2f, %.2f..%.2f) c106=(%.1f,%.1f,%.1f) c126=(%.1f,%.1f,%.1f)\n",
                       g_frame, indexCount, g_stageTexRes[0], g_stageTexRes[1],
                       g_stageTexRes[2], g_stageTexRes[3], e0min, e0max, e1min, e1max,
                       g_vsc[106][0], g_vsc[106][1], g_vsc[106][2],
                       g_vsc[126][0], g_vsc[126][1], g_vsc[126][2]);
            }
            g_dev.DrawShinyIndexed(g_shinyVB, (int)indexCount, g_meshIB, ni,
                                   tex3d, t1 >= 0 ? t1 : tex3d, t2, t3 >= 0 ? t3 : t2);
            return;
        }
    }
    g_dev.SetTexture(tex3d);
    g_dev.DrawIndexed(g_meshVB, (int)indexCount, g_meshIB, ni);
}

// ===========================================================================
// Precompiled world pushbuffer: D3DDevice_RunPushBuffer(pPushBuffer, pFixup) ret 8.
// The level's static geometry is an OFFLINE-authored NV097 method stream inside the XMF's
// FXMF contiguous segment; each frame FUN_00083300 records fixups (per-chunk jump dwords
// for visibility + constant re-uploads) and FUN_00083860 runs it. We (a) apply the fixup
// records to the PB memory, then (b) interpret the method stream, extracting draws:
// vertex layout from SET_VERTEX_DATA_ARRAY_FORMAT, geometry via ARRAY_ELEMENT16/32 and
// DRAW_ARRAYS between SET_BEGIN_END pairs, transform via the same c100..103 constant
// store the immediate path uses (SET_TRANSFORM_CONSTANT routes into g_vsc), textures via
// SET_TEXTURE_OFFSET/FORMAT/PALETTE addresses (28-bit masked FXMF pointers).
// ===========================================================================
// CONTENT-KEYED pushbuffer texture cache (session 12 perf fix for the user-reported
// intermittent slow-motion). The sea/flipbook animations do NOT cycle a small set of
// texture offsets -- measurement showed texOff is a MARCHING POINTER that never repeats,
// so an offset-keyed cache misses every frame at ANY capacity (2048 entries filled inside
// one Beach match, still ~2900 creates/400f, 19-23 ms/f). But the PIXEL CONTENT loops.
// So the cache is keyed on a hash of the SOURCE BYTES (+ format/dims/palette): a new
// offset holding content we already decoded reuses that texture with no decode, no
// upload, no allocation. After one animation loop the water is free.
//   slots OWN the textures;  g_pbContent: contentKey -> slot  (authoritative)
//   g_pbAlias: texOff -> contentKey  (fast path for STATIC geometry: skips even hashing)
// A dangling alias (its slot was evicted/rekeyed) simply fails validation and re-hashes.
struct PbTexCacheEnt { uint64_t key; uint32_t fmtdw, pal; tj::gfx::TextureHandle h; uint32_t lastUse; };
static PbTexCacheEnt g_pbTexCache[2048]; static int g_pbTexCacheN = 0;
static std::unordered_map<uint64_t, int> g_pbContent;
static std::unordered_map<uint32_t, uint64_t> g_pbAlias;
static uint32_t g_frmPbDedup = 0;      // per-frame content-dedup hits (log: dedup=)

static tj::gfx::TextureHandle ResolvePbTexture(uint32_t texOff, uint32_t texFmt, uint32_t palOff) {
    PhaseTimer _pt(&g_tTex);
    if (!texOff || !texFmt) return tj::gfx::kNoTexture;
    auto touch = [&](int i) { g_pbTexCache[i].lastUse = (uint32_t)g_frame; return g_pbTexCache[i].h; };
    // (1) alias fast path -- stable texOffs (all static level geometry) never hash.
    { auto a = g_pbAlias.find(texOff);
      if (a != g_pbAlias.end()) {
          auto c = g_pbContent.find(a->second);
          if (c != g_pbContent.end()) {
              int i = c->second;
              if (g_pbTexCache[i].key == a->second && g_pbTexCache[i].fmtdw == texFmt &&
                  g_pbTexCache[i].pal == palOff)
                  return touch(i);
          }
      } }
    int fmt = (texFmt >> 8) & 0xFF;
    int w = 1 << ((texFmt >> 20) & 0xF), h = 1 << ((texFmt >> 24) & 0xF);
    if (w < 1 || w > 2048 || h < 1 || h > 2048) return tj::gfx::kNoTexture;
    const uint8_t* pix = GameMem(texOff);
    uint32_t srcBytes = SrcPixelBytes(fmt, w, h);
    if (!IsReadable((uint32_t)(uintptr_t)pix, srcBytes)) return tj::gfx::kNoTexture;
    const uint32_t* pal = nullptr;
    if (fmt == 0x0B && palOff) {
        const uint8_t* pp = GameMem(palOff);
        if (IsReadable((uint32_t)(uintptr_t)pp, 256 * 4)) pal = (const uint32_t*)pp;
    }
    // (2) content key: source bytes + format/dims + palette bytes (a P8 surface decodes
    // differently under a different palette, so the palette must be part of the identity).
    uint64_t key = HashSrc(pix, srcBytes);
    key ^= (uint64_t)texFmt * 1099511628211ull;
    if (pal) key ^= HashSrc((const uint8_t*)pal, 256 * 4) * 3ull;
    { auto c = g_pbContent.find(key);
      if (c != g_pbContent.end()) {
          int i = c->second;
          if (g_pbTexCache[i].key == key && g_pbTexCache[i].fmtdw == texFmt) {
              if (g_pbAlias.size() < 16384) g_pbAlias[texOff] = key;   // marching offsets: bounded
              else { g_pbAlias.clear(); g_pbAlias[texOff] = key; }     // pure optimization, safe to drop
              ++g_frmPbDedup;
              return touch(i);                                          // SAME CONTENT: no decode
          }
      } }
    tj::gfx::TextureHandle handle = tj::gfx::kNoTexture;
    uint32_t* pbBuf = nullptr;
    if (DecodeScratch(fmt, pix, (size_t)w * h * 4, pal, w, h, &pbBuf)) {
        ++g_frmTexCreate;
        PhaseTimer _pu(&g_tTexUpl);
        handle = g_dev.AcquireTexture(pbBuf, w, h);
    } else {
        static uint32_t warned[128]; static int warnedN = 0;
        bool seen = false;
        for (int i = 0; i < warnedN; ++i) if (warned[i] == texOff) { seen = true; break; }
        if (!seen && warnedN < 128) { warned[warnedN++] = texOff;
            printf("[d3d8] PbTexture DECODE-FAIL off=%08x fmt=%02x %dx%d pal=%08x\n",
                   texOff, fmt, w, h, palOff); }
    }
    int slot;
    if (g_pbTexCacheN < 2048) slot = g_pbTexCacheN++;
    else {
        ++g_texEvict;                            // same thrash canary as the resource cache
        slot = 0;                                // evict least-recently-used
        for (int i = 1; i < g_pbTexCacheN; ++i)
            if (g_pbTexCache[i].lastUse < g_pbTexCache[slot].lastUse) slot = i;
        if (g_pbTexCache[slot].h >= 0) g_dev.ReleaseTexture(g_pbTexCache[slot].h);
        g_pbContent.erase(g_pbTexCache[slot].key);
    }
    g_pbTexCache[slot] = { key, texFmt, palOff, handle, (uint32_t)g_frame };
    g_pbContent[key] = slot;
    if (g_pbAlias.size() >= 16384) g_pbAlias.clear();
    g_pbAlias[texOff] = key;
    return handle;
}

static void __stdcall Br_RunPushBuffer(uint8_t* pb, uint8_t* fixup) {
    PhaseTimer _pt(&g_tPb);
    if (!pb || !g_devReady) return;
    uint32_t dataVA = *(uint32_t*)(pb + 4);
    uint32_t size   = *(uint32_t*)(pb + 0xC);
    if (!dataVA || size < 8 || !IsReadable(dataVA, size)) return;
    uint8_t* data = (uint8_t*)(uintptr_t)dataVA;
    // (a) apply this frame's fixup records {DWORD size; DWORD pbOffset; BYTE payload[size]}
    if (fixup) {
        uint32_t fdata = *(uint32_t*)(fixup + 4), fseg = *(uint32_t*)(fixup + 0xC);
        uint8_t* f = (uint8_t*)(uintptr_t)(fdata + fseg);
        for (int guard = 0; guard < 100000; ++guard) {
            uint32_t rsz = *(uint32_t*)f;
            if (rsz == 0xFFFFFFFF) break;
            uint32_t off = *(uint32_t*)(f + 4);
            if (rsz > 0x10000 || off + rsz > size) break;   // malformed -- bail
            memcpy(data + off, f + 8, rsz);
            f += 8 + rsz;
        }
    }
    // (b) interpret
    uint32_t base28 = dataVA & 0x0FFFFFFF;
    // Stream + texture state PERSISTS across Br_RunPushBuffer calls (statics): the
    // damaged-floor Shiny chunk relies on stage bindings left over from the healthy
    // chunk's stream tail (offline-authored state reuse).
    static uint32_t streamAddr[16] = {}, streamFmt[16] = {};
    static uint32_t texOffs[4] = {}, texFmts[4] = {}, palOffs[4] = {};
    // SET_TRANSFORM_PROGRAM_START (0x1EA0): nonzero start = an alternate technique's
    // microcode slot; in shipped data that is always slot 24 = the Shiny floor VS.
    static uint32_t progStart = 0;
    uint32_t prim = 0, zwrite = 1, dstFactor = 0x303;   // GL_ONE_MINUS_SRC_ALPHA default
    static uint16_t* idx = nullptr;
    if (!idx) idx = (uint16_t*)VirtualAlloc(nullptr, sizeof(uint16_t) * kMeshMaxV, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint32_t nIdx = 0;
    uint32_t constDw = 0;                                 // float cursor into g_vsc
    float* cbase = &g_vsc[0][0];
    uint32_t pos = 0, end = size - 4;
    auto flushDraw = [&]() {
        uint32_t texOff = texOffs[0], texFmt = texFmts[0], palOff = palOffs[0];
        if (!prim || nIdx < 3) { nIdx = 0; return; }
        // classify attributes by format: pos = first F32x3, normal = SECOND F32x3,
        // uv = first F32x2, color = first UB_D3D x4
        int posA = -1, uvA = -1, colA = -1, nrmA = -1;
        for (int i = 0; i < 16; ++i) {
            uint32_t f = streamFmt[i];
            uint32_t type = f & 0xF, comps = (f >> 4) & 0xF;
            if (!comps && type != 0) continue;
            if (type == 2 && comps == 3) { if (posA < 0) posA = i; else if (nrmA < 0) nrmA = i; }
            else if (type == 2 && comps == 2 && uvA < 0) uvA = i;
            else if (type == 0 && comps == 4 && colA < 0) colA = i;
        }
        if (posA < 0 || !streamAddr[posA]) { nIdx = 0; return; }
        uint32_t stride = (streamFmt[posA] >> 8) & 0xFF;
        if (!stride) { nIdx = 0; return; }
        const uint8_t* pbase = GameMem(streamAddr[posA]);
        const uint8_t* ubase = uvA  >= 0 && streamAddr[uvA]  ? GameMem(streamAddr[uvA])  : nullptr;
        const uint8_t* colb  = colA >= 0 && streamAddr[colA] ? GameMem(streamAddr[colA]) : nullptr;
        const uint8_t* nbase = nrmA >= 0 && streamAddr[nrmA] ? GameMem(streamAddr[nrmA]) : nullptr;
        uint32_t uStride = uvA  >= 0 ? (streamFmt[uvA]  >> 8) & 0xFF : 0;
        uint32_t cStride = colA >= 0 ? (streamFmt[colA] >> 8) & 0xFF : 0;
        uint32_t nStride = nrmA >= 0 ? (streamFmt[nrmA] >> 8) & 0xFF : 0;
        uint32_t maxVi = 0;
        for (uint32_t k = 0; k < nIdx; ++k) if (idx[k] > maxVi) maxVi = idx[k];
        if (!IsReadable((uint32_t)(uintptr_t)pbase, (maxVi + 1) * stride)) { nIdx = 0; return; }
        if (!g_meshVB) return;
        // c126 = per-chunk bbox CENTER: the PB vertex shader computes v0 MINUS c126 before
        // the WVP (vertices are baked level-space; a dynamic prop's matrix maps
        // center-relative coords to world). Static chunks load 0; DYNAMIC chunks
        // (animating props like the fridge door) get their live center via the frame's
        // fixups. Read at flush time: the chunk's own 0x1EA4/0xB80 loads have already
        // routed into g_vsc by now. (Adding instead of subtracting rendered the door at
        // 2x its center offset -- the "flying door".)
        const float ox = g_vsc[126][0], oy = g_vsc[126][1], oz = g_vsc[126][2];
        static int pbLog = -1;
        if (pbLog < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_PBLOG"); pbLog = e && atoi(e) ? 8 : 0; free(e); }
        if (pbLog > 0 && (ox != 0 || oy != 0 || oz != 0)) { --pbLog;
            const float* v0p = (const float*)pbase;
            printf("[pb] dyn chunk: c126=(%.2f,%.2f,%.2f) c100.row3=(%.2f,%.2f,%.2f,%.2f) v0=(%.2f,%.2f,%.2f) n=%u\n",
                   ox, oy, oz, g_vsc[100][3], g_vsc[101][3], g_vsc[102][3], g_vsc[103][3],
                   v0p[0], v0p[1], v0p[2], nIdx); }
        // SHINY floor chunk (PROGRAM_START selected the slot-24 VS): compute the env UV
        // on the CPU exactly like the microcode @0x17b1f8 -- p' = v0 - c126;
        // V = c106.xyz - p'; R = 2*dot(N,V)*N - V (unnormalized); e_i = dot(R, c[108+i]);
        // envUV = (e0, e1), divided by e3 only when |e3| > 1e-6 (static chunks upload
        // identity c108..111, giving e3 = 0 = no divide). Draw with the 4-stage combiner;
        // if the stage textures are missing, fall through to the plain base draw (never
        // drop the floor).
        if (progStart != 0 && nbase && texOffs[2] && texFmts[2]) {
            static tj::gfx::VertexPT2C* shinyVB = nullptr;
            if (!shinyVB) shinyVB = (tj::gfx::VertexPT2C*)VirtualAlloc(nullptr,
                    sizeof(tj::gfx::VertexPT2C) * kMeshMaxV, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            tj::gfx::TextureHandle t0 = ResolvePbTexture(texOffs[0], texFmts[0], palOffs[0]);
            tj::gfx::TextureHandle t1 = ResolvePbTexture(texOffs[1], texFmts[1], palOffs[1]);
            tj::gfx::TextureHandle t2 = ResolvePbTexture(texOffs[2], texFmts[2], palOffs[2]);
            tj::gfx::TextureHandle t3 = ResolvePbTexture(texOffs[3], texFmts[3], palOffs[3]);
            if (shinyVB && t0 >= 0 && t2 >= 0) {
                static int shinyLog = -1;
                if (shinyLog < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_SHINYLOG");
                                    shinyLog = e && atoi(e) ? 1 : 0; free(e); }
                float e0min = 1e9f, e0max = -1e9f, e1min = 1e9f, e1max = -1e9f;
                const float* eye = g_vsc[106];
                for (uint32_t k = 0; k < nIdx; ++k) {
                    uint32_t vi = idx[k];
                    tj::gfx::VertexPT2C& o = shinyVB[k];
                    const float* p = (const float*)(pbase + (size_t)vi * stride);
                    o.x = p[0] - ox; o.y = p[1] - oy; o.z = p[2] - oz;
                    if (ubase) { const float* uv = (const float*)(ubase + (size_t)vi * uStride); o.u0 = uv[0]; o.v0 = uv[1]; }
                    else { o.u0 = o.v0 = 0.0f; }
                    o.color = colb ? *(const uint32_t*)(colb + (size_t)vi * cStride) : 0xFFFFFFFFu;
                    const float* nrm = (const float*)(nbase + (size_t)vi * nStride);
                    float vx = eye[0] - o.x, vy = eye[1] - o.y, vz = eye[2] - o.z;
                    // Normalize the eye->vertex vector: the reflection is an environment-
                    // map direction, so its texcoords must be a unit-sphere projection.
                    // (Using the raw world-scale vector tiled the caustic texture ~150x
                    // across the floor = the "random lines" artifact; xemu shows a smooth
                    // broad sheen.) The env matrix (c108..111 = identity for the static
                    // floor) then leaves envUV = reflected direction .xy in [-1,1],
                    // mapped to [0,1] for the texture lookup.
                    float vl = vx*vx + vy*vy + vz*vz;
                    if (vl > 1e-12f) { float inv = 1.0f / sqrtf(vl); vx *= inv; vy *= inv; vz *= inv; }
                    float ndv = nrm[0]*vx + nrm[1]*vy + nrm[2]*vz;
                    float rx = 2.0f*ndv*nrm[0] - vx, ry = 2.0f*ndv*nrm[1] - vy, rz = 2.0f*ndv*nrm[2] - vz;
                    float e0 = rx*g_vsc[108][0] + ry*g_vsc[108][1] + rz*g_vsc[108][2];
                    float e1 = rx*g_vsc[109][0] + ry*g_vsc[109][1] + rz*g_vsc[109][2];
                    float e3 = rx*g_vsc[111][0] + ry*g_vsc[111][1] + rz*g_vsc[111][2];
                    if (e3 > 1e-6f || e3 < -1e-6f) { e0 /= e3; e1 /= e3; }
                    o.u1 = e0 * 0.5f + 0.5f; o.v1 = e1 * 0.5f + 0.5f;
                    if (e0 < e0min) e0min = e0; if (e0 > e0max) e0max = e0;
                    if (e1 < e1min) e1min = e1; if (e1 > e1max) e1max = e1;
                }
                if (shinyLog && g_frame % 60 == 0)
                    printf("[shiny] PB f=%d n=%u tex=%08x/%08x/%08x/%08x fmt=%08x/%08x/%08x/%08x env=(%.2f..%.2f, %.2f..%.2f) eye=(%.1f,%.1f,%.1f)\n",
                           g_frame, nIdx, texOffs[0], texOffs[1], texOffs[2], texOffs[3],
                           texFmts[0], texFmts[1], texFmts[2], texFmts[3],
                           e0min, e0max, e1min, e1max, eye[0], eye[1], eye[2]);
                uint32_t nv2 = nIdx;
                int ni2 = TriangulateToList(prim, nv2, g_meshIB, kMeshMaxI);
                nIdx = 0;
                if (ni2 < 3) return;
                float m2[16];
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) m2[r * 4 + c] = g_vsc[100 + c][r];
                ++g_dbg3dDraws;
                g_dev.SetTransform(m2);
                g_dev.SetBlendMode(dstFactor == 1 ? tj::gfx::Device::BLEND_ADD : tj::gfx::Device::BLEND_ALPHA,
                                   zwrite != 0);
                g_dev.SetUvClamp(false, false);      // shiny stages are WRAP (0x00010101)
                g_dev.DrawShinyIndexed(shinyVB, (int)nv2, g_meshIB, ni2, t0, t1, t2, t3);
                return;
            }
        }
        for (uint32_t k = 0; k < nIdx; ++k) {
            uint32_t vi = idx[k];
            tj::gfx::VertexPTC& o = g_meshVB[k];
            const float* p = (const float*)(pbase + (size_t)vi * stride);
            o.x = p[0] - ox; o.y = p[1] - oy; o.z = p[2] - oz;
            if (ubase) { const float* uv = (const float*)(ubase + (size_t)vi * uStride); o.u = uv[0]; o.v = uv[1]; }
            else { o.u = o.v = 0.0f; }
            o.color = colb ? *(const uint32_t*)(colb + (size_t)vi * cStride) : 0xFFFFFFFFu;
        }
        uint32_t nv = nIdx;
        int ni = TriangulateToList(prim, nv, g_meshIB, kMeshMaxI);
        nIdx = 0;
        if (ni < 3) return;
        float m[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[r * 4 + c] = g_vsc[100 + c][r];
        ++g_dbg3dDraws;
        g_dev.SetTransform(m);
        g_dev.SetBlendMode(dstFactor == 1 ? tj::gfx::Device::BLEND_ADD : tj::gfx::Device::BLEND_ALPHA,
                           zwrite != 0);
        g_dev.SetTexture(ResolvePbTexture(texOff, texFmt, palOff));
        g_dev.DrawIndexed(g_meshVB, (int)nv, g_meshIB, ni);
    };
    // ensure the shared mesh scratch exists
    if (!g_meshVB) {
        g_meshVB = (tj::gfx::VertexPTC*)VirtualAlloc(nullptr, sizeof(tj::gfx::VertexPTC) * kMeshMaxV, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_meshIB = (uint16_t*)VirtualAlloc(nullptr, sizeof(uint16_t) * kMeshMaxI, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_meshVB || !g_meshIB) return;
    }
    for (int guard = 0; guard < 4000000 && pos + 4 <= end; ++guard) {
        uint32_t dw = *(uint32_t*)(data + pos);
        if (dw & 1) {                                     // jump (28-bit target | 1)
            uint32_t target = (dw & 0x0FFFFFFE);
            if (target < base28 || target - base28 >= end) break;
            pos = target - base28;
            continue;
        }
        uint32_t count = (dw >> 18) & 0x7FF;
        uint32_t method = dw & 0x1FFC;
        bool noinc = (dw & 0x40000000) != 0;
        const uint32_t* pay = (const uint32_t*)(data + pos + 4);
        if (pos + 4 + 4 * count > end) break;
        if (dw == 0) break;                               // malformed
        switch (method) {
        case 0x1EA4: if (count >= 1) constDw = pay[0] * 4; break;
        case 0x0B80:
            for (uint32_t k = 0; k < count && constDw < 304 * 4; ++k)
                cbase[constDw++] = ((const float*)pay)[k];
            break;
        case 0x17FC:
            if (count >= 1) { if (pay[0]) { flushDraw(); prim = pay[0]; nIdx = 0; } else flushDraw(); }
            break;
        case 0x1800:                                      // ARRAY_ELEMENT16: 2 indices/dword
            for (uint32_t k = 0; k < count && nIdx + 2 <= (uint32_t)kMeshMaxV; ++k) {
                idx[nIdx++] = (uint16_t)(pay[k] & 0xFFFF);
                idx[nIdx++] = (uint16_t)(pay[k] >> 16);
            }
            break;
        case 0x1808:                                      // ARRAY_ELEMENT32
            for (uint32_t k = 0; k < count && nIdx < (uint32_t)kMeshMaxV; ++k)
                idx[nIdx++] = (uint16_t)pay[k];
            break;
        case 0x1810:                                      // DRAW_ARRAYS: ((n-1)<<24)|first
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t first = pay[k] & 0xFFFFFF, n = (pay[k] >> 24) + 1;
                for (uint32_t v = 0; v < n && nIdx < (uint32_t)kMeshMaxV; ++v)
                    idx[nIdx++] = (uint16_t)(first + v);
            }
            break;
        case 0x035C: if (count >= 1) zwrite = pay[0]; break;      // SET_DEPTH_MASK
        case 0x0348: if (count >= 1) dstFactor = pay[0]; break;   // SET_BLEND_FUNC_DFACTOR
        default:
            if (!noinc && count && method >= 0x1720 && method < 0x1760) {        // stream offsets
                for (uint32_t k = 0; k < count; ++k) {
                    uint32_t i = (method - 0x1720) / 4 + k; if (i < 16) streamAddr[i] = pay[k];
                }
            } else if (!noinc && count && method >= 0x1760 && method < 0x17A0) { // stream formats
                for (uint32_t k = 0; k < count; ++k) {
                    uint32_t i = (method - 0x1760) / 4 + k; if (i < 16) streamFmt[i] = pay[k];
                }
            } else if (method >= 0x1B00 && method < 0x1C00) {
                // per-stage texture state, stride 0x40: +0 OFFSET (+FORMAT when the run
                // continues), +4 FORMAT, +0x20 PALETTE. Stages 1-3 are the Shiny floor's
                // mask/env layers.
                uint32_t stage = (method - 0x1B00) >> 6, sub = (method - 0x1B00) & 0x3F;
                if (stage < 4 && count >= 1) {
                    if (sub == 0x00) { texOffs[stage] = pay[0]; if (count >= 2) texFmts[stage] = pay[1]; }
                    else if (sub == 0x04) texFmts[stage] = pay[0];
                    else if (sub == 0x20) palOffs[stage] = pay[0];
                }
            }
            else if (method == 0x1EA0 && count >= 1) progStart = pay[0];  // TRANSFORM_PROGRAM_START
            break;                                        // everything else: skip by count
        }
        pos += 4 + 4 * count;
    }
    flushDraw();
}

// D3DDevice_DrawVertices(PrimitiveType, StartVertex, VertexCount) ret 0xC.
static void __stdcall Br_DrawVertices(uint32_t prim, uint32_t startVertex, uint32_t count) {
    Draw3D(prim, nullptr, count, startVertex, nullptr, 0);
}
// D3DDevice_DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData) ret 0xC --
// pIndexData is a raw CPU pointer into the bound IB's data (game computes [0xA62B4]+off).
static void __stdcall Br_DrawIndexedVertices(uint32_t prim, uint32_t count, const uint16_t* pIndexData) {
    if (pIndexData) Draw3D(prim, pIndexData, count, 0, nullptr, 0);
}
// D3DDevice_DrawIndexedVerticesUP(Prim, VertexCount, pIndexData, pVertexData, Stride) ret 0x14
// -- the XMF UP path passes pVertexData = VB->Data | 0x80000000 (phys alias); mask it.
static void __stdcall Br_DrawIndexedVerticesUP(uint32_t prim, uint32_t count, const uint16_t* pIndexData,
                                               const void* pVerts, uint32_t stride) {
    if (!pIndexData || !pVerts) return;
    Draw3D(prim, pIndexData, count, 0, GameMem((uint32_t)(uintptr_t)pVerts), stride);
}

// D3DDevice_DrawVerticesUP(PrimitiveType, VertexCount, pVertexData, VertexStride) ret 0x10.
// The menu draws textured quads: stride 20 = float3 POS + float2 UV. PrimitiveType is the
// NV2A enum: 5=TRILIST 6=TRISTRIP 7=TRIFAN 8=QUADLIST 9=QUADSTRIP 10=POLYGON. FVF path
// (SetVertexShader handle even) = XYZRHW screen-space. Non-UI created shaders (particles,
// 3D content) route through the mesh core. Triangulate to a list via tj::gfx.
static void __stdcall Br_DrawVerticesUP(uint32_t primType, uint32_t vtxCount, const void* pData, uint32_t stride) {
    if (!g_devReady || !pData || vtxCount < 3) return;
    const bool fvf = (g_curVS & 1) == 0;   // even handle => FVF (fixed-function screen-space)
    const ShaderRec* sr = fvf ? nullptr : FindShader(g_curVS);
    if (sr && !IsUiShader(sr)) {           // particles / 3D content drawn UP -> mesh core
        Draw3D(primType, nullptr, vtxCount, 0, (const uint8_t*)pData, stride);
        return;
    }
    static tj::gfx::VertexPTC vb[1024];
    static uint16_t ib[2048];
    if (vtxCount > 1024) return;
    // HUDFlat (@0x17e980) is the UNTEXTURED 2D technique: the sprite flush FUN_000846e0
    // only rebinds SetTexture for sprites that HAVE a texture, so a HUDFlat sprite (e.g.
    // the fullscreen fade quad queued after text glyphs) draws with the PREVIOUS texture
    // still bound -- on Xbox its pixel shader ignores the texture, ours sampled it and
    // painted the FONT atlas fullscreen during every menu fade ("random letters" bug).
    // HUDFlat draws must be untextured: flat ps-c1 tint only.
    const bool flatUi = sr && sr->fn == *(uint32_t*)(uintptr_t)(0x17e980 + 0x110);
    // Resolve the texture BEFORE building vertices: linear textures use texel-space UVs
    // that must be normalized by the texture dimensions.
    tj::gfx::TextureHandle tex = (fvf || flatUi) ? tj::gfx::kNoTexture : ResolveTexture(g_curTexRes);
    float uScl = 1.0f, vScl = 1.0f;
    if (!fvf && g_lastTexLinear) { uScl = 1.0f / (float)g_lastTexW; vScl = 1.0f / (float)g_lastTexH; }
    const uint8_t* base = (const uint8_t*)pData;
    // WIDESCREEN ([0x16A254]): the game's HUD ortho squeezes 2D x0.75 into the
    // anamorphic frame; the net hardware effect is a 4:3-proportioned HUD centered in
    // the wide frame. Our UI mapping replaces that ortho, so apply the same
    // center-anchored x-scale here -- EXCEPT for elements that span the full screen
    // width (fades, transition cards, backgrounds): those must keep covering the whole
    // window, so pre-scan the draw's x extent.
    float wsx = 1.0f;
    if (*(volatile uint8_t*)(uintptr_t)0x16A254) {
        float minX = 1e9f, maxX = -1e9f;
        for (uint32_t i = 0; i < vtxCount; ++i) {
            float x = *(const float*)(base + i * stride);
            if (fvf) x /= 640.0f;                      // FVF is in screen pixels
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
        }
        if (!(minX <= 0.01f && maxX >= 0.99f)) wsx = 0.75f;
    }
    for (uint32_t i = 0; i < vtxCount; ++i) {
        const uint8_t* v = base + i * stride;
        tj::gfx::VertexPTC& o = vb[i];
        if (fvf) {
            // The only FVF (0x44) draw in the game is the stencil-shadow RESOLVE quad --
            // a fullscreen darken drawn with depth func ALWAYS (0x207) against the stencil
            // mask. Without stencil emulation it would dim the whole screen: skip it.
            if (*(volatile uint32_t*)(uintptr_t)0xA65A4 == 0x207) return;
            // XYZRHW (x,y screen px, z, rhw) + DIFFUSE. Convert screen->NDC directly.
            const float* f = (const float*)v;
            float sw = 640.0f, sh = 480.0f;
            o.x = (f[0] / (sw * 0.5f) - 1.0f) * wsx;
            o.y = 1.0f - f[1] / (sh * 0.5f);
            o.z = f[2];
            o.u = 0; o.v = 0;
            o.color = *(const uint32_t*)(v + 16);
        } else {
            // float3 POS + float2 UV: a UI-classified shader (HUD/HUDFlat microcode) whose
            // transform is pos*2-1 via c105 -- the vertex z is garbage by design (never
            // written by the game; the microcode ignores it). Emit z=0. (3D-classified
            // shaders never reach here -- they route through Draw3D above.)
            const float* p = (const float*)v;
            o.x = (p[0] * 2.0f - 1.0f) * wsx; o.y = p[1] * 2.0f - 1.0f; o.z = 0.0f;
            o.u = p[3] * uScl; o.v = p[4] * vScl;
            o.color = PackTint(g_psc[1]);   // per-sprite RGBA tint the game wrote to ps c1
        }
    }
    int ni = TriangulateToList(primType, vtxCount, ib, 2048);
    if (ni < 3) return;
    // TEMP diagnostic (TJ_TRACEBG=1): log every small UI quad draw with the bound
    // resource header -- hunting the menu-select "font atlas as fullscreen bg" bug.
    static int traceBg = -1;
    if (traceBg < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_TRACEBG"); traceBg = (e && atoi(e)) ? 1 : 0; free(e); }
    if (traceBg && vtxCount <= 4 && !fvf) {
        uint32_t rd[5] = {0,0,0,0,0};
        if (g_curTexRes && IsReadable((uint32_t)(uintptr_t)g_curTexRes, 20)) memcpy(rd, g_curTexRes, 20);
        printf("[bgtrace] f=%d prim=%u n=%u stride=%u vs=%08x res=%p hdr=%08x/%08x/%08x/%08x/%08x lin=%d %dx%d v0=(%.3f,%.3f uv %.1f,%.1f) v3=(%.3f,%.3f uv %.1f,%.1f) tint=%.2f,%.2f,%.2f,%.2f\n",
               g_frame, primType, vtxCount, stride, g_curVS, g_curTexRes,
               rd[0], rd[1], rd[2], rd[3], rd[4], (int)g_lastTexLinear, g_lastTexW, g_lastTexH,
               ((const float*)base)[0], ((const float*)base)[1],
               ((const float*)base)[3], ((const float*)base)[4],
               ((const float*)(base + (vtxCount-1)*stride))[0], ((const float*)(base + (vtxCount-1)*stride))[1],
               ((const float*)(base + (vtxCount-1)*stride))[3], ((const float*)(base + (vtxCount-1)*stride))[4],
               g_psc[1][0], g_psc[1][1], g_psc[1][2], g_psc[1][3]);
    }
    ++g_drawCalls; if (fvf) ++g_dbgFvfDraws; else ++g_dbgUiDraws;
    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    g_dev.SetTransform(ident);
    g_dev.SetAlphaBlend(true);
    g_dev.SetDepthTest(false);
    g_dev.SetTexture(tex);
    g_dev.DrawIndexed(vb, (int)vtxCount, ib, ni);
}

// D3DTexture_LockRect(pTexture, Level, pLockedRect, pRect, Flags) ret 0x14.
// Return the resource's OWN pixel memory (its Data pointer, now valid in the contiguous
// pool) with the pitch the engine computes from the format -- so the game decodes each
// texture into its own correctly-sized buffer (a shared scratch overflowed the heap).
// D3DResource: +0x04 Data, +0x0C Format {byte+0xD fmt; USize@20; VSize@24}, +0x10 Size(linear).
static uint8_t* g_lockScratch = nullptr;
static int FmtBpp(int fmt) {
    switch (fmt) {
        case 0x0C: return 4;                 // DXT1
        case 0x0E: case 0x0F: return 8;      // DXT3/5
        case 0x00: case 0x01: case 0x0B: case 0x30: return 8;   // L8/AL8/P8/A8
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x11: case 0x1D: case 0x28: case 0x2C: case 0x2D: return 16;
        default: return 32;                  // A8R8G8B8/X8R8G8B8 etc.
    }
}
static void __stdcall Br_LockRect(void* tex, uint32_t level, void* lockedRect, void* rect, uint32_t flags) {
    (void)rect; (void)flags;
    uint32_t data = tex ? *(uint32_t*)((char*)tex + 4) : 0;
    uint32_t fmtd = tex ? *(uint32_t*)((char*)tex + 0xC) : 0;
    uint32_t sizef = tex ? *(uint32_t*)((char*)tex + 0x10) : 0;
    int fmt = (fmtd >> 8) & 0xFF, logw = (fmtd >> 20) & 0xF;
    int width = 1 << (logw > level ? logw - level : 0);
    int pitch;
    if (sizef) pitch = (int)(((sizef >> 24) + 1) << 6);          // linear surface
    else if (fmt == 0x0C) pitch = 2 * (width < 4 ? 4 : width);   // DXT1
    else if (fmt == 0x0E || fmt == 0x0F) pitch = 4 * (width < 4 ? 4 : width); // DXT3/5
    else pitch = width * FmtBpp(fmt) / 8;
    if (pitch < 1) pitch = width * 4;
    void* bits;
    if (data >= 0x04000000 && data < 0x10000000) bits = (void*)(uintptr_t)data;   // pool or low-arena memory
    else { if (!g_lockScratch) g_lockScratch = (uint8_t*)VirtualAlloc(nullptr, 8<<20, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE); bits = g_lockScratch; }
    if (lockedRect) { *(uint32_t*)lockedRect = (uint32_t)pitch;
                      *(uint32_t*)((char*)lockedRect + 4) = Gp32(bits); }   // D3DLOCKED_RECT.pBits (gptr)
}

// D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil) ret 0x18
static void __stdcall Br_Clear(uint32_t count, void* rects, uint32_t flags, uint32_t color,
        float z, uint32_t stencil) {
    (void)count; (void)rects;
    if (!g_devReady) return;
    g_dev.Clear(flags, color, z, (uint8_t)stencil);
}

// Called instead of the game's render while a netplay peer is stalled. The game's render
// must not run (it MUTATES simulation-visible state -- camera shake writes a float at
// master+0x1C7C4 -- so a peer that renders more frames than its opponent desyncs).
//
// It deliberately does NOT present. The first version did EndScene/Present/Clear/BeginScene
// and pushed a freshly CLEARED backbuffer to the screen every stalled frame, alternating a
// real frame with an empty blue one -- visible flicker in both windows (user-reported).
// Presenting nothing leaves the last completed frame on screen, which is exactly what a
// paused game should look like, and the window stays responsive because the message pump
// lives on its own thread. A brief sleep keeps a stalled peer from spinning a core.
// (When the "WAITING FOR <peer>" overlay lands it will re-draw and present that frame
// explicitly rather than resurrecting the clear-then-present pattern.)
extern "C" void BridgeStallPresent() {
    if (!g_devReady) return;
    Sleep(1);
}

// Screenshot keyed on something other than the local presented-frame counter. Two LAN peers
// reach the same simulation frame after a different number of presented frames, so
// TJ_CAPTURE cannot compare them -- net_sync calls this at an agreed LOCKSTEP frame instead
// (TJ_NETCAP), which is what makes "both windows render the same thing" checkable.
// The shot is TAKEN in Br_Swap, not here: at the point net_sync can name a lockstep frame
// the game's render has returned but the frontend's 2D batch has not been flushed, and the
// backbuffer still reads as an empty background.
static char g_pendingCap[600] = "";
extern "C" void BridgeCapture(const char* path) {
    strncpy_s(g_pendingCap, path, _TRUNCATE);
}

// Drop every cache entry keyed by a GAME RESOURCE ADDRESS. Called when the frontend mode
// changes, i.e. when a level (and its resources) has been torn down: the game reuses those
// same addresses for different textures afterwards, and an entry that outlives its resource
// silently becomes a per-frame re-decode of the wrong key. Handles are RELEASED rather than
// destroyed, so the pool serves the re-creates and the GLES textures stay alive app-side
// until an explicit destroy — the release is ordered in the same command ring as the draws
// that still reference them, so nothing in flight can see a freed texture.
static void DropResourceTextureCaches() {
    int n = 0;
    for (int i = 0; i < g_texCacheN; ++i) {
        if (g_texCache[i].h >= 0) { g_dev.ReleaseTexture(g_texCache[i].h); ++n; }
        g_texCache[i] = TexCacheEnt{};
        g_texCache[i].h = tj::gfx::kNoTexture;
    }
    g_texCacheN = 0;
    g_texIndex.clear();
    // The pushbuffer cache is CONTENT-keyed, so its textures stay valid across a teardown —
    // but g_pbAlias maps a pushbuffer OFFSET to a content key, and offsets are reused by the
    // next level with different content. It is documented as a pure optimization, so drop it.
    g_pbAlias.clear();
    if (n) printf("[d3d8] level teardown: dropped %d resource-keyed textures\n", n);
}

// engine_mode.cpp: guest instructions retired + gate calls, for the frame log's workload
// column. C linkage so this file needs no engine header (it must build the same either way).
extern "C" void BridgeEngineStats(unsigned long long* insn, unsigned long long* gate);
// engine_mode.cpp: JIT recompiles / flush-alls / stale rejections / declines (jit.h JitHealth).
extern "C" void BridgeJitHealth(unsigned long long* compiled, unsigned long long* flushes,
                                unsigned long long* stale, unsigned long long* declines);
// Direct-mapped block-cache collisions (jit.h JitCauses): a self-sustaining recompile
// regime, and the leading suspect for a match that goes slow and STAYS slow.
extern "C" void BridgeJitCauses(unsigned long long* conflict);

// D3DDevice_Swap(Flags) ret 4 -- the frame flip. Present + pump the window.
static void __stdcall Br_Swap(uint32_t flags) {
    (void)flags;
    ResetPushbuffer();      // recycle the scratch pushbuffer each frame (tokens are ignored)
    if (!g_devReady) return;
    // Diagnostic: dump a frame to a BMP when TJ_CAPTURE is set (frame number).
    ++g_frame;
    // STABILITY, not average speed. The 400-frame line below reports the MEAN, and a mean of
    // 16.7 ms is exactly what a leg that stutters twice a second also reports. A player feels
    // the OUTLIERS, so every frame is timed here and the window keeps that shape: how many
    // frames missed the 16.67 ms vsync budget, how many missed it badly, and the worst one.
    {
        static LARGE_INTEGER fqpf = {}, fprev = {};
        if (!fqpf.QuadPart) { QueryPerformanceFrequency(&fqpf); QueryPerformanceCounter(&fprev); }
        LARGE_INTEGER fnow; QueryPerformanceCounter(&fnow);
        double ms = (double)(fnow.QuadPart - fprev.QuadPart) * 1000.0 / (double)fqpf.QuadPart;
        fprev = fnow;
        if (ms > g_frmMax) g_frmMax = ms;
        if (ms > 17.5) ++g_frmOver17;      // missed the vsync budget at all
        if (ms > 20.0) ++g_frmOver20;      // a visible hitch
        if (ms > 33.0) ++g_frmOver33;      // a dropped frame anyone would see
        // ...and WHY. A count of hitches names no cause, so each spike prints its own phase
        // split, taken as deltas of the same cumulative counters the 400-frame line averages.
        // The window is swap-point to swap-point, so it covers the tail of the previous
        // Br_Swap (its Present, hence `swap`) plus all of this frame's work -- one whole frame
        // period, phase-shifted by the flip. `rest` is the sim, which is the interesting one:
        // a spike that is all `rest` is the guest, a spike with `tex` is asset streaming, and
        // a spike that is only `swap` is the compositor or the pacer, not us.
        static int spikeMs = -1;
        if (spikeMs < 0) { char* se = nullptr; size_t sn = 0; _dupenv_s(&se, &sn, "TJ_SPIKE");
                           spikeMs = se ? atoi(se) : 25; free(se); }
        static int64_t pPb = 0, pDraw = 0, pTex = 0, pSwap = 0;
        static uint32_t pCre = 0, pUpd = 0;
        // The 400-frame census below ZEROES these cumulative counters, and it runs later in
        // this same function -- so the frame after it would diff against a stale high water
        // and report negative phases. Any counter going backwards means that happened.
        if (g_tPb < pPb || g_tDraw < pDraw || g_tTex < pTex || g_tSwap < pSwap ||
            g_frmTexCreate < pCre || g_frmTexUpdate < pUpd) {
            pPb = pDraw = pTex = pSwap = 0; pCre = pUpd = 0;
        }
        if (spikeMs > 0 && ms > (double)spikeMs) {
            double k = 1000.0 / (double)fqpf.QuadPart;
            printf("[spike] frame %d: %.1fms [pb %.2f draw %.2f tex %.2f swap %.2f rest %.2f] "
                   "3d=%u ui=%u texcre=%u texupd=%u\n", g_frame, ms,
                   (double)(g_tPb - pPb) * k, (double)(g_tDraw - pDraw) * k,
                   (double)(g_tTex - pTex) * k, (double)(g_tSwap - pSwap) * k,
                   ms - (double)((g_tPb - pPb) + (g_tDraw - pDraw) + (g_tSwap - pSwap)) * k,
                   g_dbg3dDraws, g_dbgUiDraws,
                   g_frmTexCreate - pCre, g_frmTexUpdate - pUpd);
        }
        // ADPF: tell the scheduler what this frame COST IN WORK, so it can hold clocks where
        // the deadline is being met instead of inferring demand from utilisation. The pacer
        // wait is subtracted deliberately -- reporting idle time as work teaches it the
        // opposite of the truth. Off unless TJ_ADPF=1; a device without the hint HAL no-ops.
        {
            double swapMs = (double)(g_tSwap - pSwap) * 1000.0 / (double)fqpf.QuadPart;
            static bool hintInit = false;
            if (!hintInit) { hintInit = true; tj::android::PerfHintInit(16666667ll); }
            double workMs = ms - swapMs;
            if (workMs > 0.0) tj::android::PerfHintReport((int64_t)(workMs * 1e6));
        }
        pPb = g_tPb; pDraw = g_tDraw; pTex = g_tTex; pSwap = g_tSwap;
        pCre = g_frmTexCreate; pUpd = g_frmTexUpdate;
    }
    // TJ_CAPTURE=N: dump capture.bmp at frame N. TJ_CAPTURE=N+ : dump capture_<frame>.bmp
    // every N frames (film strip of the whole flow in one run).
    static int capAt = -2; static bool capEvery = false;
    if (capAt == -2) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_CAPTURE");
        capAt = e ? atoi(e) : -1; if (e && strchr(e, '+')) capEvery = true; free(e); }
    if (capAt > 0 && (capEvery ? (g_frame % capAt == 0) : (g_frame == capAt))) {
        char p[512];
        if (capEvery) _snprintf_s(p, sizeof(p), _TRUNCATE, ".\\capture_%05d.bmp", g_frame);
        else          _snprintf_s(p, sizeof(p), _TRUNCATE, ".\\capture.bmp");
        g_dev.SaveBackbuffer(p); printf("[d3d8] captured frame %d\n", g_frame);
    }
    if (g_pendingCap[0]) {
        g_dev.SaveBackbuffer(g_pendingCap);
        printf("[d3d8] captured -> %s\n", g_pendingCap);
        g_pendingCap[0] = 0;
    }
    // TJ_FRAMES=N: exit cleanly after N presented frames. A wall-clock-bounded leg measures
    // "frames in 300 s", which cannot compare two builds whose difference is a few percent;
    // a FRAME-bounded leg measures "seconds for the same 7,800 frames", which can. Exit code
    // 0 so a harness can tell a completed run from a timeout (124) or a crash.
    {
        static int frameCap = -1;
        if (frameCap < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_FRAMES");
                            frameCap = e ? atoi(e) : 0; free(e); }
        if (frameCap > 0 && g_frame >= frameCap) {
            printf("[d3d8] TJ_FRAMES=%d reached — exiting\n", frameCap);
            fflush(nullptr);
            _Exit(0);
        }
    }
    { extern void FeMenuFrameTick(int); FeMenuFrameTick(g_frame); }
    // Diagnostic: trace the frontend mode byte so we can see the legal->title->menu flow.
    static uint8_t lastMode = 0xFF;
    uint8_t mode = *(volatile uint8_t*)(uintptr_t)0x184661;
    if (mode != lastMode) {
        printf("[d3d8] frame %d: frontend mode %u -> %u\n", g_frame, lastMode, mode);
        lastMode = mode;
        // A MODE CHANGE MEANS THE LEVEL'S RESOURCES DIED. The texture cache is keyed by the
        // game's RESOURCE ADDRESS, and the game reuses those addresses for entirely
        // different textures after a teardown -- so a surviving entry becomes "same key,
        // different bytes" and re-decodes and re-uploads that texture EVERY FRAME, forever.
        // Measured on Beach: the FIRST play of an arena does zero per-frame texture updates
        // (acq=121/0 then 0/0); the second and every later play does 20-68 per frame
        // (acq=0/8184 .. 0/27012) at an IDENTICAL cache size -- nothing evicted, nothing
        // leaked, purely stale keys. That regime is the entire texture cost this project has
        // been optimising, and on a display-bound device it is also ~59 uploads and ~15 mip
        // regenerations per frame in the compositor. Dropping the entries costs a few dozen
        // one-time re-creates after each transition: exactly what a first play already pays.
        DropResourceTextureCaches();
    }
    // WHICH ARENA IS SELECTED. The map carousel index-to-arena mapping has now been wrong
    // three times (ring size, then direction, then press count), and each time it silently
    // tested the WRONG map for a whole leg. The game knows the answer -- FE+0x501 is the id
    // the arena descriptor table is indexed by -- so it reports it instead of being
    // dead-reckoned from how many presses we think we sent.
    {
        static int lastArena = -1;
        uint32_t fe = LanFeMgr();
        int a = fe ? (int)*(uint8_t*)(uintptr_t)(fe + 0x501) : -1;
        if (a != lastArena) {
            static const char* const kNames[13] = { "KITCHEN","HAUNTED","SCRAPYARD","SHIP",
                "CABIN","BANQUET","BEACH","SKYSCRAPER","LAB","WILDWEST","BOXING","MARKET",
                "HELL" };
            printf("[fe] arena select -> %d (%s)\n", a,
                   (a >= 0 && a < 13) ? kNames[a] : "?");
            lastArena = a;
        }
    }
    if (g_frame % 400 == 0) {
        // tex/mem census rides along: the live-texture count and working set are the
        // leak canaries (a climb here = the slow-motion-then-crash class of bug).
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
        K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
#else
        struct { size_t WorkingSetSize; } pmc = {};
#endif
        // THE NUMBER THAT SEPARATES "the phone slowed down" FROM "the game got heavier".
        // ms/f alone cannot tell a clock cap from a busier scene, and this device moves its
        // own clock ceiling by 3-4x mid-match, so a frame-time trend on its own is unreadable.
        // Guest instructions retired per frame is workload in the guest's OWN units: flat
        // insn/f with rising ms/f is the DEVICE, rising insn/f is the GAME. Zero when engine
        // mode is off (the native x86 path retires nothing through Run).
        // REGIME COUNTERS. Steady cost and a degraded regime look identical in ms/f, and
        // the difference is what the user hit on SHIP: fine, then slow motion that persisted
        // until the map was restarted. These four say whether work is being rebuilt rather
        // than merely being slow -- JIT recompiles, code-cache flush-alls, stale-block
        // rejections (page generations bumping under live blocks), texture-cache evictions.
        // In a healthy in-match window every one of them is ~0 per frame.
        static unsigned long long prevComp = 0, prevFlush = 0, prevStale = 0, prevDecl = 0;
        unsigned long long jComp = 0, jFlush = 0, jStale = 0, jDecl = 0;
        BridgeJitHealth(&jComp, &jFlush, &jStale, &jDecl);
        static unsigned long long prevConf = 0;
        unsigned long long jConf = 0;
        BridgeJitCauses(&jConf);
        unsigned long long dConf = jConf - prevConf; prevConf = jConf;
        unsigned long long dComp = jComp - prevComp, dFlush = jFlush - prevFlush,
                           dStale = jStale - prevStale, dDecl = jDecl - prevDecl;
        prevComp = jComp; prevFlush = jFlush; prevStale = jStale; prevDecl = jDecl;
        static unsigned long long prevInsn = 0, prevGate = 0;
        unsigned long long insnNow = 0, gateNow = 0;
        BridgeEngineStats(&insnNow, &gateNow);
        double insnPerF = (insnNow >= prevInsn) ? (double)(insnNow - prevInsn) / 400.0 : 0.0;
        double gatePerF = (gateNow >= prevGate) ? (double)(gateNow - prevGate) / 400.0 : 0.0;
        prevInsn = insnNow; prevGate = gateNow;
        extern uint32_t LowArenaHighMB();        // kernel.cpp (arena exhaustion canary)
        extern uint32_t ContigPoolLiveMB();      // kernel.cpp (contiguous-pool recycler canaries)
        extern uint32_t ContigPoolHighMB();      //   live must plateau; high < 128 forever
        // Wall-clock ms/frame over the last 400 frames = the objective slow-motion gauge
        // (a healthy frame is <=16.6 ms; TJ_FAST removes pacing so this is pure work time).
        static LARGE_INTEGER qpf = {}, prev = {};
        if (!qpf.QuadPart) { QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&prev); }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double msPer = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)qpf.QuadPart / 400.0;
        prev = now;
        double kms = 1000.0 / (double)qpf.QuadPart / 400.0;    // ticks -> ms/frame
        char netbuf[160] = "";
        tj::hybrid::NetStatus(netbuf, sizeof netbuf);          // LAN session telemetry
        printf("[d3d8] frame %d: %.2fms/f [pb %.2f draw %.2f tex %.2f swap %.2f rest %.2f] "
               "draws ui=%u 3d=%u tex=%d pool=%d cache=%d/%d acq=%u/%u skip=%u dedup=%u "
               "nullbind=%u arena=%uMB cpool=%u/%uMB ws=%zuMB insn=%.0fk/f gate=%.0f/f%s "
               "texdec %.2f/texupl %.2f (%.0fkpx/f) regime[recomp %llu conflict %llu flush %llu stale %llu decl %llu evict %u] "
               "stut %u/%u/%u max=%.1f\n",
               g_frame, msPer,
               g_tPb * kms, g_tDraw * kms, g_tTex * kms, g_tSwap * kms,
               msPer - (g_tPb + g_tDraw + g_tSwap) * kms,
               g_dbgUiDraws, g_dbg3dDraws,
               g_dev.TextureCount(), g_dev.TexturePooled(), g_texCacheN, g_pbTexCacheN,
               g_frmTexCreate, g_frmTexUpdate, g_frmTexSkip, g_frmPbDedup,
               Device_NullSrvBinds(), LowArenaHighMB(),
               ContigPoolLiveMB(), ContigPoolHighMB(),
               (size_t)(pmc.WorkingSetSize >> 20), insnPerF / 1000.0, gatePerF, netbuf,
               g_tTexDec * kms, g_tTexUpl * kms, (double)g_texDecPix / 400.0 / 1000.0,
               dComp, dConf, dFlush, dStale, dDecl, g_texEvict,
               g_frmOver17, g_frmOver20, g_frmOver33, g_frmMax);
        g_frmTexCreate = g_frmTexUpdate = g_frmTexSkip = g_frmPbDedup = 0;
        g_tPb = g_tDraw = g_tTex = g_tSwap = 0;
        g_tTexDec = g_tTexUpl = 0; g_texDecPix = 0; g_texEvict = 0;
        g_frmMax = 0.0; g_frmOver17 = g_frmOver20 = g_frmOver33 = 0;
        // TJ_ENG_PROF2 (session-30 temporary instrumentation): cumulative engine
        // counters + dispatch totals every 400 frames; diff consecutive snaps offline
        // to isolate the in-match interval.
        if (tj::engine::EngineProf2On()) {
            tj::engine::EngineProf2Snap((uint32_t)g_frame);
            uint64_t inv = 0, gc = 0; uint32_t miss = 0;
            tj::hybrid::DispatchStats(&inv, &miss, &gc);
            printf("[prof2] disp invokes=%llu gcalls=%llu\n",
                   (unsigned long long)inv, (unsigned long long)gc);
        }
    }
    g_dbgUiDraws = g_dbg3dDraws = g_dbgFvfDraws = g_dbgSkips = g_dbgBoneUploads = 0;
    // Diagnostic (TJ_ENTDUMP=1): dump the scene entity table (ptr @[0x15c470c]+0x1D4,
    // 0x54-byte records: +0x40 identity flag, +0x44 RenderObject handle, +0x48 anim state)
    // to compare first-title vs attract-return-title logo state.
    static int entDump = -1;
    if (entDump < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_ENTDUMP"); entDump = e && atoi(e) ? 1 : 0; free(e); }
    if (entDump && g_frame % 1000 == 0) {
        uint32_t master = *(uint32_t*)(uintptr_t)0x15c470c;
        uint32_t entArr = master && IsReadable(master + 0x1D4, 4) ? *(uint32_t*)(uintptr_t)(master + 0x1D4) : 0;
        if (entArr && IsReadable(entArr, 0x54 * 12)) {
            printf("[ent] frame %d table=%08x:", g_frame, entArr);
            for (int i = 0; i < 12; ++i) {
                const uint8_t* e2 = (const uint8_t*)(uintptr_t)(entArr + i * 0x54);
                printf(" [%d f=%u h=%08x a=%u]", i, e2[0x40], *(uint32_t*)(e2 + 0x44), *(uint16_t*)(e2 + 0x48));
            }
            printf("\n");
        } else printf("[ent] frame %d: no entity table (master=%08x arr=%08x)\n", g_frame, master, entArr);
    }
    // Screen-transition capture: snapshot every presented frame (ping-pong), and when
    // the frontend screen id changes, latch the PREVIOUS frame's snapshot -- that is the
    // old screen's last image, which the next screens' backgrounds sample through the
    // temp texture header @0x95EA2C (logo bg, save prompt, level select, loading).
    {
        static tj::gfx::TextureHandle ping[2] = { tj::gfx::kNoTexture, tj::gfx::kNoTexture };
        static int cur = 0;
        static uint8_t lastScreen = 0xFF;
        static int capW = 0, capH = 0;
        if (ping[0] < 0 || capW != g_backW || capH != g_backH) {
            // (re)create at the current backbuffer size -- a live resolution change
            // invalidates the old snapshots (the stale latched capture stays usable).
            ping[0] = g_dev.CreateCaptureTexture(); ping[1] = g_dev.CreateCaptureTexture();
            capW = g_backW; capH = g_backH;
        }
        if (ping[0] >= 0 && ping[1] >= 0) {
            uint8_t screen = 0xFF;
            uint32_t master = *(volatile uint32_t*)(uintptr_t)0x15c470c;
            if (master && IsReadable(master + 0x4D4, 4)) {
                uint32_t feObj = *(uint32_t*)(uintptr_t)(master + 0x4D4);
                if (feObj && IsReadable(feObj, 4)) screen = *(uint8_t*)(uintptr_t)feObj;
            }
            if (screen != lastScreen) {
                if (lastScreen != 0xFF) {           // not the very first frame
                    // latch the previous frame's snapshot; rotate a fresh texture in
                    tj::gfx::TextureHandle old = g_transitionCap;
                    g_transitionCap = ping[cur];
                    ping[cur] = old >= 0 ? old : g_dev.CreateCaptureTexture();
                }
                lastScreen = screen;
            }
            cur ^= 1;
            // ONLY WHILE THE FRONTEND IS UP. This snapshot exists so the NEXT frontend
            // screen can sample the previous one's last image; in a match nothing samples
            // it at all -- ResolveTexture returns TransparentTex() for the capture header
            // whenever mode == 7 (see the tempTex branch there). Capturing anyway cost a
            // full-screen blit on every in-match frame, which on a weaker GPU is not a
            // rounding error: the compositor PACES the sim (one present per frame, vsync
            // on, no frame dropping), so display work over budget becomes literal slow
            // motion rather than a dropped frame.
            // ...but not at full rate in a match. IN THE FRONTEND every frame is captured,
            // because the next screen samples the previous frame's image and a transition can
            // happen on any frame. IN A MATCH nothing samples it -- ResolveTexture returns
            // TransparentTex() for the capture header whenever mode == 7 -- so the only thing
            // the capture is still for is the FIRST frontend screen after the match ends,
            // which wants the match's last picture. One in eight frames keeps that image at
            // most 130 ms old (invisible on a background) and removes 87% of the cost: a
            // full-screen blit per frame is not a rounding error on a weak GPU, and this
            // thread paces the simulation, so display work over budget becomes slow motion.
            const bool inMatch = *(volatile uint8_t*)(uintptr_t)0x184661 == 7;
            if (ping[cur] >= 0 && (!inMatch || (g_frame & 7) == 0))
                g_dev.CopyBackbufferTo(ping[cur]);
        }
    }
    { PhaseTimer _pt(&g_tSwap); g_dev.EndScene(); g_dev.Present(); }
    // NOTE: message pump runs on a dedicated UI thread (see EnsureDisplay), NOT here --
    // dispatching on the game thread walks its Xbox SEH chain and faults in combase.
    g_dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, 0xFF203040u, 1.0f, 0);
    g_dev.BeginScene();
    // 60 fps pacing -- the Xbox game logic is frame-locked, and neither vsync Present nor
    // the stubbed BlockUntilVerticalBlank limits us. TJ_FAST=1 disables (test automation).
    static int fast = -1; static LARGE_INTEGER qpf, next;
    if (fast < 0) {
        char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_FAST");
        fast = (e && atoi(e)) ? 1 : 0; free(e);
        QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&next);
    }
    if (!fast) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        next.QuadPart += qpf.QuadPart / 60;
        if (next.QuadPart < now.QuadPart) next = now;            // fell behind: resync
        else {
            LONGLONG waitMs = (next.QuadPart - now.QuadPart) * 1000 / qpf.QuadPart;
            if (waitMs > 2) Sleep((DWORD)(waitMs - 1));
            do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart);
        }
    }
}

// D3DResource_Release(pResource) ret 4 -- null-safe no-op (our resources are freed on
// device teardown; leaking during bring-up is fine).
static void __stdcall Br_ResourceRelease(void* res) { (void)res; }

// Diagnostic-friendly native replacement of the scene entity-by-name lookup FUN_00079b50
// (cdecl(scene, name) -> entity index; scene+4 = count, scene+0x1c = 0x54-byte entity
// array, name = packed 4-char dword at entity+0x48; not-found returns 0 like the
// original). TJ_NAMELOG=1 logs every lookup -- which named effect entities the game
// requests, and when.
static uint32_t __cdecl Br_FindEntByName(uint32_t scene, const char* name) {
    uint32_t count = *(uint32_t*)(uintptr_t)(scene + 4);
    uint32_t arr   = *(uint32_t*)(uintptr_t)(scene + 0x1c);
    uint32_t key; memcpy(&key, name, 4);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (*(uint32_t*)(uintptr_t)(arr + i * 0x54 + 0x48) == key) { found = i; break; }
    static int nameLog = -1;
    if (nameLog < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_NAMELOG"); nameLog = e && atoi(e) ? 1 : 0; free(e); }
    if (nameLog) printf("[name] f=%d scene=%08x '%c%c%c%c' -> %u\n", g_frame, scene,
                        name[0], name[1], name[2], name[3], found);
    return found;
}

// --- Input (Xbox XAPI device layer) -----------------------------------------
// Real input wiring. The game's device manager (FUN_00013950) polls
// XGetDeviceChanges with the GAMEPAD type table @0xe7d2c and latches insertion/
// removal masks (&0xF) at 0x184659/0x18465a; per-pad objects then open a handle and
// call XInputGetState(handle, &state) with state = Xbox XINPUT_STATE:
//   +0 dwPacketNumber, +4 wButtons, +6 bAnalogButtons[8] (A,B,X,Y,Black,White,LT,RT),
//   +0xE sThumbLX, +0x10 sThumbLY, +0x12 sThumbRX, +0x14 sThumbRY  (0x18 bytes).
// Sources merged per poll: real XInput pad (tj::input::Poll), a keyboard fallback
// (only while our window is foreground), and a TJ_INPUT scripted sequence
// ("frame:button,frame:button" with start/back/up/down/left/right/a/b/x/y) used by
// the automated bring-up tests to drive the menus without a physical pad.
static const uint32_t kDevTypeGamepad = 0xe7d2c;

// TJ_INPUT comma list. Two token families:
//  LEGACY "frame:token[:holdFrames]" -- fires at an absolute frame (start/back/up/down/
//  left/right, a/b/x/y, sup/sdown/sleft/sright). Fragile against variable match lengths
//  and menu fades; kept for backwards compatibility.
//  GATED (screen-aware, self-retrying -- the fix for every desync class hit during the
//  session-12 arena sweep):
//   "@<scr>:<token>"  fire <token> when the frontend screen id == <scr> (screens: 2 legal,
//                     3 logo, 20 press-start, 1 save prompt, 4 main menu, 11 quick-game
//                     setup, 16 map carousel). Activation presses (a/start) RE-FIRE every
//                     ~500 frames until the screen actually changes (an eaten press just
//                     retries); direction presses fire once. A gated token is SKIPPED if
//                     the flow reaches the NEXT gated token's screen without passing its
//                     own (handles optional screens like the save prompt).
//   "@qg:1"           composite "reach the QUICK GAME map carousel" state machine over
//                     screen ids only: 20/1 -> start, 4 -> alternate down/a, 10 (an
//                     accidental CHALLENGE entry) -> b to back out and retry, 11 -> start,
//                     2/3 -> wait, any other screen -> b. Done when 16 (carousel) shows.
//                     Every eaten press and wrong turn self-corrects.
//   "@map:<N>"        on the map carousel (16): press right/left (shortest wrap over 13,
//                     live cursor [screen+0x1A6]) every ~450f until cursor == N, then
//                     start; retries until the screen changes.
//  The gated sequencer only runs while frontend mode (0x184661) == 3, so in-match frames
//  can never consume or desync it, and it re-arms cleanly after every match.
struct ScriptPress { int frame, hold; uint16_t btn; int analogIdx; int16_t lx, ly;
                     int gate;        // -1 = legacy frame token, else required screen id
                     int mapTgt;      // >=0: @map target cursor (gate implied 0x10)
                     int mapRight;    // >=0: @mapr — press RIGHT exactly this many times
                     int arenaId;     // >=0: @arena — press RIGHT until FE+0x501 == this
                     int menuOff; };  // >=0: @menu target sel offset (gate implied 4)
static ScriptPress g_script[160]; static int g_scriptN = -1;   // -1 = not parsed yet
static void ParseInputScript() {
    g_scriptN = 0;
    char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_INPUT");
    if (!e) return;
    for (char* tok = strtok(e, ","); tok && g_scriptN < 160; tok = strtok(nullptr, ",")) {
        ScriptPress& s = g_script[g_scriptN];
        s.frame = 0; s.hold = 12; s.btn = 0; s.analogIdx = -1; s.lx = s.ly = 0;
        s.gate = -1; s.mapTgt = -1; s.menuOff = -1; s.mapRight = -1; s.arenaId = -1;
        char* b;
        if (tok[0] == '@') {
            char* colon = strchr(tok, ':'); if (!colon) continue;
            *colon = 0; b = colon + 1;
            if (!_stricmp(tok + 1, "map"))     { s.mapTgt = atoi(b); s.gate = 0x10; b = nullptr; }
            // "@mapr:<N>" — press RIGHT exactly N times on the carousel, then A. @map's
            // shortest-path walk cannot express this, and the arena a given count lands on is
            // something the USER reads off the screen; counting presses in the direction they
            // actually pressed is the only description that survives a carousel whose ring
            // size and direction have both been wrong before.
            else if (!_stricmp(tok + 1, "mapr")) { s.mapRight = atoi(b); s.gate = 0x10; b = nullptr; }
            // "@arena:<id>" — THE ONE THAT ACTUALLY WORKS. Press RIGHT until the game says
            // the selected arena IS the one asked for, then A. Every counting scheme tried
            // before this got the wrong map: the ring excludes two arenas, RIGHT and LEFT
            // were inverted, and a press landing during a fade is simply eaten. Reading
            // FE+0x501 makes all three irrelevant -- it is feedback, not dead reckoning.
            // ids: 0 KITCHEN 1 HAUNTED 2 SCRAPYARD 3 SHIP 4 CABIN 5 BANQUET 6 BEACH
            //      7 SKYSCRAPER 8 LAB 9 WILDWEST 10 BOXING 11 MARKET 12 HELL
            //      (BOXING and HELL are excluded from versus mode and cannot be reached)
            else if (!_stricmp(tok + 1, "arena")) { s.arenaId = atoi(b); s.gate = 0x10; b = nullptr; }
            else if (!_stricmp(tok + 1, "qg")) { s.menuOff = 1; s.gate = 4; b = nullptr; }
            else                               { s.gate = atoi(tok + 1); }
        } else {
            char* colon = strchr(tok, ':'); if (!colon) continue;
            *colon = 0;
            s.frame = atoi(tok);
            b = colon + 1;
            char* colon2 = strchr(b, ':');
            if (colon2) { *colon2 = 0; s.hold = atoi(colon2 + 1); if (s.hold < 1) s.hold = 12; }
        }
        if (b) {
            if      (!_stricmp(b, "start")) s.btn = 0x10;
            else if (!_stricmp(b, "back"))  s.btn = 0x20;
            else if (!_stricmp(b, "up"))    s.btn = 0x01;
            else if (!_stricmp(b, "down"))  s.btn = 0x02;
            else if (!_stricmp(b, "left"))  s.btn = 0x04;
            else if (!_stricmp(b, "right")) s.btn = 0x08;
            else if (!_stricmp(b, "a")) s.analogIdx = 0;
            else if (!_stricmp(b, "b")) s.analogIdx = 1;
            else if (!_stricmp(b, "x")) s.analogIdx = 2;
            else if (!_stricmp(b, "y")) s.analogIdx = 3;
            else if (!_stricmp(b, "sup"))    s.ly = 32000;
            else if (!_stricmp(b, "sdown"))  s.ly = -32000;
            else if (!_stricmp(b, "sleft"))  s.lx = -32000;
            else if (!_stricmp(b, "sright")) s.lx = 32000;
            else continue;
        }
        if (s.gate >= 0)
            printf("[input] script[%d]: gate scr=%d %s\n", g_scriptN,
                   s.gate, s.mapTgt >= 0 ? "map" : s.arenaId >= 0 ? "arena"
                                        : s.mapRight >= 0 ? "mapr"
                                        : s.menuOff >= 0 ? "menu" : "press");
        else
            printf("[input] script[%d]: frame %d hold %d\n", g_scriptN, s.frame, s.hold);
        ++g_scriptN;
    }
    free(e);
}
#ifndef _WIN32
// Android input feeds EVERY PORT, one per controller. It used to feed port 0 only -- the
// touch-overlay rule, which was right while touch was the only input and wrong the moment
// controllers worked: a second pad plugged into the phone drove player 1 alongside the first
// and seats 2-4 read a dead pad. native_main now routes each DEVICE to its own seat and
// forwards them here; touch stays player 1's alone, on the app side. On the qemu/headless leg
// AndroidSetPad is never called, so g_androidActive stays false and this is a no-op (script
// only), keeping the S5c determinism legs untouched.
static CRITICAL_SECTION g_androidPadLock;
static bool g_androidPadInit = false;
static bool g_androidActive = false;
static tj::input::XboxGamepad g_androidPad[4]{};
static void AndroidPadEnsureInit() {
    if (!g_androidPadInit) { InitializeCriticalSection(&g_androidPadLock); g_androidPadInit = true; }
}
extern "C" void AndroidSetPad(int port, uint16_t buttons, const uint8_t* analog8,
                              int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    if (port < 0 || port >= 4) return;
    AndroidPadEnsureInit();
    EnterCriticalSection(&g_androidPadLock);
    g_androidActive = true;
    tj::input::XboxGamepad& a = g_androidPad[port];
    a.wButtons = buttons;
    if (analog8) memcpy(a.bAnalogButtons, analog8, 8);
    a.sThumbLX = lx; a.sThumbLY = ly;
    a.sThumbRX = rx; a.sThumbRY = ry;
    LeaveCriticalSection(&g_androidPadLock);
}
static void MergeKeyboard(tj::input::XboxGamepad& gp, int port) {
    if (!g_androidActive) return;               // qemu/headless: never activated -> script only
    if (port < 0 || port >= 4) return;
    AndroidPadEnsureInit();
    EnterCriticalSection(&g_androidPadLock);
    const tj::input::XboxGamepad& a = g_androidPad[port];
    gp.wButtons |= a.wButtons;
    for (int i = 0; i < 8; ++i)
        if (a.bAnalogButtons[i] > gp.bAnalogButtons[i])
            gp.bAnalogButtons[i] = a.bAnalogButtons[i];
    if (a.sThumbLX) gp.sThumbLX = a.sThumbLX;
    if (a.sThumbLY) gp.sThumbLY = a.sThumbLY;
    if (a.sThumbRX) gp.sThumbRX = a.sThumbRX;
    if (a.sThumbRY) gp.sThumbRY = a.sThumbRY;
    LeaveCriticalSection(&g_androidPadLock);
}
#else
static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static void MergeKeyboard(tj::input::XboxGamepad& gp, int port) {
    if (port != 0) return;      // the keyboard is one set of controls -- player 1's alone
    // While a LAN text modal is capturing, typing must not also drive the menu (pressing
    // 'A' would type A *and* move the cursor left).
    if (tj::hybrid::LanTextCaptureActive()) return;
    // Accept keyboard while ANY window of this process is foreground (game window OR the
    // console) -- users often click the console and then "keyboard doesn't work".
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    if (fgPid != GetCurrentProcessId()) return;
    if (KeyDown(VK_UP))     gp.wButtons |= 0x01;
    if (KeyDown(VK_DOWN))   gp.wButtons |= 0x02;
    if (KeyDown(VK_LEFT))   gp.wButtons |= 0x04;
    if (KeyDown(VK_RIGHT))  gp.wButtons |= 0x08;
    if (KeyDown(VK_RETURN)) gp.wButtons |= 0x10;   // Start
    if (KeyDown(VK_BACK))   gp.wButtons |= 0x20;   // Back
    if (KeyDown('Z')) gp.bAnalogButtons[0] = 255;  // A
    if (KeyDown('X')) gp.bAnalogButtons[1] = 255;  // B
    if (KeyDown('C')) gp.bAnalogButtons[2] = 255;  // X
    if (KeyDown('V')) gp.bAnalogButtons[3] = 255;  // Y
    if (KeyDown('Q')) gp.bAnalogButtons[5] = 255;  // White
    if (KeyDown('E')) gp.bAnalogButtons[4] = 255;  // Black
    if (KeyDown('R')) gp.bAnalogButtons[6] = 255;  // LT
    if (KeyDown('T')) gp.bAnalogButtons[7] = 255;  // RT
    if (KeyDown('A')) gp.sThumbLX = -32767; else if (KeyDown('D')) gp.sThumbLX = 32767;
    if (KeyDown('S')) gp.sThumbLY = -32767; else if (KeyDown('W')) gp.sThumbLY = 32767;
}
#endif // _WIN32
static void ApplyPress(tj::input::XboxGamepad& gp, const ScriptPress& s,
                       uint16_t btnOverride = 0) {
    if (btnOverride) { gp.wButtons |= btnOverride; return; }
    if (s.analogIdx >= 0) gp.bAnalogButtons[s.analogIdx] = 255;
    else if (s.btn) gp.wButtons |= s.btn;
    if (s.lx) gp.sThumbLX = s.lx;
    if (s.ly) gp.sThumbLY = s.ly;
}
static void MergeScript(tj::input::XboxGamepad& gp) {
    if (g_scriptN < 0) ParseInputScript();
    // Legacy absolute-frame tokens.
    for (int i = 0; i < g_scriptN; ++i) {
        if (g_script[i].gate >= 0) continue;
        if (g_frame >= g_script[i].frame && g_frame < g_script[i].frame + g_script[i].hold)
            ApplyPress(gp, g_script[i]);
    }
    // Gated sequencer (see the format comment above ParseInputScript).
    static int gi = -1;              // current gated token (index into g_script)
    static int pressEnd = 0;         // active 12-frame press window end
    static uint16_t pressBtn = 0; static int pressAnalog = -1;
    static int lastAct = 0;          // frame of the last gated action (spacing/retry timer)
    static int firedOnce = 0;        // current token has fired at least once
    static int g_mapReck = -1;       // @map dead-reckoned cursor (-1 = fresh entry)
    auto nextGated = [](int from) { for (int i = from + 1; i < g_scriptN; ++i)
                                        if (g_script[i].gate >= 0) return i; return -1; };
    if (gi < 0) { gi = nextGated(-1); if (gi < 0) return; }
    if (gi >= g_scriptN || gi < 0) return;
    uint8_t mode = *(volatile uint8_t*)(uintptr_t)0x184661;
    if (mode != 3) { lastAct = g_frame; pressEnd = 0; return; }   // FE only; re-arm timers
    // Sustain an in-flight press.
    if (g_frame < pressEnd) {
        if (pressAnalog >= 0) gp.bAnalogButtons[pressAnalog] = 255;
        else gp.wButtons |= pressBtn;
        return;
    }
    uint32_t master = *(volatile uint32_t*)(uintptr_t)0x15C470C;
    uint32_t mgr = master ? *(uint32_t*)(uintptr_t)(master + 0x4D4) : 0;
    if (!mgr) return;
    uint8_t scr = *(uint8_t*)(uintptr_t)mgr;
    ScriptPress& s = g_script[gi];
    auto beginPress = [&](uint16_t btn, int analog) {
        pressBtn = btn; pressAnalog = analog; pressEnd = g_frame + 12;
        lastAct = g_frame + 12; firedOnce = 1;
        if (analog >= 0) gp.bAnalogButtons[analog] = 255; else gp.wButtons |= btn;
    };
    auto advance = [&]() { gi = nextGated(gi); firedOnce = 0; lastAct = g_frame; g_mapReck = -1; };
    if (s.menuOff >= 0) {             // @qg -- reach the QUICK GAME map carousel
        static int qgSub = 0;         // on the menu: 0 = press down next, 1 = press a next
        if (scr == 0x10) { qgSub = 0; advance(); return; }           // carousel reached
        if (scr == 2 || scr == 3) return;                            // boot screens: wait
        if (g_frame - lastAct < 500) return;
        // Screens that just need "acknowledge" (title, save prompt, 4-player setup, and
        // anything unexpected) get a ROTATING press: START, then A, then B. Which button
        // dismisses a prompt varies -- the boot save prompt behaves differently once a
        // save file exists, and START alone left a run parked on it. Rotating is
        // self-healing: whichever the screen wants lands within three attempts, and B
        // eventually backs out of anything genuinely unrecognised.
        static int ack = 0;
        static uint8_t ackScr = 0xFF;
        if (scr != ackScr) { ackScr = (uint8_t)scr; ack = 0; }
        if (scr == 4) { beginPress(qgSub ? 0 : 0x02, qgSub ? 0 : -1); qgSub ^= 1; return; }
        switch (ack++ % 3) {
            case 0:  beginPress(0x10, -1); break;                    // START
            case 1:  beginPress(0, 0);     break;                    // A
            default: beginPress(0, 1);     break;                    // B
        }
        return;
    }
    if (scr != s.gate) {
        // Screen changed after an activation fired -> token done. Or the flow reached
        // the NEXT token's screen without ever showing ours -> optional-screen skip.
        bool activation = s.mapTgt >= 0 || s.mapRight >= 0 || s.arenaId >= 0 || s.menuOff >= 0 ||
                          s.analogIdx == 0 || s.btn == 0x10;
        if (firedOnce && activation) { advance(); return; }
        int ni = nextGated(gi);
        if (ni >= 0 && scr == g_script[ni].gate) {
            printf("[input] gated[%d] skipped (screen %u reached)\n", gi, scr);
            advance(); return;
        }
        // WATCHDOG: parked on a screen this token knows nothing about (e.g. the post-match
        // RESULTS screen 17, which waits for input forever, or an unexpected modal). Press
        // B every ~900 frames to back out toward somewhere the script recognises. Without
        // this a sweep silently stalls -- an all-arena run once sat on screen 17 for a
        // million frames looking like a hang.
        if (g_frame - lastAct >= 900) {
            printf("[input] gated[%d] watchdog: unexpected screen %u, backing out\n", gi, scr);
            beginPress(0, 1);         // B
            firedOnce = 0;            // a watchdog press is not the token firing
        }
        return;                       // not our screen yet -- wait
    }
    if (s.arenaId >= 0) {             // @arena:N -- press RIGHT until the game agrees
        uint32_t fe = LanFeMgr();
        int cur = fe ? (int)*(uint8_t*)(uintptr_t)(fe + 0x501) : -1;
        // 120-frame spacing, not the 500 the dead-reckoned walk needs: overshooting is
        // self-correcting here (keep pressing RIGHT and the ring comes back around), so the
        // walk can run at the carousel's own pace instead of the pace that guarantees every
        // press is counted. A 13-arena ring then settles in ~26 s instead of ~110.
        if (cur == s.arenaId) {
            if (g_frame - lastAct >= 300) beginPress(0, 0);    // A = SELECT (retries)
        } else if (g_frame - lastAct >= 120) {
            beginPress(0x08, -1);                              // 0x08 = RIGHT
        }
        return;
    }
    if (s.mapRight >= 0) {            // @mapr:N -- N presses RIGHT, then select
        if (g_mapReck < 0) g_mapReck = 0;                      // carousel resets on entry
        if (g_mapReck >= s.mapRight) {
            if (g_frame - lastAct >= 1200) beginPress(0, 0);   // A = SELECT (retries)
        } else if (g_frame - lastAct >= 500) {
            beginPress(0x08, -1);                              // 0x08 = RIGHT
            ++g_mapReck;
        }
        return;
    }
    if (s.mapTgt >= 0) {              // @map:N -- dead-reckoned carousel walk
        // The RE'd cursor field (+0x1A6) is DEAD on the live carousel object (stuck at 0
        // while the visual carousel scrolls -- an hour of continuous presses proved every
        // spaced press registers). So count our own presses instead: the carousel resets
        // to Kitchen (idx 0) on every entry (verified across all probes), and each press
        // moves exactly one step at this spacing.
        // RING SIZE IS 11, NOT 13: the game excludes two arenas from versus mode even when
        // everything is unlocked. Using 13 here made every leftward wrap land two arenas
        // short (@map:9 -> Cabin, @map:10 -> Ship), silently testing the wrong maps.
        // DIRECTION IS INVERTED, and it silently tested the WRONG ARENA for a whole session:
        // on this carousel LEFT advances the arena id and RIGHT walks back. Measured, not
        // guessed -- five runs whose loaded arena was read out of the item-table hook:
        // @map:0 -> 0, @map:3 -> 8, @map:6 -> 5, @map:8 -> 3, @map:9 -> 2, i.e. exactly
        // (kRing - N) % kRing under the old code. So LEFT is the "forward" press.
        const int kRing = 11;
        if (g_mapReck < 0) g_mapReck = 0;                      // carousel resets on entry
        if (g_mapReck == s.mapTgt) {
            if (g_frame - lastAct >= 1200) beginPress(0, 0);   // A = SELECT (retries)
        } else if (g_frame - lastAct >= 500) {
            int fwd = (s.mapTgt - g_mapReck + kRing) % kRing;
            if (fwd * 2 <= kRing) { beginPress(0x04, -1); g_mapReck = (g_mapReck + 1) % kRing; }
            else                  { beginPress(0x08, -1); g_mapReck = (g_mapReck + kRing - 1) % kRing; }
        }
        return;
    }
    if (s.menuOff >= 0) {             // @menu:off -- feedback-driven menu selection
        uint32_t sel = *(uint32_t*)(uintptr_t)(mgr + 4);
        if (sel == mgr + (uint32_t)s.menuOff) {
            if (g_frame - lastAct >= 450) beginPress(0, 0);              // a (retries)
        } else if (g_frame - lastAct >= 450) {
            beginPress(0x02, -1);                                        // down
        }
        return;
    }
    // Simple gated press.
    bool activation = s.analogIdx == 0 || s.btn == 0x10;
    if (firedOnce && !activation) { advance(); return; }   // directions fire once
    if (g_frame - lastAct >= 500) beginPress(s.btn, s.analogIdx);        // (re)fire
}

static void __stdcall Br_XInitDevices(uint32_t count, void* types) { (void)count; (void)types; }
// One bit per Xbox port. NETPLAY: report one pad per participating seat so the game offers
// a HUMAN slot for each -- otherwise the remote player's port reads as absent, the setup
// repack fills it with a CPU, and the fighter ignores the inputs arriving over the wire.
// This is now an EDGE QUEUE rather than a one-shot: the LAN player count is not known at
// boot (it is decided in the lobby), so ports appear and disappear as the session changes,
// exactly the way the game's own hot-plug path expects. Single-player, with no session and
// one pad, reports the same mask 1 it always did.
static int LocalPadCount();

static uint32_t DesiredPadMask() {
    uint32_t mask = 1;                     // port 0 always
    // LOCAL PLAY: one port per controller. The game only lets a second, third or fourth
    // player join a match if it has been told those pads exist, so this is what makes local
    // 2-4 player work at all -- previously it always reported exactly one pad.
    int local = LocalPadCount();
    if (local > 1 && local <= 4) mask = (1u << local) - 1u;
    tj::hybrid::LanState st = tj::hybrid::LanGetState();
    if (tj::hybrid::NetArmed() || st == tj::hybrid::LAN_LOBBY ||
        st == tj::hybrid::LAN_STARTING || st == tj::hybrid::LAN_MATCH) {
        int players = 2;
        char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_NET_PLAYERS");
        if (e && *e) players = atoi(e);
        free(e);
        int seats = 0;
        for (int i = 0; i < tj::hybrid::LanSlotCount(); ++i) {
            const tj::hybrid::LanSlotInfo* s = tj::hybrid::LanSlot(i);
            if (s && s->kind) seats = i + 1;
        }
        if (seats > players) players = seats;
        if (players < 1 || players > 4) players = 2;
        mask = (1u << players) - 1u;
    }
    return mask;
}
static uint32_t __stdcall Br_XGetDeviceChanges(void* devType, uint32_t* ins, uint32_t* rem) {
    if (ins) *ins = 0; if (rem) *rem = 0;
    if ((uintptr_t)devType != kDevTypeGamepad) return 0;
    static uint32_t reported = 0;
    static bool first = true;
    uint32_t want = DesiredPadMask();
    if (!first && want == reported) return 0;
    uint32_t add = want & ~reported, del = reported & ~want;
    first = false; reported = want;
    if (ins) *ins = add;
    if (rem) *rem = del;
    printf("[input] gamepad change: mask %X (ins %X rem %X)\n", want, add, del);
    return 1;
}
// XInputOpen(pDeviceType, dwPort, dwSlot, pPollingParams) -> tagged per-port handle.
static uint32_t __stdcall Br_XInputOpen(uint32_t devType, uint32_t port, uint32_t slot, void* poll) {
    (void)devType; (void)slot; (void)poll;
    printf("[input] XInputOpen port %u\n", port);
    return 0x1000 + (port & 3);
}
static uint32_t __stdcall Br_XInputClose(uint32_t h) { (void)h; return 0; }
// Physical-pad polling runs on a DEDICATED THREAD started while the process is still
// pristine (before MapXbeImage): XInput1_4 lazily initializes COM/RPC plumbing on its
// first call, and that init FAILS when first attempted on the game thread inside the
// mangled process (Xbox SEH chain, remapped low memory) -- it then caches the failure
// and reports every pad as disconnected forever. The thread also owns the index scan
// (the user's pad can enumerate at any XInput index) and publishes a snapshot the game
// thread reads under a light lock.
static CRITICAL_SECTION g_padLock;
static tj::input::XboxGamepad g_padSnap[4];      // indexed by XBOX PORT (= player number)
static bool g_padLive[4];
static int  g_padCount = 0;

// ONE CONTROLLER PER PLAYER. This used to merge every connected PC pad into Xbox port 0 --
// buttons OR'd together, largest-magnitude axis wins -- so that whichever pad the user
// touched would drive the game. That is right for one player and completely wrong for the
// four the game supports: every extra controller just moved player 1, and players 2-4 read a
// dead pad.
//
// A port is claimed on the pad's FIRST ACTUAL INPUT, not when it enumerates. This machine
// reports four XInput devices with one controller attached: vendor software and wireless
// dongles routinely expose idle virtual pads, so index order says nothing about who is
// playing. Claiming on connection would have made a real controller "player 3" and looked
// exactly as broken as the bug this replaces. Claiming on first press means the pad someone
// actually touches becomes player 1, the next one to be touched player 2, and idle phantom
// devices never take a seat. Assignments are sticky for the session; unplugging frees the
// port without renumbering anybody still playing.
static bool PadIsActive(const tj::input::XboxGamepad& gp) {
    if (gp.wButtons) return true;
    for (int b = 0; b < 8; ++b) if (gp.bAnalogButtons[b] > 40) return true;
    const int kStick = 12000;      // far beyond any resting drift
    return (gp.sThumbLX > kStick || gp.sThumbLX < -kStick) ||
           (gp.sThumbLY > kStick || gp.sThumbLY < -kStick) ||
           (gp.sThumbRX > kStick || gp.sThumbRX < -kStick) ||
           (gp.sThumbRY > kStick || gp.sThumbRY < -kStick);
}

#ifdef _WIN32
static DWORD WINAPI InputThreadMain(LPVOID) {
    bool conn[4] = {};                 // XInput index -> connected?
    int  portOf[4] = { -1, -1, -1, -1 };   // XInput index -> Xbox port (-1 = not claimed yet)
    DWORD nextScan = 0;
    for (;;) {
        tj::input::XboxGamepad snap[4]{};
        bool live[4] = {};
        DWORD now = GetTickCount();
        bool rescan = now >= nextScan;
        if (rescan) nextScan = now + 1000;      // disconnected-index polls are slow: ~1/s
        for (int i = 0; i < 4; ++i) {
            if (!conn[i] && !rescan) continue;
            tj::input::XboxGamepad gp{};
            bool got = tj::input::Poll(i, gp);
            if (got != conn[i]) {
                conn[i] = got;
                printf("[input] PC pad %d %s\n", i, got ? "connected" : "disconnected");
                if (!got && portOf[i] >= 0) {
                    printf("[input] player %d's controller was unplugged\n", portOf[i] + 1);
                    portOf[i] = -1;
                }
            }
            if (!got) continue;
            if (portOf[i] < 0) {
                if (!PadIsActive(gp)) continue;          // idle/phantom pad: claims nothing
                bool used[4] = {};
                for (int k = 0; k < 4; ++k) if (portOf[k] >= 0) used[portOf[k]] = true;
                int p = 0; while (p < 4 && used[p]) ++p;
                if (p >= 4) continue;
                portOf[i] = p;
                printf("[input] PC pad %d -> player %d (first input)\n", i, p + 1);
            }
            snap[portOf[i]] = gp;
            live[portOf[i]] = true;
        }
        int count = 0;
        for (int p = 0; p < 4; ++p) if (live[p]) count = p + 1;   // highest occupied port
        EnterCriticalSection(&g_padLock);
        for (int p = 0; p < 4; ++p) { g_padSnap[p] = snap[p]; g_padLive[p] = live[p]; }
        g_padCount = count;
        LeaveCriticalSection(&g_padLock);
        Sleep(4);
    }
}
#endif // _WIN32

// How many Xbox ports currently have a controller on them. Drives the hot-plug mask the game
// is told about, which is what lets it accept a second, third and fourth local player.
static int LocalPadCount() {
    EnterCriticalSection(&g_padLock);
    int n = g_padCount;
    LeaveCriticalSection(&g_padLock);
    return n;
}

// How many people are sitting at THIS PC, as netplay should count them: at least one (the
// keyboard always drives player 1 even with no pad attached).
extern "C" int GetLocalPlayerCount() {
    // TJ_LOCAL_PLAYERS=<n> pretends n people are sitting here. The automated LAN harness has no
    // controllers at all, so without this there is no way to exercise two players sharing a PC
    // -- which is exactly where the seat bookkeeping (timeouts, per-seat input, naming) breaks.
    static int forced = -1;
    if (forced < 0) { char* e = nullptr; size_t sz = 0; _dupenv_s(&e, &sz, "TJ_LOCAL_PLAYERS");
                      forced = (e && *e) ? atoi(e) : 0; free(e);
                      if (forced > 0) printf("[input] TJ_LOCAL_PLAYERS=%d\n", forced); }
    if (forced > 0) return forced > 4 ? 4 : forced;
    int n = LocalPadCount();
    return n < 1 ? 1 : (n > 4 ? 4 : n);
}
static void StartInputThread() {
    static bool started = false;
    if (started) return;
    started = true;
    InitializeCriticalSection(&g_padLock);
#ifdef _WIN32
    HANDLE t = CreateThread(nullptr, 0, InputThreadMain, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    printf("[input] pad poll thread started\n");
#else
    printf("[input] headless: no pad poll thread (TJ_INPUT script drives port 0)\n");
#endif
}
// The controls of ONE local player on THIS instance, as netplay should see them: their own
// physical pad (unless TJ_NOINPUT), plus -- for local player 1 only -- the keyboard and the
// scripted sequence. net_lan.cpp schedules this D frames ahead and sends it; while armed it
// is never applied to a port directly.
//
// `which` is the LOCAL player index (0 = the first controller on this PC), not the Xbox port
// and not the seat: a second player sharing this PC is local player 1 here but may hold any
// seat in the lobby.
extern "C" void GetLocalPadForNet(tj::hybrid::NetInput* out, int which) {
    tj::input::XboxGamepad gp{};
    if (which < 0 || which > 3) which = 0;
    static int noInput = -1;
    if (noInput < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_NOINPUT");
                       noInput = e && atoi(e) ? 1 : 0; free(e); }
    if (!noInput) {
        EnterCriticalSection(&g_padLock);
        gp = g_padSnap[which];
        LeaveCriticalSection(&g_padLock);
        MergeKeyboard(gp, which);
    }
    if (which == 0) MergeScript(gp);
    out->buttons = gp.wButtons;
    memcpy(out->analog, gp.bAnalogButtons, 8);
    out->thumb[0] = gp.sThumbLX; out->thumb[1] = gp.sThumbLY;
    out->thumb[2] = gp.sThumbRX; out->thumb[3] = gp.sThumbRY;
}

static uint32_t __stdcall Br_XInputGetState(uint32_t h, void* state) {
    if (!state) return 0;
    memset(state, 0, 0x18);
    int port = (h >= 0x1000 && h < 0x1004) ? (int)(h - 0x1000) : 0;
    tj::input::XboxGamepad gp{};
    // NETPLAY: when armed, EVERY port is served from the agreed network inputs for the
    // frame being stepped -- the local player's own pad included, because it too is played
    // D frames after it was pressed. Local hardware must never leak into a port directly
    // or the two peers would see different inputs for the same frame.
    if (tj::hybrid::NetArmed()) {
        if (const tj::hybrid::NetInput* ni = tj::hybrid::NetPad(port)) {
            gp.wButtons = ni->buttons;
            memcpy(gp.bAnalogButtons, ni->analog, 8);
            gp.sThumbLX = ni->thumb[0]; gp.sThumbLY = ni->thumb[1];
            gp.sThumbRX = ni->thumb[2]; gp.sThumbRY = ni->thumb[3];
        }
    } else if (port >= 0 && port < 4) {
        // TJ_NOINPUT=1: ignore PHYSICAL input (pad + keyboard) and accept only the scripted
        // sequence. Determinism A/B runs are controlled experiments: a connected pad's
        // analog drift or a stray keypress feeds player 1 differently in run A than run B,
        // producing a divergence that looks like a simulation bug but is just hardware.
        static int noInput = -1;
        if (noInput < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_NOINPUT");
                           noInput = e && atoi(e) ? 1 : 0; free(e);
                           if (noInput) printf("[input] TJ_NOINPUT: physical pad+keyboard ignored\n"); }
        if (!noInput) {
            EnterCriticalSection(&g_padLock);
            gp = g_padSnap[port];
            LeaveCriticalSection(&g_padLock);
        }
        // The keyboard and the TJ_INPUT script are player 1's, and only player 1's: they are
        // a single set of controls, so feeding them to every port would make one keypress
        // move all four fighters at once.
        if (!noInput) MergeKeyboard(gp, port);
        if (port == 0) MergeScript(gp);
    }
    static uint32_t packet[4];
    *(uint32_t*)state = ++packet[port];
    memcpy((char*)state + 4, &gp, sizeof(gp));   // 18-byte Xbox gamepad after the packet dword
    static bool logged = false;
    if (!logged) { logged = true; printf("[input] XInputGetState live (port %d)\n", port); }
    return 0;                              // ERROR_SUCCESS
}
// XInputSetState(handle, pFeedback) -- rumble. The game skips further updates while the
// XINPUT_FEEDBACK header dword reads 0x3E5 (ERROR_IO_PENDING), so complete the fake I/O
// immediately: status 0 = success. (Real rumble passthrough can come later.)
static uint32_t __stdcall Br_XInputSetState(uint32_t h, void* feedback) {
    (void)h; if (feedback) *(uint32_t*)feedback = 0; return 0;
}
// Xbox XINPUT_CAPABILITIES (0x19 bytes): +0 SubType (1 = gamepad), then the In-caps
// gamepad image, then Out rumble WORDs at +0x15/+0x17 -- the game requires BOTH == 0xFFFF
// to enable rumble. (The old code stopped at +0x13, leaving the rumble words as stack
// garbage -- rumble enable was random per run.)
static uint32_t __stdcall Br_XInputGetCapabilities(uint32_t h, void* caps) {
    (void)h; if (!caps) return 0;
    memset(caps, 0, 0x19);
    uint8_t* c = (uint8_t*)caps;
    c[0] = 0x01;                           // XINPUT_DEVSUBTYPE gamepad
    memset(c + 4, 0xFF, 0x11);             // In-caps: "all controls supported"
    c[0x15] = 0xFF; c[0x16] = 0xFF;        // Out.wLeftMotorSpeed  = 0xFFFF
    c[0x17] = 0xFF; c[0x18] = 0xFF;        // Out.wRightMotorSpeed = 0xFFFF
    return 0;
}

// --- Audio (DSOUND) -----------------------------------------------------------
// Audio isn't needed to reach the front-end menu; the real DirectSoundCreate init
// programs the MCPX/DSP and raises handle exceptions here. Bypass it: return a large
// zeroed fake IDirectSound object + DS_OK so the game's audio init "succeeds"; any
// DSOUND methods that then fault on the fake object get stubbed as we hit them.
// Disable audio wholesale: overwrite every DSOUND function the game calls with
// `xor eax,eax; ret N` (return DS_OK, clean N stdcall args). DirectSoundCreate touches
// MCPX audio hardware MMIO (0xFExxxxxx) that doesn't exist here, so it -- and all the
// IDirectSound*/IDirectSoundBuffer* methods -- become no-ops. Safe because a stubbed
// method never dereferences `this`, so even a null buffer from a stubbed Create is fine.
// Overwrite a function entry with `xor eax,eax; ret N` (return 0, clean N stdcall args).
static void StubReturn0(uint32_t va, uint16_t retN) {
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)va, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t* p = (uint8_t*)(uintptr_t)va;
    p[0] = 0x31; p[1] = 0xC0;                          // xor eax,eax
    if (retN == 0) { p[2] = 0xC3; }                    // ret
    else { p[2] = 0xC2; p[3] = (uint8_t)retN; p[4] = (uint8_t)(retN >> 8); } // ret N
    VirtualProtect((void*)(uintptr_t)va, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

// GPU-sync / pushbuffer functions that spin on absent NV2A hardware -> return immediately.
static void InstallGpuSyncStubs() {
    StubReturn0(0x9ca30, 8);   // D3D_BlockOnTime
    StubReturn0(0x9cd60, 4);   // D3D_BlockOnResource (also covers BlockUntilNotBusy thunk)
    StubReturn0(0x97850, 0);   // D3DDevice_BlockUntilVerticalBlank
    StubReturn0(0x9cce0, 0);   // D3DDevice_MakeSpace
    StubReturn0(0x9cba0, 8);   // D3D_MakeRequestedSpace_8
    StubReturn0(0x97d60, 4);   // D3DDevice_SetViewport (our Device owns the viewport)
    printf("[d3d8] GPU-sync stubs installed\n");
}

static void InstallAudioStubs() {
    for (int i = 0; i < kDSStubCount; ++i)
        StubReturn0(kDSStubs[i].va, kDSStubs[i].retN);
    // Game sound-manager functions that deref the (now-null) DSOUND objects -> no-op.
    StubReturn0(0x88b00, 0);   // music/media stream load callback
    StubReturn0(0x892d0, 0);   // per-frame sound update
    StubReturn0(0x88970, 0);   // sound DoWork tick
    printf("[d3d8] audio disabled: %d DSOUND functions stubbed\n", kDSStubCount);
}

// --- Guard the per-vertex brightness/status flush (FUN_0007db80) ----------------------
// It writes vertex colors through the write-combined alias 0x80000000|VB_Data. A stale /
// torn-down object left in the effect queue across a level transition (mode 4->5) has a
// garbage VB Data (observed 0x10000000, whose alias 0x90000000 is unmapped) -> access
// violation. Validate the object's VB Data is in the mapped+aliased game-memory range
// [0x04000000, 0x10000000) before letting the original run; skip bogus objects (they are
// being destroyed anyway, so nothing visible is lost). This is deterministic in attract
// (crashed at frame ~29958 every run) and is the true "few minutes then crash".
typedef void (__cdecl* FnDb80)(uint32_t inst, uint8_t bright, uint32_t arg3);
static uint32_t g_orig_db80 = 0;   // guest-window trampoline VA (GCALL it)
// Build a call-through trampoline: copy `len` bytes of the original prologue, then jmp to
// the rest. `len` must be >= 5 and land on an instruction boundary; the copied bytes must
// be position-independent (no rel jumps). FUN_0007db80's prologue (mov eax,[esp+0xc];
// sub esp,0x40 = 7 bytes) is both.
// (call-through trampolines now come from xdk_patch's guest-window pad — MakeGuestTramp)
// Validate EVERY part's vertex buffer -- the game loops all parts, so a bad VB on ANY
// part crashes (an earlier version checked only part[0] and still crashed on part[3]).
static bool BrightnessObjOk(uint32_t inst) {
    if (!IsReadable(inst + 0x44, 4)) return false;
    uint32_t parts = *(uint32_t*)(uintptr_t)(inst + 0x44);
    if (!parts || !IsReadable(parts, 8)) return false;
    uint32_t partArr = *(uint32_t*)(uintptr_t)parts;
    uint16_t partCnt = *(uint16_t*)(uintptr_t)(parts + 4);
    if (partCnt == 0) return true;                            // nothing to write
    if (partCnt > 256 || !partArr) return false;             // absurd count = garbage
    for (uint32_t p = 0; p < partCnt; ++p) {
        uint32_t part = partArr + p * 0x54;                  // part records, 0x54 stride
        if (!IsReadable(part + 0x40, 4)) return false;
        uint32_t meshHdr = *(uint32_t*)(uintptr_t)(part + 0x40);
        if (!meshHdr || !IsReadable(meshHdr + 8, 4)) return false;
        uint32_t vbRes = *(uint32_t*)(uintptr_t)(meshHdr + 4);
        if (!vbRes || !IsReadable(vbRes + 8, 4)) return false;
        uint32_t vbData = *(uint32_t*)(uintptr_t)(vbRes + 4);  // game does (vbData|0x80000000)
        if (vbData < 0x04000000 || vbData >= 0x10000000) return false;  // WC alias unmapped
    }
    return true;
}
static void __cdecl Hk_Brightness(uint32_t inst, uint8_t bright, uint32_t arg3) {
    if (BrightnessObjOk(inst)) { GCALL(Cdecl, FnDb80, g_orig_db80, inst, bright, arg3); return; }
    static int logged = 0;
    if (logged < 8) { ++logged;
        printf("[fx] skip stale brightness object inst=%08x (a part VB is out of range/unreadable)\n", inst); }
}

// Install the Tier-1 bridge hooks. Returns count patched.
int InstallD3D8Bridge() {
    // Set up the CDevice at its real global (0xa6760) with pushbuffer + query fields EARLY:
    // the engine touches device state during framebuffer setup, before Init3DEnvironment.
    SetupDevice();
    // Seed the render-state SHADOW dwords the bridge consults: they live in zero-filled
    // BSS and the game only writes them when specific passes run. Until the first in-game
    // stencil pass restored [0xA65CC], the boot frontend ran with a zero color mask and
    // Draw3D dropped EVERY 3D draw -- the missing first-boot title logo. Seed with the
    // game's own restore constants so the ==0 stencil-pass detection stays meaningful.
    SetGlobal32(0xA65CC, 0x01010101);   // SET_COLOR_MASK: all channels enabled
    SetGlobal32(0xA65C0, 1);            // zwrite default TRUE
    SetGlobal32(0xA65B8, 0x302);        // src blend GL_SRC_ALPHA
    SetGlobal32(0xA65BC, 0x303);        // dst blend GL_ONE_MINUS_SRC_ALPHA
    int n = 0;
    n += PatchJump(0x7c6f0, HOOK_CDECL(Br_Init3DEnvironment), "SYS_Init3DEnvironment");
    n += PatchJump(0x95f70, HOOK_STD(Br_CreateDevice),     "Direct3D_CreateDevice");
    n += PatchJump(0x98910, HOOK_STD(Br_Clear),            "D3DDevice_Clear");
    n += PatchJump(0x9b1d0, HOOK_STD(Br_Swap),             "D3DDevice_Swap");
    n += PatchJump(0x95aa0, HOOK_STD(Br_ResourceRelease),  "D3DResource_Release");
    n += PatchJump(0x97f90, HOOK_STD(Br_SetTexture),       "D3DDevice_SetTexture");
    n += PatchJump(0x99570, HOOK_STD(Br_SetStreamSource),  "D3DDevice_SetStreamSource");
    n += PatchJump(0x981d0, HOOK_STD(Br_SetIndices),       "D3DDevice_SetIndices");
    n += PatchJump(0x98140, HOOK_STD(Br_SetPalette),       "D3DDevice_SetPalette");
    n += PatchJump(0x958c0, HOOK_STD(Br_LockRect),         "D3DTexture_LockRect");
    // Shaders + draw (semantic replacement)
    n += PatchJump(0x99950, HOOK_STD(Br_SetVertexShader),  "SetVertexShader");
    n += PatchJump(0x99cc0, HOOK_STD(Br_SetPixelShader),   "SetPixelShader");
    n += PatchJump(0x99200, HOOK_STD(Br_CreateVertexShader),"CreateVertexShader");
    n += PatchJump(0x99c70, HOOK_STD(Br_CreatePixelShader), "CreatePixelShader");
    n += PatchJump(0x993a0, HOOK_FC(Br_SetVSConst4),       "SetVertexShaderConstant4");
    n += PatchJump(0x99340, HOOK_FC(Br_SetVSConst1),       "SetVertexShaderConstant1");
    n += PatchJump(0x99ec0, HOOK_STD(Br_SetPSConst),       "SetPixelShaderConstant");
    n += PatchJump(0x99530, HOOK_FC(Br_SetVSConstN),       "SetVertexShaderConstantNotInline");
    n += PatchJump(0x99450, HOOK_FC(Br_SetVSConstN),       "SetVertexShaderConstantNotInlineFast");
    n += PatchJump(0x99600, HOOK_STD(Br_LoadVertexShader), "LoadVertexShader");
    n += PatchJump(0x99660, HOOK_STD(Br_SelectVertexShader),"SelectVertexShader");
    n += PatchJump(0x99ac0, HOOK_STD(Br_SetVertexShaderInput),"SetVertexShaderInput");
    n += PatchJump(0x9b410, HOOK_STD(Br_DrawVerticesUP),   "DrawVerticesUP");
    // 3D mesh path
    n += PatchJump(0x9b6c0, HOOK_STD(Br_DrawVertices),     "DrawVertices");
    n += PatchJump(0x9b760, HOOK_STD(Br_DrawIndexedVertices), "DrawIndexedVertices");
    n += PatchJump(0x9b580, HOOK_STD(Br_DrawIndexedVerticesUP), "DrawIndexedVerticesUP");
    n += PatchJump(0x9a280, HOOK_STD(Br_RunPushBuffer),    "RunPushBuffer");
    // Render-to-texture pass redirect (motion-blur smear textures, preview panels).
    n += PatchJump(0x82660, HOOK_CDECL(Br_RttBegin),       "RttBegin");
    n += PatchJump(0x826e0, HOOK_CDECL(Br_RttContinue),    "RttContinue");
    n += PatchJump(0x82620, HOOK_CDECL(Br_RttEnd),         "RttEnd");
    n += PatchJump(0x79b50, HOOK_CDECL(Br_FindEntByName),  "FindEntityByName");   // diag: TJ_NAMELOG
    // Brightness/status flush guard (skip stale objects w/ bad VB pointers). Build the
    // call-through trampoline from the UNPATCHED prologue FIRST, then patch the entry.
    g_orig_db80 = MakeGuestTramp(0x7db80, 7, "d3d8:tr.brightness");
    if (g_orig_db80) n += PatchJump(0x7db80, HOOK_CDECL(Hk_Brightness), "BrightnessGuard");
    // Audio: fully disabled (MCPX hardware can't run natively). GPU-sync waits: stubbed.
    InstallAudioStubs();
    InstallGpuSyncStubs();
    // Input (XAPI device layer)
    n += PatchJump(0xe7e38, HOOK_STD(Br_XInitDevices),     "XInitDevices");
    n += PatchJump(0xe8b09, HOOK_STD(Br_XGetDeviceChanges),"XGetDeviceChanges");
    n += PatchJump(0xe8bd8, HOOK_STD(Br_XInputOpen),       "XInputOpen");
    n += PatchJump(0xe8c2e, HOOK_STD(Br_XInputClose),      "XInputClose");
    n += PatchJump(0xe8e12, HOOK_STD(Br_XInputGetState),   "XInputGetState");
    n += PatchJump(0xe8e85, HOOK_STD(Br_XInputSetState),   "XInputSetState");
    n += PatchJump(0xe8c3a, HOOK_STD(Br_XInputGetCapabilities), "XInputGetCapabilities");
    printf("[d3d8] bridge installed: %d hooks\n", n);
    return n;
}

// Self-test (mode "gfx"): create the display from the hybrid process and clear a few
// frames, proving tj_runtime works in-process before we wire it into the game.
int GfxSelfTest() {
    printf("\n=== gfx self-test: native D3D11 from the hybrid process ===\n");
    if (!EnsureDisplay(1280, 720, 1)) return 1;
    for (int i = 0; i < 120; ++i) {
        PumpMessages();
        uint32_t argb = 0xFF000000u | ((i * 2 & 0xFF) << 16) | 0x40; // animate blue->purple
        g_dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, argb, 1.0f, 0);
        g_dev.BeginScene(); g_dev.EndScene(); g_dev.Present();
        Sleep(8);
    }
    printf("[d3d8] gfx self-test done\n");
    return 0;
}

} // namespace tj::hybrid
