// Phase-2 LAN, stages 4-5: match-launch parity with QUICK GAME, the shared FIGHT SETTINGS,
// real ROUNDS, and player names in the banners.
#pragma once
#include <cstdint>

namespace tj::hybrid {

int  InstallLanMatch();

// Called from every LAN-hosted screen's Update. Returns true when the agreed launch must
// happen NOW: the caller then returns the retail launch sentinel 0x16 so the game runs its
// own mode 3->4->5->6->7 path (loading screen, arena intro, "READY!" -- all retail).
bool LanMatchLaunchIfPending();

// Once per presented frame, from the frontend tick: post-launch assertions and the
// match-over -> back-to-lobby handover.
void LanMatchFrameTick(int frame);

// The lobby opens the REAL retail FIGHT SETTINGS screen (id 7); this makes its exit route
// back to the lobby instead of OPTIONS, and captures TIME/ROUNDS/DIFFICULTY on the way out.
void LanOpenFightSettings();
bool LanFightSettingsOpen();

// ARENA PICTURE: the lobby's ARENA row opens the REAL retail level-select carousel (id 16)
// with its preview cards, and what the host confirms there becomes the lobby's arena.
void LanOpenArenaSelect();
bool LanArenaSelectOpen();

// The 0x74-byte settings blob is snapshotted on entering the LAN flow and restored on
// leaving, so a LAN session can never rewrite the player's single-player preferences or
// unlocks (the retail validator responds to an out-of-range byte by resetting all of them).
void LanBlobSnapshot();
void LanBlobRestore();

// How many costumes THIS PC's save has actually unlocked for a character (from the snapshot,
// so it is the player's real save even while the live table is forced). That is what limits
// what they may PICK. LanCostumeFillTable fills the live table to the static ceiling so a
// costume the OTHER player unlocked still resolves to its real art here.
int  LanCostumeUnlocked(int charId);
void LanCostumeFillTable();

// Helpers shared with the UI.
uint32_t LanMaster();
uint32_t LanFeMgr();
uint32_t LanScreen(int id);
const char* LanArenaName(int arena);
const char* LanCharName(int charId);
const char* LanTeamName(int team);      // "TEAM A" / "الفريق أ", from the retail table

}  // namespace tj::hybrid
