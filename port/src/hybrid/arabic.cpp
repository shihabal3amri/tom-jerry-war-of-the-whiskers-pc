// Arabic localization -- see arabic.h for why it lives outside the retail string table.
//
// WHAT THE ENGINE GIVES US, all verified against the disassembly before any code was
// written (FUN_00015460 builds the font, FUN_00015EB0 draws it, FUN_00016680 measures it,
// FUN_000796C0 turns a glyph rectangle into a textured quad):
//
//   font+0x000  glyph records, 16 bytes: {u32 resource, u16 texIdx, u16 x0,y0,x1,y1}
//   font+0x800  256-entry codepage map, byte per character, 0xFF = "no glyph"
//   font+0x900  em (u16)      font+0x902  default advance for unmapped bytes (u16)
//   font+0x904  advance base (s16)        font+0x908  x scale (float, 0.8)
//
// THREE CONSEQUENCES THAT SHAPE THIS FILE:
//
//  1. THE RECTANGLE IS THE ADVANCE. Width comes from (x1-x0), not from a metrics table, so
//     an Arabic letter's connecting stroke has to reach the rectangle edge exactly or the
//     word falls apart. The generator cuts every cell through a tatweel for that reason;
//     nothing here can fix a cell that was packed wrong.
//
//  2. THE RECORD ARRAY ENDS WHERE THE MAP BEGINS -- 0x800/0x10 = 128 slots, and retail
//     fills 124. A full translation needs 102 Arabic forms and the Latin alphabet cannot
//     leave (the rows this port ADDED stay English), so both do not fit one font. Arabic
//     gets its OWN font object and the glyph loop is handed whichever suits the line.
//
//  3. THE OVERLAY FONT MUST BE TOLD TO SHUT UP. FUN_00015EB0 looks EVERY byte up a second
//     time in the paired accent font and draws that glyph on top if it is mapped. Our
//     bytes land in the accented range, so each one is cleared to 0xFF there -- otherwise
//     Arabic letters come out wearing acutes and umlauts.
//
// SHAPING IS A RUNTIME PASS, not a build step. port/tools/arabic_font.py generates the
// glyph art and the tables, but strings ship in READING order and are shaped here, on the
// line the engine is about to draw. That ordering is what lets a translated string carry a
// printf conversion: the game's own vsprintf substitutes the value first, and the shaper
// runs afterwards. Shaped at build time, "%d" would have been reversed to "d%" long before
// the number existed. port/tools/arabic_shape_test.py gates the two shapers against each
// other over every shipped string.
#include "hybrid/arabic.h"
#include "hybrid/xdk_patch.h"
#include "hybrid/guest_call.h"

#include "hybrid/host_compat.h"
#include "runtime/assets/xmf_texture.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tj::hybrid {

const char* UserDataDir();          // file_io.cpp -- %LOCALAPPDATA%\TomJerryWOW
static void IniPath(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "%s\\tomjerry.ini", UserDataDir());
}

// ---- the generated pack ---------------------------------------------------------
#pragma pack(push, 1)
struct PackHdr {
    char     magic[4];        // "TJAR"
    uint16_t version;         // 1
    uint16_t atlasW, atlasH;  // replacement sheet, power of two (512 x 512)
    uint16_t stripY, stripH;  // the Arabic rows; everything above is retail art
    uint16_t glyphCount, stringCount;
    uint16_t srcW, srcH;      // the RETAIL sheet this pack was generated against
    uint8_t  srcFmt, sigLen;  // its Xbox format code, and how many signature bytes follow
    uint16_t sigOff;          // where in its source pixels the signature was taken
    uint8_t  firstByte;       // glyph-codepage byte of glyph 0 (the rest follow it)
    uint8_t  keepLen;         // retail characters the Arabic font must keep (digits etc)
    uint16_t logicalCount;    // base characters the shaper understands
    uint16_t ligCount;        // lam-alef pairs
    uint16_t logicalFirst;    // logical-codepage byte of base character 0
    uint16_t spriteCount;     // controller glyphs (see PackSprite)
    uint16_t pad_;
};
// A whole replacement texture, keyed by the first bytes of the RETAIL one it stands in for.
// Used for the controller face buttons -- the retail art is the 2001 glossy orb, these are
// the current Series X|S look. NOT a language feature: they apply whenever the pack is
// installed, in English too, because a button glyph is not text.
struct PackSprite { uint16_t w, h; uint8_t fmt, sigLen; uint16_t pad; };
struct PackGlyph { uint8_t byte, pad; uint16_t x0, y0, x1, y1; };
// One per BASE character: its joining class (0 non-joining, 1 right-joining, 2 dual) and the
// glyph byte to emit for each contextual form. Everything is resolved by the generator, so
// the runtime shaper never touches Unicode -- it is pure table lookup. 0xFF means the
// translation never produced that form and the shaper falls back to the isolated one.
struct PackLogical { uint8_t byte, join, form[4]; };   // form[] = iso, final, initial, medial
struct PackLig     { uint8_t lam, alef, iso, fin; };
#pragma pack(pop)

static uint8_t*   g_pack = nullptr;         // whole file
static PackHdr*   g_hdr = nullptr;
static PackGlyph* g_glyphs = nullptr;
static const uint8_t* g_strings = nullptr;  // {u16 idx, u16 len, bytes} * stringCount
static const uint8_t*  g_sig = nullptr;     // sigLen bytes of the retail sheet
static const uint8_t*  g_keep = nullptr;    // keepLen retail characters to carry over
static const PackLogical* g_logical = nullptr;
static const PackLig*     g_ligs = nullptr;
static const uint8_t*     g_sprites = nullptr;   // spriteCount x {PackSprite, sig, RGBA}
static const uint32_t* g_strip = nullptr;   // atlasW * stripH, 0xAARRGGBB

static bool g_loaded = false;               // the pack is in memory
static bool g_on = false;                   // Arabic is the selected language
static uint32_t g_arFont = 0;               // our own font object (guest arena)

// ---- guest layout ---------------------------------------------------------------
static const uint32_t kMasterPtr   = 0x015C470C;  // -> master object
static const uint32_t kFontSetOff  = 0x4D0;       // master + 0x4D0 -> font-set object
static const int      kSlots = 128;   // records run font+0x000..0x7FF, where the map begins

static bool Readable(uint32_t p, uint32_t len) {
    uint32_t end = p + len;
    if (end < p || p < 0x1000) return false;
    if (p >= 0x04000000u && end <= 0x10000000u) return true;   // the mapped game window
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end) {
        if (!VirtualQuery((void*)(uintptr_t)p, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) return false;
        p = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
    }
    return true;
}
static uint8_t*  Map(uint32_t font)          { return (uint8_t*)(uintptr_t)(font + 0x800); }
static uint8_t*  Rec(uint32_t font, int i)   { return (uint8_t*)(uintptr_t)(font + i * 0x10); }
static uint16_t& U16(uint8_t* p, int off)    { return *(uint16_t*)(p + off); }

enum FontKind { kNotFont = 0, kText, kOverlay };

// Classify by SIGNATURE, not by offset. FUN_00015460 builds the pair back to back and
// stamps each with its own header: the text font gets em 30 and maps 'A' onto the retail
// capital-A rectangle (x 1..33, y 0..34 -- entry 0 of the table at .data 0x0F6400); the
// accent-overlay font gets em 17 and maps no unaccented Latin at all. Both carry the same
// default advance (10) and x scale (0.8). Anything that fails all of that is not a font
// and is left alone -- this scan writes into whatever it accepts, so it accepts narrowly.
static FontKind ClassifyFont(uint32_t f) {
    if (!Readable(f, 0x910)) return kNotFont;
    uint16_t em = *(uint16_t*)(uintptr_t)(f + 0x900);
    if (*(uint16_t*)(uintptr_t)(f + 0x902) != 10) return kNotFont;
    if (*(float*)(uintptr_t)(f + 0x908) != 0.8f) return kNotFont;
    uint8_t a = Map(f)[0x41];
    if (em == 30 && a < 128) {
        uint8_t* r = Rec(f, a);
        if (U16(r, 6) == 1 && U16(r, 8) == 0 && U16(r, 10) == 33 && U16(r, 12) == 34)
            return kText;
    }
    if (em == 17 && a == 0xFF) return kOverlay;
    return kNotFont;
}

// ---- the texture sheets we replace ----------------------------------------------
// ONE SLOT PER SHEET ADDRESS, and the addresses keep coming. The sheet is loaded once per
// resource container, and a container reload places it wherever the loader's arena bump
// happens to land -- which depends on what was loaded before it, so entering the frontend
// from different levels yields different addresses. Four slots are exhausted within a couple
// of matches, after which registration silently stops and the retail 256x256 sheet is
// uploaded against 512x1024 rectangles: the garbled screen again, a few matches later.
static const int kAtlasSlots = kArabicAtlasSlots;
static uint32_t g_atlasRes[kAtlasSlots] = { 0 };
static int      g_atlasResN = 0;
static uint32_t g_atlasFmtd[kAtlasSlots] = { 0 };  // ORIGINAL format dword (pre-patch)
static uint32_t* g_atlasPix[kAtlasSlots] = { nullptr };

// IDENTIFY BY CONTENT. Reaching the sheet from a glyph record means three hops --
// container->+0x24 + texIdx*0x50, then *(+8), then +4 -- and getting any of them slightly
// wrong lands on the FONG sheet sitting 0x90 bytes away, which is the same art in grey and
// therefore looks plausible while being the wrong texture (measured: the chain resolved
// 00949C70 while the engine drew with 00949D00). The source bytes are unambiguous: the
// generator records the first bytes of the retail sheet's DXT5 data, and FONT and FONG
// already differ at offset 0x30. It also catches BOTH copies -- the frontend and HUD
// containers each load the file separately -- with no extra work.
bool ArabicMatchSheet(int fmt, int w, int h, const uint8_t* pix, uint32_t avail) {
    if (!g_loaded || !g_hdr || !pix) return false;
    if (fmt != g_hdr->srcFmt || w != g_hdr->srcW || h != g_hdr->srcH) return false;
    if (avail < (uint32_t)g_hdr->sigOff + g_hdr->sigLen) return false;
    return memcmp(pix + g_hdr->sigOff, g_sig, g_hdr->sigLen) == 0;
}

// The controller glyphs, matched the same way and for the same reason: the pointer chain
// from a sprite object is not worth trusting when the bytes identify the texture exactly.
static uint32_t g_sprRes[8] = { 0 };
static int      g_sprIdx[8] = { 0 };
static int      g_sprN = 0;

static const uint8_t* SpriteAt(int i) {
    const uint8_t* q = g_sprites;
    for (int k = 0; k < i; ++k) {
        const PackSprite* sp = (const PackSprite*)q;
        q += sizeof(PackSprite) + sp->sigLen + (size_t)sp->w * sp->h * 4;
    }
    return q;
}

bool ArabicMatchSprite(int fmt, int w, int h, const uint8_t* pix, uint32_t avail, int* out) {
    if (!g_loaded || !g_sprites || !pix) return false;
    for (int i = 0; i < g_hdr->spriteCount; ++i) {
        const uint8_t* q = SpriteAt(i);
        const PackSprite* sp = (const PackSprite*)q;
        if (sp->fmt != fmt || sp->w != w || sp->h != h) continue;
        if (avail < sp->sigLen) continue;
        if (memcmp(pix, q + sizeof(PackSprite), sp->sigLen) != 0) continue;
        *out = i;
        return true;
    }
    return false;
}

void ArabicRegisterSprite(uint32_t res, int idx) {
    for (int i = 0; i < g_sprN; ++i) if (g_sprRes[i] == res) return;
    if (g_sprN >= 8) return;
    g_sprRes[g_sprN] = res; g_sprIdx[g_sprN] = idx; ++g_sprN;
    printf("[ar] controller glyph %d matched: res=%08X\n", idx, res);
}

int ArabicSpriteFor(uint32_t res) {
    for (int i = 0; i < g_sprN; ++i) if (g_sprRes[i] == res) return g_sprIdx[i];
    return -1;
}

const uint32_t* ArabicSpritePixels(int idx, int* w, int* h) {
    if (!g_loaded || !g_sprites || idx < 0 || idx >= g_hdr->spriteCount) return nullptr;
    const uint8_t* q = SpriteAt(idx);
    const PackSprite* sp = (const PackSprite*)q;
    *w = sp->w; *h = sp->h;
    return (const uint32_t*)(q + sizeof(PackSprite) + sp->sigLen);
}

// D3DTexture_GetLevelDesc -- which is what the engine divides the glyph rectangle by --
// reads the size straight out of the resource's packed format dword (USize log2 at bits
// 20-23, VSize at 24-27). Retelling it 512x512 is what lets retail rectangles keep their
// exact texel coordinates: the numbers do not move, the divisor does, and the retail art
// sits in the top-left quadrant where those same texels are.
// The size the engine DIVIDES GLYPH RECTANGLES BY lives in the resource header's format
// dword, and this is the only place that writes it.
static uint32_t AtlasFmtd(uint32_t orig) {
    uint32_t uL = 0, vL = 0;
    for (uint32_t v = g_hdr->atlasW; v > 1; v >>= 1) ++uL;
    for (uint32_t v = g_hdr->atlasH; v > 1; v >>= 1) ++vL;
    return (orig & ~0x0FF00000u) | (uL << 20) | (vL << 24);
}

void ArabicRegisterSheet(uint32_t res) {
    for (int i = 0; i < g_atlasResN; ++i) if (g_atlasRes[i] == res) return;
    if (g_atlasResN >= kAtlasSlots) return;
    int i = g_atlasResN++;
    g_atlasRes[i] = res;
    g_atlasFmtd[i] = *(uint32_t*)(uintptr_t)(res + 0x0C);
    *(uint32_t*)(uintptr_t)(res + 0x0C) = AtlasFmtd(g_atlasFmtd[i]);
    printf("[ar] sheet %d matched: res=%08X fmtd %08X -> %08X (%ux%u)\n", i, res,
           g_atlasFmtd[i], *(uint32_t*)(uintptr_t)(res + 0x0C), g_hdr->atlasW, g_hdr->atlasH);
}

// ⚠ THE PATCH DOES NOT SURVIVE A CONTAINER RELOAD, AND THE ADDRESS COMES BACK.
// The format dword is part of the D3D resource struct carried verbatim inside FONT.xmf, so
// reloading GFX\SFX\FONT.PS2 (every level entry, every return to the frontend) restores the
// retail 256x256 value -- and because the loader's arena is reset immediately before the
// reload, the resource lands at the SAME address. So ArabicIsFontAtlas() still says yes, the
// bridge returns the cached 512x1024 substitute without re-reading the header, and
// ArabicRegisterSheet's "already known" early-out means nothing ever re-patches the size.
// From the first transition on, the engine divides 512x1024 glyph rectangles by 256x256:
// every glyph samples the wrong region, v >= 1.0 wraps back into the retail quadrant, and
// the screen fills with fragments of Latin letters. That is the garbled post-match screen.
void ArabicReassertSheet(uint32_t res) {
    if (!g_loaded) return;
    for (int i = 0; i < g_atlasResN; ++i) {
        if (g_atlasRes[i] != res) continue;
        uint32_t* fmtd = (uint32_t*)(uintptr_t)(res + 0x0C);
        uint32_t want = AtlasFmtd(g_atlasFmtd[i]);
        if (*fmtd == want) return;                       // still ours
        // Only ever rewrite a dword that is still the RETAIL one we recorded. If the arena
        // handed this address to something else entirely, leave it alone.
        if (*fmtd != g_atlasFmtd[i]) return;
        *fmtd = want;
        printf("[ar] sheet %d: FONT.PS2 reloaded, size re-asserted %08X -> %08X\n",
               i, g_atlasFmtd[i], want);
        return;
    }
}

bool ArabicIsFontAtlas(uint32_t res) {
    for (int i = 0; i < g_atlasResN; ++i) if (g_atlasRes[i] == res) return true;
    return false;
}

const uint32_t* ArabicAtlasPixels(uint32_t res, int* w, int* h) {
    // Gated on the PACK, not on the font install: the bridge can meet the sheet before the
    // fonts exist, and once its size has been retold the substitution has to follow or
    // retail text samples a quarter of a sheet that is still 256 wide.
    if (!g_loaded) return nullptr;
    int i = -1;
    for (int k = 0; k < g_atlasResN; ++k) if (g_atlasRes[k] == res) { i = k; break; }
    if (i < 0) return nullptr;
    *w = g_hdr->atlasW; *h = g_hdr->atlasH;
    if (g_atlasPix[i]) return g_atlasPix[i];
    // The retail quadrant is decoded from the GAME'S OWN texture with the port's own
    // decoder, so Latin text is texel-identical to an unmodified build -- the Arabic
    // build must not quietly re-render every English glyph through a second code path.
    uint32_t data = *(uint32_t*)(uintptr_t)(res + 0x04);
    int fmt = (int)((g_atlasFmtd[i] >> 8) & 0xFF);
    int rw = 1 << ((g_atlasFmtd[i] >> 20) & 0xF), rh = 1 << ((g_atlasFmtd[i] >> 24) & 0xF);
    size_t n = (size_t)*w * (size_t)*h;
    uint32_t* buf = (uint32_t*)calloc(n, 4);
    if (!buf) return nullptr;
    uint32_t* tmp = (uint32_t*)calloc((size_t)rw * rh, 4);
    bool ok = tmp && Readable(data, 16) &&
              tj::assets::DecodeXboxTextureInto(fmt, (const uint8_t*)(uintptr_t)data,
                                                (size_t)rw * rh * 4, nullptr, rw, rh, tmp);
    if (ok)
        for (int y = 0; y < rh && y < *h; ++y)
            memcpy(buf + (size_t)y * *w, tmp + (size_t)y * rw, (size_t)(rw < *w ? rw : *w) * 4);
    else
        printf("[ar] retail atlas decode FAILED res=%08X fmt=%02X %dx%d -- Latin text will "
               "be blank\n", res, fmt, rw, rh);
    free(tmp);
    for (int y = 0; y < g_hdr->stripH; ++y) {
        int dy = g_hdr->stripY + y;
        if (dy >= *h) break;
        memcpy(buf + (size_t)dy * *w, g_strip + (size_t)y * g_hdr->atlasW,
               (size_t)g_hdr->atlasW * 4);
    }
    g_atlasPix[i] = buf;
    printf("[ar] atlas built for res=%08X: retail %dx%d + %d Arabic rows -> %dx%d\n",
           res, rw, rh, g_hdr->stripH, *w, *h);
    return buf;
}

// ---- the Arabic font ------------------------------------------------------------
// ARABIC GETS ITS OWN FONT OBJECT, and this is forced, not a preference. A record array
// holds 128 glyphs; the translation needs 102 Arabic forms, and the Latin alphabet cannot
// leave, because the strings this port ADDED (the LAN screens, RESOLUTION/DISPLAY, the
// MULTIPLAYER submenu) stay English and would otherwise draw blank. 102 + 26 + digits +
// punctuation does not fit. So the retail font is never modified at all: a second font is
// built beside it holding the Arabic forms plus only the retail characters Arabic strings
// actually reuse (digits and punctuation -- `keepLen` of them, listed by the generator),
// and the glyph loop is handed whichever font suits the string in front of it.
//
// Two things fall out of that, both good: English mode is bit-for-bit the retail path
// (nothing to restore, nothing to get wrong), and switching language is a flag, not a
// rebuild.
static void BuildArabicFont(uint32_t proto) {
    uint32_t f = (uint32_t)(uintptr_t)GuestObjAlloc(0x910, 16);
    if (!f) { printf("[ar] could not allocate the Arabic font\n"); return; }
    memcpy((void*)(uintptr_t)f, (const void*)(uintptr_t)proto, 0x910);   // header + art refs
    uint8_t* map = Map(f);
    // Keep only the retail characters the Arabic strings still use; everything else in the
    // map goes, which is what frees their slots.
    bool slotUsed[kSlots] = { false };
    for (int c = 0; c < 256; ++c) {
        bool keep = false;
        for (int k = 0; k < g_hdr->keepLen; ++k) if (g_keep[k] == c) { keep = true; break; }
        if (!keep) { map[c] = 0xFF; continue; }
        if (map[c] < kSlots) slotUsed[map[c]] = true;
    }
    uint8_t proto16[16];
    memcpy(proto16, Rec(proto, 0), 16);      // resource + texture index, from a real glyph
    int next = 0, placed = 0;
    for (int i = 0; i < g_hdr->glyphCount; ++i) {
        while (next < kSlots && slotUsed[next]) ++next;
        if (next >= kSlots) {
            printf("[ar] FONT FULL: %d of %d glyphs placed (%d slots kept for retail "
                   "characters)\n", placed, (int)g_hdr->glyphCount, g_hdr->keepLen);
            break;
        }
        uint8_t* r = Rec(f, next);
        memcpy(r, proto16, 16);
        U16(r, 6)  = g_glyphs[i].x0; U16(r, 8)  = g_glyphs[i].y0;
        U16(r, 10) = g_glyphs[i].x1; U16(r, 12) = g_glyphs[i].y1;
        map[g_glyphs[i].byte] = (uint8_t)next;
        slotUsed[next] = true;
        ++placed;
    }
    g_arFont = f;
    printf("[ar] Arabic font %08X built from %08X: %d glyphs + %d retail characters "
           "in %d slots\n", f, proto, placed, g_hdr->keepLen, kSlots);
}

// The paired overlay font is FUN_00015EB0's OWN rule, copied verbatim from 0x15EBE-0x15ED3:
// the frontend text font pairs with +0x1248, everything else with +0x2578. It is looked up
// INSIDE the glyph loop, from the font pointer, so it cannot be intercepted -- only
// reproduced. Our bytes have to be cleared there or every Arabic letter draws wearing an
// acute or an umlaut, since the codepage range Arabic occupies is the accented-Latin one.
static void SilenceOverlays() {
    if (!Readable(kMasterPtr, 4)) return;
    uint32_t master = *(uint32_t*)(uintptr_t)kMasterPtr;
    if (!master || !Readable(master + kFontSetOff, 4)) return;
    uint32_t set = *(uint32_t*)(uintptr_t)(master + kFontSetOff);
    if (!set) return;
    for (uint32_t off : { 0x1248u, 0x2578u }) {
        uint32_t ov = set + off;
        FontKind k = ClassifyFont(ov);
        printf("[ar] overlay candidate %08X (set+0x%04X): classify=%d\n", ov, off, (int)k);
        if (k != kOverlay) { fflush(stdout); continue; }
        for (int i = 0; i < g_hdr->glyphCount; ++i) Map(ov)[g_glyphs[i].byte] = 0xFF;
        printf("[ar] overlay font %08X (set+0x%04X) silenced\n", ov, off);
        fflush(stdout);
    }
    fflush(stdout);
}

// ...AND THE GAME BUILDS THEM AGAIN. Silencing once is not enough: the overlay map is
// rebuilt from the retail accent table every time the game reloads GFX\SFX\FONT.PS2, which
// is every level entry and every return to the frontend. The Arabic glyph bytes run
// 0x80..0xEC and 35 of them collide with the accented codes the rebuild re-populates, so
// from the first transition onward each of those letters had a retail ACCENT quad drawn on
// top of it -- which is what the user saw as tashkeel appearing "after the match" on words
// that carry none. Detected with a canary rather than a hook: if the first Arabic byte the
// overlay used to map is mapped again, the game has rebuilt it underneath us.
static void ResilenceOverlays() {
    if (!g_loaded || !g_hdr->glyphCount || !Readable(kMasterPtr, 4)) return;
    uint32_t master = *(uint32_t*)(uintptr_t)kMasterPtr;
    if (!master || !Readable(master + kFontSetOff, 4)) return;
    uint32_t set = *(uint32_t*)(uintptr_t)(master + kFontSetOff);
    if (!set) return;
    for (uint32_t off : { 0x1248u, 0x2578u }) {
        uint32_t ov = set + off;
        // STRUCTURAL, not classified. ClassifyFont also demands the em and the Latin-map
        // shape, and a miss there silently skips the clear -- which is what left the phone
        // wearing accents. The default-advance stamp is enough to know this is one of the
        // font objects the builder wrote, and clearing OUR pack's bytes in it is harmless
        // for any font except the Arabic one itself.
        if (ov == g_arFont || !Readable(ov, 0x910)) continue;
        if (*(uint16_t*)(uintptr_t)(ov + 0x902) != 10) continue;
        uint8_t* map = Map(ov);
        bool dirty = false;
        for (int i = 0; i < g_hdr->glyphCount; ++i)
            if (map[g_glyphs[i].byte] != 0xFF) { dirty = true; break; }
        if (!dirty) continue;
        for (int k = 0; k < g_hdr->glyphCount; ++k) map[g_glyphs[k].byte] = 0xFF;
        static int said = 0;
        if (said < 4) { ++said;
            printf("[ar] overlay %08X (set+0x%04X) carried our glyph bytes -- silenced\n", ov, off);
            fflush(stdout);
        }
    }
}

// Called once per PRESENTED FRAME (fe_menu's tick). The game rebuilds the overlay map on
// every level entry and every return to the frontend, so this has to be re-asserted from a
// path that is guaranteed to run -- not from the draw path, which on ARM never showed any
// sign of executing this.
void ArabicFrameTick(int) {
    if (!g_loaded || !g_on) return;
    ResilenceOverlays();
}

// THE FONT COMES TO US. There is more than one font object and they do not all hang off one
// root -- the frontend pair is built by FUN_00015460 and the HUD/OSD pair by FUN_000158A0,
// each screen keeps its own style block, and the title-screen prompt uses neither of the two
// a walk from master+0x4D0 reaches. So nothing models that graph: FUN_00015EB0 takes the
// font as its first argument, and the first real text font it hands us is the prototype.
static uint32_t g_seen[16];
static int      g_seenN = 0;

// ---- the shaper -----------------------------------------------------------------
// Strings are stored in READING order, one byte per character, and shaped HERE rather than
// offline. That is what lets a translated string carry a printf conversion: the game's own
// vsprintf substitutes the number first, and only then does this run. Pre-shaping could
// never work for those -- "%d" reversed is "d%", and the value would be formatted into a
// string whose direction had already been decided.
//
// This sits on FUN_00015EB0 and FUN_00016680, which is exactly the right place: both are
// handed ONE line, after vsprintf and after the newline split, and both must agree (the
// draw is positioned using the measure, so shaping only one would centre every Arabic line
// by its unshaped width).
static const PackLogical* LogicalOf(uint8_t b) {
    if (!g_logical || b < g_hdr->logicalFirst) return nullptr;
    unsigned i = (unsigned)(b - g_hdr->logicalFirst);
    return i < g_hdr->logicalCount ? &g_logical[i] : nullptr;
}
static const PackLig* LigFor(uint8_t a, uint8_t b) {
    for (int i = 0; i < g_hdr->ligCount; ++i)
        if (g_ligs[i].lam == a && g_ligs[i].alef == b) return &g_ligs[i];
    return nullptr;
}

// Does this line contain Arabic? Only then is anything done -- English strings (including
// every one this port added) keep drawing through the retail font, untouched.
static bool HasArabic(const uint8_t* s) {
    if (!s || !g_logical) return false;
    for (; *s; ++s) {
        if (*s == 7 || *s == 8) { if (s[1]) ++s; continue; }   // escape + selector: skip both
        if (LogicalOf(*s)) return true;
    }
    return false;
}

// Does the CURRENT draw have a colour array? The 0 restore emitted by the reorder pass
// below is the first thing ever to reach FUN_00015EB0's '0' branch, which dereferences the
// caller's colour pointer without checking it -- retail data is all 1 and never restores.
// Measurement keeps it true: escapes are zero-width there, so this cannot move any layout.
static bool g_haveCol = true;

struct Tok {
    uint8_t out, esc;                  // emitted byte(s); esc != 0 = a two-byte escape
    const PackLogical* lg;             // the base character, if it is one
    const PackLig* lig;                // the lam-alef pair, if it is one
    bool ltr;                          // part of a left-to-right run (digits/Latin/escapes)
    uint8_t col;                       // colour selector in force at this token, in READING
                                       // order; 0 marks the escape token itself
};
static bool JoinsNext(const Tok& t) { return t.lg && t.lg->join == 2; }
static bool JoinsPrev(const Tok& t) { return t.lig || (t.lg && t.lg->join != 0); }

// ⚠ THE SHAPED LINE IS HANDED TO THE GUEST. Hk_GlyphRun passes it straight to the engine's
// glyph loop as a pointer argument, so it has to be GUEST-VISIBLE -- below 4 GB. A host
// static is above that on ARM, and the dispatch layer's gptr tripwire correctly refused the
// call ("guest-call arg 1 above 4 GB"), which is what killed the phone the moment Arabic was
// switched on. Four deep, exactly as before: a caller may hold a shaped line across the
// measure-then-draw pair.
static const uint8_t* Shape(const uint8_t* in) {
    static const int kShapeSlots = 4, kShapeBytes = 512;
    static uint8_t* ring = (uint8_t*)GuestObjAlloc(kShapeSlots * kShapeBytes, 16);
    static int next = 0;
    uint8_t* out = ring + next * kShapeBytes; next = (next + 1) & (kShapeSlots - 1);

    // 1. tokenize. Escapes are atomic, and lam+alef folds into one ligature BEFORE forms are
    //    chosen -- an unligated lam-alef is not merely ugly in Arabic, it is wrong.
    Tok tok[256];
    int n = 0;
    const uint8_t* p = in;
    // NOTHING IS HOISTED. A leading escape used to be lifted to the output head to keep a
    // button sprite's overhang in the margin -- the engine places the pen from the icon's
    // SINGLE width and only then doubles it, so a full-size icon reaches back to the LEFT of
    // its pen. But "the output head" is the visual LEFT, and on an RTL line that is the END
    // of the sentence -- so on a line carrying TWO prompts the first glyph teleported past
    // the whole line and came to rest beside the OTHER prompt's word. The browser footer
    // read "[B] SELECT    [A] BACK": each button labelled with the wrong action, which is
    // what the user reported as the huge gap before رجوع. A plain reversal is simply correct
    // instead: logical [esc][sp][WORD] reverses to visual [WORD][sp][esc], so the glyph lands
    // at the RTL read-first end of its OWN word AND the author's separating space ends up on
    // the glyph's LEFT -- exactly where the overhang needs it. The guard in the emit loop
    // covers the one retail string that carries no separator of its own.
    while (*p && n < 255) {
        Tok& t = tok[n];
        t = Tok{};
        if ((*p == 7 || *p == 8) && p[1]) {
            t.out = p[0]; t.esc = p[1]; t.ltr = true; p += 2;
        } else if (const PackLig* L = p[1] ? LigFor(p[0], p[1]) : nullptr) {
            t.lig = L; p += 2;
        } else {
            t.out = *p;
            t.lg = LogicalOf(*p);
            t.ltr = !t.lg && ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                              (*p >= 'a' && *p <= 'z'));
            ++p;
        }
        ++n;
    }
    // 2. contextual form per token, from its neighbours' joining classes
    for (int i = 0; i < n; ++i) {
        Tok& t = tok[i];
        bool pj = i > 0 && JoinsNext(tok[i - 1]);
        bool nj = i + 1 < n && JoinsPrev(tok[i + 1]);
        if (t.lig) { t.out = pj ? t.lig->fin : t.lig->iso; continue; }
        if (!t.lg) continue;                       // ASCII, punctuation: passes through
        uint8_t g = t.lg->form[pj ? (nj ? 3 : 1) : (nj ? 2 : 0)];
        if (g == 0xFF) g = t.lg->form[0];          // form never packed: isolated is legible
        t.out = g;
    }
    // 2b. COLOUR IS STATE, NOT A CHARACTER. \x07+sel switches the colour of everything the
    //     glyph loop draws AFTER it, and retail puts that escape at the start of a settings
    //     VALUE -- which the item renderer has already strcat'd onto its LABEL, so it lands
    //     in the MIDDLE of the drawn line. Reversing it with the text moved its scope: the
    //     value lost the orange and the label gained it, which is exactly the "colours are
    //     not the same" the user reported on FIGHT SETTINGS. Numeric values looked right
    //     only because a number and its escape form one LTR run that survives reversal
    //     together -- so the six translated word-values broke and the ASCII ones did not,
    //     and that split IS the inconsistency. Record the colour in force at every token in
    //     READING order, then re-open it at each boundary of the REVERSED stream below.
    {
        uint8_t cur = '0';                         // '0' = whatever colour the caller passed
        for (int k = 0; k < n; ++k) {
            if (tok[k].out == 7 && tok[k].esc) { cur = tok[k].esc; tok[k].col = 0; }
            else                                tok[k].col = cur;
        }
    }
    // 3. VISUAL order. The glyph loop draws left to right and Arabic reads right to left, so
    //    the tokens are reversed -- but a run of digits, Latin or escapes is one left-to-right
    //    unit that keeps its own internal order: "10" reversed is "01", a different number.
    int w = 0;
    int i = n;
    uint8_t emitted = '0';                         // colour currently open in the OUTPUT
    uint8_t prevOut = 0;                           // previously EMITTED token's byte;
                                                   // the guard is per TOKEN, not per byte
                                                   // (after "\x08X" the last byte is 'X')
    while (i > 0 && w < kShapeBytes - 5) {
        int e = i;                                 // [s, e) is the run ending at i
        int s = i - 1;
        if (tok[s].ltr) { while (s > 0 && tok[s - 1].ltr) --s; }
        else e = s + 1;
        for (int k = s; k < e && w < kShapeBytes - 5; ++k) {
            if (!tok[k].col) continue;             // the escape token itself: state, not text
            if (tok[k].col != emitted) {           // cross a colour boundary -> re-open it
                // The '0' restore is the only branch retail data never exercises, and the
                // engine dereferences the caller's colour array in it without a null check.
                if (tok[k].col != '0' || g_haveCol) {
                    out[w++] = 7; out[w++] = tok[k].col;
                    emitted = tok[k].col;
                }
            }
            // CLEARANCE FOR THE SPRITE'S LEFT OVERHANG. The icon is placed from its
            // SINGLE width and then doubled for an UPPERCASE selector, so a full-size
            // one reaches back past its pen and eats the separating space. English never
            // shows this because there the icon LEADS and the overhang falls into the
            // margin; in Arabic it trails its word, so the overhang lands on the text.
            // Measured on the browser footer: one space left 4px of gap where English
            // has 11. Full size gets two spaces, half size (lowercase) keeps one, and an
            // icon with nothing to its left needs none -- the margin absorbs it.
            if (tok[k].out == 8 && prevOut) {
                int want = (tok[k].esc >= 'A' && tok[k].esc <= 'Z') ? 3 : 2;
                int have = 0;
                for (int q = w; q > 0 && out[q - 1] == ' '; --q) ++have;
                while (have < want && w < kShapeBytes - 6) { out[w++] = ' '; ++have; }
            }
            out[w++] = tok[k].out;
            if (tok[k].esc) out[w++] = tok[k].esc;
            prevOut = tok[k].out;
        }
        i = s;
    }
    out[w] = 0;
    return out;
}

// The two hooks hand the ORIGINAL (logical) line to FontFor and the SHAPED one to the
// engine; both must see the same decision, so the test is on the logical text.
static const uint8_t* ShapeIfArabic(const uint8_t* s) {
    return (g_on && g_loaded && HasArabic(s)) ? Shape(s) : s;
}

// -> the font the glyph loop should actually use for this line.
static uint32_t FontFor(uint32_t font, const uint8_t* s) {
    if (!g_loaded || !font) return font;
    bool known = false;
    for (int i = 0; i < g_seenN; ++i) if (g_seen[i] == font) { known = true; break; }
    if (!known) {
        if (g_seenN < 16) g_seen[g_seenN++] = font;
        if (!g_arFont && ClassifyFont(font) == kText) { BuildArabicFont(font); SilenceOverlays(); }
    }
    if (!g_on || !g_arFont || !HasArabic(s)) return font;
    // (the overlay re-assert lives in ArabicFrameTick -- once a frame, not once a LINE;
    //  Readable() now costs a mincore syscall per page outside the pool window)
    return ClassifyFont(font) == kText ? g_arFont : font;
}

// ---- the hooks on the text path -------------------------------------------------
// Both halves have to agree: FUN_000167F0 MEASURES a line to place it (centring, right
// alignment) and then DRAWS it, and it passes the same font to each. Hooking only the draw
// would centre every Arabic line by its Latin width.
using FnGlyphRun = void (__cdecl*)(uint32_t font, const uint8_t* s, float x, float y,
                                   float size, const uint32_t* col);
using FnTextWidth = float (__cdecl*)(uint32_t font, const uint8_t* s, float scale);
static uint32_t Orig_GlyphRun = 0, Orig_TextWidth = 0;

static void __cdecl Hk_GlyphRun(uint32_t font, const uint8_t* s, float x, float y,
                                float size, const uint32_t* col) {
    g_haveCol = (col != nullptr);
    uint32_t f = FontFor(font, s);                 // sequenced BEFORE the shape, so the flag
    const uint8_t* sh = ShapeIfArabic(s);          // is set for the pass that reads it
    GCALL(Cdecl, FnGlyphRun, Orig_GlyphRun, f, sh, x, y, size, col);
}
static float __cdecl Hk_TextWidth(uint32_t font, const uint8_t* s, float scale) {
    return GCALL(Cdecl, FnTextWidth, Orig_TextWidth, FontFor(font, s), ShapeIfArabic(s), scale);
}

// ---- language state -------------------------------------------------------------
bool ArabicAvailable() { return g_loaded; }
bool ArabicEnabled()   { return g_loaded && g_on; }
void ArabicSetEnabled(bool on) {
    if (!g_loaded) return;
    g_on = on;
    printf("[ar] language -> %s\n", on ? "ARABIC" : "ENGLISH");
}

// ---- strings --------------------------------------------------------------------
const char* ArabicText(uint16_t idx) {
    if (!g_loaded || !g_on) return nullptr;
    const uint8_t* p = g_strings;
    for (int i = 0; i < g_hdr->stringCount; ++i) {
        uint16_t sidx = *(const uint16_t*)p, len = *(const uint16_t*)(p + 2);
        if (sidx == idx) return (const char*)(p + 4);      // NUL-terminated by the packer
        p += 4 + len + 1;
    }
    return nullptr;
}

const char* AssetRoot();            // file_io.cpp -- the extracted game tree

// WHERE THE PACK LIVES, in order:
//   1. beside the executable -- the PC install and the copyable dist folder;
//   2. the asset root -- ANDROID, where the executable is libtjgame.so in the app's
//      read-only native-lib dir and the pack ships with the game data instead.
// ⚠ Split on EITHER separator: this used to look only for a backslash, so on Android it
// left the path as the executable itself and could never have found anything.
static bool OpenPack(const char* dir, const char* file, char* out, size_t cap, FILE** f) {
    if (!dir || !*dir) return false;
    char sep = strchr(dir, '\\') ? '\\' : '/';
    _snprintf_s(out, cap, _TRUNCATE, "%s%c%s", dir, sep, file);
    return fopen_s(f, out, "rb") == 0 && *f;
}

int InstallArabic() {
    char path[MAX_PATH];
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* s = strrchr(exeDir, '\\');
    char* s2 = strrchr(exeDir, '/');
    if (s2 && (!s || s2 > s)) s = s2;
    if (s) *s = 0; else exeDir[0] = 0;

    FILE* f = nullptr;
    if (!OpenPack(exeDir, "arabic_font.bin", path, sizeof path, &f) &&
        !OpenPack(AssetRoot(), "arabic_font.bin", path, sizeof path, &f)) {
        printf("[ar] no arabic_font.bin beside '%s' or in '%s' -- Arabic unavailable\n",
               exeDir, AssetRoot() ? AssetRoot() : "");
        return 0;
    }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    // ⚠ BELOW 4 GB, NOT malloc. Every string in this pack is handed to the GUEST by
    // Hk_GetText, and on ARM the host heap sits above 4 GB -- so each one would take
    // GuestInternStr's relocating path, whose cache is 256 entries against this pack's 282
    // strings. Past the cache it re-duplicates into the guest arena on EVERY draw ("a string
    // per frame", exactly as the comment on GuestInternStr warns), which exhausts the
    // below-4GB region within seconds of switching to Arabic and takes the process down
    // through LowAlloc's FATAL. Allocating the pack low makes GuestInternStr's
    // already-guest-visible early-out hit for every string, so nothing is ever copied.
    // Never freed, which is correct: it is loaded once and lives for the process.
    g_pack = (uint8_t*)GuestObjAlloc((size_t)n, 16);
    size_t rd = g_pack ? fread(g_pack, 1, (size_t)n, f) : 0;
    fclose(f);
    if (rd != (size_t)n || n < (long)sizeof(PackHdr)) { printf("[ar] short read\n"); return 0; }
    g_hdr = (PackHdr*)g_pack;
    if (memcmp(g_hdr->magic, "TJAR", 4) || g_hdr->version != 5) {
        printf("[ar] bad pack magic/version\n"); return 0;
    }
    g_sig = g_pack + sizeof(PackHdr);
    g_keep = g_sig + g_hdr->sigLen;
    g_glyphs = (PackGlyph*)(g_keep + g_hdr->keepLen);
    g_logical = (const PackLogical*)(g_glyphs + g_hdr->glyphCount);
    g_ligs = (const PackLig*)(g_logical + g_hdr->logicalCount);
    const uint8_t* p = (const uint8_t*)(g_ligs + g_hdr->ligCount);
    g_strings = p;
    for (int i = 0; i < g_hdr->stringCount; ++i) p += 4 + *(const uint16_t*)(p + 2) + 1;
    p = (const uint8_t*)(((uintptr_t)p + 3) & ~(uintptr_t)3);
    g_strip = (const uint32_t*)p;
    long need = (long)(p - g_pack) + (long)g_hdr->atlasW * g_hdr->stripH * 4;
    if (need > n) { printf("[ar] pack truncated (%ld < %ld)\n", n, need); return 0; }
    g_sprites = g_pack + need;                       // the controller glyphs follow the strip
    { const uint8_t* q = g_sprites;
      for (int i = 0; i < g_hdr->spriteCount; ++i) {
          const PackSprite* sp = (const PackSprite*)q;
          q += sizeof(PackSprite) + sp->sigLen + (size_t)sp->w * sp->h * 4;
      }
      if (q - g_pack > n) { printf("[ar] sprite table truncated\n"); return 0; } }
    g_loaded = true;
    printf("[ar] pack: %ux%u sheet, %u Arabic rows at y=%u, %u glyphs + %u retail chars, "
           "%u strings; retail sheet %ux%u fmt %02X matched on %u bytes at +%u\n",
           g_hdr->atlasW, g_hdr->atlasH, g_hdr->stripH, g_hdr->stripY,
           g_hdr->glyphCount, g_hdr->keepLen, g_hdr->stringCount, g_hdr->srcW, g_hdr->srcH,
           g_hdr->srcFmt, g_hdr->sigLen, g_hdr->sigOff);
    printf("[ar] shaper: %u base characters from byte %02X, %u lam-alef ligatures\n",
           g_hdr->logicalCount, g_hdr->logicalFirst, g_hdr->ligCount);
    // Prologues, hand-read; neither copied run contains a relative branch:
    //   0x15EB0  83 EC 44 (sub esp,0x44) + A1 0C 47 5C 01 (mov eax,[0x15C470C])  = 8
    //   0x16680  83 EC 0C (sub esp,0x0C) + A1 0C 47 5C 01                        = 8
    Orig_GlyphRun  = MakeGuestTramp(0x15EB0, 8, "ar:tr.glyphrun");
    Orig_TextWidth = MakeGuestTramp(0x16680, 8, "ar:tr.textwidth");
    if (!Orig_GlyphRun || !Orig_TextWidth) {
        printf("[ar] trampoline alloc failed -- Arabic unavailable\n");
        g_loaded = false; return 0;
    }
    // BOTH halves are typed. FUN_00016680 returns its width in ST0, which dispatch.h now
    // marshals (GuestMarshalPushF32) -- free on x86, where the bracket's fnsave already
    // captures what the hook left in ST0, and an explicit fld on the virtual x87 on ARM.
    int hooks = PatchJump(0x15EB0, HOOK_CDECL(Hk_GlyphRun),  "AR_GlyphRun")
              + PatchJump(0x16680, HOOK_CDECL(Hk_TextWidth), "AR_TextWidth");
    if (hooks != 2) {
        printf("[ar] could not hook the text path (%d/2) -- Arabic unavailable\n", hooks);
        g_loaded = false; return 0;
    }
    // Boot language: tomjerry.ini, overridden by TJ_ARABIC for scripted runs.
    char ini[MAX_PATH]; IniPath(ini, sizeof ini);
    g_on = GetPrivateProfileIntA("Language", "Arabic", 0, ini) != 0;
    char v[8] = { 0 };
    if (GetEnvironmentVariableA("TJ_ARABIC", v, sizeof v) && (v[0] == '0' || v[0] == '1'))
        g_on = v[0] == '1';
    printf("[ar] language at boot: %s\n", g_on ? "ARABIC" : "ENGLISH");
    return 1;
}

// Persisted beside the resolution the VIDEO screen writes (fe_menu's tomjerry.ini).
void ArabicSave() {
    char ini[MAX_PATH]; IniPath(ini, sizeof ini);
    BOOL ok = WritePrivateProfileStringA("Language", "Arabic", g_on ? "1" : "0", ini);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini);   // flush the mapping cache
    printf("[ar] saved Language/Arabic=%d to %s (%s)\n", g_on ? 1 : 0, ini,
           ok ? "ok" : "FAILED");
}

}  // namespace tj::hybrid
