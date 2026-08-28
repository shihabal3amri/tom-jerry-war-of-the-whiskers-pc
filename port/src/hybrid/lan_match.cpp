// Phase-2 LAN, stages 4-5 -- everything between "the lobby said GO" and "the retail match
// is running identically on every PC".
//
// LAUNCH PARITY (stage 4). The LAN lobby does NOT hand-write the match setup. It writes
// retail screen 11's OWN fields (the QUICK GAME 4-player setup screen) and then calls the
// screen's own commit `FUN_00025EB0`, so the game performs its own slot -> team repack.
// LAN slots therefore end up byte-identical in semantics to a couch Quick Game, which is
// the whole point: every downstream consumer of FE+0x4A4.. is retail code we did not have
// to re-derive. Decoded from the disassembly of 0x25EB0:
//   screen11 + 0x2DD8 + i*0x84 = player type (0 HUMAN, 1 CPU, 2 OFF) for layouts 0/1
//   screen11 + 0x31E8 + i*0x84 = team
//   screen11 + 0xA0            = layout (0 = free-for-all; the commit then assigns teams)
//   FE + 0x4A8/+0x4AC/+0x4AF + i*0x17 = costume / spare / character, slot-indexed
// and the commit repacks those into the TEAM-MAJOR table the simulation actually reads
// (FE+0x4A4 + team*0x17: +0 aiFlag, +3 count, +4 costume, +0xB char, +0xE pad index).
// The lobby's Update then returns the retail LAUNCH SENTINEL 0x16 and the game runs its own
// mode 3->4->5->6->7 path: loading screen, arena intro, "READY!" -- all retail.
//
// ROUNDS (stage 5). Retail lies to the player: best-of-N is gated on `FE+0x502 == 0`
// (CHALLENGE) at 0x17280/0x17286, so QUICK GAME and TOURNAMENT are always one round no
// matter what FIGHT SETTINGS says. The gate is NOT simply removed -- the tournament win
// counter at 0x17261 increments per ROUND, so opening the gate would award a tournament win
// per round. Instead the unconditional match-over store at 0x172EC is replaced with the
// real rule plus a correction that undoes the per-round tournament tick.
#include "lan_match.h"
#include "net_lan.h"
#include "hybrid/meat_rush.h"
#include "hybrid/guest_call.h"
#include "net_sync.h"
#include "xdk_patch.h"

#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tj::hybrid {

// --- game addresses (all confirmed by disassembly; see port/LAN_PLAN.md section 0) ------
static const uint32_t kMasterPtr   = 0x15C470C;
static const uint32_t kQuickCommit = 0x25EB0;    // screen 11's own commit
static const uint32_t kRoundEnd    = 0x172EC;    // `mov byte [ecx+0x27D],1`, 7 B -> 0x172F3
static const uint32_t kRoundEndRet = 0x172F3;
static const uint32_t kFightUpdate = 0x27E50;    // FIGHT SETTINGS Update (5-byte prologue)
static const uint32_t kNameFn      = 0x40900;    // player label  (`mov al,[ecx+0x7021]`)
static const uint32_t kTeamNameFn  = 0x40950;    // team label    (`mov al,[ecx+0x7022]`)
static const uint32_t kGetText     = 0x19910;
static const uint32_t kSettings    = 0x16A1F8;   // settings blob, 0x74 bytes
static const uint32_t kSetTime     = 0x16A264;
static const uint32_t kSetRounds   = 0x16A268;
static const uint32_t kSetDiff     = 0x16A269;
static const uint32_t kSetMeat     = 0x16A26B;   // MEAT RUSH (meat_ui.cpp owns the row)
static const uint32_t kArenaUnlock = 0x16A245;   // 13 flags
static const uint32_t kCharUnlock  = 0x16A23A;   // 11 flags

using FnThis   = void     (__fastcall*)(uint32_t self, uint32_t);
using FnUpdate = uint32_t (__fastcall*)(uint32_t self, uint32_t);
using FnName   = char*    (__fastcall*)(uint32_t self, uint32_t);

uint32_t LanMaster() { return *(volatile uint32_t*)(uintptr_t)kMasterPtr; }
uint32_t LanFeMgr()  { uint32_t m = LanMaster(); return m ? *(uint32_t*)(uintptr_t)(m + 0x4D4) : 0; }
uint32_t LanScreen(int id) {
    uint32_t fe = LanFeMgr();
    if (!fe || id < 0 || id > 21) return 0;
    return *(uint32_t*)(uintptr_t)(fe + 4 + (uint32_t)id * 4);
}
typedef char* (__cdecl* FnGetTextC)(uint32_t idx);
static char* Text(uint16_t idx) { return GCALL(Cdecl, FnGetTextC, kGetText, idx); }
// ARENA ID -> NAME STRING IS NOT 0x8F + id. The name table at string 0x8F..0x9B is in a
// DIFFERENT ORDER from the arena ids, so labelling by `0x8F + id` named most arenas wrongly
// and the player picked one thing and got another ("HELL" was id 9, which actually loads
// WILDWEST -- user-reported). Ground truth is the level-FILE table the loader itself uses,
// `0xF6920 + id*0xE` / `0xF69D8 + id*0xE`, dumped per id:
//   0 KITCHEN  1 HAUNTED  2 SCRAP   3 SHIP     4 CABIN   5 BANQUET  6 BEACH
//   7 SKY      8 LAB      9 WILDWEST 10 BOXING 11 MARKET 12 HELL(LAVA)
static const uint16_t kArenaNameStr[13] = {
    0x8F,   // 0  KITCHEN.PS2   THE KITCHEN
    0x94,   // 1  HAUNTED.PS2   THE HAUNTED HOUSE
    0x92,   // 2  SCRAP.PS2     THE SCRAPYARD
    0x91,   // 3  SHIP.PS2      THE SHIP
    0x93,   // 4  CABIN.PS2     THE CABIN
    0x95,   // 5  BANQUET.PS2   THE BANQUET HALL
    0x90,   // 6  BEACH.PS2     THE BEACH
    0x97,   // 7  SKY.PS2       THE SKYSCRAPER
    0x9A,   // 8  LAB.PS2       THE LAB
    0x96,   // 9  WILDWEST.PS2  THE WILD WEST
    0x99,   // 10 BOXING.PS2    THE BOXING RING
    0x9B,   // 11 MARKET.PS2    THE MARKETPLACE
    0x98,   // 12 HELL.PS2      HELL
};
const char* LanArenaName(int arena) {
    return (arena >= 0 && arena < 13) ? Text(kArenaNameStr[arena]) : "?";
}
const char* LanCharName(int charId) {
    return (charId >= 0 && charId < 11) ? Text((uint16_t)(0x07 + charId)) : "?";
}
// TEAM A..D. The lobby used to draw the literal "TEAM %c", which stayed English in Arabic
// AND could not be translated cheaply: the letter is A-D, and the Arabic font has no Latin
// capitals -- it keeps only the retail characters Arabic strings actually reuse. Retail
// already ships the whole phrase per letter for its own setup screen (0xAE..0xB1, "TEAM A"
// / "الفريق أ"), so going through the string table is both correct in every language and
// free: no new glyph, no new string, and the offline and LAN screens now read identically.
const char* LanTeamName(int team) {
    return Text((uint16_t)(0xAE + (team & 3)));
}

// --- settings-blob custody ---------------------------------------------------
// A LAN session must never silently rewrite the player's single-player preferences or --
// far worse -- their unlocks: the blob validator FUN_0002DD00 responds to an out-of-range
// byte by memcpy'ing all 0x74 bytes back to defaults. So the whole blob is snapshotted on
// entering the LAN flow and restored on leaving, and every network-sourced write is clamped
// here rather than handed to the validator.
static uint8_t g_blobSave[0x74];
static bool    g_blobHeld = false;

void LanBlobSnapshot() {
    if (g_blobHeld) return;
    memcpy(g_blobSave, (const void*)(uintptr_t)kSettings, sizeof g_blobSave);
    g_blobHeld = true;
}
void LanBlobRestore() {
    if (!g_blobHeld) return;
    memcpy((void*)(uintptr_t)kSettings, g_blobSave, sizeof g_blobSave);
    g_blobHeld = false;
    printf("[lan] settings blob restored\n");
}

// --- costumes -----------------------------------------------------------------
// Two different questions, two different answers.
//
// WHAT A PLAYER MAY PICK is what their own save has EARNED. The blob's first 66 bytes are 11
// six-byte records -- ids [0..4], count [5] -- and a fresh save has exactly one costume per
// character. Read from the SNAPSHOT, so it stays the player's real save even while the live
// table is forced below.
//
// WHAT THIS PC CAN DRAW is every costume that exists, because the other player may have
// unlocked one this PC has not and their choice still has to render. The static ceiling at
// 0x169915 + ch*6 is identical on every machine (TOM 5, JERRY 5, SPIKE/BUTCH/NIBBLES/DUCKLING
// 3, the rest 1) and the ids are contiguous 0..n-1, so filling the live table from it makes
// every id resolve without remapping anyone's choice -- the same deal as the arena unlock.
// NEVER write a count above 5 or an id above 4: character 10's record ends at 0x16A239 and the
// very next byte IS validated, so a spill makes FUN_0002DD00 reset the whole blob.
int LanCostumeUnlocked(int charId) {
    if (charId < 0 || charId >= 11) return 1;
    int n = g_blobHeld ? g_blobSave[charId * 6 + 5]
                       : *(volatile uint8_t*)(uintptr_t)(kSettings + charId * 6 + 5);
    return (n >= 1 && n <= 5) ? n : 1;
}
void LanCostumeFillTable() {
    for (int ch = 0; ch < 11; ++ch) {
        uint8_t mx = *(volatile uint8_t*)(uintptr_t)(0x169915 + (uint32_t)ch * 6);
        if (mx < 1 || mx > 5) mx = 1;
        uint8_t* rec = (uint8_t*)(uintptr_t)(kSettings + (uint32_t)ch * 6);
        for (uint8_t k = 0; k < mx; ++k) rec[k] = k;
        rec[5] = mx;
    }
}

static void ApplyRules(const LanMatchConfig& c) {
    uint32_t t = c.timeLimit;
    if (t != 0xFFFFFFFFu && t != 0xE10u && t != 0x1518u && t != 0x1C20u) t = 0x1518u;  // 90 s
    uint8_t r = (c.rounds == 1 || c.rounds == 3 || c.rounds == 5) ? c.rounds : 1;
    uint8_t d = c.difficulty <= 2 ? c.difficulty : 1;
    *(uint32_t*)(uintptr_t)kSetTime = t;
    *(uint8_t*)(uintptr_t)kSetRounds = r;
    *(uint8_t*)(uintptr_t)kSetDiff = d;
    // MEAT RUSH is SIM-AFFECTING -- it steers the item scheduler -- so it must be applied on
    // every peer here, before the match arms, and never from a local toggle. The byte is the
    // one the FIGHT SETTINGS row owns (blob 0x16A26B), and it is inside LanBlobSnapshot's
    // custody, so the client's own saved preference is restored when the LAN flow ends.
    uint8_t meat = c.meat;
    if (meat != 5 && meat != 10 && meat != 15 && meat != 20 && meat != 0xFF) meat = 5;
    *(uint8_t*)(uintptr_t)kSetMeat = meat;
    MeatRushSetMode(c.mode == 2);                     // the LOBBY'S MODE row decides the mode
    MeatRushSetTarget(meat == 0xFF ? 0 : meat);       // FIGHT SETTINGS only sets the target
    printf("[lan] rules: time=%u rounds=%u diff=%u meat=%u\n", t, r, d, meat);
    // VIBRATION (0x16A255) and PLAYER MARKERS (0x16A257) are deliberately NOT synced: they
    // only gate the local rumble call and the local HUD draw, and the host's rumble
    // preference must not disable the client's.
}

// --- player names in the banners ---------------------------------------------
// FUN_00040900 is thiscall(fighter) -> char* and maps fighter+0x7021 (the player index) to
// the retail strings "PLAYER 1".."PLAYER 4". The banner ring FUN_00059880 stores the
// returned pointer and draws it FRAMES LATER, so the buffer must be static, never a local.
static char (&g_bannerName)[4][20] = *(char(*)[4][20])GuestObjAlloc(80, 8);
static char (&g_bannerTeam)[4][20] = *(char(*)[4][20])GuestObjAlloc(80, 8);
static bool g_namesLive = false;

static char* __fastcall Hk_PlayerName(uint32_t self, uint32_t) {
    uint8_t idx = *(uint8_t*)(uintptr_t)(self + 0x7021);
    if (g_namesLive && idx < 4 && g_bannerName[idx][0]) {
        static uint32_t shouted = 0;      // proof the banner path actually asks for our name
        if (shouted < 8) {
            ++shouted;
            printf("[lan] banner asked for player %u at frame %u -> '%s'\n",
                   idx, NetSyncFrame(), g_bannerName[idx]);
        }
        return g_bannerName[idx];
    }
    switch (idx) {
    case 0: return Text(0x12); case 1: return Text(0x13);
    case 2: return Text(0xA1); case 3: return Text(0xA2);
    }
    return Text(0x12);
}
static char* __fastcall Hk_TeamName(uint32_t self, uint32_t) {
    uint8_t idx = *(uint8_t*)(uintptr_t)(self + 0x7022);
    if (g_namesLive && idx < 4 && g_bannerTeam[idx][0]) return g_bannerTeam[idx];
    switch (idx) {
    case 0: return Text(0xBA); case 1: return Text(0xBB);
    case 2: return Text(0xBC); case 3: return Text(0xBD);
    }
    return Text(0xBA);
}
static void SetBannerNames(const LanMatchConfig& c) {
    memset(g_bannerName, 0, sizeof g_bannerName);
    memset(g_bannerTeam, 0, sizeof g_bannerTeam);
    for (int i = 0; i < 4; ++i) {
        if (c.slotKind[i] == 1 && c.slotName[i][0])
            _snprintf_s(g_bannerName[i], sizeof(g_bannerName[i]), _TRUNCATE, "%s", c.slotName[i]);
        else if (c.slotKind[i] == 2)
            _snprintf_s(g_bannerName[i], sizeof(g_bannerName[i]), _TRUNCATE, "CPU %d", i + 1);
        if (g_bannerName[i][0])
            _snprintf_s(g_bannerTeam[i], sizeof(g_bannerTeam[i]), _TRUNCATE, "%s", g_bannerName[i]);
    }
    g_namesLive = true;
}

// --- real ROUNDS (stage 5) ----------------------------------------------------
static uint8_t  g_lanRounds = 1;          // authoritative copy, only ever set at launch
static bool     g_lanRoundsLive = false;
static volatile uint32_t g_roundEndRet = kRoundEndRet;

// eax = winner index, ecx = master, at 0x172EC. The round-win counters master[0x284+w] and
// the rounds-played counter master[0x288] have already been bumped by retail code above.
static void __cdecl RoundEndC(int winner) {
    uint32_t m = LanMaster();
    if (!m) return;
    uint8_t* master = (uint8_t*)(uintptr_t)m;
    uint32_t fe = LanFeMgr();
    if (!g_lanRoundsLive || g_lanRounds <= 1 || winner < 0 || winner > 3) {
        master[0x27D] = 1;                       // retail behaviour: one round, match over
        return;
    }
    uint8_t R = g_lanRounds;
    if (master[0x288] >= R || master[0x284 + winner] >= (uint8_t)((R >> 1) + 1)) {
        master[0x27D] = 1;                       // series decided
    } else if (fe && *(uint8_t*)(uintptr_t)(fe + 0x508)) {
        // Not over: undo the per-ROUND tournament tick at 0x17261 so a tournament win is
        // awarded once per MATCH. Leaving master[0x27D] at 0 makes the retail fade handler
        // fall into its own next-round-same-arena path ([0x184660] = 6), which re-enters our
        // reseed barrier -- zero new match-flow code.
        if (master[0x280 + winner]) --master[0x280 + winner];
    }
}
#if defined(_M_IX86)
__declspec(naked) static void Hk_RoundEnd() {
    __asm {
        pushad
        pushfd
        push eax
        call RoundEndC
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_roundEndRet]
    }
}
#endif
// Engine mode must NOT reach the naked wrapper above through the host-call escape: this
// is a mid-function jmp-in/jmp-out hook, so [esp] is the round-over function's live data
// (a heap pointer), not a return address — the escape's displacement handed the guest a
// thunk address as its pointer and it crashed dereferencing it (session 24, frame
// 10,645). The registered semantic below is what the wrapper does, minus the register
// theater the interpreter's CpuState makes unnecessary.
static void __cdecl RoundEndEngine(tj::engine::CpuState& s) {
    RoundEndC((int)s.r[tj::engine::EAX]);
}

// --- FIGHT SETTINGS over LAN (stage 5) ----------------------------------------
// Deliberately not a clone: the player already knows this screen pixel-for-pixel because it
// IS that screen. Both exits (CONFIRM commits at 0x2804E, B discards at 0x2802E) return 6
// = OPTIONS; while the lobby owns the screen we reroute that to 15 = the LAN lobby and
// capture what the player chose.
static bool g_settingsOpen = false;
static uint32_t Orig_FightUpdate = 0;   // guest-window trampoline VA

void LanOpenFightSettings() { g_settingsOpen = true; }
bool LanFightSettingsOpen() { return g_settingsOpen; }

static uint32_t __fastcall Hk_FightUpdate(uint32_t self, uint32_t) {
    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_FightUpdate, self, 0);
    if (!g_settingsOpen || r != 6) return r;
    g_settingsOpen = false;
    if (LanIsHost()) {
        uint32_t t = *(uint32_t*)(uintptr_t)kSetTime;
        uint8_t  rd = *(uint8_t*)(uintptr_t)kSetRounds;
        uint8_t  df = *(uint8_t*)(uintptr_t)kSetDiff;
        uint8_t  mt = *(uint8_t*)(uintptr_t)kSetMeat;
        LanHostSetRules(t, rd, df, mt);
        printf("[lan] fight settings: time=%u rounds=%u diff=%u meat=%u (broadcast)\n",
               t, rd, df, mt);
    }
    return 15;                                   // back to the LAN lobby
}

// --- ARENA PICTURE: borrow the retail LEVEL SELECT -----------------------------
// The lobby's ARENA row was a word, and the user asked for the picture. The game already has
// the right screen and it is not a clone either: id 16 is the versus carousel with the named
// preview cards ("A FRIDGE TOO FAR", the Wild West saloon, ...). Its Update returns the
// LAUNCH sentinel 0x16 on A and `FE+0x50C` on B, so the back route is data (set +0x50C to the
// lobby) and only the confirm return has to be rewritten -- the FIGHT SETTINGS deal exactly.
//
// A IS SIDE-EFFECT FREE FROM THE LAN FLOW. Its two commits -- FUN_00022350 and the FE+0x4C6
// write via FUN_0002DA10 -- are both gated on FE+0x502, and the LAN flow holds that at 4
// (0x22350 returns immediately on 4). So confirming only plays the SFX and returns.
//
// The screen navigates on behalf of a PLAYER, so it needs one: its outer loop runs
// FE+0x500 times and does nothing at all unless FE+0x4A7 (that player's pad count) is
// non-zero, which is state the QUICK GAME path would have left behind and the lobby has not.
static const uint32_t kLevelUpdate = 0x22420;
static bool     g_arenaPickOpen = false;
static uint32_t Orig_LevelUpdate = 0;

void LanOpenArenaSelect() {
    uint32_t fe = LanFeMgr();
    if (!fe) return;
    LanBlobSnapshot();
    // The carousel's ring is the UNLOCKED arenas, but the LAN lobby offers the same eleven to
    // every host whatever their own save says -- and a host who silently got a shorter list
    // than the one the lobby advertises is a bug report. Unlock all thirteen for the
    // duration; ids 10 and 12 still drop out, by the screen's OWN marker rule, which is
    // exactly where LanArenaSelectable's list comes from. The settings blob is under custody
    // (snapshot on entering the LAN flow, restore on leaving) so this never reaches the save.
    for (int i = 0; i < 13; ++i) *(uint8_t*)(uintptr_t)(kArenaUnlock + i) = 1;
    *(uint8_t*)(uintptr_t)(fe + 0x500) = 1;        // one navigator...
    *(uint8_t*)(uintptr_t)(fe + 0x4A7) = 1;        // ...holding one pad...
    *(uint8_t*)(uintptr_t)(fe + 0x4A4) = 0;        // ...which is not marked done...
    *(uint8_t*)(uintptr_t)(fe + 0x4B2) = 0;        // ...and is pad 0.
    *(uint8_t*)(uintptr_t)(fe + 0x502) = 4;        // QUICK GAME family (also gates the commits)
    *(uint32_t*)(uintptr_t)(fe + 0x50C) = 15;      // B returns to the LAN lobby
    *(uint8_t*)(uintptr_t)(fe + 0x501) = LanConfigArena();
    uint32_t s16 = LanScreen(16);
    if (s16) {
        *(uint8_t*)(uintptr_t)(s16 + 0x245) = 0;   // not mid-transition
        *(uint32_t*)(uintptr_t)(s16 + 0x3B4) = 0;  // not the tournament winner-picks variant
    }
    g_arenaPickOpen = true;
}
bool LanArenaSelectOpen() { return g_arenaPickOpen; }

static uint32_t __fastcall Hk_LevelUpdate(uint32_t self, uint32_t) {
    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_LevelUpdate, self, 0);
    if (!g_arenaPickOpen || r == 0x10) return r;   // 0x10 = still on the carousel
    g_arenaPickOpen = false;
    uint32_t fe = LanFeMgr();
    uint8_t picked = fe ? *(uint8_t*)(uintptr_t)(fe + 0x501) : 0;
    if (r == 0x16 && LanIsHost() && LanArenaSelectable(picked)) {
        LanHostSetArena(picked);
        printf("[lan] arena picked from the carousel: %u (%s)\n", picked, LanArenaName(picked));
    }
    return 15;                                     // both exits land back in the lobby
}

// --- launch (stage 4) ---------------------------------------------------------
static bool     g_launched = false;
static int      g_assertIn = 0;
static uint8_t  g_slotPort[4];

static uint8_t KindToType(uint8_t kind) {
    // screen 11 player type, layout 0/1 encoding: 0 = HUMAN, 1 = CPU, 2 = OFF.
    switch (kind) { case 1: return 0; case 2: return 1; default: return 2; }
}

bool LanMatchLaunchIfPending() {
    if (!LanConsumeLaunch()) return false;
    const LanMatchConfig* pc = LanConfig();
    uint32_t fe = LanFeMgr();
    uint32_t s11 = LanScreen(11);
    if (!pc || !fe || !s11) {
        printf("[lan] LAUNCH ABORTED: cfg=%p fe=%08X screen11=%08X\n", (void*)pc, fe, s11);
        return false;
    }
    const LanMatchConfig& c = *pc;
    LanBlobSnapshot();
    ApplyRules(c);
    // ZERO THE MATCH/IDLE TICK. DAT_015C4710 is bumped by FUN_0002E020 on every update,
    // before the mode jumptable, and is only reset by certain frontend transitions -- so at
    // the moment two peers arm it holds however many frames each of them happened to spend
    // in its own menus (measured: 263 vs 388). It is compared against the match time limit,
    // so a difference means the two peers would end the round on different frames. This runs
    // on lockstep frame 0 on every peer, so the value is identical from here on.
    *(volatile uint32_t*)(uintptr_t)0x15C4710 = 0;

    for (int i = 0; i < 4; ++i) {
        uint8_t ch = c.slotChar[i] < 11 ? c.slotChar[i] : 0;
        uint8_t co = c.slotCostume[i] < LanCostumeCount(ch) ? c.slotCostume[i] : 0;
        // +0x4A8 IS NOT THE COSTUME. It is the 0..5 POWER/HANDICAP meter -- the one retail's
        // setup screen draws as a row of stars -- and FUN_00040B10 applies it to all seven of
        // a fighter's stat bytes as `stat += (100 - stat) * meter / 6`. The costume is
        // +0x4AC (FUN_0001B290 writes `FE + (slot+0x34)*0x17`, and 0x34*0x17 = 0x4AC). This
        // code had them the other way round: a costume choice would have become a stat
        // handicap and the real costume was forced to 0. Harmless only while nothing could
        // pick a costume; the lobby can now, so it had to be right.
        *(uint8_t*)(uintptr_t)(fe + 0x4A8 + i * 0x17) = 0;    // no handicap over LAN
        *(uint8_t*)(uintptr_t)(fe + 0x4AC + i * 0x17) = co;   // costume id
        *(uint8_t*)(uintptr_t)(fe + 0x4AF + i * 0x17) = ch;
        *(uint8_t*)(uintptr_t)(s11 + 0x2DD8 + i * 0x84) = KindToType(c.slotKind[i]);
        *(uint8_t*)(uintptr_t)(s11 + 0x31E8 + i * 0x84) = c.slotTeam[i] < 4 ? c.slotTeam[i] : (uint8_t)i;
        g_slotPort[i] = (uint8_t)i;
    }
    *(uint8_t*)(uintptr_t)(s11 + 0xA0) = c.teamMode < 3 ? c.teamMode : 0;

    GCALL(Fastcall, FnThis, kQuickCommit, s11, 0);  // the game repacks our slots itself

    // UNLOCK WHAT THE LOBBY CHOSE, do not remap it. Each PC has its own save, so the arena
    // the host picked may be locked on the client -- and a peer that quietly substituted a
    // different arena would load a different level and desync instantly. The 0x74-byte blob
    // is snapshotted on entering the LAN flow and restored on leaving, so this cannot leak
    // into the player's single-player unlocks.
    uint8_t arena = c.arena < 13 ? c.arena : 0;
    *(uint8_t*)(uintptr_t)(kArenaUnlock + arena) = 1;
    for (int i = 0; i < 4; ++i)
        if (c.slotKind[i] && c.slotChar[i] < 11) *(uint8_t*)(uintptr_t)(kCharUnlock + c.slotChar[i]) = 1;
    LanCostumeFillTable();       // so a costume the OTHER player unlocked resolves here too
    *(uint8_t*)(uintptr_t)(fe + 0x501) = arena;
    *(uint8_t*)(uintptr_t)(fe + 0x502) = 4;                            // QUICK GAME family
    *(uint8_t*)(uintptr_t)(fe + 0x508) = c.mode == 1 ? 1 : 0;          // tournament flag
                                                                       // (mode 2 = MEAT RUSH)
    uint32_t m = LanMaster();
    if (m && c.mode == 1) {
        *(uint8_t*)(uintptr_t)(m + 0x28A) = c.tournFirstTo ? c.tournFirstTo : 3;
        *(uint8_t*)(uintptr_t)(m + 0x1C90D) = 1;                       // clear the win table
    }

    g_lanRounds = (c.rounds == 1 || c.rounds == 3 || c.rounds == 5) ? c.rounds : 1;
    g_lanRoundsLive = true;
    SetBannerNames(c);
    g_launched = true;
    g_assertIn = 40;                             // check the slot map once the match is live
    printf("[lan] LAUNCH match %08X arena=%u(%s) rounds=%u kinds=%u%u%u%u chars=%u%u%u%u\n",
           c.matchId, arena, LanArenaName(arena), g_lanRounds,
           c.slotKind[0], c.slotKind[1], c.slotKind[2], c.slotKind[3],
           c.slotChar[0], c.slotChar[1], c.slotChar[2], c.slotChar[3]);
    return true;
}

// The two bytes the simulation actually reads for a fighter's control source. Logged, not
// asserted-fatal: a mismatch means the repack disagreed with the lobby and every input
// would go to the wrong fighter, so it must be visible in the log of any bad run.
static void AssertSlotMap() {
    uint32_t m = LanMaster();
    if (!m) return;
    const LanMatchConfig* c = LanConfig();
    if (!c) return;
    int bad = 0;
    for (int i = 0; i < 4; ++i) {
        if (c->slotKind[i] != 1) continue;
        uint32_t f = m + 0x4E0 + (uint32_t)i * 0x7080;
        uint8_t ai = *(uint8_t*)(uintptr_t)(f + 0x7074);
        uint8_t port = *(uint8_t*)(uintptr_t)(f + 0x7020);
        uint8_t pidx = *(uint8_t*)(uintptr_t)(f + 0x7021);
        printf("[lan] slot %d: +0x7074(ai)=%u +0x7020(port)=%u +0x7021(player)=%u\n",
               i, ai, port, pidx);
        if (ai != 0 || port != g_slotPort[i]) ++bad;
    }
    if (bad) printf("[lan] WARN: %d LAN slot(s) did NOT map to their own pad\n", bad);
}

// --- per-frame ----------------------------------------------------------------
void LanMatchFrameTick(int frame) {
    (void)frame;
    if (g_assertIn > 0 && --g_assertIn == 0) AssertSlotMap();
    // The carousel is also the ATTRACT screen: its idle tick would launch a demo match out
    // from under a host who is taking their time choosing an arena. The LAN screens hold that
    // counter at zero for the same reason; do it here too, since screen 16's Update is the
    // game's own and does not know about us.
    if (g_arenaPickOpen && !NetArmed()) *(volatile uint32_t*)(uintptr_t)0x15C4710 = 0;

    // Match over -> hand control back to the lobby. Lockstep stays armed through the whole
    // retail post-match loop (results 17, rematch, tournament standings) so any player can
    // drive it from their own pad; it is dropped only once the frontend has been rebuilt,
    // which is the moment the retail flow would otherwise land on the main menu.
    // NOT "the previous mode was >= 4": the retail post-match route is 7 -> 2 (frontend
    // REBUILD) -> 3, so by the time mode reads 3 the previous value is 2. Latch instead.
    static bool inMatch = false;
    uint8_t mode = *(volatile uint8_t*)(uintptr_t)0x184661;
    if (!g_launched || LanGetState() != LAN_MATCH) { inMatch = false; return; }
    if (mode >= 4) inMatch = true;
    if (inMatch && mode == 3) {
        uint32_t fe = LanFeMgr();
        uint8_t scr = fe ? *(uint8_t*)(uintptr_t)fe : 0xFF;
        if (scr != 17 && scr != 13 && scr != 16) {         // results / standings / carousel
            inMatch = false;
            g_launched = false;
            g_lanRoundsLive = false;
            g_namesLive = false;
            LanMatchEnded();
            if (fe) {                                       // hop straight back to the lobby
                *(uint32_t*)(uintptr_t)(fe + 0x60) = 15;
                *(uint8_t*)(uintptr_t)(fe + 0x5C) = 1;
                *(uint8_t*)(uintptr_t)(fe + 0x5D) = 1;
                printf("[lan] match over -> LAN lobby\n");
            }
        }
    }
}

// --- install ------------------------------------------------------------------
// (call-through trampolines now come from xdk_patch's guest-window pad — MakeGuestTramp)

// Every site verifies its retail bytes before it is patched, so future XBE or build drift
// fails visibly instead of silently corrupting the simulation.
static bool Expect(uint32_t va, const uint8_t* want, int n, const char* label) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)va;
    for (int i = 0; i < n; ++i)
        if (p[i] != want[i]) {
            printf("[lan] WARN patch %s @%08X: byte %d is %02X, expected %02X -- SKIPPED\n",
                   label, va, i, p[i], want[i]);
            return false;
        }
    return true;
}

int InstallLanMatch() {
    int n = 0;
    static const uint8_t kRoundEndBytes[] = { 0xC6, 0x81, 0x7D, 0x02, 0x00, 0x00, 0x01 };
    if (Expect(kRoundEnd, kRoundEndBytes, 7, "rounds")) {
        // Naked jmp-in/jmp-out continuation — engine mode services it via the
        // registered jmp-hook, never the dispatch table or the call-shaped escape.
        if (PatchJmpHook(kRoundEnd, /*callShaped=*/false, NAKED_HOOK(Hk_RoundEnd),
                         kRoundEndRet, &RoundEndEngine, /*escapeSafe=*/false,
                         "lan:rounds")) {
            DWORD old;
            VirtualProtect((void*)(uintptr_t)kRoundEnd, 8, PAGE_EXECUTE_READWRITE, &old);
            ((uint8_t*)(uintptr_t)kRoundEnd)[5] = 0x90;      // pad the 7-byte site
            ((uint8_t*)(uintptr_t)kRoundEnd)[6] = 0x90;
            VirtualProtect((void*)(uintptr_t)kRoundEnd, 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)kRoundEnd, 8);
            ++n;
        }
    }
    static const uint8_t kFightBytes[] = { 0xA1, 0x0C, 0x47, 0x5C, 0x01 };
    if (Expect(kFightUpdate, kFightBytes, 5, "fightsettings")) {
        Orig_FightUpdate = MakeGuestTramp(kFightUpdate, 5, "lan:tr.fight");
        if (Orig_FightUpdate && PatchJump(kFightUpdate, HOOK_FC(Hk_FightUpdate), "lan:fightsettings")) ++n;
    }
    static const uint8_t kLevelBytes[] = { 0x83, 0xEC, 0x10, 0xA1, 0x0C, 0x47, 0x5C, 0x01 };
    if (Expect(kLevelUpdate, kLevelBytes, 8, "levelselect")) {
        Orig_LevelUpdate = MakeGuestTramp(kLevelUpdate, 8, "lan:tr.level");
        if (Orig_LevelUpdate && PatchJump(kLevelUpdate, HOOK_FC(Hk_LevelUpdate), "lan:levelselect")) ++n;
    }
    static const uint8_t kNameBytes[] = { 0x8A, 0x81, 0x21, 0x70, 0x00, 0x00 };
    if (Expect(kNameFn, kNameBytes, 6, "playername") &&
        PatchJump(kNameFn, HOOK_FC(Hk_PlayerName), "lan:playername")) ++n;
    static const uint8_t kTeamBytes[] = { 0x8A, 0x81, 0x22, 0x70, 0x00, 0x00 };
    if (Expect(kTeamNameFn, kTeamBytes, 6, "teamname") &&
        PatchJump(kTeamNameFn, HOOK_FC(Hk_TeamName), "lan:teamname")) ++n;
    printf("[lan] match hooks installed (%d/5)\n", n);
    return n;
}

}  // namespace tj::hybrid
