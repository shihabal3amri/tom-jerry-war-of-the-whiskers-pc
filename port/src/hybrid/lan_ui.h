// Phase-2 LAN, stage 6: the in-game UI -- a LAN GAME row on the main menu, the server
// browser on dead retail screen 5, the lobby on dead retail screen 15, and a text-entry
// modal for names, passwords and direct IPs.
#pragma once
#include <cstdint>

namespace tj::hybrid {

int  InstallLanUi();

// Custom localized strings. fe_menu's Hk_GetText (the native replacement of FUN_00019910)
// asks this first: indices >= 0x100 never occur in retail data.
const char* LanCustomText(uint16_t idx);

// Called from the main-menu Build/Update hooks in fe_menu.cpp.
void     LanMenuBuild(uint32_t self);
// The main-menu row's label is COMPOSED into lan_ui's own buffer, not looked up from the
// string table: the item stores only the index and g_txt[] holds the bytes. The frontend
// factory calls a screen's Build ONCE per frontend construction, never on a screen change,
// so a language switch made on OPTIONS has to re-compose it explicitly.
void     LanMenuRefreshText();
uint32_t LanMenuUpdate(uint32_t self, uint32_t stockResult);

// Once per presented frame, from FeMenuFrameTick.
void LanUiFrameTick(int frame);

// Keyboard capture for the text modal (WndProc -> lock-free ring -> game thread).
bool LanTextCaptureActive();

}  // namespace tj::hybrid

extern "C" void LanTextCapturePush(int ch);   // called from WndProc (any thread)
