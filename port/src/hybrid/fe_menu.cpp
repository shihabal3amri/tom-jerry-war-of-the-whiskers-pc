// Frontend menu injection: re-enable the game's HIDDEN VIDEO options screen and add a
// native RESOLUTION row to it.
//
// The retail build ships a complete VIDEO screen (id 9: WIDESCREEN toggle + CONFIRM,
// builder FUN_00028C00, update FUN_00028D30) whose menu entry exists on the OPTIONS
// screen (item at +0x1A8, label 73 "VIDEO", routed by the update handler) but is never
// AppendItem'd -- the row was cut from retail. We append it, fix the one retail bug that
// cutting it hid (the transition early-out returns screen id 8 instead of 9), and splice
// our own RESOLUTION item into the VIDEO screen. The resolution is applied live (window
// + backbuffer resize) and persisted in tomjerry.ini [Display] Width/Height, which the
// loader reads at boot (hybrid_run.cpp).
//
// All game structs/VAs from the options-menu RE (session 10):
//   menu item = 0x84 bytes: +0x40/+0x44 x/y floats, +0x49 selectable, +0x60/+0x64
//   next/prev, +0x7A label string index (u16), +0x7C value string index (s8, 0x24=none).
//   Screen: +0x04 selected item, vtbl+0x18 AppendItem. Text lookup FUN_00019910 =
//   cdecl(idx) -> char*: base 0x114C20, 0xFF bytes/entry, 0xE3 entries/language --
//   indices >= 0xE3 never occur in retail data, so they address our custom strings.
#include "hybrid/xdk_patch.h"
#include "hybrid/meat_rush.h"
#include "hybrid/lan_ui.h"
#include "hybrid/audio_ui.h"
#include "hybrid/arabic.h"
#include "hybrid/guest_call.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace tj::hybrid {

bool ResizeDisplay(int w, int h);            // d3d8_bridge.cpp
bool SetDisplayModeKind(int kind);           // d3d8_bridge.cpp (0 windowed/1 borderless/2 fullscreen)
int  GetDisplayModeKind();

// ---- game functions (x86 thiscall thunked through fastcall: ecx=this, edx unused) ----
using FnReady      = uint8_t  (__cdecl*)();                                    // 0x198e0
using FnThis       = void     (__fastcall*)(uint32_t self, uint32_t);
using FnThisU32    = void     (__fastcall*)(uint32_t self, uint32_t, uint32_t);
using FnThisU32U32 = uint8_t  (__fastcall*)(uint32_t self, uint32_t, uint32_t, uint32_t);
// Host->guest seam (guest_call.h): resolved through GuestFnPtr at EVERY call -- engine
// mode arms after all Install*() have run, so a static-init value would freeze the raw
// native address. Native mode: the raw address, exactly as shipped.
#define ItemCtor(...)   GCALL(Fastcall, FnThis,    0x1A340, __VA_ARGS__)  // MenuOptionItem ctor
#define SetLabel(...)   GCALL(Fastcall, FnThisU32, 0x19E70, __VA_ARGS__)  // (u16 label idx)
#define SetValue(...)   GCALL(Fastcall, FnThisU32, 0x19E80, __VA_ARGS__)  // (u8 value idx; 0x24 = none)
#define SetAlign(...)   GCALL(Fastcall, FnThisU32, 0x19E90, __VA_ARGS__)
#define SetScale(...)   GCALL(Fastcall, FnThisU32, 0x1A320, __VA_ARGS__)  // (float as raw bits)
#define AppendItem(...) GCALL(Fastcall, FnThisU32, 0x1D030, __VA_ARGS__)  // Screen::AppendItem(item)

// ---- resolution mode table + current state ----
struct Mode { int w, h; };
static const Mode kModes[] = {
    { 640, 480 }, { 960, 720 }, { 1280, 960 }, { 1600, 1200 },
    { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 },
};
static const int kModeCount = (int)(sizeof(kModes) / sizeof(kModes[0]));
static int  g_mode = 0;              // index into kModes (current/pending)
// The labels are returned to GUEST code by Hk_GetText, so they live in the
// guest-visible arena; defaults are written at install (InstallFeMenu).
static char (&g_resLabel)[64] = *(char(*)[64])GuestObjAlloc(64, 8);
static int  g_dispKind = 0;          // 0 windowed / 1 borderless / 2 fullscreen
static char (&g_dispLabel)[64] = *(char(*)[64])GuestObjAlloc(64, 8);
static const char* kDispNames[3] = { "WINDOWED", "BORDERLESS", "FULLSCREEN" };

static void RefreshLabel() {
    int dk = g_dispKind < 0 || g_dispKind > 2 ? 0 : g_dispKind;
    // These two rows are composed at runtime, so they cannot be plain pack strings -- but
    // they are still written in READING order, because the shaper runs on the finished line
    // and puts the digits where right-to-left text wants them. DISPLAY has three fixed
    // states and is a whole pack string per state.
    const char* pre = ArabicEnabled() ? ArabicText(0x1F1) : nullptr;
    if (pre) {
        _snprintf_s(g_resLabel, sizeof(g_resLabel), _TRUNCATE, "%s %dX%d",
                    pre, kModes[g_mode].w, kModes[g_mode].h);
        const char* d = ArabicText((uint16_t)(0x1F3 + dk));
        _snprintf_s(g_dispLabel, sizeof(g_dispLabel), _TRUNCATE, "%s", d ? d : "");
        return;
    }
    _snprintf_s(g_resLabel, sizeof(g_resLabel), _TRUNCATE, "RESOLUTION: %dX%d",
                kModes[g_mode].w, kModes[g_mode].h);
    _snprintf_s(g_dispLabel, sizeof(g_dispLabel), _TRUNCATE, "DISPLAY: %s", kDispNames[dk]);
}
const char* UserDataDir();          // file_io.cpp -- %LOCALAPPDATA%\TomJerryWOW
static void IniPath(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "%s\\tomjerry.ini", UserDataDir());
}
// Called from InstallFeMenu with the boot resolution the loader chose.
static void SyncModeFromDisplay(int w, int h) {
    g_mode = 0;
    for (int i = 0; i < kModeCount; ++i)
        if (kModes[i].w == w && kModes[i].h == h) { g_mode = i; break; }
    RefreshLabel();
}
static void ApplyAndPersist() {
    char ini[MAX_PATH]; IniPath(ini, sizeof(ini));
    char v[16];
    _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", kModes[g_mode].w);
    WritePrivateProfileStringA("Display", "Width", v, ini);
    _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", kModes[g_mode].h);
    WritePrivateProfileStringA("Display", "Height", v, ini);
    ResizeDisplay(kModes[g_mode].w, kModes[g_mode].h);
    printf("[fe] resolution %dx%d applied + saved to tomjerry.ini\n",
           kModes[g_mode].w, kModes[g_mode].h);
}

// ---- custom localized-text: full native replacement of FUN_00019910 (tiny, formula
// verified against the disassembly; readiness check preserved via the original helper).
static char (&g_exitQ)[64] = *(char(*)[64])GuestObjAlloc(64, 8);  // filled at install
// SAVE-GAME WORDING. The save messages all talk about "the Xbox hard disk" and "your Xbox
// console"; on this port the saves are files in _SAVES next to the executable, so the retail
// text is simply wrong. These override the localized entries in place. Two things must be
// preserved exactly: 0xD6 is used as a printf FORMAT with the shortfall (keep one %d), and
// \x08x / \x08o are the A/B button glyphs.
// WHAT THIS MACHINE IS CALLED, in the player's own words. The retail strings said "Xbox";
// this port replaced that with "PC", which is wrong on the Android build -- the user saw
// "exists on this PC" on a phone. The hybrid layer builds for BOTH targets from these same
// sources, so the word is chosen at COMPILE time and concatenated into the literals: one
// table, no runtime cost, and no way for the two wordings to drift apart.
// ⚠ Line width: these are drawn into the retail dialog box, which was sized for the
// longer original wording ("the Xbox hard disk"), so the extra characters are safe.
#ifdef __ANDROID__
#define TJ_HOST_WORD    "phone"
#define TJ_HOST_WORD_UC "PHONE"
#else
#define TJ_HOST_WORD    "PC"
#define TJ_HOST_WORD_UC "PC"
#endif

static struct { uint16_t idx; const char* text; } kSaveText[] = {   // strings
    // relocated to the guest arena at install (the guest stores the pointers)
    { 0xD0, "No TOM AND JERRY saved game\nexists on this " TJ_HOST_WORD ".\nDo you wish to create a new save?" },
    { 0xD1, "A TOM AND JERRY saved game\nexists on this " TJ_HOST_WORD ".\nDo you wish to load the saved game?" },
    { 0xD2, "Loading saved game. Please don't turn\noff your " TJ_HOST_WORD "." },
    { 0xD4, "Saving game. Please don't turn\noff your " TJ_HOST_WORD "." },
    { 0xD6, "There is not enough free disk space\nto save games.\nYou need to free %d more blocks.\n"
            "Press \x08x to continue without saving\nor \x08o to free more space." },
    { 0xD7, "A TOM AND JERRY saved game already\nexists on this " TJ_HOST_WORD ". Would you\nlike to replace it "
            "with a new save?\nThe old save will be lost." },
};
static char* __cdecl Hk_GetText(uint32_t idxArg) {
    uint16_t idx = (uint16_t)idxArg;
    // Arabic answers FIRST: it is a 7th language the retail 6-slot table cannot hold,
    // so it overrides retail indices rather than adding new ones. Null = not translated.
    if (const char* ar = ArabicText(idx)) return (char*)GuestInternStr(ar);
    switch (idx) {                                            // custom (DLL-side) strings
    case 0xE3: return g_resLabel;
    case 0xE4: return g_dispLabel;
    case 0xE5: return g_exitQ;
    // The Arabic wording of this row comes from the pack (ArabicText answered above); this
    // is the English half, and it must be listed BEFORE the `idx > 0xE5` catch-all.
    // ⚠ INTERNED, like every other host literal below. A raw host-image pointer is fine on
    // x86 (identity, and it fits in 32 bits) but the ARM image loads ABOVE 4 GB, so the
    // guest -- which STORES this pointer -- gets a truncated address and faults the moment it
    // draws the row. It only started crashing when the Android build gained arabic.cpp and
    // ArabicAvailable() finally became true there, so the LANGUAGE row existed for the first
    // time: entering OPTIONS on the phone died instantly.
    case 0x1F0: return (char*)GuestInternStr("LANGUAGE: ENGLISH");
    }
    // Same rule: kSaveText is host .rodata, and the save dialogs would fault on ARM too.
    for (const auto& s : kSaveText) if (s.idx == idx) return (char*)GuestInternStr(s.text);
    // The guest STORES the returned pointer, so any host-image literal a provider hands back
    // must be interned into the guest arena (below 4 GB) — identity passthrough on x86, the
    // relocation on ARM. (MeatMenuText/MeatCustomText return host .rodata; lan_ui/audio_ui
    // already return arena buffers, so intern is a no-op there.)
    if (const char* lan = LanCustomText(idx)) return (char*)GuestInternStr(lan);  // LAN UI rows (0x100+)
    if (const char* a = AudioCustomText(idx)) return (char*)GuestInternStr(a);    // audio sliders (0x1C0+)
    if (const char* mt = MeatCustomText(idx)) return (char*)GuestInternStr(mt);  // MAX MEAT row (0x1A0, 0xE6+)
    if (const char* mm = MeatMenuText(idx)) return (char*)GuestInternStr(mm);    // MULTIPLAYER menu (0x1C8+)
    if (idx > 0xE5) return g_resLabel;                        // unused custom slots
    if (!GCALL0(Cdecl, FnReady, 0x198e0)) return (char*)0x116FFC;    // "" until text ready
    uint32_t lang = *(uint8_t*)(uintptr_t)0x114C18;
    return (char*)(uintptr_t)((lang * 0xE3 + idx) * 0xFF + 0x114C20);
}

// ---- minimal trampoline (prologue lengths hand-verified in the disassembly; the copied
// bytes contain no relative branches) ----
// (call-through trampolines now come from xdk_patch's guest-window pad — MakeGuestTramp)

// ---- OPTIONS screen builder (FUN_00027590) post-hook: append the hidden VIDEO row ----
// ---- LANGUAGE row (OPTIONS) ----------------------------------------------------
// A DLL-side item, deliberately: the OPTIONS A-press dispatch (0x2796D..0x279DA) compares
// the selection against eight FIXED offsets into the screen object, so an item that is not
// one of them falls through to the default and A does nothing. That leaves this row's input
// entirely to the Update hook below, with no retail screen transition to suppress.
// (The retail LANGUAGES item at self+0x3B8 IS in that dispatch -- which is exactly why it
// is not the one used here.)
static uint8_t (&g_langItem)[0x84] = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static const uint16_t kLangLabel = 0x1F0;
static void MakeCustomRow(uint8_t* store, uint16_t label, float y,
                          uint32_t sibling, uint32_t prevIt);   // defined below

static uint32_t Orig_OptionsBuild = 0;   // guest-window trampoline VAs (GCALL them)
static void __fastcall Hk_OptionsBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_OptionsBuild, self, 0);
    uint32_t vid = self + 0x1A8;                 // the fully-built, never-appended item
    *(float*)(uintptr_t)(vid + 0x40) = 0.5f;
    *(float*)(uintptr_t)(vid + 0x44) = 0.65f;    // the free row (CHEATS 0.58, EXIT 0.72)
    // Splice into the child list right after CHEATS MENU (+0x22C) so d-pad order matches
    // the on-screen order (AppendItem put it after EXIT: down from CHEATS skipped to
    // EXIT and VIDEO only came after EXIT -- user-reported).
    uint32_t cheats = self + 0x22C;
    uint32_t after = *(uint32_t*)(uintptr_t)(cheats + 0x60);
    *(uint32_t*)(uintptr_t)(vid + 0x60) = after;
    *(uint32_t*)(uintptr_t)(vid + 0x64) = cheats;
    *(uint32_t*)(uintptr_t)(cheats + 0x60) = vid;
    if (after) *(uint32_t*)(uintptr_t)(after + 0x64) = vid;
    else *(uint32_t*)(uintptr_t)(self + 0x18) = vid;   // was the tail
    if (!ArabicAvailable()) return;                    // no pack shipped: no row to offer
    MakeCustomRow(g_langItem, kLangLabel, 0.66f, cheats, vid);
    // EIGHT rows now. Retail spaced six of them 0.30..0.72 and the port's VIDEO row took
    // the gap at 0.65; rather than squeeze a ninth gap, the whole column is re-spaced
    // evenly between the same two ends, so the screen keeps its retail extent.
    // Retail's own pitch is 0.07 and must be kept: a line's quad is ~0.065 tall at this
    // item scale (taller in Arabic, which needs a descender band), so a tighter pitch makes
    // rows collide. Eight rows at 0.07 start at 0.26 to leave the footer prompts clear.
    static const uint32_t kRow[] = { 0xA0, 0x334, 0x124, 0x2B0, 0x22C, 0x1A8, 0, 0x43C };
    for (int i = 0; i < 8; ++i) {
        uint32_t it = kRow[i] ? self + kRow[i] : (uint32_t)(uintptr_t)g_langItem;
        *(float*)(uintptr_t)(it + 0x44) = 0.26f + 0.07f * i;
    }
}

// ---- VIDEO screen builder (FUN_00028C00) post-hook: splice our RESOLUTION and
// DISPLAY items ----
// The 0x310-byte screen object has no spare slots; the items live in DLL memory (they
// are only ever reached through list pointers). Rebuilt on every FE build (the factory
// re-runs on each return to the frontend).
static uint8_t (&g_resItem)[0x84]  = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_dispItem)[0x84] = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
// Construct a custom menu row: ctor + label + geometry, display fields (colors/justify/
// font) inherited from an existing sibling item, spliced into the list after `prevIt`.
static void MakeCustomRow(uint8_t* store, uint16_t label, float y,
                          uint32_t sibling, uint32_t prevIt) {
    uint32_t it = (uint32_t)(uintptr_t)store;
    memset(store, 0, 0x84);
    ItemCtor(it, 0);
    SetLabel(it, 0, label);
    SetValue(it, 0, 0x24);                       // no separate value string
    SetAlign(it, 0, 1);
    float scale = 0.04f;
    SetScale(it, 0, *(uint32_t*)&scale);
    *(float*)(uintptr_t)(it + 0x40) = 0.5f;
    *(float*)(uintptr_t)(it + 0x44) = y;
    memcpy((void*)(uintptr_t)(it + 0x50), (const void*)(uintptr_t)(sibling + 0x50), 0x10);
    *(uint16_t*)(uintptr_t)(it + 0x70) = *(uint16_t*)(uintptr_t)(sibling + 0x70);
    *(uint32_t*)(uintptr_t)(it + 0x74) = *(uint32_t*)(uintptr_t)(sibling + 0x74);
    uint32_t next = *(uint32_t*)(uintptr_t)(prevIt + 0x60);
    *(uint32_t*)(uintptr_t)(it + 0x60) = next;
    *(uint32_t*)(uintptr_t)(it + 0x64) = prevIt;
    *(uint32_t*)(uintptr_t)(prevIt + 0x60) = it;
    if (next) *(uint32_t*)(uintptr_t)(next + 0x64) = it;
}
static uint32_t Orig_VideoBuild = 0;
static void __fastcall Hk_VideoBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_VideoBuild, self, 0);
    // Diagnostic bisect: TJ_FE_NOROW=1 keeps the VIDEO screen stock (no custom items).
    static int noRow = -1;
    if (noRow < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_FE_NOROW");
                     noRow = e && atoi(e) ? 1 : 0; free(e); }
    if (noRow) return;
    uint32_t ws = self + 0xA0, cf = self + 0x124;
    // Rows: WIDESCREEN 0.30 / RESOLUTION 0.37 / DISPLAY 0.44 / CONFIRM moved to 0.51.
    *(float*)(uintptr_t)(cf + 0x44) = 0.51f;
    MakeCustomRow(g_resItem,  0xE3, 0.37f, cf, ws);
    MakeCustomRow(g_dispItem, 0xE4, 0.44f, cf, (uint32_t)(uintptr_t)g_resItem);
}

// ---- VIDEO screen Enter (FUN_00028BC0) post-hook: show the live resolution ----
static uint32_t Orig_VideoEnter = 0;
static void __fastcall Hk_VideoEnter(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_VideoEnter, self, 0);
    RefreshLabel();
}

// ---- VIDEO screen Update (FUN_00028D30) wrap: L/R cycles the mode on our row; the
// screen's own CONFIRM commits (we apply on the same press). Update signature:
// thiscall(), NO args, plain ret (verified: every exit is `ret`, and what looks like an
// arg read is a zeroed local) -> returns next screen id. Input tests: FUN_00013470 =
// thiscall on the input object [master+0x4BC], (id, pad) -> al; id 3 = left, 4 = right
// (from the WIDESCREEN handler's own usage; it passes pad = 0).
using FnUpdate = uint32_t (__fastcall*)(uint32_t self, uint32_t);
static uint32_t Orig_VideoUpdate = 0;
// Auto-pair the game's WIDESCREEN (anamorphic 16:9 projection, settings byte
// [0x16A254]) with the picked resolution's aspect: a 16:9 window without it renders a
// stretched 4:3 image. The screen's own toggle stays usable as an override -- we also
// sync its local state (+0x120) and value text (21 ON / 22 OFF) so the stock CONFIRM
// path doesn't commit a stale value over ours.
static void PairWidescreen(uint32_t videoScreen) {
    uint8_t ws = (kModes[g_mode].w * 3 != kModes[g_mode].h * 4) ? 1 : 0;
    *(uint8_t*)(uintptr_t)0x16A254 = ws;
    if (videoScreen) {
        *(uint8_t*)(uintptr_t)(videoScreen + 0x120) = ws;
        SetValue(videoScreen + 0xA0, 0, ws ? 0x15 : 0x16);
    }
    printf("[fe] widescreen %s (paired with %dx%d)\n", ws ? "ON" : "OFF",
           kModes[g_mode].w, kModes[g_mode].h);
}
// One-shot boot pairing, called from the frame tick once the save-loaded settings have
// settled: if the saved widescreen flag contradicts the ini resolution's aspect, fix it
// (a 16:9 window without the anamorphic projection renders everything stretched). The
// VIDEO screen's toggle still allows a manual override for the session.
void FeMenuFrameTick(int frame) {
    // LAN session traffic (beacons, browser, lobby, the START handshake) must flow while the
    // simulation is NOT armed, so it is pumped here -- once per presented frame -- as well
    // as from the lockstep tick.
    LanUiFrameTick(frame);
    AudioUiFrameTick(frame);   // apply the volumes saved in tomjerry.ini, once, at the main menu
    ArabicFrameTick(frame);    // re-assert the accent-overlay silencing
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    if (!master) return;
    // TJ_UNLOCK=1 (test harness / opt-in): replicate the retail ALL-ARENAS cheat by
    // writing the settings-blob flags directly (RE: cheat comparator FUN_00020690,
    // ALL-ARENAS action FUN_0002dc20). 13 arena flags @0x16A245 (idx 0 Kitchen ..
    // 12 Marketplace; level-select carousel skips zero flags), 11 character flags
    // @0x16A23A (the retail cheat sets these too), master flag 0x16A256. The blob
    // validator (FUN_0002dd00) range-checks 0/1 only and the saver signs the blob
    // as-is, so the pokes are save-safe. Re-applied EVERY frame: with no save on disk,
    // each frontend rebuild re-runs the save-load path, which RESETS the blob to
    // locked defaults — a once-only poke got undone after the first match (observed:
    // arena-sweep level-select rights clamped at Beach again from cycle 3).
    static int unlock = -1;
    if (unlock < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_UNLOCK");
                      unlock = e && atoi(e) ? 1 : 0; free(e); }
    if (unlock) {
        memset((void*)(uintptr_t)0x16A245, 1, 13);   // all arenas
        memset((void*)(uintptr_t)0x16A23A, 1, 11);   // all characters (retail cheat parity)
        *(uint8_t*)(uintptr_t)0x16A252 = 1;
        *(uint8_t*)(uintptr_t)0x16A256 = 1;
        static bool announced = false;
        if (!announced) { announced = true;
            printf("[fe] TJ_UNLOCK: all arenas + characters unlocked (re-applied per frame)\n"); }
    }
    // Frontend flow tracer (always on, throttled to transitions): screen-id changes +
    // the level-select carousel cursor (screen 0x10, idx @+0x1A6). This is the
    // instrument that ends blind input-script debugging: the log shows exactly which
    // screen each scripted press landed on and which carousel moves registered.
    {
        static uint8_t lastScr = 0xFF; static int lastCur = -1;
        uint32_t mgr0 = *(uint32_t*)(uintptr_t)(master + 0x4D4);
        if (mgr0) {
            uint8_t scr = *(uint8_t*)(uintptr_t)mgr0;
            if (scr != lastScr) { printf("[fe] screen %u -> %u (frame %d)\n", lastScr, scr, frame); lastScr = scr; lastCur = -1; }
            if (scr == 0x10) {
                int cur = *(uint16_t*)(uintptr_t)(mgr0 + 0x1A6);
                if (cur != lastCur) { printf("[fe] map cursor -> %d (frame %d)\n", cur, frame); lastCur = cur; }
            }
            if (scr == 4) {          // main menu: log the selected-item offset (for @menu)
                static uint32_t lastSel = 0;
                uint32_t sel = *(uint32_t*)(uintptr_t)(mgr0 + 4);
                if (sel != lastSel) { printf("[fe] menu sel +0x%X (frame %d)\n", sel - mgr0, frame); lastSel = sel; }
            }
        }
    }
    // Trigger once when the MAIN MENU (screen id 4) is first reached: by then the boot
    // save prompt has finished, and a loaded save has already overwritten the settings
    // blob (0x16A1F8..0x16A26B, widescreen included) -- pairing any earlier gets undone.
    static bool paired = false;
    if (paired) return;
    uint32_t mgr = *(uint32_t*)(uintptr_t)(master + 0x4D4);
    if (!mgr || *(uint32_t*)(uintptr_t)mgr != 4) return;
    paired = true;
    uint8_t want = (kModes[g_mode].w * 3 != kModes[g_mode].h * 4) ? 1 : 0;
    if (*(uint8_t*)(uintptr_t)0x16A254 != want) PairWidescreen(0);
}
static uint32_t __fastcall Hk_VideoUpdate(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t selBefore = *(uint32_t*)(uintptr_t)(self + 4);
    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_VideoUpdate, self, 0);
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t input = master ? *(uint32_t*)(uintptr_t)(master + 0x4BC) : 0;
    if (!input) return r;
    if (selBefore == (uint32_t)(uintptr_t)g_resItem) {
        uint8_t left  = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 3, 0);
        uint8_t right = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 4, 0);
        if (left || right) {
            g_mode = (g_mode + (right ? 1 : kModeCount - 1)) % kModeCount;
            RefreshLabel();
            ApplyAndPersist();
            PairWidescreen(self);
        }
    } else if (selBefore == (uint32_t)(uintptr_t)g_dispItem) {
        uint8_t left  = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 3, 0);
        uint8_t right = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 4, 0);
        if (left || right) {
            g_dispKind = (g_dispKind + (right ? 1 : 2)) % 3;
            RefreshLabel();
            if (SetDisplayModeKind(g_dispKind)) {
                g_dispKind = GetDisplayModeKind();   // fullscreen may fall back
                RefreshLabel();
                char ini[MAX_PATH]; IniPath(ini, sizeof(ini));
                char v[8]; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_dispKind);
                WritePrivateProfileStringA("Display", "Mode", v, ini);
            }
        }
    }
    return r;
}

// ---- OPTIONS screen Update (FUN_000278C0) wrap: the LANGUAGE row ----------------
// Left, right or A all toggle -- the row shows the CURRENT language, so there are only two
// states and no direction to get wrong. English mode is the untouched retail text path, so
// switching is a flag: every menu item re-fetches its label through Hk_GetText as it draws,
// and the whole screen comes back in the other language on the next frame.
static uint32_t Orig_OptionsUpdate = 0;
static uint32_t __fastcall Hk_OptionsUpdate(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t selBefore = *(uint32_t*)(uintptr_t)(self + 4);
    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_OptionsUpdate, self, 0);
    if (selBefore != (uint32_t)(uintptr_t)g_langItem) return r;
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t input = master ? *(uint32_t*)(uintptr_t)(master + 0x4BC) : 0;
    if (!input) return r;
    uint8_t left  = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 3, 0);
    uint8_t right = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 4, 0);
    uint8_t press = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 1, 0);
    if (left || right || press) {
        ArabicSetEnabled(!ArabicEnabled());
        ArabicSave();
        RefreshLabel();          // the VIDEO rows are composed, not looked up: rebuild them
        LanMenuRefreshText();    // ...and so is the main menu's LAN GAME row (lan_ui g_txt)
    }
    return r;
}

// ---- MAIN MENU (screen id 4, vtbl 0xEFC84): EXIT item + quit-confirm dialog ----
// The menu ships a cut VERSUS item at +0x1AC whose A-press site still exists in the
// update (returns screen id 0x0E). We relabel it EXIT (retail string 0x6A), append it,
// and intercept the 0x0E return to open a confirm dialog built exactly like the game's
// own boot save prompt (screen 1): a message line + YES/NO items at x 0.25/0.75,
// left/right to choose (input ids 3/4), A (id 1) to confirm, B (id 2) to cancel,
// default NO. Retail strings: 0xD8 "Are you sure you want to quit?", 0x27 YES, 0x28 NO.
static uint8_t (&g_exitMsg)[0x84] = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_exitYes)[0x84] = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_exitNo)[0x84]  = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static bool    g_exitModal = false;
static uint8_t g_savedVis[24]; static uint32_t g_savedItems[24]; static int g_savedN = 0;
using FnSetSel = void (__fastcall*)(uint32_t self, uint32_t, uint32_t item, uint32_t cursor);
#define SetSelected(...) GCALL(Fastcall, FnSetSel, 0x1D230, __VA_ARGS__)
static void PlayUiSound(uint32_t id) {
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t snd = master ? *(uint32_t*)(uintptr_t)(master + 0x1C904) : 0;
    if (snd) GCALL(Fastcall, FnThisU32, 0x705E0, snd, 0, id);
}
static void ExitModalShow(uint32_t self, bool show) {
    // toggle the stock items' visibility (walk the child list, skipping ours)
    if (show) {
        g_savedN = 0;
        for (uint32_t it = *(uint32_t*)(uintptr_t)(self + 0x14); it && g_savedN < 24;
             it = *(uint32_t*)(uintptr_t)(it + 0x60)) {
            if (it == (uint32_t)(uintptr_t)g_exitMsg || it == (uint32_t)(uintptr_t)g_exitYes ||
                it == (uint32_t)(uintptr_t)g_exitNo) continue;
            g_savedItems[g_savedN] = it;
            g_savedVis[g_savedN++] = *(uint8_t*)(uintptr_t)(it + 0x48);
            *(uint8_t*)(uintptr_t)(it + 0x48) = 0;
        }
    } else {
        for (int i = 0; i < g_savedN; ++i)
            *(uint8_t*)(uintptr_t)(g_savedItems[i] + 0x48) = g_savedVis[i];
    }
    uint8_t vis = show ? 1 : 0;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitMsg + 0x48) = vis;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x48) = vis;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x48) = vis;
    // selectable only while shown (nav ignores visibility, so keep them unreachable
    // from the normal menu)
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x49) = vis;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x49) = vis;
    for (uint32_t c = 0; c < 4; ++c)
        SetSelected(self, 0, (uint32_t)(uintptr_t)(show ? g_exitNo : (uint8_t*)(uintptr_t)(self + 0x20)), c);
    g_exitModal = show;
}
static uint32_t Orig_MenuBuild = 0;
static void __fastcall Hk_MenuBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_MenuBuild, self, 0);
    g_exitModal = false;
    uint32_t ex = self + 0x1AC;                  // the cut VERSUS item -> EXIT
    SetLabel(ex, 0, 0x6A);                       // "EXIT"
    *(float*)(uintptr_t)(ex + 0x44) = 0.85f;     // below OPTIONS (0.7)
    *(uint8_t*)(uintptr_t)(ex + 0x48) = 1;       // visible
    *(uint8_t*)(uintptr_t)(ex + 0x49) = 1;       // selectable
    *(uint8_t*)(uintptr_t)(ex + 0x4B) = 0;       // not disabled
    AppendItem(self, 0, ex);
    // confirm-dialog items (hidden until EXIT is pressed), styled like the EXIT row
    MakeCustomRow(g_exitMsg, 0xD8, 0.40f, ex, ex);           // "Are you sure...?"
    MakeCustomRow(g_exitYes, 0x27, 0.60f, ex, (uint32_t)(uintptr_t)g_exitMsg);
    MakeCustomRow(g_exitNo,  0x28, 0.60f, ex, (uint32_t)(uintptr_t)g_exitYes);
    *(float*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x40) = 0.25f;
    *(float*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x40) = 0.75f;
    uint32_t hide[3] = { (uint32_t)(uintptr_t)g_exitMsg, (uint32_t)(uintptr_t)g_exitYes,
                         (uint32_t)(uintptr_t)g_exitNo };
    for (uint32_t it : hide) { *(uint8_t*)(uintptr_t)(it + 0x48) = 0;
                               *(uint8_t*)(uintptr_t)(it + 0x49) = 0; }
    LanMenuBuild(self);          // LAN GAME row on the other cut item (+0x230 -> screen 5)
    MeatMenuBuild(self);         // MULTIPLAYER row on +0x2B4; QUICK GAME/TOURNAMENT move into it
}
static uint32_t Orig_MenuUpdate = 0;
static uint32_t __fastcall Hk_MenuUpdate(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t input = master ? *(uint32_t*)(uintptr_t)(master + 0x4BC) : 0;
    if (g_exitModal) {
        // modal: the stock update never runs (no nav, no attract timer)
        if (input) for (uint32_t pad = 0; pad < 4; ++pad) {
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 3, pad)) {   // left -> YES
                PlayUiSound(5);
                for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, (uint32_t)(uintptr_t)g_exitYes, c);
            }
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 4, pad)) {   // right -> NO
                PlayUiSound(5);
                for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, (uint32_t)(uintptr_t)g_exitNo, c);
            }
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 2, pad)) {   // B = cancel
                PlayUiSound(1);
                ExitModalShow(self, false);
                return 4;
            }
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 1, pad)) {   // A = confirm
                PlayUiSound(0x13);
                bool yes = *(uint32_t*)(uintptr_t)(self + 4) == (uint32_t)(uintptr_t)g_exitYes;
                ExitModalShow(self, false);
                if (yes) {
                    printf("[fe] EXIT confirmed -- quitting\n"); fflush(stdout);
                    ExitProcess(0);
                }
                return 4;
            }
        }
        return 4;
    }
    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_MenuUpdate, self, 0);
    // MULTIPLAYER is a DLL-side item, so retail's A dispatch does not know it: catch the
    // press here. (The cut +0x2B4 slot was tried first and is unusable -- FUN_00021010
    // moves the cursor off it whenever fewer than two pads are connected.)
    if (input) {
        uint32_t sel = *(uint32_t*)(uintptr_t)(self + 4);
        if (sel == MeatMenuRow())
            for (uint32_t pad = 0; pad < 4; ++pad)
                if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 1, pad)) return 14;
    }
    if (r == 0x0E) {                              // the EXIT item's press site fired
        PlayUiSound(0x13);
        ExitModalShow(self, true);
        return 4;
    }
    return LanMenuUpdate(self, r);                // r == 5 = the LAN row; also the launch gate
}

int InstallFeMenu() {
    // Guest-visible text: defaults for the arena-resident labels, and the save-text
    // literals duplicated into the arena (the guest stores Hk_GetText's pointers).
    if (!g_resLabel[0])  strcpy_s(g_resLabel, "RESOLUTION: 640X480");
    if (!g_dispLabel[0]) strcpy_s(g_dispLabel, "DISPLAY: WINDOWED");
    if (!g_exitQ[0])     strcpy_s(g_exitQ, "ARE YOU SURE YOU WANT TO EXIT?");
    static bool dupped = false;
    if (!dupped) {
        dupped = true;
        for (auto& st : kSaveText) st.text = GuestStrDup(st.text);
    }
    // Boot resolution = same ini read (and clamp) the loader did for EnsureDisplay.
    char ini[MAX_PATH]; IniPath(ini, sizeof(ini));
    int bootW = (int)GetPrivateProfileIntA("Display", "Width",  640, ini);
    int bootH = (int)GetPrivateProfileIntA("Display", "Height", 480, ini);
    if (bootW < 320 || bootW > 7680 || bootH < 240 || bootH > 4320) { bootW = 640; bootH = 480; }
    SyncModeFromDisplay(bootW, bootH);
    // Boot display mode (0 windowed / 1 borderless / 2 exclusive fullscreen).
    int bootDisp = (int)GetPrivateProfileIntA("Display", "Mode", 0, ini);
    if (bootDisp >= 1 && bootDisp <= 2) {
        SetDisplayModeKind(bootDisp);
        g_dispKind = GetDisplayModeKind();       // may have fallen back
    }
    // Widescreen at boot: keep the saved setting ([0x16A254] loads from the save later);
    // it re-pairs whenever the user changes resolution in the menu.
    RefreshLabel();
    // Trampolines FIRST (PatchJump overwrites the prologues). Lengths hand-verified:
    // 0x27590/0x28C00: push ebx/ebp/esi + mov esi,ecx + push edi = 6 bytes;
    // 0x28BC0: mov al,[0x16A254] = 5 bytes; 0x28D30: push ecx + mov eax,[imm] = 6 bytes.
    Orig_OptionsBuild = MakeGuestTramp(0x27590, 6, "fe:tr.optbuild");
    Orig_VideoBuild   = MakeGuestTramp(0x28C00, 6, "fe:tr.vidbuild");
    Orig_VideoEnter   = MakeGuestTramp(0x28BC0, 5, "fe:tr.videnter");
    Orig_VideoUpdate  = MakeGuestTramp(0x28D30, 6, "fe:tr.vidupdate");
    // main menu (screen 4): builder push ebx/ebp/esi + mov esi,ecx + push edi = 6 bytes;
    // update push ecx + mov eax,[imm32] = 6 bytes (both verified in the disassembly).
    Orig_MenuBuild    = MakeGuestTramp(0x21140, 6, "fe:tr.menubuild");
    Orig_MenuUpdate   = MakeGuestTramp(0x212D0, 6, "fe:tr.menuupdate");
    if (!Orig_OptionsBuild || !Orig_VideoBuild || !Orig_VideoEnter || !Orig_VideoUpdate ||
        !Orig_MenuBuild || !Orig_MenuUpdate) {
        printf("[fe] trampoline alloc failed -- menu injection skipped\n");
        return 0;
    }
    int n = 0;
    n += PatchJump(0x21140, HOOK_FC(Hk_MenuBuild),  "FE_MenuBuild");
    n += PatchJump(0x212D0, HOOK_FC(Hk_MenuUpdate), "FE_MenuUpdate");
    // Diagnostic bisect: TJ_FE_NOVID=1 leaves the VIDEO screen's enter/update stock.
    char* e = nullptr; size_t sz = 0; _dupenv_s(&e, &sz, "TJ_FE_NOVID");
    bool noVid = e && atoi(e); free(e);
    n += PatchJump(0x19910, HOOK_CDECL(Hk_GetText),  "FE_GetText");
    n += PatchJump(0x27590, HOOK_FC(Hk_OptionsBuild),"FE_OptionsBuild");
    n += PatchJump(0x28C00, HOOK_FC(Hk_VideoBuild),  "FE_VideoBuild");
    if (!noVid) {
        n += PatchJump(0x28BC0, HOOK_FC(Hk_VideoEnter),  "FE_VideoEnter");
        n += PatchJump(0x28D30, HOOK_FC(Hk_VideoUpdate), "FE_VideoUpdate");
    }
    // OPTIONS Update (vtable 0xEF06C+0x18). Prologue 51 (push ecx) + A1 0C 47 5C 01
    // (mov eax,[0x15C470C]) = 6 bytes, no relative branch.
    Orig_OptionsUpdate = MakeGuestTramp(0x278C0, 6, "fe:tr.optupdate");
    if (Orig_OptionsUpdate)
        n += PatchJump(0x278C0, HOOK_FC(Hk_OptionsUpdate), "FE_OptionsUpdate");
    else
        printf("[fe] WARN: OPTIONS update trampoline failed -- no LANGUAGE row input\n");
    // Retail bug hidden by the cut: the transition early-out returns screen id 8
    // (AUDIO) instead of 9 (VIDEO). mov eax,8 at 0x28D62 -> imm32 at 0x28D63.
    if (*(uint8_t*)(uintptr_t)0x28D62 == 0xB8 && *(uint32_t*)(uintptr_t)0x28D63 == 8) {
        DWORD old; VirtualProtect((void*)(uintptr_t)0x28D63, 4, PAGE_EXECUTE_READWRITE, &old);
        *(uint32_t*)(uintptr_t)0x28D63 = 9;
        VirtualProtect((void*)(uintptr_t)0x28D63, 4, old, &old);
        ++n;
    } else {
        printf("[fe] WARN: transition-id bytes @0x28D62 unexpected -- imm patch skipped\n");
    }
    printf("[fe] menu injection installed (%d patches), boot mode %dx%d\n", n, bootW, bootH);
    return n;
}

} // namespace tj::hybrid
