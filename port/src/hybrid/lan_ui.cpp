// Phase-2 LAN, stage 6 -- the in-game UI.
//
// DESIGN RULE: it must look like the 2003 disc shipped with it. Every row here is a real
// retail MenuOptionItem (ctor FUN_0001A340, 0x84 BYTES) drawn by the game's own item
// renderer through the game's own font, with the game's own move/confirm/back SFX. Nothing
// is drawn through the D3D11 bridge.
//
// WHERE THE SCREENS LIVE. The retail frontend pre-allocates 22 screen objects in an array
// at FE_MGR+4+id*4 and calls Build on every one of them at each frontend rebuild
// (FUN_0001FD80 -- the allocation sizes and constructors in that function are what pin the
// ids down). Two of those ids are DEAD: id 5 (0x4568 bytes, ctor 0x1E190, vtable 0xEF048)
// and id 15 (0x456C, ctor 0x1EA40, vtable 0xEF1D4) are cut multiplayer setup screens that
// nothing in retail ever navigates to. LAN_PLAN proposed swapping their vtables for
// DLL-side copies; replacing their three unique vtable entries outright (Build 0x2A300 /
// Update 0x2A4F0 / Enter 0x2A020 and 0x294A0 / 0x29690 / 0x291A0) is simpler, has no
// object-lifetime questions, and leaves the base class's Render, AppendItem and navigation
// -- which is what makes these behave like retail screens -- completely untouched. The
// objects' own sub-widgets are simply never appended, so they never draw.
//
// The route in is the CUT main-menu item at screen4+0x230 ("TAG VERSUS"): its A-press site
// already ends `mov eax,5; ret` (0x218AA), so relabelling and appending it is the whole
// navigation change. That branch also sets the leftover TAG preset flags FE+0x4C9/+0x4CA,
// which our Enter normalises back to a plain QUICK GAME setup.
#include "lan_ui.h"
#include "arabic.h"
#include "lan_match.h"
#include "net_lan.h"
#include "net_sync.h"
#include "xdk_patch.h"
#include "hybrid/guest_call.h"

#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

namespace tj::hybrid {

// --- game entry points -------------------------------------------------------
using FnThis       = void     (__fastcall*)(uint32_t self, uint32_t);
using FnThisU32    = void     (__fastcall*)(uint32_t self, uint32_t, uint32_t);
using FnThisU32U32 = uint8_t  (__fastcall*)(uint32_t self, uint32_t, uint32_t, uint32_t);
using FnSetSel     = void     (__fastcall*)(uint32_t self, uint32_t, uint32_t it, uint32_t cur);

// Host->guest seam (guest_call.h): each name resolves through GuestFnPtr at EVERY call
// -- engine mode arms only after all Install*() have run, so a static-init-time value
// would freeze the raw native address. Native mode: the raw address, as shipped.
#define ItemCtor(...)    GCALL(Fastcall, FnThis,       0x1A340, __VA_ARGS__)  // MenuOptionItem ctor
#define SetLabel(...)    GCALL(Fastcall, FnThisU32,    0x19E70, __VA_ARGS__)  // (u16) -> item+0x7A
#define SetValue(...)    GCALL(Fastcall, FnThisU32,    0x19E80, __VA_ARGS__)  // (u8; 0x24 = none)
#define SetAlign(...)    GCALL(Fastcall, FnThisU32,    0x19E90, __VA_ARGS__)  // -> item+0x70
#define AppendItem(...)  GCALL(Fastcall, FnThisU32,    0x1D030, __VA_ARGS__)  // Screen::AppendItem
#define SetSelected(...) GCALL(Fastcall, FnSetSel,     0x1D230, __VA_ARGS__)
#define SetBusy(...)     GCALL(Fastcall, FnThisU32,    0x1DA60, __VA_ARGS__)  // FE_MGR+0x1B0 = 1
#define InputTest(...)   GCALL(Fastcall, FnThisU32U32, 0x13470, __VA_ARGS__)  // (id, pad) -> pressed
#define PlaySfx(...)     GCALL(Fastcall, FnThisU32,    0x705E0, __VA_ARGS__)
#define TickItems(...)   GCALL(Fastcall, FnThis,       0x1D2C0, __VA_ARGS__)  // vtable+8 on every item

// The retail CHARACTER CARD widget (0x8D8 bytes). Screens 5 and 15 each embed four of them
// at self+0x20 and their CUT retail Build (0x2A300 / 0x294A0 -- the two functions this file
// replaces) drove them exactly like this: load, mark, append. See the lobby below.
using FnCardXY = void (__fastcall*)(uint32_t self, uint32_t, float sx, float sy);
#define CardLoad(...)     GCALL(Fastcall, FnThis,    0x1B580, __VA_ARGS__)  // bind the 11 character sprites
#define CardSetScale(...) GCALL(Fastcall, FnCardXY,  0x1B870, __VA_ARGS__)
#define CardSetChar(...)  GCALL(Fastcall, FnThisU32, 0x1BB10, __VA_ARGS__)  // -> card+0x8D5 (fade target)
#define CardSetState(...) GCALL(Fastcall, FnThisU32, 0x1BB30, __VA_ARGS__)  // 1 = the "chosen" pulse
#define CardSetStep(...)  GCALL(Fastcall, FnThisU32, 0x1BBB0, __VA_ARGS__)  // -> card+0x8D6, fade speed

// Input ids confirmed in use by the retail screens we mirror (screen 7's Update maps 6 ->
// "next item" and 5 -> "previous item"; the VIDEO screen uses 3/4 for left/right and the
// quit-confirm modal uses 1/2 for A/B). The LAN UI deliberately uses only these six.
// Ids 7 and 8 are retail's "cycle forward" / "cycle backward" -- what QUICK GAME's own
// four-column setup screen binds every value change to, and what its hint band advertises as
// A and X. Verified on this port by driving screen 11: A stepped a team letter A -> B -> C and
// X stepped it back, so both ids reach the pad layer. Id 0 is START.
enum { IN_START = 0, IN_A = 1, IN_B = 2, IN_LEFT = 3, IN_RIGHT = 4, IN_UP = 5, IN_DOWN = 6,
       IN_NEXT = 7, IN_PREV = 8 };

static void Sfx(uint32_t id) {
    uint32_t m = LanMaster();
    uint32_t snd = m ? *(uint32_t*)(uintptr_t)(m + 0x1C904) : 0;
    if (snd) PlaySfx(snd, 0, id);
}
static uint32_t InputObj() {
    uint32_t m = LanMaster();
    return m ? *(uint32_t*)(uintptr_t)(m + 0x4BC) : 0;
}
// Any local pad (the keyboard merges into port 0) may drive the LAN menus.
static bool Pressed(uint32_t id) {
    uint32_t in = InputObj();
    if (!in) return false;
    for (uint32_t pad = 0; pad < 4; ++pad) if (InputTest(in, 0, id, pad)) return true;
    return false;
}

// --- custom text table -------------------------------------------------------
// Retail data has 0xE3 strings per language, so anything from 0x100 up is unambiguously
// ours. Row text is rebuilt into these buffers every frame from live session state.
static const uint16_t kTextBase = 0x100;
static const int kTextN = 160;
static char (&g_txt)[kTextN][56] = *(char(*)[kTextN][56])GuestObjAlloc(
    sizeof(char[kTextN][56]), 8);   // guest holds pointers into it (Hk_GetText)

// THE FONT IS PROPORTIONAL, so a "column" cannot be made of padding spaces -- every row would
// sit at its own ragged offset (user-reported). Anything that has to line up is therefore a
// SEPARATE ITEM at a fixed x with ALIGN_LEFT, and the one item per row that carries the row's
// name stays selectable so the game's own selection flare still marks the whole row.
static const int kBrowseRows = 5;
enum {
    // LAN GAME (browser)
    T_TITLE5 = 0, T_NAME_L, T_NAME_V, T_PASS_L, T_PASS_V, T_HOST, T_JOINIP,
    T_HDR_NAME, T_HDR_PLAYERS, T_HDR_MODE, T_HDR_PING, T_HDR_STATE,
    T_ROWNAME0, T_ROWPLAY0 = T_ROWNAME0 + kBrowseRows,
    T_ROWMODE0 = T_ROWPLAY0 + kBrowseRows, T_ROWPING0 = T_ROWMODE0 + kBrowseRows,
    T_ROWTAG0  = T_ROWPING0 + kBrowseRows, T_EMPTY = T_ROWTAG0 + kBrowseRows,
    T_STATUS5, T_FOOT5,
    // LAN LOBBY -- four seat columns (heading, portrait, name, character, type, team) over a
    // shared block, with retail's two corner status items, hint band and prompt.
    T_TITLE15, T_LSTATEL, T_LSTATER,
    T_SLOTHEAD0, T_SLOTNAME0 = T_SLOTHEAD0 + 4, T_SLOTCHAR0 = T_SLOTNAME0 + 4,
    T_SLOTSKIN0 = T_SLOTCHAR0 + 4, T_SLOTTYPE0 = T_SLOTSKIN0 + 4,
    T_SLOTTEAM0 = T_SLOTTYPE0 + 4,
    T_SET = T_SLOTTEAM0 + 4, T_ARENA, T_MODE, T_START,
    T_HINT0, T_PROMPT = T_HINT0 + 3,
    // text-entry modal
    T_MTITLE, T_MTEXT, T_MFOOT,
    T_GRID0, T_GRIDEND = T_GRID0 + 41,
    T_COUNT
};
static_assert(T_COUNT <= kTextN, "custom text table too small");

const char* LanCustomText(uint16_t idx) {
    if (idx < kTextBase || idx >= kTextBase + kTextN) return nullptr;
    return g_txt[idx - kTextBase];
}
// A '%' reaching the game's vsprintf-based text path is a live crash and bytes < 0x20 are
// colour/button-glyph escapes, so everything written here is scrubbed except the escapes
// this file inserts itself (see kBtn*).
static void Txt(int slot, const char* fmt, ...) {
    char tmp[sizeof(g_txt[0])];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(tmp, sizeof tmp, _TRUNCATE, fmt, ap);
    va_end(ap);
    char* o = g_txt[slot];
    int n = 0;
    for (const char* p = tmp; *p && n < (int)sizeof(g_txt[0]) - 1; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '%') continue;
        if (c < 0x20 && c != 0x08) continue;         // keep only the button-glyph escape
        o[n++] = (char)c;
        if (c == 0x08 && p[1] && n < (int)sizeof(g_txt[0]) - 1) o[n++] = *++p;
    }
    o[n] = 0;
}
// Add to a row that has already been composed this frame, so a marker can be tacked onto
// whatever the cell happens to say. Same filtering as Txt: '%' is dropped, because the row
// text is handed to the game's own formatter and a stray one would eat the next character.
static void TxtAppend(uint16_t slot, const char* fmt, ...) {
    char tmp[sizeof(g_txt[0])];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(tmp, sizeof tmp, _TRUNCATE, fmt, ap);
    va_end(ap);
    char* o = g_txt[slot];
    int n = (int)strlen(o);
    for (const char* p = tmp; *p && n < (int)sizeof(g_txt[0]) - 1; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '%' || (c < 0x20 && c != 0x08)) continue;
        o[n++] = (char)c;
    }
    o[n] = 0;
}
// A BUTTON GLYPH MUST BE PRECEDED BY A SPACE, never by a printing character: FUN_00016680
// charges DOUBLE width for the D/O/S/T/X escapes and the drawer paints the glyph over that
// double cell, so it covers whatever sits immediately before it. Measured: "... ("BTN_A" TO
// SEE IT)" lost its opening bracket while the closing one survived.
#define BTN_A "\x08X"
#define BTN_B "\x08O"
#define BTN_D "\x08D"                 // the d-pad glyph, as used by retail's own hint band

// --- localization ------------------------------------------------------------
// These screens are the port's own, and unlike a retail menu label most of their text is
// rebuilt EVERY FRAME out of live session state ("YOUR NAME: <name>", "SKIN 2/4"), so it
// cannot be a whole pack string. Only the FIXED part is translated; the composition stays
// here, written in READING order, and the runtime shaper places the digits and the Latin
// runs (a typed player name, an IP address) where right-to-left text wants them.
//
// The English literal stays at the call site ON PURPOSE: it is the fallback, and it is what
// makes this file still readable by someone who does not read Arabic.
enum {                                 // pack indices; the text lives in tools/arabic_lan.py
    TXT_TITLE = 0x200, TXT_YOURNAME, TXT_PASSWORD, TXT_NONE, TXT_HOSTGAME, TXT_JOINIP,
    TXT_GAMESON, TXT_DIFFVER, TXT_INMATCH, TXT_FULL, TXT_LOCKED, TXT_OPEN, TXT_QUICK,
    TXT_TOURNEY, TXT_NOGAMES,
    TXT_LOBBY = 0x210, TXT_HOSTING, TXT_JOINED, TXT_YOU, TXT_PLAYER, TXT_COMPUTER,
    TXT_SKIN, TXT_SEATOPEN, TXT_REMOVE, TXT_TEAM, TXT_ARENA, TXT_FIGHTSET, TXT_MODE,
    TXT_STARTMATCH, TXT_QUICKMATCH, TXT_TOURNAMENT, TXT_MEATRUSH, TXT_DPADMOVE,
    TXT_CHANGE, TXT_LEAVE, TXT_BEGIN, TXT_READYWAIT, TXT_WHENREADY, TXT_READY,
    TXT_KDELETE = 0x228, TXT_KSPACE, TXT_KDONE,
    TXT_ENTERNAME = 0x230, TXT_SETPW, TXT_ENTERIP, TXT_GAMELOCKED,
    TXT_BACK = 0x250, TXT_SELECT, TXT_CANCEL, TXT_SEARCHING, TXT_HOSTINGON,
    TXT_JOINING, TXT_STARTING, TXT_CONNECTING,
};
static const char* L(uint16_t id, const char* en) {
    if (ArabicEnabled())
        if (const char* a = ArabicText(id)) return a;
    return en;
}

// --- measuring text ----------------------------------------------------------
// THE FONT IS PROPORTIONAL AND THE LAYOUT IS COLUMNS, so "12 characters" is not a width:
// twelve wide glyphs run into the next column where twelve narrow ones fit (user-reported --
// player names collided with the CHARACTER column in the lobby). `FUN_00016680(font, str,
// scale)` is the game's own measurer and returns the drawn width in EXACTLY the normalised
// x space the item positions use -- FUN_000167F0 centres a string by subtracting half of
// this number, so a width compared against a column gap is an apples-to-apples comparison
// at every resolution and at both aspects (the 16:9 squeeze scales positions and glyphs by
// the same 0.75 about the centre, so it cannot change whether two columns collide).
//
// The font pointer is the one the item renderer measures with: the base item ctor 0x19E10
// sets item+0x74 = *(u32*)(master+0x4D0) + 0x91C and FUN_00019EB0 passes **(u32**)(item+0x74).
using FnTextWidth = float (__cdecl*)(uint32_t font, const char* s, float scale);
#define MeasureText(...) GCALL(Cdecl, FnTextWidth, 0x16680, __VA_ARGS__)

static uint32_t TextFont() {
    uint32_t m = LanMaster();
    if (!m) return 0;
    uint32_t txt = *(uint32_t*)(uintptr_t)(m + 0x4D0);
    return txt ? *(uint32_t*)(uintptr_t)(txt + 0x91C) : 0;
}
static float TextWidth(const char* s, float scale) {
    uint32_t f = TextFont();
    if (!f || !s || !*s) return 0.0f;
    return MeasureText(f, s, scale);
}

// --- row construction --------------------------------------------------------
// Items live in DLL memory: they are only ever reached through the screen's +0x60/+0x64
// list pointers, and the screen destructor destroys its embedded sub-objects only -- it
// never walks the item list -- so a DLL-side item is never handed to the game allocator.
// Rebuilt on every Build because the frontend factory re-runs on each return to the FE.
// item+0x70 is a BITFIELD, decoded from FUN_000167F0: bit0 subtracts half the drawn string
// width (horizontal centre), bit1 subtracts half the line height (vertical centre), bit2
// subtracts the full width (right align). So 0 = left edge at x, 1 = centred on x, 4 = right
// edge at x -- which is what makes fixed columns possible.
enum { ALIGN_LEFT = 0, ALIGN_CENTRE = 1, ALIGN_RIGHT = 4 };

// THE SCALE LADDER. Retail uses a handful of build-time constants per screen and never
// rescales anything at runtime; the first version of this file used eleven ad-hoc scales and
// then multiplied whichever one it was by an arbitrary ratio every frame, so two rows with
// similar text came out visibly different sizes. These are retail screen 11's own values.
static const float kScTitle  = 0.055f;   // screen title
static const float kScCorner = 0.037f;   // the two top-corner status items
static const float kScRow    = 0.040f;   // a full-width selectable row
static const float kScCell   = 0.032f;   // a value inside a seat column
static const float kScName   = 0.032f;   // the seat's player name
static const float kScHead   = 0.030f;   // the seat's "PLAYER n" heading
static const float kScPrompt = 0.035f;   // "PRESS START ..." prompt
static const float kScHint   = 0.025f;   // the button-hint band
static const float kScaleLadder[] = { 0.055f, 0.040f, 0.037f, 0.035f, 0.032f, 0.030f, 0.025f };

// COLOUR HIERARCHY. Every item defaults to the base grey rgba(128,128,128,128), which is why
// the first version read as one undifferentiated wall of text. Retail's multi-column screen
// uses exactly two blocks and FUN_00019D30 to apply them.
static const uint32_t kColOrange = 0x169F54;   // (249,101,30,127)  titles, captions, hints
static const uint32_t kColGold   = 0x169F64;   // (255,173,26,127)  values the player changes
#define SetColour(...) GCALL(Fastcall, FnThisU32, 0x19D30, __VA_ARGS__)

// THE D-PAD ESCAPE DRAWS NOTHING HERE. "\x08D" is charged DOUBLE width by FUN_00016680 (it is
// in the D/O/S/T/X set) and the space is duly reserved, but no glyph appears -- under either
// text style, the default at [master+0x4D0]+0x91C or the alternate at +0x2568 that
// FUN_0001A300 selects and that retail's own hint bands use. \x08X and \x08O are fine. Rather
// than ship a hint with a hole in it, the d-pad is spelled out. (Whatever backs that glyph
// index is not resolving in this port; it is cosmetic and not worth a texture hunt.)

// GAME FONT METRICS (XBE-verified): font[+0x900] = 30px em, tracking 0, x-aspect 0.8. `y` is
// the TOP of the em box (align bit1 makes it the centre by subtracting 0.5*S); ink runs
// 1.133*S below it (the 34px letter cell) and the shipped line pitch is exactly 4/3*S. A
// SELECTED row is drawn at 1.15*S (item+0x6C). So a row pitch must be >= 1.31*S_upper just to
// avoid touching -- the old lobby used 0.038 at S=0.034, which is 1.12*S and actually
// OVERLAPPED once the upper row was selected. Everything here is spaced >= 1.5*max(S).
static const float kPitchMin = 1.5f;

// A Row's storage is GUEST-WALKED (the screen's +0x60/+0x64 item list points into
// it), so it lives in the guest-visible arena — allocated per instance at static-init.
struct Row {
    uint8_t* mem;
    Row() : mem((uint8_t*)GuestObjAlloc(0x84, 8)) {}
};
static uint32_t MakeRow(Row& r, uint16_t label, float x, float y, bool selectable, float scale,
                        int align = ALIGN_CENTRE, uint32_t colour = 0) {
    uint32_t it = Gp32(r.mem);
    memset(r.mem, 0, 0x84);
    ItemCtor(it, 0);
    SetLabel(it, 0, label);
    SetValue(it, 0, 0x24);                        // no separate value string
    SetAlign(it, 0, (uint32_t)align);
    if (colour) SetColour(it, 0, colour);
    *(float*)(uintptr_t)(it + 0x40) = x;
    *(float*)(uintptr_t)(it + 0x44) = y;
    *(float*)(uintptr_t)(it + 0x68) = scale;
    *(uint8_t*)(uintptr_t)(it + 0x48) = 1;
    *(uint8_t*)(uintptr_t)(it + 0x49) = selectable ? 1 : 0;
    *(uint8_t*)(uintptr_t)(it + 0x4B) = 0;
    return it;
}
static void RowShow(uint32_t it, bool vis, bool sel) {
    *(uint8_t*)(uintptr_t)(it + 0x48) = vis ? 1 : 0;
    *(uint8_t*)(uintptr_t)(it + 0x49) = sel ? 1 : 0;
}
// DISABLED, NOT HIDDEN. item+0x4B = 1 makes the game's own renderer draw the row at
// rgba(128,128,128,64) -- half alpha, the retail "you cannot touch this" treatment. A client
// used to see the host-only rows at full brightness and nothing happened when it pressed them.
static void RowEnable(uint32_t it, bool enabled) {
    *(uint8_t*)(uintptr_t)(it + 0x4B) = enabled ? 0 : 1;
}

// MAKE A ROW FIT ITS COLUMN. Two levers, in this order, because they degrade differently:
// shrinking the scale keeps every character readable and is exact in one step (the measured
// width is linear in the scale), and only once the scale has hit a floor is text actually
// thrown away. `maxW` is the drawn width the row is allowed to occupy -- for a CENTRED item
// that is twice the smaller of its two gaps, for a left/right aligned one the gap itself.
static void Fit(Row& r, int slot, float baseScale, float maxW, float minScale) {
    uint32_t it = (uint32_t)(uintptr_t)r.mem;
    char* s = g_txt[slot];
    // SNAP DOWN THE LADDER rather than solving for maxW/w. A continuous ratio gives every
    // over-long row its own private size and the screen stops looking authored; stepping to
    // the next rung down keeps the whole screen on the same handful of sizes.
    float scale = baseScale;
    for (int i = 0; i < (int)(sizeof kScaleLadder / sizeof kScaleLadder[0]); ++i) {
        if (kScaleLadder[i] > scale + 1e-6f) continue;      // start at the row's own rung
        scale = kScaleLadder[i];
        if (TextWidth(s, scale) <= maxW || scale <= minScale) break;
    }
    if (scale < minScale) scale = minScale;
    *(float*)(uintptr_t)(it + 0x68) = scale;
    if (TextWidth(s, scale) <= maxW) return;
    // Still too wide at the floor: ellipsize. Never cut between a 0x08 button-glyph escape
    // and the letter that selects the glyph -- a lone 0x08 makes the renderer eat the
    // terminator and walk off the end of the buffer.
    // Guest-arena scratch, NOT a stack array: this buffer is MEASURED through the guest's
    // FnTextWidth, and a 64-bit host stack pointer trips the >4GB tripwire (session-29
    // on-device LAN crash — Android's default name "LOCALHOST" is wide enough to reach
    // this path where PC names never did). Single-threaded UI build path: static is safe.
    static char (&buf)[sizeof(g_txt[0])] =
        *(char(*)[sizeof(g_txt[0])])GuestObjAlloc(sizeof(g_txt[0]), 8);
    int n = (int)strlen(s);
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    for (--n; n > 0; --n) {
        if ((unsigned char)s[n - 1] == 0x08) continue;
        memcpy(buf, s, (size_t)n);
        buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = 0;
        if (TextWidth(buf, scale) <= maxW) { memcpy(s, buf, (size_t)n + 3); return; }
    }
    s[0] = 0;
}
static void ScreenReset(uint32_t self) {
    for (int i = 0; i < 4; ++i) *(uint32_t*)(uintptr_t)(self + 4 + i * 4) = 0;  // per-cursor sel
    *(uint32_t*)(uintptr_t)(self + 0x14) = 0;                                   // list head
    *(uint32_t*)(uintptr_t)(self + 0x18) = 0;                                   // list tail
    *(uint8_t*)(uintptr_t)(self + 0x1C) = 0;                                    // single cursor
}
static uint32_t SelItem(uint32_t self) { return *(uint32_t*)(uintptr_t)(self + 4); }
static void Select(uint32_t self, uint32_t it) { if (it) SetSelected(self, 0, it, 0); }
// 1-D navigation over the visible+selectable rows, mirroring FUN_0001D070/0x1D150 but
// driven by us so the modal can borrow the same d-pad without fighting the screen.
static void NavStep(uint32_t self, int dir) {
    uint32_t cur = SelItem(self);
    uint32_t head = *(uint32_t*)(uintptr_t)(self + 0x14);
    if (!head) return;
    if (!cur) { cur = head; }
    uint32_t it = cur;
    for (int guard = 0; guard < 64; ++guard) {
        it = *(uint32_t*)(uintptr_t)(it + (dir > 0 ? 0x60 : 0x64));
        if (!it) {                                   // wrap
            if (dir > 0) it = head;
            else { it = *(uint32_t*)(uintptr_t)(self + 0x18); }
            if (!it) return;
        }
        if (*(uint8_t*)(uintptr_t)(it + 0x49) && !*(uint8_t*)(uintptr_t)(it + 0x4B) &&
            *(uint8_t*)(uintptr_t)(it + 0x48)) { Select(self, it); Sfx(5); return; }
        if (it == cur) return;
    }
}

// --- the text-entry modal ----------------------------------------------------
// A modal, not a third dead screen id: one less "is it really dead?" bet, and fe_menu's
// quit-confirm modal is the proven pattern. The keyboard works at the same time -- WM_CHAR
// is captured in WndProc into a lock-free ring and drained here on the game thread.
enum ModalKind : uint8_t { MODAL_NONE = 0, MODAL_NAME, MODAL_PASSWORD, MODAL_IP, MODAL_JOINPW };
static ModalKind g_modal = MODAL_NONE;
static char  g_modalBuf[64];
static int   g_modalLen = 0;
static int   g_modalMax = 12;
static int   g_gridSel = 0;
static int   g_pendingJoin = -1;
static char  g_hostPassword[24] = "";

// 42 cells in ROWS OF 10, 10, 10, 9, 3. The last row is its own row because SPACE / DELETE /
// DONE are multi-character labels: on a uniform grid they are centred on the same tight
// spacing as the single letters and simply overlap each other into an unreadable smear.
// **SPACE BELONGS IN THAT ROW, NOT AT THE END OF THE DIGIT ROW.** It used to sit at column 9
// of row 3 on the 0.058 pitch: the word is ~0.146 wide centred on x 0.761, so it painted
// straight over the "." at x 0.703 and the full stop was invisible -- which also made an IP
// address untypeable on the pad (user-reported). The nav is fully row-length aware, so moving
// it is purely a change to kRowStart/kRowLen.
static const char* kCellText[42] = {
    "A","B","C","D","E","F","G","H","I","J",
    "K","L","M","N","O","P","Q","R","S","T",
    "U","V","W","X","Y","Z","0","1","2","3",
    "4","5","6","7","8","9","-","'",".","DELETE",
    "SPACE","DONE"
};
static const char kCellChar[42] = {
    'A','B','C','D','E','F','G','H','I','J',
    'K','L','M','N','O','P','Q','R','S','T',
    'U','V','W','X','Y','Z','0','1','2','3',
    '4','5','6','7','8','9','-','\'','.',0,
    ' ', 0
};
// Last row reads DELETE / SPACE / DONE: the space bar belongs in the middle, where a keyboard
// puts it, with the two commands flanking it.
static const int kCellN = 42, kCellDel = 39, kCellOk = 41;
static const int   kRowN = 5;
static const int   kRowStart[kRowN] = { 0, 10, 20, 30, 39 };
static const int   kRowLen[kRowN]   = { 10, 10, 10, 9, 3 };
static const float kRowY[kRowN]     = { 0.40f, 0.48f, 0.56f, 0.64f, 0.74f };
static const float kRowGap[kRowN]   = { 0.058f, 0.058f, 0.058f, 0.058f, 0.30f };
static int CellRow(int i) {
    for (int r = kRowN - 1; r > 0; --r) if (i >= kRowStart[r]) return r;
    return 0;
}

static volatile LONG g_keyHead = 0, g_keyTail = 0;
static volatile int  g_keyRing[64];
bool LanTextCaptureActive() { return g_modal != MODAL_NONE; }

static void ModalOpen(ModalKind k, const char* initial) {
    g_modal = k;
    // A NAME is capped at 12 because it is concatenated into rows and banners that format
    // into fixed stack buffers. AN ADDRESS IS NOT A NAME: "192.168.100.101" is 15 characters
    // and a hostname can be longer still, and the 12-char cap made it impossible to type one
    // (user-reported). Addresses go to getaddrinfo/inet_pton, never to the text renderer.
    g_modalMax = (k == MODAL_IP) ? 40 : 12;
    g_modalLen = 0; g_modalBuf[0] = 0; g_gridSel = 0;
    if (initial) { strncpy_s(g_modalBuf, initial, _TRUNCATE); g_modalLen = (int)strlen(g_modalBuf); }
    g_keyTail = g_keyHead;                     // discard anything typed before it opened
    Sfx(0x13);
}
static void ModalClose() { g_modal = MODAL_NONE; g_pendingJoin = -1; }
static void ModalPut(char c) {
    if (g_modalLen >= g_modalMax || g_modalLen >= (int)sizeof(g_modalBuf) - 1) return;
    g_modalBuf[g_modalLen++] = c;
    g_modalBuf[g_modalLen] = 0;
}
static void ModalCommit() {
    switch (g_modal) {
    case MODAL_NAME:     LanSetName(g_modalBuf); break;
    case MODAL_PASSWORD: strncpy_s(g_hostPassword, g_modalBuf, _TRUNCATE); break;
    case MODAL_IP:       LanJoinIp(g_modalBuf, g_hostPassword); break;
    case MODAL_JOINPW:   if (g_pendingJoin >= 0) LanJoinGame(g_pendingJoin, g_modalBuf); break;
    default: break;
    }
    Sfx(0x13);
    ModalClose();
}
// Returns true while the modal owns the frame (the host screen then does nothing else).
static bool ModalUpdate() {
    if (g_modal == MODAL_NONE) return false;
    while (g_keyTail != g_keyHead) {
        int ch = g_keyRing[g_keyTail & 63];
        g_keyTail = (g_keyTail + 1) & 0x3FFFFFF;
        if (ch == '\b') { if (g_modalLen) g_modalBuf[--g_modalLen] = 0; }
        else if (ch == '\r' || ch == '\n') { ModalCommit(); return true; }
        else if (ch == 27) { Sfx(1); ModalClose(); return true; }
        else if (ch >= 32 && ch < 127) {
            char c = (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == ' ' || c == '-' || c == '\'' || c == '.') ModalPut(c);
        }
    }
    if (Pressed(IN_B))    { Sfx(1); ModalClose(); return true; }
    // Row-aware navigation: the rows are not all the same width, so left/right wraps inside
    // the row and up/down clamps the column into the row it lands on.
    int r = CellRow(g_gridSel), c = g_gridSel - kRowStart[r];
    if (Pressed(IN_LEFT))  { c = (c + kRowLen[r] - 1) % kRowLen[r]; Sfx(5); }
    if (Pressed(IN_RIGHT)) { c = (c + 1) % kRowLen[r]; Sfx(5); }
    if (Pressed(IN_UP) || Pressed(IN_DOWN)) {
        float frac = kRowLen[r] > 1 ? (float)c / (float)(kRowLen[r] - 1) : 0.5f;
        r = (r + (Pressed(IN_DOWN) ? 1 : kRowN - 1)) % kRowN;
        c = (int)(frac * (kRowLen[r] - 1) + 0.5f);
        Sfx(5);
    }
    if (c < 0) c = 0;
    if (c >= kRowLen[r]) c = kRowLen[r] - 1;
    g_gridSel = kRowStart[r] + c;
    if (Pressed(IN_A)) {
        if (g_gridSel == kCellOk) { ModalCommit(); return true; }
        if (g_gridSel == kCellDel) { if (g_modalLen) g_modalBuf[--g_modalLen] = 0; Sfx(1); }
        else { ModalPut(kCellChar[g_gridSel]); Sfx(0x13); }
    }
    return true;
}
static const char* ModalTitle() {
    switch (g_modal) {
    case MODAL_NAME:     return L(TXT_ENTERNAME, "ENTER YOUR NAME");
    case MODAL_PASSWORD: return L(TXT_SETPW,
                             "SET A PASSWORD - LEAVE IT EMPTY FOR AN OPEN GAME");
    case MODAL_IP:       return L(TXT_ENTERIP, "ENTER THE HOST'S ADDRESS");
    case MODAL_JOINPW:   return L(TXT_GAMELOCKED, "THIS GAME IS LOCKED");
    default:             return "";
    }
}

// --- screen plumbing ---------------------------------------------------------
static bool AutoDriving();        // TJ_LAN headless harness is driving (see the bottom)
static uint32_t FeMgr() { return LanFeMgr(); }
static bool Transitioning() {
    uint32_t fe = FeMgr();
    return fe && *(uint8_t*)(uintptr_t)(fe + 0x5C) != 0;
}
// The frontend's idle path (FUN_0002E0A0() == 0x708 -> FE+0x502 = 5 -> screen 0x16) would
// launch an attract match out from under a connected peer. Hold the idle tick at zero while
// a LAN screen is up -- but never while armed, where it IS the match clock.
static void SuppressAttract() {
    if (!NetArmed()) *(volatile uint32_t*)(uintptr_t)0x15C4710 = 0;
}
// Both peers must return the retail launch sentinel from the SAME lockstep frame, so
// BARRIER_READY is only sent once the frontend sits on a screen whose Update we hook and
// is not mid-transition.
bool LanFrontendCanLaunch() {
    if (*(volatile uint8_t*)(uintptr_t)0x184661 != 3) return false;
    uint32_t fe = FeMgr();
    if (!fe) return false;
    if (*(uint8_t*)(uintptr_t)(fe + 0x5C)) return false;
    uint32_t scr = *(uint32_t*)(uintptr_t)fe;
    return scr == 15 || scr == 4 || scr == 5;
}

// ============================================================ SCREEN 5: LAN HOME
// Column x positions. The 16:9 projection squeezes x toward the centre by 0.75, so anything
// inside 0.10..0.95 is safely on screen at both aspects and the columns stay lined up.
//
// THE SELECTION FLARE IS DRAWN AT THE ITEM'S RAW x, ignoring the alignment adjustment -- a
// left-aligned selectable row therefore throws the burst off the left edge. So the item that
// carries a row's selection is always CENTRED (the flare lands behind its text), and only the
// non-selectable columns beside it use left/right alignment.
//
// EVERY COLUMN NOW DECLARES ITS WIDTH BUDGET and Fit() enforces it, so a long game name
// shrinks and finally ellipsizes instead of running into the next column. The budgets add up
// to the row: name 0.06..0.46 (centred on 0.26), players 0.50..0.57, mode 0.59..0.70,
// state 0.71..0.97 (right-aligned, so it grows leftwards).
static const float kColName = 0.26f, kColPlay = 0.50f, kColMode = 0.59f, kColState = 0.97f;
static const float kWName = 0.40f, kWPlay = 0.07f, kWMode = 0.11f, kWState = 0.26f;
static const float kWLine = 0.90f;          // anything centred on 0.5 and spanning the row
static const float kBrowRow = 0.043f, kBrowHdr = 0.030f, kBrowSmall = 0.032f;
static Row g_b_title, g_b_name, g_b_pass, g_b_nameV, g_b_passV,
           g_b_host, g_b_ip, g_b_hdr,
           g_b_rowName[kBrowseRows], g_b_rowPlay[kBrowseRows], g_b_rowMode[kBrowseRows],
           g_b_rowTag[kBrowseRows],
           g_b_empty, g_b_status, g_b_foot;
static Row g_m_title, g_m_text, g_m_foot, g_m_cell[42];
static uint32_t g_bIt_name, g_bIt_pass, g_bIt_host, g_bIt_ip, g_bIt_row[kBrowseRows];
// The VALUE halves of the name and password rows. Non-selectable: the label row is
// still the one the cursor lands on and the one that opens the editor.
static uint32_t g_bIt_nameV, g_bIt_passV;

// The modal's rows are appended to whichever screen builds them; both screens build their
// own copies of the same static Row storage, which is safe because only one screen is ever
// live at a time and Build always relinks from scratch.
static void BuildModalRows(uint32_t self) {
    AppendItem(self, 0, MakeRow(g_m_title, kTextBase + T_MTITLE, 0.5f, 0.15f, false, 0.050f));
    AppendItem(self, 0, MakeRow(g_m_text,  kTextBase + T_MTEXT,  0.5f, 0.27f, false, 0.065f));
    for (int i = 0; i < kCellN; ++i) {
        int r = CellRow(i), c = i - kRowStart[r];
        float x = 0.5f + ((float)c - (kRowLen[r] - 1) * 0.5f) * kRowGap[r];
        AppendItem(self, 0, MakeRow(g_m_cell[i], (uint16_t)(kTextBase + T_GRID0 + i),
                                    x, kRowY[r], false, 0.045f));
    }
    AppendItem(self, 0, MakeRow(g_m_foot, kTextBase + T_MFOOT, 0.5f, 0.88f, false, 0.033f));
}

// WHILE THE MODAL IS UP, THE SCREEN UNDERNEATH MUST GO AWAY. Without this the browser's own
// rows keep drawing at their own y positions and the grid lands on top of them -- the letters
// are there, they are just illegible (user-reported). This is fe_menu's quit-confirm pattern:
// save +0x48 (visible) and +0x49 (selectable) for every item that is not ours, zero both, and
// restore on close. Nav ignores visibility and tests only +0x49/+0x4B, so both must go.
static uint32_t g_modalHost = 0;
static uint32_t g_modalSaved[80]; static uint8_t g_modalVis[80], g_modalSel[80];
static int      g_modalSavedN = 0;

static bool IsModalItem(uint32_t it) {
    if (it == (uint32_t)(uintptr_t)g_m_title.mem || it == (uint32_t)(uintptr_t)g_m_text.mem ||
        it == (uint32_t)(uintptr_t)g_m_foot.mem) return true;
    for (int i = 0; i < kCellN; ++i) if (it == (uint32_t)(uintptr_t)g_m_cell[i].mem) return true;
    return false;
}
static void ShowModalRows(bool show) {
    RowShow((uint32_t)(uintptr_t)g_m_title.mem, show, false);
    RowShow((uint32_t)(uintptr_t)g_m_text.mem, show, false);
    RowShow((uint32_t)(uintptr_t)g_m_foot.mem, show, false);
    // The cells become the screen's selectable items, so the cursor is the game's OWN
    // selection flare rather than ">X<" brackets -- which also kept changing the cell's width.
    for (int i = 0; i < kCellN; ++i) RowShow((uint32_t)(uintptr_t)g_m_cell[i].mem, show, show);
}
static void SelectCell(uint32_t self) {
    if (g_gridSel < 0 || g_gridSel >= kCellN) g_gridSel = 0;
    Select(self, (uint32_t)(uintptr_t)g_m_cell[g_gridSel].mem);
}
static void ModalAttach(uint32_t self) {
    g_modalSavedN = 0;
    for (uint32_t it = *(uint32_t*)(uintptr_t)(self + 0x14); it && g_modalSavedN < 80;
         it = *(uint32_t*)(uintptr_t)(it + 0x60)) {
        if (IsModalItem(it)) continue;
        g_modalSaved[g_modalSavedN] = it;
        g_modalVis[g_modalSavedN] = *(uint8_t*)(uintptr_t)(it + 0x48);
        g_modalSel[g_modalSavedN] = *(uint8_t*)(uintptr_t)(it + 0x49);
        *(uint8_t*)(uintptr_t)(it + 0x48) = 0;
        *(uint8_t*)(uintptr_t)(it + 0x49) = 0;
        ++g_modalSavedN;
    }
    ShowModalRows(true);
    g_modalHost = self;
    SelectCell(self);
}
static void ModalDetach(uint32_t self) {
    for (int i = 0; i < g_modalSavedN; ++i) {
        *(uint8_t*)(uintptr_t)(g_modalSaved[i] + 0x48) = g_modalVis[i];
        *(uint8_t*)(uintptr_t)(g_modalSaved[i] + 0x49) = g_modalSel[i];
    }
    g_modalSavedN = 0;
    ShowModalRows(false);
    g_modalHost = 0;
    (void)self;                                   // the caller re-selects its own row
}
static void RefreshModalText() {
    Txt(T_MTITLE, "%s", ModalTitle());
    Txt(T_MTEXT, "%s_", g_modalBuf);
    Txt(T_MFOOT, BTN_A " %s    " BTN_B " %s", L(TXT_SELECT, "SELECT"), L(TXT_CANCEL, "CANCEL"));
    // The three action keys are labels, not characters: translate them. Everything above
    // them is a Latin character the player is actually typing and must stay as it is.
    for (int i = 0; i < kCellN; ++i) {
        const char* cell = kCellText[i];
        if (i == kCellDel)      cell = L(TXT_KDELETE, "DELETE");
        else if (i == kCellOk)  cell = L(TXT_KDONE,   "DONE");
        else if (i == kCellOk - 1) cell = L(TXT_KSPACE, "SPACE");
        Txt(T_GRID0 + i, "%s", cell);
    }
    // The title is a whole sentence and the entry field can hold a 40-character hostname:
    // both were drawn at a fixed scale and simply ran off the sides, over the grid
    // (user-reported "the keyboard overlaps"). They shrink now instead.
    Fit(g_m_title, T_MTITLE, 0.050f, kWLine, 0.026f);
    Fit(g_m_text,  T_MTEXT,  0.065f, kWLine, 0.028f);
    Fit(g_m_foot,  T_MFOOT,  0.033f, kWLine, 0.022f);
}

static void __fastcall Hk_BrowseBuild(uint32_t self, uint32_t) {
    ScreenReset(self);
    const float kRow = kBrowRow, kHdr = kBrowHdr, kSmall = kBrowSmall;
    AppendItem(self, 0, MakeRow(g_b_title, kTextBase + T_TITLE5, 0.5f, 0.07f, false, 0.055f));
    g_bIt_name = MakeRow(g_b_name, kTextBase + T_NAME_L, 0.5f, 0.19f, true, kRow);
    g_bIt_pass = MakeRow(g_b_pass, kTextBase + T_PASS_L, 0.5f, 0.26f, true, kRow);
    // Same rows, same y: the value is a separate ITEM so that in Arabic it is a line of
    // its own with no Arabic on it, and therefore draws through the retail font.
    g_bIt_nameV = MakeRow(g_b_nameV, kTextBase + T_NAME_V, 0.5f, 0.19f, false, kRow,
                          ALIGN_RIGHT);
    g_bIt_passV = MakeRow(g_b_passV, kTextBase + T_PASS_V, 0.5f, 0.26f, false, kRow,
                          ALIGN_RIGHT);
    g_bIt_host = MakeRow(g_b_host, kTextBase + T_HOST,   0.5f, 0.34f, true, kRow);
    g_bIt_ip   = MakeRow(g_b_ip,   kTextBase + T_JOINIP, 0.5f, 0.41f, true, kRow);
    AppendItem(self, 0, g_bIt_name); AppendItem(self, 0, g_bIt_pass);
    AppendItem(self, 0, g_bIt_nameV); AppendItem(self, 0, g_bIt_passV);
    AppendItem(self, 0, g_bIt_host); AppendItem(self, 0, g_bIt_ip);

    // One centred section title, no column headers: "PLAYERS MODE PING" ran into each other
    // and into the title, and the data below reads perfectly well without them.
    AppendItem(self, 0, MakeRow(g_b_hdr, kTextBase + T_HDR_NAME, 0.5f, 0.51f, false, kHdr));
    for (int i = 0; i < kBrowseRows; ++i) {
        float y = 0.58f + i * 0.055f;
        g_bIt_row[i] = MakeRow(g_b_rowName[i], (uint16_t)(kTextBase + T_ROWNAME0 + i), kColName, y, false, kSmall);
        AppendItem(self, 0, g_bIt_row[i]);
        AppendItem(self, 0, MakeRow(g_b_rowPlay[i], (uint16_t)(kTextBase + T_ROWPLAY0 + i), kColPlay,  y, false, kSmall, ALIGN_LEFT));
        AppendItem(self, 0, MakeRow(g_b_rowMode[i], (uint16_t)(kTextBase + T_ROWMODE0 + i), kColMode,  y, false, kSmall, ALIGN_LEFT));
        AppendItem(self, 0, MakeRow(g_b_rowTag[i],  (uint16_t)(kTextBase + T_ROWTAG0  + i), kColState, y, false, kSmall, ALIGN_RIGHT));
    }
    AppendItem(self, 0, MakeRow(g_b_empty, kTextBase + T_EMPTY, 0.5f, 0.63f, false, kSmall));
    AppendItem(self, 0, MakeRow(g_b_status, kTextBase + T_STATUS5, 0.5f, 0.89f, false, kHdr));
    AppendItem(self, 0, MakeRow(g_b_foot, kTextBase + T_FOOT5, 0.5f, 0.95f, false, kHdr));
    BuildModalRows(self);
    ShowModalRows(false);
}

// Same compile-time host word as the save prompts (fe_menu.cpp): on Android this is a
// phone, not a PC, and the LAN browser is one of the few places the player is told so.
#ifndef TJ_HOST_WORD_UC
#ifdef __ANDROID__
#define TJ_HOST_WORD_UC "PHONE"
#else
#define TJ_HOST_WORD_UC "PC"
#endif
#endif

static void RefreshBrowserText() {
    Txt(T_TITLE5, "%s", L(TXT_TITLE, "LAN GAME"));
    // ENGLISH keeps the single composed row it has always had. ARABIC splits label from
    // value, because a Latin name on an Arabic line is drawn by the Arabic font and that
    // font has no Latin letters -- the name simply did not appear (user-reported).
    // Two columns about the centre: the label sits to the RIGHT of it and the value to
    // the LEFT, which is the order an Arabic reader wants them in.
    const bool arSplit = ArabicEnabled();
    if (arSplit) {
        Txt(T_NAME_L, "%s", L(TXT_YOURNAME, "YOUR NAME:"));
        Txt(T_NAME_V, "%s", LanGetName());
    } else {
        Txt(T_NAME_L, "%s  %s", L(TXT_YOURNAME, "YOUR NAME:"), LanGetName());
        Txt(T_NAME_V, " ");
    }
    // The password VALUE is Latin only when the player typed one; "NONE" is a pack string
    // and Arabic, so it stays on the label line where it reads naturally.
    if (arSplit) {
        // ALWAYS split, even when the value is the Arabic "NONE": splitting only
        // sometimes left this row centred while the name row above it was anchored to a
        // column, so the two labels sat at completely different x and looked broken.
        Txt(T_PASS_L, "%s", L(TXT_PASSWORD, "PASSWORD:"));
        Txt(T_PASS_V, "%s", g_hostPassword[0] ? g_hostPassword : L(TXT_NONE, "NONE"));
    } else {
        Txt(T_PASS_L, "%s  %s", L(TXT_PASSWORD, "PASSWORD:"),
            g_hostPassword[0] ? g_hostPassword : L(TXT_NONE, "NONE"));
        Txt(T_PASS_V, " ");
    }
    Txt(T_HOST, "%s", L(TXT_HOSTGAME, "HOST A GAME"));
    Txt(T_JOINIP, "%s", L(TXT_JOINIP, "JOIN BY ADDRESS"));
    int n = LanGameCount();
    Txt(T_HDR_NAME, "%s", L(TXT_GAMESON, "GAMES ON THIS NETWORK"));
    for (int i = 0; i < kBrowseRows; ++i) {
        const LanGameInfo* g = LanGameAt(i);
        bool live = g != nullptr;
        if (live) {
            const char* tag = !g->compatible ? L(TXT_DIFFVER, "DIFFERENT VERSION")
                            : g->inMatch     ? L(TXT_INMATCH, "IN A MATCH")
                            : g->players >= g->maxPlayers ? L(TXT_FULL, "FULL")
                            : g->locked      ? L(TXT_LOCKED, "LOCKED")
                                             : L(TXT_OPEN, "OPEN");
            Txt(T_ROWNAME0 + i, "%s", g->host);
            Txt(T_ROWPLAY0 + i, "%u/%u", g->players, g->maxPlayers);
            Txt(T_ROWMODE0 + i, "%s", g->mode ? L(TXT_TOURNEY, "TOURNEY") : L(TXT_QUICK, "QUICK"));
            Txt(T_ROWTAG0 + i, "%s", tag);
        }
        bool joinable = live && g->compatible && !g->inMatch && g->players < g->maxPlayers;
        RowShow(g_bIt_row[i], live, joinable);
        RowShow((uint32_t)(uintptr_t)g_b_rowPlay[i].mem, live, false);
        RowShow((uint32_t)(uintptr_t)g_b_rowMode[i].mem, live, false);
        RowShow((uint32_t)(uintptr_t)g_b_rowTag[i].mem, live, false);
    }
    Txt(T_EMPTY, "%s", L(TXT_NOGAMES,
         "NO GAMES YET - IS THE OTHER " TJ_HOST_WORD_UC " ON THIS NETWORK?"));
    RowShow((uint32_t)(uintptr_t)g_b_empty.mem, n == 0, false);
    Txt(T_STATUS5, "%s", LanStatusLine());
    Txt(T_FOOT5, BTN_B " %s    " BTN_A " %s", L(TXT_BACK, "BACK"), L(TXT_SELECT, "SELECT"));

    const float kRow = kBrowRow, kHdr = kBrowHdr, kSmall = kBrowSmall;
    // Split: label anchored just right of centre and running right, value ending just
    // left of centre. Unsplit: the label is centred exactly as before and the value row
    // is empty and hidden, so English is byte-for-byte the layout it always had.
    // TWO RIGHT-ALIGNED COLUMNS. Arabic reads from the right, so the edge the eye runs down
    // is the label's RIGHT edge -- left-aligning them lined up the edge nobody reads and left
    // the right edges 59px apart, which is what still looked broken. Both columns are anchored
    // by their right edge instead, and the value column is placed from the MEASURED width of
    // the widest label (the game's own measurer is hooked, so this is the real shaped Arabic
    // width) rather than from a guessed constant that a longer label would silently overrun.
    if (arSplit) {
        // Column widths from the WIDEST entry in each column, so the two rows share one pair
        // of edges; then the whole block is centred on 0.5 -- the axis HOST A GAME and JOIN BY
        // ADDRESS sit on -- so all four rows of this list line up as one group.
        const float kGutter = 0.035f;
        float wl = TextWidth(g_txt[T_NAME_L], kRow);
        float wp = TextWidth(g_txt[T_PASS_L], kRow);
        float labW = wl > wp ? wl : wp;
        float vn = TextWidth(g_txt[T_NAME_V], kRow);
        float vp = TextWidth(g_txt[T_PASS_V], kRow);
        float valW = vn > vp ? vn : vp;
        float total = labW + kGutter + valW;
        if (total > kWLine) total = kWLine;                // never wider than the row
        float kLabRight = 0.5f + total * 0.5f;             // the block, centred on 0.5
        float valRight  = kLabRight - labW - kGutter;
        if (valRight < 0.06f) valRight = 0.06f;            // never off the left of the screen
        SetAlign(g_bIt_name,  0, ALIGN_RIGHT);
        SetAlign(g_bIt_pass,  0, ALIGN_RIGHT);
        *(float*)(uintptr_t)(g_bIt_name  + 0x40) = kLabRight;
        *(float*)(uintptr_t)(g_bIt_pass  + 0x40) = kLabRight;
        *(float*)(uintptr_t)(g_bIt_nameV + 0x40) = valRight;
        *(float*)(uintptr_t)(g_bIt_passV + 0x40) = valRight;
        Fit(g_b_name,  T_NAME_L, kRow, 0.34f, 0.026f);
        Fit(g_b_pass,  T_PASS_L, kRow, 0.34f, 0.026f);
        Fit(g_b_nameV, T_NAME_V, kRow, valRight - 0.06f, 0.026f);
        Fit(g_b_passV, T_PASS_V, kRow, valRight - 0.06f, 0.026f);
    } else {
        SetAlign(g_bIt_name, 0, ALIGN_CENTRE);
        SetAlign(g_bIt_pass, 0, ALIGN_CENTRE);
        *(float*)(uintptr_t)(g_bIt_name + 0x40) = 0.5f;
        *(float*)(uintptr_t)(g_bIt_pass + 0x40) = 0.5f;
        Fit(g_b_name, T_NAME_L, kRow, kWLine, 0.026f);
        Fit(g_b_pass, T_PASS_L, kRow, kWLine, 0.026f);
    }
    RowShow(g_bIt_nameV, arSplit, false);
    RowShow(g_bIt_passV, arSplit, false);
    Fit(g_b_hdr,  T_HDR_NAME, kHdr, kWLine, 0.022f);
    for (int i = 0; i < kBrowseRows; ++i) {
        Fit(g_b_rowName[i], T_ROWNAME0 + i, kSmall, kWName,  0.022f);
        Fit(g_b_rowPlay[i], T_ROWPLAY0 + i, kSmall, kWPlay,  0.020f);
        Fit(g_b_rowMode[i], T_ROWMODE0 + i, kSmall, kWMode,  0.020f);
        Fit(g_b_rowTag[i],  T_ROWTAG0  + i, kSmall, kWState, 0.020f);
    }
    Fit(g_b_empty,  T_EMPTY,   kSmall, kWLine, 0.022f);
    Fit(g_b_status, T_STATUS5, kHdr,   kWLine, 0.020f);
    Fit(g_b_foot,   T_FOOT5,   kHdr,   kWLine, 0.020f);
}

static void __fastcall Hk_BrowseEnter(uint32_t self, uint32_t) {
    uint32_t fe = FeMgr();
    if (fe) {                                   // clear the cut TAG VERSUS preset flags
        *(uint8_t*)(uintptr_t)(fe + 0x4C9) = 0;
        *(uint8_t*)(uintptr_t)(fe + 0x4CA) = 0;
        *(uint8_t*)(uintptr_t)(fe + 0x502) = 4;
        *(uint8_t*)(uintptr_t)(fe + 0x508) = 0;
    }
    LanBlobSnapshot();
    if (LanGetState() == LAN_OFF) LanOpen(false, nullptr, nullptr);
    RefreshBrowserText();
    Select(self, g_bIt_host);
}

static uint32_t __fastcall Hk_BrowseUpdate(uint32_t self, uint32_t) {
    uint32_t fe = FeMgr();
    if (fe) SetBusy(fe, 0, 1);
    SuppressAttract();
    if (Transitioning()) return 5;
    if (LanMatchLaunchIfPending()) return 0x16;

    if (g_modal != MODAL_NONE) {
        if (g_modalHost != self) ModalAttach(self);
        ModalUpdate();
        if (g_modal == MODAL_NONE) {
            ModalDetach(self);
            RefreshBrowserText();
            Select(self, g_bIt_host);
            return 5;
        }
        SelectCell(self);
        RefreshModalText();
        return 5;
    }
    if (LanGetState() == LAN_LOBBY) return 15;              // a JOIN was accepted

    uint32_t sel = SelItem(self);
    if (Pressed(IN_DOWN)) NavStep(self, +1);
    if (Pressed(IN_UP))   NavStep(self, -1);
    if (Pressed(IN_B) && !AutoDriving()) {
        Sfx(1);
        LanClose("left the LAN screen");
        LanBlobRestore();
        return 4;
    }
    if (Pressed(IN_A)) {
        if (sel == g_bIt_name) { ModalOpen(MODAL_NAME, LanGetName()); }
        else if (sel == g_bIt_pass) { ModalOpen(MODAL_PASSWORD, g_hostPassword); }
        else if (sel == g_bIt_host) {
            Sfx(0x13);
            LanOpen(true, LanGetName(), g_hostPassword);
            return 15;
        } else if (sel == g_bIt_ip) { ModalOpen(MODAL_IP, ""); }
        else for (int i = 0; i < kBrowseRows; ++i) {
            if (sel != g_bIt_row[i]) continue;
            const LanGameInfo* g = LanGameAt(i);
            if (!g) break;
            if (g->locked) { g_pendingJoin = i; ModalOpen(MODAL_JOINPW, ""); }
            else { Sfx(0x13); LanJoinGame(i, ""); }
            break;
        }
    }
    RefreshBrowserText();
    return 5;
}

// ============================================================ SCREEN 15: LAN LOBBY
// THE LOBBY IS NOW FOUR SEATS ACROSS, EACH WITH THE RETAIL CHARACTER PORTRAIT.
//
// The old layout was three text columns per row (name / CHARACTER / status) and that is what
// collided: a proportional font with no width measurement means a long name simply runs into
// the column beside it. Seats side by side remove the collision by construction -- each seat
// owns a 0.21-wide slice of the row and nothing has a neighbour to run into -- and they show
// the character as ART instead of as a word, which is what the user asked for.
//
// THE ART IS THE GAME'S OWN. Screens 5 and 15 are the CUT multiplayer setup screens, and each
// still embeds FOUR REAL CHARACTER CARDS at self+0x20 (4 x 0x8D8, ctor 0x1BBC0, vtable
// 0xEEED8). The screen constructor builds them on every frontend rebuild; the cut Build we
// replaced (0x2A300 / 0x294A0) then did exactly three things per card -- FUN_0001B580 to bind
// the eleven character sprites, +0x49 = 1, AppendItem -- which is what is reproduced below.
// A card is a plain widget off the same base as a MenuOptionItem (base ctor 0x19CE0: +0x40 x,
// +0x44 y, +0x48 visible, +0x49 selectable, +0x60/+0x64 list links), so the screen's own
// Render walks to it and calls its vtable+4 like any other row.
//
// Two card calls per frame carry all the state:
//   FUN_0001BB10(card, charId)  -- the character to show; the card's Update (vtable+8) fades
//                                 the chosen sprite's alpha up to 0x80 and the other ten down
//                                 in steps of card+0x8D6, which is why TickItems must run.
//   FUN_0001BB30(card, 1)       -- the "this one is chosen" pulse the retail screens use.
// A locked character still draws, in black -- the card render tints from the unlock table at
// 0x16A23A, the same table LanCycleChar skips on.
// ---------------------------------------------------------------------------------------
// THE GEOMETRY AND THE CONTROLS ARE RETAIL SCREEN 11'S, MEASURED OUT OF IT.
//
// Screen 11 is the QUICK GAME 4-player setup: four coloured columns, each with a portrait,
// a type, a team letter and a power meter, one cursor per pad, d-pad to move and A/X to
// cycle. It is the screen this lobby should have been from the start. Its Build (0x269C0)
// writes one set of coordinates and its refresh (0x26260) OVERWRITES most of them on every
// entry -- these are the refresh values, the ones actually on screen:
//     columns   x_i = 0.155 + 0.23*i                    [0xEFDE0] + i*[0xEFDE4]
//     heading   y 0.185  scale 0.030
//     portrait  y 0.305  FUN_0001B870(0.5, 0.5)         <- retail's own size for four across
//     value 1   y 0.395  scale 0.032
//     value 2   y 0.495  scale 0.032
//     title     x 0.50 y 0.08  scale 0.055
//     corners   x 0.15 / 0.84, y 0.10, scale 0.037
//     hints     y 0.845, x 0.20 / 0.49 / 0.80, scale 0.025
//     prompt    x 0.50 y 0.895 scale 0.035
// The portrait was previously drawn at scale 1.0 -- DOUBLE retail's -- which is what forced
// everything else into the margins and made the rows collide.
static const float kSeatX[4]  = { 0.155f, 0.385f, 0.615f, 0.845f };
// 0.200 against the 0.23 seat pitch leaves a 0.030 gap. It was 0.195, and a 12-character name
// at the 0.025 scale floor measured 0.197 -- just over, so it ellipsized to "LONGESTNA..".
static const float kSeatW     = 0.200f;      // width budget for a seat's text
static const float kCardScale = 0.5f;
// The seat block is pulled up from retail's y's so the bottom third can hold the ARENA
// PICTURE, and carries a fifth control (SKIN). Pitch 0.056 at the 0.030 cell scale is 1.87x
// -- above the 1.31x touch floor and above the 1.75x that is the tightest retail ships.
static const float kSeatHeadY = 0.160f, kSeatCardY = 0.262f, kSeatNameY = 0.348f,
                   kSeatCharY = 0.404f, kSeatSkinY = 0.460f, kSeatTypeY = 0.516f,
                   kSeatTeamY = 0.572f;
// Left half of the shared block = the arena and its picture; right half = the match controls.
// The picture's scale is calibrated, not guessed: at 0.30 the composite measured ~0.41 of
// screen width, so 0.19 gives the ~0.26-wide thumbnail the column has room for.
static const float kArenaX    = 0.250f, kArenaLabelY = 0.618f;
// The picture is a real rectangle now, and its ASPECT COMES FROM THE GEOMETRY: the bk quad is
// 5.008868 x 2.0 local units under an orthographic projection, so in this normalised 640x480
// space W/H must be 2.504434 * 480/640 = 1.878325. (The bk/md TEXTURES are 256x128 = 2:1 --
// matching those instead made the preview a quarter too tall.)
static const float kArenaPicX = 0.250f, kArenaPicY = 0.752f;
static const float kArenaPicW = 0.300f, kArenaPicH = 0.159714f;   // 0.300 / 1.878325
static const float kSharedXR  = 0.700f, kSharedW = 0.42f;
static const float kSharedY[3] = { 0.640f, 0.710f, 0.780f };
static const float kHintY     = 0.850f, kPromptY = 0.905f;

static Row g_l_title, g_l_stateL, g_l_stateR, g_l_slotHead[4],
           g_l_slotName[4], g_l_slotChar[4], g_l_slotSkin[4], g_l_slotType[4],
           g_l_slotTeam[4],
           g_l_set, g_l_arena, g_l_mode, g_l_start,
           g_l_hint[3], g_l_prompt;
static uint32_t g_lIt_char[4], g_lIt_skin[4], g_lIt_type[4], g_lIt_team[4],
                g_lIt_set, g_lIt_arena, g_lIt_mode, g_lIt_start;

static uint32_t LobbyCard(uint32_t self, int i) { return self + 0x20 + (uint32_t)i * 0x8D8; }
static bool g_lCardsLive = false;      // Build got far enough to bind and append the cards

// --- the costume swatch ------------------------------------------------------
// A NUMBER IS NOT A PREVIEW. Retail's QUICK GAME setup shows each player's costume as ART --
// the swatch at the bottom of every column -- and that is the 0x724 FIGHTER widget
// (ctor 0x1B300, vtable 0xEEEAC, render 0x1B3E0). Screens 5/15 do not embed one, but the port
// can construct one in its OWN memory: the ctor chain (0x1B300 -> 0x19CE0, five 0x19990,
// 0x1ACE0) allocates nothing, joins no global list and registers no destructor. The only
// outside pointer it captures is the frontend sprite set at FE+0x3B8 -- which is exactly why
// it has to be reconstructed on every Build rather than once, or it dangles across a frontend
// rebuild. Its render drives its own pulse and needs no per-frame help.
//
// +0x723 ("locked") is forced to 1 so the swatch draws for EVERY character: 0x1B3E0 returns
// early when the character has a single costume (it tests the same static ceiling table at
// 0x169915) unless that flag is set, and a column that loses its badge for five of the eleven
// characters just looks broken.
//
// The costume the swatch resolves comes from the SAVE blob, so the launch path force-fills
// that table to the static ceiling (see lan_match.cpp) -- otherwise a costume the local save
// has not earned resolves to the wrong sprite.
#define FighterCtor(...)    GCALL(Fastcall, FnThis,    0x1B300, __VA_ARGS__)
#define FighterSetSlot(...) GCALL(Fastcall, FnThisU32, 0x1B020, __VA_ARGS__)
static uint8_t (&g_swatch)[4][0x724] = *(uint8_t(*)[4][0x724])GuestObjAlloc(
    sizeof(uint8_t[4][0x724]), 8);  // fighter-swatch objects, guest-rendered
static bool    g_swatchLive = false;
// Offset from the portrait's centre so the badge sits on its lower-right CORNER rather than
// over the character's face (the portrait is ~0.095 x 0.136 about kSeatCardY).
static const float kSwatchDX = 0.062f, kSwatchDY = 0.030f;
// SHRINKING IT TAKES A TRICK. 0x1B3E0 rewrites +0x710 back to 0.75 at the top of every frame
// while +0x70C is zero, so a scale written once is gone by the next draw. Setting +0x70C
// switches it to the "pulsing" branch, which instead ADDS +0x718 each frame -- so with +0x718
// at zero the pulse has no amplitude and +0x710 simply stays wherever it is put. At retail's
// 0.75 the badge ran into the player name underneath it.
static const float kSwatchScale = 0.42f;

// --- the arena picture, in the lobby -----------------------------------------
// THE PICTURE IS A 2D BLIT INTO AN EXACT RECTANGLE, not the carousel's geometry re-drawn.
//
// The first two attempts re-drew the retail carousel's layered 3D geometry, shrunk. That can
// never look right small: the card's backdrop plane is about 1.5 screen-widths wide, so
// full-screen retail only ever shows its MIDDLE and the edges are cropped by the screen. Shrink
// the whole composite to fit a thumbnail and you see the overhang -- the arena scene floating
// on a wider plate with the menu showing through around it, which is exactly the "transparent
// background" that was reported. There is no scissor to crop it with.
//
// The arena art is reachable as flat textures instead. Every arena layer is a single flat quad
// (bbox min.z == max.z) and `bk` and `md` subtend the SAME screen rectangle in retail, so
// blitting both into one destination rect reproduces the card exactly, edge to edge, at any
// size. The game's own 2D sprite call takes a scene and a raw material INDEX:
//     FUN_000797B0(scene, matIndex, x0, y0, x1, y1, &rgba)
// which is the very call the character portraits reach through FUN_00019B30.
//
// WHY THE INDEX AND NOT A NAME: LEVEL.XMF's 73 materials carry no names (material+0x38 holds
// {u16 id, 0x0011}), all its texture name slots are the literal string "xxxx" and its IDSF
// table is empty -- so the name resolver FUN_00079BA0 matches nothing and silently binds
// material 0. Passing a name here gets you the wrong picture with no error.
using FnDraw2D = void (__cdecl*)(uint32_t scene, uint32_t matIndex,
                                 float x0, float y0, float x1, float y1, const uint32_t* rgba);
#define Draw2D(...) GCALL(Cdecl, FnDraw2D, 0x797B0, __VA_ARGS__)
// EVERY RECTANGLE BELOW IS MEASURED FROM LEVEL.XMF's OWN SUBMESH BOUNDING BOXES. The first
// attempt guessed that the two foreground props tiled the card's halves; they do not, and that
// guess was wrong on all thirteen arenas. The projection is ORTHOGRAPHIC -- retail parks the
// layers at z +2.5 / -10 / -20 / -30 / -40 purely for painter order and writes a uniform 1.08
// node scale for skye/bk/md/fg -- so a layer's screen size is just its local size, and the
// widths growing 5.0089 (bk) < 6.5406 (md) < 7.8206 (fg) is parallax OVERSCAN that retail
// simply lets run off the edges of the screen.
//
// Rects are fractions of the CARD (the bk quad): u 0 = its left edge, 1 = its right; v 0 = its
// top, 1 = its bottom. Anything outside 0..1 is overscan, and CardBlit CROPS it with a
// source-UV window instead of squashing it -- which reproduces exactly what the screen edges
// do at full size. Arena order is the game's own, 0 KITCHEN .. 12 HELL.
struct CardRect { float u0, v0, u1, v1; };
static const uint16_t kArenaSky = 72;      // one shared backdrop plate, drawn under everything
static const uint16_t kArenaBk[13] = { 18, 71, 66, 62, 60, 70, 64, 12, 30, 68, 34, 22, 26 };
static const uint16_t kArenaMd[13] = { 17, 36, 65, 61, 59, 69, 63, 11, 29, 67, 33, 21, 25 };
// The two real foreground props are 2.0 x 2.0 world squares hard against the card's left and
// right EDGES with a gap between them -- 39.93% of the card width each, not 50%. That 25%
// oversize is exactly why they looked too big. The fg's third submesh (material 56) is a 16x16
// texture with alpha 0 in every texel; it exists only to widen the object's bounding box for
// the carousel's slide animation, so it is never drawn.
// FIVE ARENAS STORE THEIR TWO PROPS IN THE OPPOSITE ORDER -- submesh order inside the fg object
// is not consistent -- so SCRAPYARD, SHIP, CABIN, BEACH and WILD WEST had theirs mirrored.
static const uint16_t kArenaFgL[13] = { 16, 38, 46, 52, 54, 58, 48, 10, 28, 50, 32, 20, 24 };
static const uint16_t kArenaFgR[13] = { 15, 37, 47, 53, 55, 57, 49,  9, 27, 51, 31, 19, 23 };
static const CardRect kRectBk  = {  0.000000f, 0.0f, 1.000000f, 1.0f };
static const CardRect kRectMd  = { -0.152902f, 0.0f, 1.152902f, 1.0f };
static const CardRect kRectFgL = { -0.038369f, 0.0f, 0.360923f, 1.0f };
static const CardRect kRectFgR = {  0.607559f, 0.0f, 1.006851f, 1.0f };
// THE NAME PLATE. Retail draws it and the preview did not, which is why the picture had no
// arena name on it. It needs its own rect for two independent reasons: its size differs per
// arena (1.75 to 3.16 wide against the card's 5.01), and retail leaves its node at scale 1.00
// while the rest of the card runs at 1.08.
static const uint16_t kArenaTt[13] = { 41, 14, 7, 42, 39, 8, 45, 5, 40, 43, 44, 6, 13 };
static const CardRect kRectTt[13] = {
    /*  0 KITCHEN    */ { 0.329142f, 0.102169f, 0.670858f, 0.890601f },
    /*  1 HAUNTED    */ { 0.242766f, 0.284267f, 0.757234f, 0.708502f },
    /*  2 SCRAPYARD  */ { 0.338580f, 0.284681f, 0.661420f, 0.708089f },
    /*  3 SHIP       */ { 0.338086f, 0.175332f, 0.661914f, 0.817437f },
    /*  4 CABIN      */ { 0.241860f, 0.230696f, 0.758140f, 0.769304f },
    /*  5 BANQUET    */ { 0.247349f, 0.194767f, 0.752651f, 0.798002f },
    /*  6 BEACH      */ { 0.240314f, 0.195919f, 0.759686f, 0.796850f },
    /*  7 SKYSCRAPER */ { 0.267563f, 0.239081f, 0.732437f, 0.753689f },
    /*  8 LAB        */ { 0.208181f, 0.303240f, 0.791819f, 0.689529f },
    /*  9 WILDWEST   */ { 0.247349f, 0.194767f, 0.752651f, 0.798002f },
    /* 10 BOXING     */ { 0.267563f, 0.239081f, 0.732437f, 0.753689f },
    /* 11 MARKET     */ { 0.242766f, 0.316148f, 0.757234f, 0.676621f },
    /* 12 HELL       */ { 0.242766f, 0.316148f, 0.757234f, 0.676621f },
};

// FUN_000797B0 is only a wrapper -- it stuffs {material, 0,0,1,1} on the stack and tail-calls
// FUN_00079260, which takes the UV rect explicitly. Calling the inner one directly is the same
// code path, and it is what lets an overscanning layer be CROPPED to the card rather than
// squashed into it (and so kept out of the rest of the menu).
// ⚠ 4-BYTE-SLOT RULE. The guest reads this descriptor as {u32 material, 4 floats} = 20
// bytes. `const void* mat` was 4 bytes on the 32-bit Windows host and is EIGHT on
// aarch64 — which would silently move every float to the wrong offset and hand the 2D
// painter garbage UVs. A guest-visible field is a uint32_t, never a host pointer.
struct Blit2D { uint32_t mat; float u0, v0, u1, v1; };
static_assert(sizeof(Blit2D) == 20, "the guest reads this as u32 + 4 floats");
using FnDraw2DUV = void (__cdecl*)(Blit2D* desc, float x0, float y0, float x1, float y1,
                                   const uint32_t* rgba);
#define Draw2DUV(...) GCALL(Cdecl, FnDraw2DUV, 0x79260, __VA_ARGS__)

// THE NEUTRAL TINT retail's own items use. GUEST ARENA, not a host static, for the same
// reason the descriptor below is: the guest 2D painter DEREFERENCES this pointer, and a
// host-image address is above 4 GB on ARM.
static const uint32_t* NeutralTint() {
    static uint32_t* p = nullptr;
    if (!p) {
        p = (uint32_t*)GuestObjAlloc(4 * sizeof(uint32_t), 4);
        p[0] = p[1] = p[2] = p[3] = 0x80;
    }
    return p;
}

static void CardBlit(uint32_t scene, uint16_t mat, const CardRect& r,
                     float X0, float Y0, float X1, float Y1, const uint32_t* rgba) {
    const float w = r.u1 - r.u0, h = r.v1 - r.v0;
    if (!(w > 1e-6f) || !(h > 1e-6f)) return;
    const float cu0 = r.u0 < 0.0f ? 0.0f : r.u0, cu1 = r.u1 > 1.0f ? 1.0f : r.u1;
    const float cv0 = r.v0 < 0.0f ? 0.0f : r.v0, cv1 = r.v1 > 1.0f ? 1.0f : r.v1;
    if (cu1 <= cu0 || cv1 <= cv0) return;                  // entirely overscan
    // ⚠ GUEST-ARENA SCRATCH, NOT A STACK LOCAL. FUN_00079260 DEREFERENCES this descriptor,
    // so its address must be guest-visible. A host stack local works on x86 (the host stack
    // is below 4 GB) and FATALs on the device with `guest-call arg 0 above 4 GB calling
    // guest 00079260` — which is precisely what killed HOSTING a LAN game on the phone.
    // The qemu leg CANNOT catch this class: its whole image is below 4 GB (gate S5c's
    // lesson), so the device is the only detector. Same fix shape as Fit()'s ellipsize
    // buffer. Single-threaded UI render path (Screen::Render walks the widget list on the
    // game thread), so a static is safe.
    static Blit2D& d = *(Blit2D*)GuestObjAlloc(sizeof(Blit2D), 8);
    d.mat = *(uint32_t*)(uintptr_t)(scene + 0x24) + (uint32_t)mat * 0x50;
    d.u0 = (cu0 - r.u0) / w; d.u1 = (cu1 - r.u0) / w;      // the still-visible source window
    d.v0 = (cv0 - r.v0) / h; d.v1 = (cv1 - r.v0) / h;
    const float W = X1 - X0, H = Y1 - Y0;
    Draw2DUV(&d, X0 + cu0 * W, Y0 + cv0 * H, X0 + cu1 * W, Y0 + cv1 * H, rgba);
}

// A DLL-side widget with our own 4-entry vtable, appended to the screen like any other row so
// the screen's own Render walks to it. The base layout the screen cares about is just
// +0x40/+0x44 (position), +0x48 (visible) and +0x60/+0x64 (list links).
static void __fastcall PicNop(uint32_t, uint32_t) {}
static void __fastcall PicRender(uint32_t self, uint32_t) {
    if (!*(uint8_t*)(uintptr_t)(self + 0x48)) return;
    uint32_t fe = FeMgr();
    if (!fe) return;
    const int arena = (int)LanConfigArena();
    if (arena < 0 || arena >= 13) return;
    const uint32_t scene = fe + 0x38C;                    // the LEVEL.XMF scene
    const uint32_t* kRgba = NeutralTint();   // guest arena — the guest reads these 4 words
    const float x0 = kArenaPicX - kArenaPicW * 0.5f, x1 = kArenaPicX + kArenaPicW * 0.5f;
    const float y0 = kArenaPicY - kArenaPicH * 0.5f, y1 = kArenaPicY + kArenaPicH * 0.5f;
    // Retail's own painter order. The queue is strictly insertion-ordered -- no sort key, no
    // depth test -- so submission order IS draw order; and the 2D path sets all of its own
    // blend, depth and cull state on every flush, so there is nothing here to set or restore.
    // The sky goes down first, stretched to the whole card: it is a flat dark plate, so
    // stretching it is invisible, and it guarantees there is no transparent gap anywhere --
    // which matters for HAUNTED, whose "backdrop" is an 8x8 fully transparent texture and
    // which would otherwise show the menu through the middle of its own picture.
    CardBlit(scene, kArenaSky,        kRectBk,        x0, y0, x1, y1, kRgba);
    CardBlit(scene, kArenaBk[arena],  kRectBk,        x0, y0, x1, y1, kRgba);
    CardBlit(scene, kArenaMd[arena],  kRectMd,        x0, y0, x1, y1, kRgba);
    CardBlit(scene, kArenaFgL[arena], kRectFgL,       x0, y0, x1, y1, kRgba);
    CardBlit(scene, kArenaFgR[arena], kRectFgR,       x0, y0, x1, y1, kRgba);
    CardBlit(scene, kArenaTt[arena],  kRectTt[arena], x0, y0, x1, y1, kRgba);
}
static uint32_t MakeArenaPic() {
    // Stage-5 dispatch (dispatch.h): the widget's vtable entries are guest->host
    // boundaries — guest Screen::Render calls through them. Register and store keys
    // (identical words on the x86 host). The vtable AND the widget object are
    // guest-walked (their addresses live in the screen's item list), so they come
    // from the guest-visible arena — plain statics on x86, below 4 GB on ARM.
    static uint32_t* picVtbl = nullptr;
    static uint8_t*  picObj = nullptr;
    if (!picVtbl) {
        picVtbl = (uint32_t*)GuestObjAlloc(4 * 4, 4);
        picObj  = (uint8_t*)GuestObjAlloc(0x68, 8);
    }
    picVtbl[0] = DispatchRegister(HOOK_FC(PicNop),    "lan:pic.nop");   // dtor -- never runs
    picVtbl[1] = DispatchRegister(HOOK_FC(PicRender), "lan:pic.render");// Screen::Render
    picVtbl[2] = picVtbl[0];                                            // item tick
    picVtbl[3] = picVtbl[0];
    memset(picObj, 0, 0x68);
    uint32_t it = Gp32(picObj);
    *(uint32_t*)(uintptr_t)(it + 0x00) = Gp32(picVtbl);
    *(float*)(uintptr_t)(it + 0x40) = kArenaPicX;
    *(float*)(uintptr_t)(it + 0x44) = kArenaPicY;
    *(uint8_t*)(uintptr_t)(it + 0x48) = 1;
    *(uint8_t*)(uintptr_t)(it + 0x49) = 0;            // never selectable
    return it;
}

// --- the 2-D cursor ----------------------------------------------------------
// THE OLD LOBBY NAVIGATED A LINKED LIST. The seats are drawn as COLUMNS but the items were
// appended seat-by-seat, so UP/DOWN walked ACROSS the screen -- exactly the complaint. Retail
// keeps two integers per cursor (screen11+0x60A8 col, +0x60B8 row) and indexes a static
// [col + row*4] table of cell codes where -1 is a hole to step over. This is that, for us.
enum {
    C_NONE = 0, C_CHAR, C_SKIN, C_TYPE, C_TEAM, C_ARENA, C_SET, C_MODE, C_START
};
static const int kRows = 7;
// A control is DUPLICATED across every cell it visually spans rather than left as a hole:
// retail's hole-skip only retries once along an axis, so a hole beneath a populated column
// strands the cursor. ARENA is one tall cell on the left, beside its picture; the three match
// controls stack on the right.
static const uint8_t kCell[kRows][4] = {
    { C_CHAR,  C_CHAR,  C_CHAR,  C_CHAR  },   // row 0: the character in each seat
    { C_SKIN,  C_SKIN,  C_SKIN,  C_SKIN  },   // row 1: its costume
    { C_TYPE,  C_TYPE,  C_TYPE,  C_TYPE  },   // row 2: OPEN / CPU (the host fills seats here)
    { C_TEAM,  C_TEAM,  C_TEAM,  C_TEAM  },   // row 3: the team letter
    { C_ARENA, C_ARENA, C_SET,   C_SET   },   // row 4
    { C_ARENA, C_ARENA, C_MODE,  C_MODE  },   // row 5
    { C_ARENA, C_ARENA, C_START, C_START },   // row 6
};
static int g_curCol = 0, g_curRow = 0;

// A SECOND PERSON AT THE SAME PC GETS THEIR OWN CURSOR. Retail screen 11 has exactly one
// selection flare, and it belongs to this machine's player 1 -- so the extra players get a
// cursor that is locked to their OWN seat column and walks only the four rows in it
// (character, skin, type, team). That is everything they can legitimately change anyway: the
// arena, the rules and START are the host's, and a second player sharing a PC is never the
// host in their own right. Locking the column also means their cursor needs no flare of its
// own: it is drawn as a "<P3" caret appended to the cell they are on.
static int g_cur2Row[4] = { 0, 0, 0, 0 };          // by LOCAL player index (1..3 used)
static const int kSeatRows = 4;                    // rows 0..3 are the per-seat controls

// Per-pad edge test. `Pressed` deliberately accepts ANY pad so a lone player can use whichever
// controller they picked up; once two people share the lobby each must be read separately or
// one d-pad would move both cursors.
static bool PressedPad(uint32_t id, int pad) {
    uint32_t in = InputObj();
    return in && pad >= 0 && pad < 4 && InputTest(in, 0, id, (uint32_t)pad) != 0;
}

static uint32_t CellItem(int row, int col) {
    switch (kCell[row][col]) {
    case C_CHAR:  return g_lIt_char[col];
    case C_SKIN:  return g_lIt_skin[col];
    case C_TYPE:  return g_lIt_type[col];
    case C_TEAM:  return g_lIt_team[col];
    case C_ARENA: return g_lIt_arena;
    case C_SET:   return g_lIt_set;
    case C_MODE:  return g_lIt_mode;
    case C_START: return g_lIt_start;
    default:      return 0;
    }
}
// The cursor may LAND on anything the screen shows; whether the action does something is the
// action's business. Punching holes based on "can this peer drive it" is what made rows appear
// and disappear under the cursor as the lobby state changed.
static void CursorApply(uint32_t self) {
    if (g_curRow < 0) g_curRow = kRows - 1;
    if (g_curRow >= kRows) g_curRow = 0;
    if (g_curCol < 0) g_curCol = 3;
    if (g_curCol >= 4) g_curCol = 0;
    uint32_t it = CellItem(g_curRow, g_curCol);
    if (it) Select(self, it);
}
// Stepping past a control that spans several cells has to compare the RESOLVED ITEM, never the
// cell code: all four seat columns carry the SAME code (C_CHAR, C_TYPE, ...), so a code
// comparison skipped every column and the guard dumped the cursor back where it started --
// left/right did nothing at all and the first and last columns were unreachable.
static void CursorMove(uint32_t self, int dcol, int drow) {
    int col = g_curCol, row = g_curRow;
    const uint32_t from = CellItem(row, col);
    for (int guard = 0; guard < 8; ++guard) {
        col = (col + dcol + 4) & 3;
        row = (row + drow + kRows) % kRows;
        uint32_t it = CellItem(row, col);
        if (!it) continue;                           // a hole: keep stepping the same way
        if (it == from) continue;                    // the same control again: step past it
        break;
    }
    g_curCol = col; g_curRow = row;
    CursorApply(self);
    Sfx(5);
    printf("[lan] cursor -> col %d row %d (cell %d)\n", col, row, kCell[row][col]);
}

// Player 1's view of the pad. With one person at this PC any controller drives the menus (the
// long-standing behaviour). The moment a second person is in the lobby that has to stop: their
// d-pad would move BOTH cursors and their A press would change player 1's character.
static bool PressedP1(uint32_t id) {
    return LanLocalCount() > 1 ? PressedPad(id, 0) : Pressed(id);
}

static void __fastcall Hk_LobbyBuild(uint32_t self, uint32_t) {
    ScreenReset(self);
    g_lCardsLive = false;
    g_swatchLive = false;
    AppendItem(self, 0, MakeRow(g_l_title, kTextBase + T_TITLE15,
                                0.5f, 0.08f, false, kScTitle, ALIGN_CENTRE, kColOrange));
    AppendItem(self, 0, MakeRow(g_l_stateL, kTextBase + T_LSTATEL,
                                0.15f, 0.10f, false, kScCorner, ALIGN_CENTRE, kColGold));
    AppendItem(self, 0, MakeRow(g_l_stateR, kTextBase + T_LSTATER,
                                0.84f, 0.10f, false, kScCorner, ALIGN_CENTRE, kColGold));
    for (int i = 0; i < 4; ++i) {
        AppendItem(self, 0, MakeRow(g_l_slotHead[i], (uint16_t)(kTextBase + T_SLOTHEAD0 + i),
                                    kSeatX[i], kSeatHeadY, false, kScHead, ALIGN_CENTRE,
                                    kColOrange));
        // The card next, so it draws BEHIND the seat's text if the two ever overlap.
        // FUN_0001B580 binds sprites out of the frontend's own set at FE+0x3B8, so it is only
        // ever safe from a Build the frontend factory is driving -- which is the only caller.
        if (FeMgr()) {
            uint32_t card = LobbyCard(self, i);
            CardLoad(card, 0);
            *(float*)(uintptr_t)(card + 0x40) = kSeatX[i];
            *(float*)(uintptr_t)(card + 0x44) = kSeatCardY;
            CardSetScale(card, 0, kCardScale, kCardScale);
            CardSetStep(card, 0, 4);                   // the ctor's own fade step
            CardSetState(card, 0, 0);
            // NOT selectable: the CHARACTER row under it carries the cursor, so the game's
            // flare lands behind that text (the flare is drawn at the item's RAW x, which is
            // why anything carrying a selection has to be centred).
            *(uint8_t*)(uintptr_t)(card + 0x48) = 0;
            *(uint8_t*)(uintptr_t)(card + 0x49) = 0;
            AppendItem(self, 0, card);
            g_lCardsLive = true;

            uint32_t sw = (uint32_t)(uintptr_t)g_swatch[i];
            memset(g_swatch[i], 0, sizeof g_swatch[i]);
            FighterCtor(sw, 0);
            FighterSetSlot(sw, 0, (uint32_t)i);       // rebinds its five quads to FE+0x3B8
            *(float*)(uintptr_t)(sw + 0x40) = kSeatX[i] + kSwatchDX;
            *(float*)(uintptr_t)(sw + 0x44) = kSeatCardY + kSwatchDY;
            *(uint8_t*)(uintptr_t)(sw + 0x723) = 1;   // draw even for one-costume characters
            *(uint8_t*)(uintptr_t)(sw + 0x70C) = 1;   // take the branch that leaves +0x710 be
            *(float*)(uintptr_t)(sw + 0x718) = 0.0f;  // ...with no pulse amplitude
            *(float*)(uintptr_t)(sw + 0x710) = kSwatchScale;
            *(uint8_t*)(uintptr_t)(sw + 0x48) = 1;
            *(uint8_t*)(uintptr_t)(sw + 0x49) = 0;    // the SKIN text row carries the cursor
            AppendItem(self, 0, sw);
            g_swatchLive = true;
        }
        AppendItem(self, 0, MakeRow(g_l_slotName[i], (uint16_t)(kTextBase + T_SLOTNAME0 + i),
                                    kSeatX[i], kSeatNameY, false, kScName, ALIGN_CENTRE,
                                    kColGold));
        g_lIt_char[i] = MakeRow(g_l_slotChar[i], (uint16_t)(kTextBase + T_SLOTCHAR0 + i),
                                kSeatX[i], kSeatCharY, true, kScCell);
        g_lIt_skin[i] = MakeRow(g_l_slotSkin[i], (uint16_t)(kTextBase + T_SLOTSKIN0 + i),
                                kSeatX[i], kSeatSkinY, true, kScCell);
        g_lIt_type[i] = MakeRow(g_l_slotType[i], (uint16_t)(kTextBase + T_SLOTTYPE0 + i),
                                kSeatX[i], kSeatTypeY, true, kScCell);
        g_lIt_team[i] = MakeRow(g_l_slotTeam[i], (uint16_t)(kTextBase + T_SLOTTEAM0 + i),
                                kSeatX[i], kSeatTeamY, true, kScCell);
        AppendItem(self, 0, g_lIt_char[i]);
        AppendItem(self, 0, g_lIt_skin[i]);
        AppendItem(self, 0, g_lIt_type[i]);
        AppendItem(self, 0, g_lIt_team[i]);
    }
    g_lIt_arena = MakeRow(g_l_arena, kTextBase + T_ARENA, kArenaX,   kArenaLabelY, true, kScRow);
    g_lIt_set   = MakeRow(g_l_set,   kTextBase + T_SET,   kSharedXR, kSharedY[0],  true, kScRow);
    g_lIt_mode  = MakeRow(g_l_mode,  kTextBase + T_MODE,  kSharedXR, kSharedY[1],  true, kScRow);
    g_lIt_start = MakeRow(g_l_start, kTextBase + T_START, kSharedXR, kSharedY[2],  true, kScRow);
    AppendItem(self, 0, g_lIt_arena);
    AppendItem(self, 0, g_lIt_set);
    AppendItem(self, 0, g_lIt_mode);
    AppendItem(self, 0, g_lIt_start);
    // The arena picture goes in LAST so it is submitted after the menu's own art.
    AppendItem(self, 0, MakeArenaPic());
    // The hint band is retail's, verbatim: three short verbs on one line, never a single
    // string glued together with padding spaces in a proportional font.
    static const float kHintX[3] = { 0.20f, 0.49f, 0.80f };
    for (int i = 0; i < 3; ++i)
        AppendItem(self, 0, MakeRow(g_l_hint[i], (uint16_t)(kTextBase + T_HINT0 + i),
                                    kHintX[i], kHintY, false, kScHint, ALIGN_CENTRE,
                                    kColOrange));
    AppendItem(self, 0, MakeRow(g_l_prompt, kTextBase + T_PROMPT,
                                0.5f, kPromptY, false, kScPrompt, ALIGN_CENTRE, kColGold));
    BuildModalRows(self);
    ShowModalRows(false);
}

// Who may drive a given cell. The cursor can always LAND on one -- this only decides whether
// the action does anything, and whether the row is drawn at half alpha.
// "Mine" means ANY seat this PC drives, not just the primary one. With two people sharing a
// machine the second seat is every bit as local as the first, and testing `col == LanLocalSlot()`
// locked that player out of their own character, skin and team.
static bool SeatIsMine(int col) {
    const LanSlotInfo* s = LanSlot(col);
    return s && s->isLocal != 0;
}

static bool CellEnabled(int row, int col) {
    const bool host = LanIsHost();
    const int  me   = col;                       // ownership is per-seat now, see SeatIsMine
    (void)me;
    const LanSlotInfo* s = LanSlot(col);
    switch (kCell[row][col]) {
    case C_CHAR: return SeatIsMine(col) || (host && s && s->kind == 2);
    // A character with only one costume has nothing to cycle, so the cell greys out on its
    // own -- five of the eleven are like that, and the ceiling is the same on every PC.
    // Only what THIS save has unlocked. On a fresh save that is one costume per character, so
    // the row is simply dimmed until the player earns something -- progression is preserved.
    case C_SKIN: return (SeatIsMine(col) || (host && s && s->kind == 2)) && s && s->kind &&
                        LanCostumeUnlocked(s->charId) > 1;
    // The host may act on ANY seat that is not this PC's: empty cycles OPEN<->CPU, and a
    // seat with a person in it is removed. Somebody has to be able to clear a seat when a
    // pad dies or a player wanders off.
    case C_TYPE: return host && !SeatIsMine(col);
    case C_TEAM: return SeatIsMine(col) || (host && s && s->kind);
    case C_ARENA: case C_SET: case C_MODE: case C_START: return host;
    default: return false;
    }
}

static void RefreshLobbyText(uint32_t self) {
    const bool host = LanIsHost();
    const int  me   = LanLocalSlot();
    Txt(T_TITLE15, "%s", L(TXT_LOBBY, "LAN LOBBY"));
    int players = 0;
    for (int i = 0; i < 4; ++i) { const LanSlotInfo* s = LanSlot(i); if (s && s->kind) ++players; }
    Txt(T_LSTATEL, "%s", host ? L(TXT_HOSTING, "HOSTING") : L(TXT_JOINED, "JOINED"));
    if (LanPingMs()) Txt(T_LSTATER, "%d/4 - %uMS", players, LanPingMs());
    else             Txt(T_LSTATER, "%d/4", players);

    for (int i = 0; i < 4; ++i) {
        uint32_t card = LobbyCard(self, i);
        const LanSlotInfo* s = LanSlot(i);
        const bool used = s && s->kind;
        const bool mine = (i == me) || (host && s && s->kind == 2);
        Txt(T_SLOTHEAD0 + i, "%s %d", i == me ? L(TXT_YOU, "YOU - P")
                                            : L(TXT_PLAYER, "PLAYER"), i + 1);
        Txt(T_SLOTNAME0 + i, "%s", !used ? " "
            : (s->kind == 2 ? L(TXT_COMPUTER, "COMPUTER") : s->name));
        // EVERY CELL STAYS VISIBLE, even on an empty seat. Hiding a cell the cursor can still
        // land on means the selection flare draws on an invisible row -- the cursor simply
        // disappears when you move into an open seat. Empty cells show "-" and are disabled.
        Txt(T_SLOTCHAR0 + i, "%s", used ? LanCharName(s->charId) : "-");
        // "n/m" only where m is knowable -- this PC's own unlock state. A remote player's
        // save is not ours to report, so their seat just names the skin they picked.
        if (!used)                       Txt(T_SLOTSKIN0 + i, "-");
        else if (!mine)                  Txt(T_SLOTSKIN0 + i, "%s %d",
                                         L(TXT_SKIN, "SKIN"), s->costume + 1);
        else if (LanCostumeUnlocked(s->charId) <= 1) Txt(T_SLOTSKIN0 + i, "%s 1",
                                                     L(TXT_SKIN, "SKIN"));
        else Txt(T_SLOTSKIN0 + i, "%s %d/%d", L(TXT_SKIN, "SKIN"),
                 s->costume + 1, LanCostumeUnlocked(s->charId));
        // One meaning per slot, always. The old lobby put "ADD CPU" in the READY slot, so the
        // same line meant a state on a filled seat and an instruction on an empty one.
        Txt(T_SLOTTYPE0 + i, "%s", !used ? L(TXT_SEATOPEN, "OPEN")
            : (s->kind == 2 ? L(TXT_COMPUTER, "CPU") : L(TXT_PLAYER, "HUMAN")));
        // Say what the button will actually do, and only while the host is standing on it: a
        // cell that reads "HUMAN" gives no hint that A throws that player out of the game.
        if (host && used && s->kind == 1 && !SeatIsMine(i) &&
            g_curRow == 2 && g_curCol == i)
            Txt(T_SLOTTYPE0 + i, "%s", L(TXT_REMOVE, "REMOVE?"));
        const bool rdy = (s && s->kind == 1 && s->ready);
        if (used) Txt(T_SLOTTEAM0 + i, "%s%s%s", LanTeamName(s->team),
                      rdy ? "  " : "", rdy ? L(TXT_READY, "READY") : "");
        else      Txt(T_SLOTTEAM0 + i, "-");
        // The extra local players' cursors. They have no selection flare of their own -- there
        // is only one on this screen and it is player 1's -- so each marks the cell it sits on
        // with its player number. Appended after the cell text is composed, so it survives
        // whatever the row happens to say this frame.
        for (int j = 1; j < LanLocalCount(); ++j) {
            if (LanLocalSlotAt(j) != i) continue;
            int row = g_cur2Row[j];
            if (row < 0 || row >= kSeatRows) row = 0;
            static const uint16_t kRowText[kSeatRows] = { T_SLOTCHAR0, T_SLOTSKIN0,
                                                          T_SLOTTYPE0, T_SLOTTEAM0 };
            TxtAppend((uint16_t)(kRowText[row] + i), "  <P%d", i + 1);
        }
        RowShow((uint32_t)(uintptr_t)g_l_slotName[i].mem, used, false);
        RowShow(g_lIt_char[i], true, true);
        RowShow(g_lIt_skin[i], true, true);
        RowShow(g_lIt_type[i], true, true);
        RowShow(g_lIt_team[i], true, true);
        if (g_swatchLive) {
            uint32_t sw = (uint32_t)(uintptr_t)g_swatch[i];
            *(uint8_t*)(uintptr_t)(sw + 0x48) = used ? 1 : 0;
            if (used) {
                // Written every frame and AFTER any setSlot: 0x1B020 pulls the character out
                // of FE+0x4AF, which the lobby has not filled in yet -- that only happens at
                // launch. The lobby's own state is the truth here.
                *(uint8_t*)(uintptr_t)(sw + 0x721) = s->charId;
                *(uint8_t*)(uintptr_t)(sw + 0x722) = s->costume;
            }
        }
        if (g_lCardsLive) {
            *(uint8_t*)(uintptr_t)(card + 0x48) = used ? 1 : 0;
            if (used) {
                CardSetChar(card, 0, s->charId);
                // The pulse marks the seat you are steering, exactly as on the retail
                // character select. 0x1BB30 only rewrites animation fields, so re-asserting
                // it every frame is free.
                CardSetState(card, 0, i == me ? 1 : 0);
            }
        }
        Fit(g_l_slotHead[i], T_SLOTHEAD0 + i, kScHead, kSeatW, 0.025f);
        Fit(g_l_slotName[i], T_SLOTNAME0 + i, kScName, kSeatW, 0.024f);
        Fit(g_l_slotChar[i], T_SLOTCHAR0 + i, kScCell, kSeatW, 0.025f);
        Fit(g_l_slotSkin[i], T_SLOTSKIN0 + i, kScCell, kSeatW, 0.025f);
        Fit(g_l_slotType[i], T_SLOTTYPE0 + i, kScCell, kSeatW, 0.025f);
        Fit(g_l_slotTeam[i], T_SLOTTEAM0 + i, kScCell, kSeatW, 0.025f);
    }

    Txt(T_ARENA, "%s  %s", L(TXT_ARENA, "ARENA:"), LanArenaName(LanConfigArena()));
    Txt(T_SET,   "%s", L(TXT_FIGHTSET, "FIGHT SETTINGS"));
    const char* const kModeName[3] = { L(TXT_QUICKMATCH, "QUICK MATCH"),
                                       L(TXT_TOURNAMENT, "TOURNAMENT"),
                                       L(TXT_MEATRUSH,   "MEAT RUSH") };
    Txt(T_MODE,  "%s  %s", L(TXT_MODE, "MODE:"),
        kModeName[LanConfigMode() < 3 ? LanConfigMode() : 0]);
    Txt(T_START, "%s", L(TXT_STARTMATCH, "START MATCH"));
    Txt(T_HINT0 + 0, "%s", L(TXT_DPADMOVE, "D-PAD MOVE"));
    Txt(T_HINT0 + 1, BTN_A " %s", L(TXT_CHANGE, "CHANGE"));
    Txt(T_HINT0 + 2, BTN_B " %s", L(TXT_LEAVE, "LEAVE"));
    // One prompt, in retail's own slot, saying the single most useful thing: for the host why
    // it cannot start yet, for everyone else what START does.
    const char* why = host ? LanStartRefusal() : nullptr;
    if (host)                Txt(T_PROMPT, "%s", why ? why : L(TXT_BEGIN, "PRESS START TO BEGIN"));
    else if (LanSlot(me) && LanSlot(me)->ready) Txt(T_PROMPT, "%s", L(TXT_READYWAIT, "READY - WAITING FOR THE HOST"));
    else                     Txt(T_PROMPT, "%s", L(TXT_WHENREADY, "PRESS START WHEN YOU ARE READY"));

    Fit(g_l_arena,  T_ARENA,  kScRow, kSharedW, 0.030f);
    Fit(g_l_set,    T_SET,    kScRow, kSharedW, 0.030f);
    Fit(g_l_mode,   T_MODE,   kScRow, kSharedW, 0.030f);
    Fit(g_l_start,  T_START,  kScRow, kSharedW, 0.030f);
    Fit(g_l_prompt, T_PROMPT, kScPrompt, kWLine, 0.025f);
    Fit(g_l_stateL, T_LSTATEL, kScCorner, 0.26f, 0.025f);
    Fit(g_l_stateR, T_LSTATER, kScCorner, 0.26f, 0.025f);
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < 4; ++c) {
            uint32_t it = CellItem(r, c);
            if (it) RowEnable(it, CellEnabled(r, c));
        }
}

static void __fastcall Hk_LobbyEnter(uint32_t self, uint32_t) {
    LanBlobSnapshot();
    // After the snapshot, so LanCostumeUnlocked still reports the player's real save while
    // every costume -- including one only the other player has earned -- can still be drawn.
    LanCostumeFillTable();
    // Start on your own seat's character -- the one thing everybody wants to change first.
    g_curCol = LanLocalSlot() & 3;
    g_curRow = 0;
    RefreshLobbyText(self);
    CursorApply(self);
}

static uint32_t __fastcall Hk_LobbyUpdate(uint32_t self, uint32_t) {
    uint32_t fe = FeMgr();
    if (fe) SetBusy(fe, 0, 1);
    SuppressAttract();
    if (Transitioning()) return 15;
    if (LanMatchLaunchIfPending()) return 0x16;
    if (LanGetState() == LAN_OFF) return 4;

    // The cards animate their character crossfade in their Update (vtable+8), which is what
    // the base screen's item tick calls; every retail screen that owns cards runs it first.
    // Our own rows' vtable+8 is 0x241B0 -- a bare ret -- so this costs them nothing.
    TickItems(self, 0);

    if (g_modal != MODAL_NONE) {
        if (g_modalHost != self) ModalAttach(self);
        ModalUpdate();
        if (g_modal == MODAL_NONE) {
            ModalDetach(self);
            RefreshLobbyText(self);
            CursorApply(self);
        } else {
            SelectCell(self);
            RefreshModalText();
        }
        return 15;
    }

    if (PressedP1(IN_B) && !AutoDriving()) { Sfx(1); LanLeave(); LanBlobRestore(); return 4; }

    // MOVE. The d-pad moves the cursor and nothing else -- left/right between the seat
    // columns, up/down between the controls in one. That is the whole point of the redesign:
    // the layout is a grid, so the d-pad has to be a grid.
    if (PressedP1(IN_LEFT))  CursorMove(self, -1, 0);
    if (PressedP1(IN_RIGHT)) CursorMove(self, +1, 0);
    if (PressedP1(IN_UP))    CursorMove(self, 0, -1);
    if (PressedP1(IN_DOWN))  CursorMove(self, 0, +1);

    // CHANGE. Retail moved every value change onto A (id 7, forward) and X (id 8, backward)
    // the moment a screen had more than one column, and prints that mapping in its hint band.
    // Confirmed on this port by driving the real screen 11: A stepped TEAM A -> B -> C and X
    // stepped it back. IN_A (id 1) is accepted as a synonym for forward.
    // ...but START MUST NOT ALSO CHANGE THE VALUE. Retail's "next" id is satisfied by START as
    // well, so pressing START to go ready was silently cycling whatever the cursor sat on --
    // pick your character, press START, and the character changed. START means ready (or, for
    // the host, begin) and nothing else, so it is taken out of the running here first.
    const bool startPressed = PressedP1(IN_START);
    int step = startPressed ? 0
             : (PressedP1(IN_NEXT) || PressedP1(IN_A)) ? +1 : PressedP1(IN_PREV) ? -1 : 0;
    if (step) {
        const int col = g_curCol;
        const int me  = LanLocalSlot();
        const LanSlotInfo* s = LanSlot(col);
        if (!CellEnabled(g_curRow, col)) {
            Sfx(1);                                  // the retail "refused" blip
        } else switch (kCell[g_curRow][col]) {
        case C_CHAR:
            if (SeatIsMine(col)) LanCycleCharFor(col, step); else LanHostCycleChar(col, step);
            Sfx(5);
            break;
        case C_SKIN:
            if (SeatIsMine(col)) LanCycleCostumeFor(col, step); else LanHostCycleCostume(col, step);
            Sfx(5);
            break;
        case C_TYPE:
            if (s && s->kind == 1) LanHostKick(col);      // a person is sitting there
            else                   LanHostToggleCpu(col); // OPEN <-> CPU
            Sfx(5);
            break;
        case C_TEAM:
            if (SeatIsMine(col)) LanCycleTeamFor(col, step); else LanHostCycleTeam(col, step);
            Sfx(5);
            break;
        case C_ARENA: {
            int a = LanConfigArena();
            for (int i = 0; i < 13; ++i) {
                a = (a + step + 13) % 13;
                if (LanArenaSelectable(a)) break;
            }
            LanHostSetArena((uint8_t)a); Sfx(5);
            break;
        }
        case C_MODE:
            // three-way now: QUICK MATCH -> TOURNAMENT -> MEAT RUSH
            LanHostSetMode((uint8_t)((LanConfigMode() + 1) % 3)); Sfx(5);
            break;
        case C_SET:
            Sfx(0x13);
            LanOpenFightSettings();
            return 7;                                // the REAL retail FIGHT SETTINGS screen
        case C_START:
            if (LanHostCanStart()) { Sfx(0x13); LanHostStart(); }
            else Sfx(1);
            break;
        default: break;
        }
        (void)s;
    }

    // EXTRA LOCAL PLAYERS. Local player j reads pad j and drives seat LanLocalSlotAt(j) and
    // nothing else -- no screen changes, no host controls, so none of the transitions above can
    // be triggered from here and the flow below stays player 1's.
    for (int j = 1; j < LanLocalCount(); ++j) {
        const int seat = LanLocalSlotAt(j);
        if (seat < 0) continue;
        int& row = g_cur2Row[j];
        if (row < 0 || row >= kSeatRows) row = 0;
        if (PressedPad(IN_UP, j))   { row = (row + kSeatRows - 1) % kSeatRows; Sfx(5); }
        if (PressedPad(IN_DOWN, j)) { row = (row + 1) % kSeatRows; Sfx(5); }

        const bool start2 = PressedPad(IN_START, j);
        int step2 = start2 ? 0
                  : (PressedPad(IN_NEXT, j) || PressedPad(IN_A, j)) ? +1
                  : PressedPad(IN_PREV, j) ? -1 : 0;
        if (step2) {
            if (!CellEnabled(row, seat)) {
                Sfx(1);
            } else switch (kCell[row][seat]) {
            case C_CHAR: LanCycleCharFor(seat, step2);    Sfx(5); break;
            case C_SKIN: LanCycleCostumeFor(seat, step2); Sfx(5); break;
            case C_TEAM: LanCycleTeamFor(seat, step2);    Sfx(5); break;
            // C_TYPE (OPEN/CPU) is the host's seat-filling control, not a player's own.
            default: Sfx(1); break;
            }
        }
        if (start2) {
            const LanSlotInfo* s2 = LanSlot(seat);
            LanSetReadyFor(seat, !(s2 && s2->ready));
            Sfx(0x13);
        }
    }

    // START is the commit, exactly as retail's own setup screen says on the tin: the host
    // begins the match, everyone else toggles their own ready flag.
    if (startPressed) {
        if (LanIsHost()) {
            if (LanHostCanStart()) { Sfx(0x13); LanHostStart(); } else Sfx(1);
        } else {
            const LanSlotInfo* s = LanSlot(LanLocalSlot());
            LanSetReady(!(s && s->ready));
            Sfx(0x13);
        }
    }
    RefreshLobbyText(self);
    return 15;
}

// ============================================================ MAIN MENU ROW
// Retail main-menu geometry (from FUN_00021140): CHALLENGE +0x20 y0.25, QUICK GAME +0xA4
// y0.40, TOURNAMENT +0x128 y0.55, OPTIONS +0x338 y0.70, plus the cut items +0x1AC (EXIT,
// taken by fe_menu), +0x230 and +0x2B4. Adding a sixth row means re-spacing all of them.
static const float kMenuY[6] = { 0.22f, 0.33f, 0.44f, 0.55f, 0.66f, 0.77f };
void LanMenuBuild(uint32_t self) {
    uint32_t chal = self + 0x20, quick = self + 0xA4, tour = self + 0x128,
             lan = self + 0x230, opts = self + 0x338, exit_ = self + 0x1AC;
    *(float*)(uintptr_t)(chal + 0x44) = kMenuY[0];
    *(float*)(uintptr_t)(quick + 0x44) = kMenuY[1];
    *(float*)(uintptr_t)(tour + 0x44) = kMenuY[2];
    *(float*)(uintptr_t)(lan + 0x44) = kMenuY[3];
    *(float*)(uintptr_t)(opts + 0x44) = kMenuY[4];
    *(float*)(uintptr_t)(exit_ + 0x44) = kMenuY[5];
    SetLabel(lan, 0, kTextBase + T_TITLE5);          // "LAN GAME"
    *(float*)(uintptr_t)(lan + 0x40) = 0.5f;
    *(uint8_t*)(uintptr_t)(lan + 0x48) = 1;
    *(uint8_t*)(uintptr_t)(lan + 0x49) = 1;
    *(uint8_t*)(uintptr_t)(lan + 0x4B) = 0;
    // Splice after TOURNAMENT so the d-pad order matches the on-screen order (AppendItem
    // would put it at the tail, behind the footer item).
    uint32_t after = *(uint32_t*)(uintptr_t)(tour + 0x60);
    *(uint32_t*)(uintptr_t)(lan + 0x60) = after;
    *(uint32_t*)(uintptr_t)(lan + 0x64) = tour;
    *(uint32_t*)(uintptr_t)(tour + 0x60) = lan;
    if (after) *(uint32_t*)(uintptr_t)(after + 0x64) = lan;
    else *(uint32_t*)(uintptr_t)(self + 0x18) = lan;
    LanMenuRefreshText();
}

// The LAN row's label is a SNAPSHOT of the language, and Build is NOT a per-entry hook: the
// frontend factory runs every screen's vtable+0x08 once, when the frontend object is built,
// while a screen-to-screen change runs only Update (+0x10) and Enter (+0x14). So toggling
// LANGUAGE on OPTIONS and walking back to the main menu left the previous language's bytes
// sitting here -- and the two halves the user reported are the same defect seen from each
// side. English -> Arabic simply stayed English. Arabic -> English made the row VANISH:
// the retail font's codepage map at font+0x800 is memset to 0xFF and filled only for the
// 124 retail characters, so every byte of the stale Arabic string is unmapped, and an
// unmapped byte draws nothing and advances the pen -- a present, still-selectable, entirely
// invisible row. Entering the browser repaired it because RefreshBrowserText writes the
// SAME slot. Every other main-menu row is a live per-draw table lookup, which is why this
// one row was alone in being wrong.
void LanMenuRefreshText() {
    Txt(T_TITLE5, "%s", L(TXT_TITLE, "LAN GAME"));
}

// The main menu is also a launch-capable screen: it is where the headless harness sits, and
// it is where a peer can be when GO arrives if it walked out of the lobby.
uint32_t LanMenuUpdate(uint32_t self, uint32_t stockResult) {
    (void)self;
    if (LanMatchLaunchIfPending()) return 0x16;
    return stockResult;
}

// ============================================================ headless harness
// TJ_LAN=host|join drives the session without the UI. While it is on, the LAN screens must
// ignore B: the scripted-input watchdog presses B on any screen it does not recognise (which
// includes ours), and that would silently leave the session mid-test.
static bool AutoDriving() {
    static int on = -1;
    if (on < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_LAN");
                  on = (e && *e) ? 1 : 0; free(e); }
    return on != 0;
}
// TJ_LAN=host|join drives the whole session without the UI so stages 3-5 can be verified by
// the scripted two-instance runner before the menus exist (and afterwards, as a regression).
static void AutoDrive(int frame) {
    static int mode = -1;                       // 0 off, 1 host, 2 join
    static char pw[32] = "", gname[32] = "";
    static int started = 0, wantPlayers = 2, arena = -1, rounds = 0, timeIdx = -1, myChar = -1;
    if (mode < 0) {
        char* e = nullptr; size_t n = 0;
        _dupenv_s(&e, &n, "TJ_LAN");
        mode = (e && *e) ? (_stricmp(e, "host") == 0 ? 1 : 2) : 0;
        free(e);
        e = nullptr; _dupenv_s(&e, &n, "TJ_LAN_PW"); if (e) strncpy_s(pw, e, _TRUNCATE); free(e);
        e = nullptr; _dupenv_s(&e, &n, "TJ_LAN_NAME"); if (e) strncpy_s(gname, e, _TRUNCATE); free(e);
        e = nullptr; _dupenv_s(&e, &n, "TJ_NET_PLAYERS"); wantPlayers = e && *e ? atoi(e) : 2; free(e);
        e = nullptr; _dupenv_s(&e, &n, "TJ_LAN_ARENA"); arena = e && *e ? atoi(e) : -1; free(e);
        e = nullptr; _dupenv_s(&e, &n, "TJ_LAN_ROUNDS"); rounds = e && *e ? atoi(e) : 0; free(e);
        e = nullptr; _dupenv_s(&e, &n, "TJ_LAN_TIME"); timeIdx = e && *e ? atoi(e) : -1; free(e);
        e = nullptr; _dupenv_s(&e, &n, "TJ_LAN_CHAR"); myChar = e && *e ? atoi(e) : -1; free(e);
        if (mode) printf("[lan] AUTO %s (players=%d arena=%d rounds=%d)\n",
                         mode == 1 ? "HOST" : "JOIN", wantPlayers, arena, rounds);
    }
    if (!mode) return;
    uint32_t fe = FeMgr();
    if (!fe) return;
    // The session opens as soon as the frontend exists (discovery can run during the boot
    // screens); the START handshake is separately gated on LanFrontendCanLaunch(), so both
    // peers still leave for the match from the same kind of screen on the same frame.
    if (LanGetState() == LAN_OFF) {
        if (gname[0]) LanSetName(gname);
        LanOpen(mode == 1, gname[0] ? gname : nullptr, pw);
        if (mode == 1) {
            static const uint32_t kTime[4] = { 0xE10u, 0x1518u, 0x1C20u, 0xFFFFFFFFu };
            // The host's own MEAT RUSH preference (blob 0x16A26B, set on the FIGHT SETTINGS
            // screen) seeds the lobby, exactly as TIME and ROUNDS do.
            LanHostSetRules(timeIdx >= 0 && timeIdx < 4 ? kTime[timeIdx] : 0xFFFFFFFFu,
                            (uint8_t)(rounds ? rounds : 1), 1,
                            *(uint8_t*)(uintptr_t)0x16A26B);
            if (arena >= 0) LanHostSetArena((uint8_t)arena);
        }
        return;
    }
    if (myChar >= 0) {
        const LanSlotInfo* s = LanSlot(LanLocalSlot());
        if (s && s->charId != (uint8_t)myChar) { LanCycleChar(+1); }
    }
    if (mode == 2) {
        if (LanGetState() == LAN_BROWSE && (frame & 31) == 0) {
            for (int i = 0; i < LanGameCount(); ++i) {
                const LanGameInfo* g = LanGameAt(i);
                if (g && g->compatible && !g->inMatch && g->players < g->maxPlayers) {
                    printf("[lan] AUTO joining '%s'\n", g->host);
                    LanJoinGame(i, pw);
                    break;
                }
            }
        }
        if (LanGetState() == LAN_LOBBY) {
            const LanSlotInfo* s = LanSlot(LanLocalSlot());
            if (s && !s->ready && (frame & 31) == 0) LanSetReady(true);
        }
        return;
    }
    if (!started && LanGetState() == LAN_LOBBY) {
        int humans = 0;
        for (int i = 0; i < 4; ++i) { const LanSlotInfo* s = LanSlot(i); if (s && s->kind == 1) ++humans; }
        if (humans >= wantPlayers && LanHostCanStart()) {
            started = 1;
            printf("[lan] AUTO start (%d players)\n", humans);
            LanHostStart();
        }
    }
    if (started && LanGetState() == LAN_LOBBY) started = 0;    // ready for the next match
}

void LanUiFrameTick(int frame) {
    LanPump();
    LanMatchFrameTick(frame);
    AutoDrive(frame);
}

// ============================================================ install
extern "C" void LanTextCapturePush(int ch) {
    LONG h = g_keyHead;
    g_keyRing[h & 63] = ch;
    g_keyHead = (h + 1) & 0x3FFFFFF;
}

int InstallLanUi() {
    int n = 0;
    n += PatchJump(0x2A300, HOOK_FC(Hk_BrowseBuild),  "lan:browse.build")  ? 1 : 0;
    n += PatchJump(0x2A4F0, HOOK_FC(Hk_BrowseUpdate), "lan:browse.update") ? 1 : 0;
    n += PatchJump(0x2A020, HOOK_FC(Hk_BrowseEnter),  "lan:browse.enter")  ? 1 : 0;
    n += PatchJump(0x294A0, HOOK_FC(Hk_LobbyBuild),   "lan:lobby.build")   ? 1 : 0;
    n += PatchJump(0x29690, HOOK_FC(Hk_LobbyUpdate),  "lan:lobby.update")  ? 1 : 0;
    n += PatchJump(0x291A0, HOOK_FC(Hk_LobbyEnter),   "lan:lobby.enter")   ? 1 : 0;
    for (int i = 0; i < kTextN; ++i) g_txt[i][0] = 0;
    Txt(T_TITLE5, "%s", L(TXT_TITLE, "LAN GAME"));
    printf("[lan] UI installed (%d/6 screen hooks)\n", n);
    LanContractDump();          // TJ_LAN_HASHDUMP=1 only: log the contract at startup
    return n;
}

}  // namespace tj::hybrid
