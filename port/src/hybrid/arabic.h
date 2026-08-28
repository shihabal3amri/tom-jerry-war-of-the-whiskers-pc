// Arabic localization: a 7th language the retail table has no room for.
//
// The retail string table is a FIXED 6 x 227 x 255-byte block in .data (0x114C20), so
// Arabic is not stored there at all -- fe_menu's Hk_GetText already fully replaces the
// retail lookup (FUN_00019910), and this module simply answers first.
//
// The font is the real work. Retail glyphs are rectangles in a 256x256 hand-painted
// atlas, addressed through a 256-entry codepage map at font+0x800 -- so ANY byte can be a
// glyph, and Arabic needs no engine change to be *addressable*. What it does need:
//   * glyph art, which does not fit the full retail sheet -> the sheet is replaced by a
//     512x512 one (retail art in the top-left quadrant, VERBATIM and decoded by the same
//     decoder, so Latin text is unchanged texel for texel).
//   * shaping, because the glyph loop walks bytes forward with no notion of direction.
//     Strings ship in READING order and are shaped at DRAW time, on the line the engine is
//     about to measure and draw -- which is what lets one carry a printf conversion, since
//     the game's vsprintf has already substituted the value by then.
//   * somewhere to put 102 glyphs. The record array runs font+0x000..0x7FF -- 128 slots,
//     of which retail uses 124 -- and the Latin alphabet cannot leave, because the strings
//     this port ADDED (the LAN screens, RESOLUTION/DISPLAY, the MULTIPLAYER submenu) stay
//     English. So the retail font is never touched: Arabic gets its OWN font object,
//     holding the Arabic forms plus only the retail characters Arabic strings reuse, and
//     the glyph loop is handed whichever of the two suits the line in front of it. English
//     mode is then the untouched retail path, and switching language is a flag.
#pragma once
#include <cstdint>

namespace tj::hybrid {

// Loads port/tools' generated `arabic_font.bin` from the exe directory and hooks the text
// path. No guest memory is touched here -- the font objects do not exist until the game's
// own init has run, so the Arabic font is built the first time a real one draws something.
int InstallArabic();

// --- language selection (the LANGUAGE row on the OPTIONS screen) -----------------
bool ArabicAvailable();          // the pack loaded: the row can be offered at all
bool ArabicEnabled();            // Arabic is the selected language right now
void ArabicSetEnabled(bool on);  // instant -- English mode is the untouched retail path
void ArabicSave();               // persist to tomjerry.ini, beside the display settings

// Hk_GetText provider. Returns nullptr for every index Arabic does not override (which,
// today, is all but the title-screen prompt).
const char* ArabicText(uint16_t idx);

// Once per presented frame. Re-asserts the accent-overlay silencing, which the game undoes
// whenever it reloads its font container. Cheap: a byte compare when nothing has changed.
void ArabicFrameTick(int frame);

// --- d3d8_bridge seam: the font sheet substitution -------------------------------
// The sheet is identified by its CONTENT, not by a pointer: the same file is loaded once
// per resource container (frontend and HUD each have their own copy) and the engine
// reaches it through three levels of indirection off a glyph record -- a chain that lands
// on the neighbouring FONG sheet if any hop is read wrong. A signature of the source
// bytes is exact, catches every copy, and cannot silently pick the wrong texture.

// Already-known sheet? (cheap, checked before any decode work)
// How many distinct sheet ADDRESSES may be tracked. The bridge's own texture cache must use
// the SAME number: if it is smaller, a sheet beyond its capacity is re-created on every bind
// and never reclaimed -- an unbounded texture + mip-chain leak.
static const int kArabicAtlasSlots = 16;
bool ArabicIsFontAtlas(uint32_t res);
// Do these source pixels look like the retail glyph sheet?
bool ArabicMatchSheet(int fmt, int w, int h, const uint8_t* pix, uint32_t avail);
// Record a matched sheet and retell the resource it is now atlasW x atlasH -- which is
// what D3DTexture_GetLevelDesc reports back to the engine, and therefore what glyph
// rectangles get divided by.
void ArabicRegisterSheet(uint32_t res);
// The game RELOADS GFX\SFX\FONT.PS2 on every level entry and every return to the frontend,
// into the SAME arena address, restoring the retail 256x256 size from the file. Call this on
// the already-known path BEFORE substituting, or from the first transition onward the engine
// divides 512x1024 glyph rectangles by 256x256 and the screen fills with garbage.
void ArabicReassertSheet(uint32_t res);
// The replacement pixels (0xAARRGGBB, w*h). Built once per resource: retail art decoded
// out of the guest's own texture, Arabic strip composited below it. Null if not ready.
const uint32_t* ArabicAtlasPixels(uint32_t res, int* w, int* h);

// --- controller glyphs -----------------------------------------------------------
// The face buttons, redrawn in the current Xbox style and matched the same way. NOT a
// language feature: a button glyph is not text, so these apply in English too.
bool ArabicMatchSprite(int fmt, int w, int h, const uint8_t* pix, uint32_t avail, int* out);
void ArabicRegisterSprite(uint32_t res, int idx);
int  ArabicSpriteFor(uint32_t res);          // -1 if this resource is not one of ours
const uint32_t* ArabicSpritePixels(int idx, int* w, int* h);

}  // namespace tj::hybrid
