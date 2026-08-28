// MULTIPLAYER -- a real main-menu entry holding QUICK GAME, TOURNAMENT and MEAT RUSH.
//
// MEAT RUSH IS A GAME MODE, NOT A SETTING. The main menu becomes
//     CHALLENGE / MULTIPLAYER / LAN GAME / OPTIONS / EXIT
// and QUICK GAME + TOURNAMENT move off it into the submenu beside MEAT RUSH. FIGHT SETTINGS
// keeps only the mode's RULE (MAX MEAT), the way TIME and ROUNDS are rules.
//
// The submenu is hosted on DEAD SCREEN 14 (vtable 0xEF1A8): all three of its vtable functions
// are single-referenced, so it takes three PatchJumps and no vtable surgery -- the same recipe
// lan_ui.cpp already uses for the browser (5) and the lobby (15). The originals are never
// called: the cut screen's own widgets are never appended, so they never draw.
//
// FOUR THINGS THAT BREAK IF THEY ARE SKIPPED, all found in the recon and all handled here:
//
//  1. THE MULTIPLAYER ROW IS A DLL-SIDE ITEM, NOT THE CUT +0x2B4 SLOT. That slot looks free,
//     but retail still runs logic over it every frame: FUN_000212D0 rewrites its
//     selectable/disabled bytes from the pad count, and FUN_00021010 actively MOVES THE CURSOR
//     OFF it when fewer than two pads are connected (0x21099-0x210BD). A row built there is
//     unusable with one pad -- measured: the first A press landed on CHALLENGE instead. A
//     DLL-side item has none of that baggage, and its A press is handled in the menu's Update
//     hook, which the port already owns.
//
//  2. SCREEN 11's INIT. The frontend manager only runs screen 11's fresh setup (FUN_00026260 --
//     team mode, and the Quick Game / Tournament title) when the PREVIOUS screen was 4, tested
//     by `cmp eax, 4` at 0x1D48E. Coming from the submenu it would arrive stale. Since QUICK
//     GAME and TOURNAMENT are now reachable ONLY through the submenu, the imm8 is repointed
//     4 -> 14, which is exactly right rather than a workaround.
//
//  3. THE A-PRESS TAILS. Retail's main menu did state setup before handing over to screen 11
//     (FE+0x508 tournament flag, MASTER+0x1C90D, FE+0x501 arena, FE+0x502 family). Those rows
//     are no longer in the menu's dispatch, so the submenu does that work itself.
//
//  4. BACK. Screen 11 hardcodes `mov eax,4`, so B from the setup screen lands on the main menu
//     rather than here. That is left alone deliberately -- it matches every other retail
//     sub-screen and needs no extra hook.
#include "hybrid/meat_rush.h"
#include "hybrid/xdk_patch.h"
#include "hybrid/guest_call.h"

#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace tj::hybrid {

using FnThis    = void    (__fastcall*)(uint32_t self, uint32_t);
using FnThisU32 = void    (__fastcall*)(uint32_t self, uint32_t, uint32_t);
using FnThis2   = void    (__fastcall*)(uint32_t self, uint32_t, uint32_t, uint32_t);
using FnInputT  = uint8_t (__fastcall*)(uint32_t self, uint32_t, uint32_t id, uint32_t pad);

// Host->guest seam (guest_call.h): resolved through GuestFnPtr at EVERY call -- engine
// mode arms after all Install*() have run, so a static-init value would freeze the raw
// native address. Native mode: the raw address, exactly as shipped.
#define ItemCtor(...)    GCALL(Fastcall, FnThis,    0x1A340, __VA_ARGS__)
#define SetLabel(...)    GCALL(Fastcall, FnThisU32, 0x19E70, __VA_ARGS__)
#define SetValue(...)    GCALL(Fastcall, FnThisU32, 0x19E80, __VA_ARGS__)
#define SetAlign(...)    GCALL(Fastcall, FnThisU32, 0x19E90, __VA_ARGS__)
#define SetScale(...)    GCALL(Fastcall, FnThisU32, 0x1A320, __VA_ARGS__)
#define AppendItem(...)  GCALL(Fastcall, FnThisU32, 0x1D030, __VA_ARGS__)
#define SetSelected(...) GCALL(Fastcall, FnThis2,   0x1D230, __VA_ARGS__)
#define InputTest(...)   GCALL(Fastcall, FnInputT,  0x13470, __VA_ARGS__)

static const uint32_t kMasterPtr  = 0x15C470C;
static const uint32_t kMultiBuild = 0x2C570;   // screen 14 (dead) -- all three unique
static const uint32_t kMultiUpd   = 0x2C970;
static const uint32_t kMultiEnter = 0x2C3F0;
static const uint32_t kScr11Prev  = 0x1D490;   // imm8 of `cmp eax,4` at 0x1D48E
static const uint32_t kScr11Title = 0x262AC;   // `call 0x19E70` in FUN_00026260 -- the setup
                                               // screen's title, 0xCC Quick Game / 0xCD Tournament

// custom strings: 0x1C8..0x1CF, clear of lan_ui (0x100..0x19F), meat_ui (0x1A0) and audio (0x1C0)
static const uint16_t kTextBase = 0x1C8;
enum { T_TITLE = 0, T_QUICK, T_TOURN, T_MEAT, T_COUNT };
static const char* const kText[T_COUNT] = { "MULTIPLAYER", "QUICK GAME", "TOURNAMENT", "MEAT RUSH" };

const char* MeatMenuText(uint16_t idx) {
    return (idx >= kTextBase && idx < kTextBase + T_COUNT) ? kText[idx - kTextBase] : nullptr;
}

static uint8_t (&g_title)[0x84]   = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_rows)[3][0x84] = *(uint8_t(*)[3][0x84])GuestObjAlloc(3 * 0x84, 8);
static uint8_t (&g_multi)[0x84]   = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static int  g_sel = 0;
static bool g_launch = false;    // FIGHT SETTINGS confirmed; go on to the setup screen
static bool g_installed = false;

static inline uint8_t*  U8(uint32_t va)  { return (uint8_t*)(uintptr_t)va; }
static inline uint32_t* U32(uint32_t va) { return (uint32_t*)(uintptr_t)va; }
static uint32_t Master() { return *(volatile uint32_t*)(uintptr_t)kMasterPtr; }
static uint32_t FeMgr()  { uint32_t m = Master(); return m ? *U32(m + 0x4D4) : 0; }
uint32_t MeatMenuRow() { return (uint32_t)(uintptr_t)g_multi; }
void MeatMenuLaunch() { g_launch = true; }

// The 4-player setup screen titles itself "QUICK GAME" or "TOURNAMENT" from FE+0x508. MEAT
// RUSH is neither, so the one SetLabel call that writes it is intercepted and given our own
// string instead. Retail's two choices are untouched.
static void __fastcall Hk_Scr11Title(uint32_t item, uint32_t edx, uint32_t idx) {
    SetLabel(item, edx, MeatRushActive() ? (uint32_t)(kTextBase + T_MEAT) : idx);
}

static uint32_t MakeItem(uint8_t* mem, uint16_t label, float y, float scale, bool selectable) {
    uint32_t it = (uint32_t)(uintptr_t)mem;
    memset(mem, 0, 0x84);
    ItemCtor(it, 0);
    SetLabel(it, 0, label);
    SetValue(it, 0, 0x24);                      // no value column
    SetAlign(it, 0, 1);                         // centre
    SetScale(it, 0, *(const uint32_t*)&scale);
    *(float*)(uintptr_t)(it + 0x40) = 0.5f;
    *(float*)(uintptr_t)(it + 0x44) = y;
    *U8(it + 0x48) = 1;
    *U8(it + 0x49) = selectable ? 1 : 0;
    *U8(it + 0x4B) = 0;
    return it;
}

static void Select(uint32_t self, int row) {
    g_sel = (row + 3) % 3;
    uint32_t it = (uint32_t)(uintptr_t)g_rows[g_sel];
    for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, it, c);
}

// ---- screen 14 ---------------------------------------------------------------
static void __fastcall Hk_MultiBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    const float kTitleScale = 0.075f, kRowScale = 0.055f;
    AppendItem(self, 0, MakeItem(g_title, kTextBase + T_TITLE, 0.18f, kTitleScale, false));
    AppendItem(self, 0, MakeItem(g_rows[0], kTextBase + T_QUICK, 0.40f, kRowScale, true));
    AppendItem(self, 0, MakeItem(g_rows[1], kTextBase + T_TOURN, 0.53f, kRowScale, true));
    AppendItem(self, 0, MakeItem(g_rows[2], kTextBase + T_MEAT,  0.66f, kRowScale, true));
    Select(self, 0);
}

static void __fastcall Hk_MultiEnter(uint32_t self, uint32_t edx) {
    (void)edx;
    Select(self, 0);
}


// The rows this port added are driven by our OWN input tests, so they have to make their own
// sound -- retail plays its click from inside the code that owns its rows. Same object and
// same ids the LAN screens use: 5 = cursor move, 0x13 = accept, 1 = back.
static void UiSfx(uint32_t id) {
    uint32_t m = *(volatile uint32_t*)(uintptr_t)kMasterPtr;
    uint32_t snd = m ? *U32(m + 0x1C904) : 0;
    if (snd) GCALL(Fastcall, FnThisU32, 0x705E0, snd, 0, id);
}

static uint32_t __fastcall Hk_MultiUpdate(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t m = Master(), fe = FeMgr();
    uint32_t in = m ? *U32(m + 0x4BC) : 0;
    if (!fe) return 14;
    if (g_launch) { g_launch = false; return 11; }   // MEAT RUSH's settings were confirmed
    if (!in) return 14;

    for (uint32_t pad = 0; pad < 4; ++pad) {
        if (InputTest(in, 0, 6, pad)) { Select(self, g_sel + 1); UiSfx(5); break; }
        if (InputTest(in, 0, 5, pad)) { Select(self, g_sel - 1); UiSfx(5); break; }
    }
    for (uint32_t pad = 0; pad < 4; ++pad) {
        if (InputTest(in, 0, 2, pad)) { UiSfx(1); return 4; }   // B -> main menu
        if (!InputTest(in, 0, 1, pad)) continue;                   // A
        // What the retail main-menu tails did before handing over to screen 11 (0x215A0 /
        // 0x215C1 / the shared 0x215D3): tournament flag, arena reset, and the QUICK GAME
        // family id. Screen 11's own fresh setup then runs, because the manager's prev-screen
        // test at 0x1D48E now names this screen.
        UiSfx(0x13);
        bool tourn = (g_sel == 1);
        *U8(fe + 0x508) = tourn ? 1 : 0;
        if (tourn && m) *U8(m + 0x1C90D) = 1;
        *U8(fe + 0x501) = 0;
        *U8(fe + 0x502) = 4;
        MeatRushSetMode(g_sel == 2);
        printf("[meat] MULTIPLAYER -> %s\n", kText[T_QUICK + g_sel]);
        // A DEDICATED FIGHT SETTINGS PASS FOR MEAT RUSH IS NOT WIRED UP YET: routing this to
        // screen 7 and back reached the screen but never returned from it, so it is left going
        // straight to the setup screen. MAX MEAT is settable from OPTIONS > FIGHT SETTINGS.
        MeatRushApplySetting();
        return 11;                                                 // the 4-player setup screen
    }
    return 14;
}

// ---- main menu ---------------------------------------------------------------
// Called from fe_menu's Hk_MenuBuild AFTER LanMenuBuild, so it owns the final layout.
static void Unsplice(uint32_t self, uint32_t it) {
    uint32_t prev = *U32(it + 0x64), next = *U32(it + 0x60);
    if (prev) *U32(prev + 0x60) = next;
    else      *U32(self + 0x14) = next;                            // it was the head
    if (next) *U32(next + 0x64) = prev;
    else      *U32(self + 0x18) = prev;                            // it was the tail
    *U32(it + 0x60) = 0; *U32(it + 0x64) = 0;
    *U8(it + 0x48) = 0; *U8(it + 0x49) = 0;                        // invisible, unselectable
}

void MeatMenuBuild(uint32_t self) {
    // ⚠ THE MODE IS A CHOICE, NOT A STICKY GLOBAL. It used to be set ONLY here-ish -- in the
    // MULTIPLAYER submenu's A-press (Hk_MultiUpdate) -- and cleared NOWHERE. CHALLENGE is a
    // main-menu row that never passes through that submenu, so a player who ran a MEAT RUSH
    // match and then started CHALLENGE got a CHALLENGE whose every stage spawned meat: g_mode
    // was still true from the previous selection. Reported from the field on BOTH platforms.
    // The main menu is the one screen every mode is chosen from, and rebuilding it is exactly
    // the moment the previous choice stops being in effect -- so the default is cleared here
    // and only an explicit selection (this submenu, or the LAN lobby's MODE row) turns it on.
    // Match series never come back through here, so nothing mid-tournament is disturbed.
    if (MeatRushActive())
        printf("[meat] main menu: MEAT RUSH cleared (a mode choice does not outlive the menu)\n");
    MeatRushSetMode(false);
    uint32_t chal = self + 0x20, quick = self + 0xA4, tour = self + 0x128,
             lan = self + 0x230, opts = self + 0x338, exit_ = self + 0x1AC;
    Unsplice(self, quick);                       // QUICK GAME and TOURNAMENT move into the
    Unsplice(self, tour);                        // submenu; the objects stay, unappended

    // Match CHALLENGE's own scale and colours so the new row is indistinguishable from the
    // retail ones.
    uint32_t multi = MakeItem(g_multi, kTextBase + T_TITLE, 0.0f,
                              *(const float*)(uintptr_t)(chal + 0x68), true);
    memcpy((void*)(uintptr_t)(multi + 0x50), (const void*)(uintptr_t)(chal + 0x50), 0x10);
    *U32(multi + 0x70) = *U32(chal + 0x70);
    *U32(multi + 0x74) = *U32(chal + 0x74);
    // splice MULTIPLAYER directly after CHALLENGE
    uint32_t after = *U32(chal + 0x60);
    *U32(multi + 0x60) = after;
    *U32(multi + 0x64) = chal;
    *U32(chal + 0x60) = multi;
    if (after) *U32(after + 0x64) = multi;
    else *U32(self + 0x18) = multi;

    // five rows now, so re-space them all
    static const float kY[5] = { 0.26f, 0.38f, 0.50f, 0.62f, 0.74f };
    *(float*)(uintptr_t)(chal  + 0x44) = kY[0];
    *(float*)(uintptr_t)(multi + 0x44) = kY[1];
    *(float*)(uintptr_t)(lan   + 0x44) = kY[2];
    *(float*)(uintptr_t)(opts  + 0x44) = kY[3];
    *(float*)(uintptr_t)(exit_ + 0x44) = kY[4];

    // Retail Build leaves the cursor on QUICK GAME, which no longer exists in this list -- and
    // a selection pointing at an unlinked item makes the first d-pad press land anywhere.
    // MULTIPLAYER is the natural replacement for it.
    for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, multi, c);
}

int InstallMeatMenu() {
    if (g_installed) return 0;
    g_installed = true;
    int n = 0;
    n += PatchJump(kMultiBuild, HOOK_FC(Hk_MultiBuild),  "meat:multi.build");
    n += PatchJump(kMultiUpd,   HOOK_FC(Hk_MultiUpdate), "meat:multi.update");
    n += PatchJump(kMultiEnter, HOOK_FC(Hk_MultiEnter),  "meat:multi.enter");
    n += PatchCallSite(kScr11Title, HOOK_FC(Hk_Scr11Title), "meat:setup title");

    DWORD old = 0;
    // Guarded byte check: if the site is not what the RE recorded, leave it alone rather than
    // scribble on whatever moved there.
    if (*U8(kScr11Prev) == 4 && VirtualProtect(U8(kScr11Prev), 1, PAGE_EXECUTE_READWRITE, &old)) {
        *U8(kScr11Prev) = 14;                                      // screen 11 inits from US
        VirtualProtect(U8(kScr11Prev), 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), U8(kScr11Prev), 1);
        ++n;
    } else printf("[meat] WARNING: screen-11 prev test at %05X is not `cmp eax,4`\n", kScr11Prev);

    printf("[meat] MULTIPLAYER menu installed (%d/5)\n", n);
    return n == 5 ? 0 : 1;
}

}  // namespace tj::hybrid
