"""Build the Arabic half of the Tom & Jerry glyph atlas.

The retail font is hand-painted gold lettering in a 256x256 DXT5 atlas (GFX/SFX/FONT.xmf,
texture "FONT"); every glyph is a rectangle in it and the rectangle's WIDTH is also the
glyph's advance.  This tool renders Arabic Presentation-Forms-B glyphs from a TrueType
face, paints them in the retail style, and packs them into a 512x512 atlas whose top-left
256x256 is the retail art VERBATIM -- so retail rectangles keep their exact texel
coordinates and only the divisor (the texture width the engine reads back from
D3DTexture_GetLevelDesc) changes.

Two things the engine forces on us:
  * the rectangle IS the advance, so a joined letter's connecting stroke has to reach the
    rectangle edge exactly, or the word breaks apart.  Each form is therefore rendered
    WITH a tatweel on every side it joins, and the cell is cut through that tatweel.
  * the engine multiplies glyph width by 0.8 (font+0x908) but not height, so the art is
    pre-stretched by 1/0.8 to come out with correct proportions.
"""
import os, re, sys, struct, argparse
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 're', 'item_render'))
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
from tex import dxt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arabic_shape import shape, FORMS, JOIN, LAM_ALEF

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
FONT_XMF = os.path.join(ROOT, 'extracted', 'GFX', 'SFX', 'FONT.xmf')
FXMF_OFF, FONT_TEX_OFF, FONG_TEX_OFF = 0x4A0, 0x80, 0x15600

SS        = 4        # supersample factor
XSTRETCH  = 1.25     # cancels the engine's 0.8 horizontal squash (font+0x908)
JOIN_KEEP = 0.38     # fraction of the tatweel kept on each joining edge
# Vertical frame. The engine anchors a glyph quad by its TOP and derives its height from
# (y1-y0), so every Arabic cell shares ONE frame and lines sit level. The cell height is the
# binding constraint: retail is all-capitals with no descenders (34 texels) and menu rows sit
# 0.07 apart, while Arabic needs a descender band -- at 58 texels the quad was 0.077 tall and
# eight OPTIONS rows overlapped. 49 fits the pitch; fit_metrics divides it between the
# tallest ascender and the deepest descender the translation actually uses.
MARGIN    = 2        # blank texels above the tallest ascender
RETAIL_BASELINE = 34 # the retail cell height, which for an all-capitals font IS its baseline
CELL_H    = 49       # provisional; fit_metrics returns the real one (baseline + descenders)
OUTLINE_R = 2.6      # outline radius in texels (retail reads ~2.5)

# RETAIL PALETTE, MEASURED -- not picked by eye. Every number below is the mean of the
# opaque pixels of the 26 retail capitals in GFX/SFX/FONT.xmf, split into fill and outline by
# luminance and bucketed by height within the glyph (the measurement is reproduced by
# `--measure`). The first attempt at this file guessed a dramatic pale-yellow-to-dark-orange
# gradient; the atlas says something quite different:
#
#   t=0.05  (249, 207,  97)      <- only the very top is lighter
#   t=0.10  (240, 196,  84)
#   t=0.15..0.75   ~(228, 177,  58)   <- FLAT. The body is one saturated amber.
#   outline mean (67, 47, 12), quartiles (8, 6, 2) .. (107, 73, 0)
#   the brightest fill pixels, at every height, ~(255, 219, 107)  <- the rim, and it is
#                                                                   AMBER, not white
# So the retail look is a flat amber body with a bright amber rim and a dark brown outline.
# A gradient is the wrong model for it entirely.
#
# ⚠ The body constant below is BRIGHTER than the measured retail body, on purpose. What has
# to match is the RENDERED mean, and Arabic strokes are thinner and more curved than blocky
# Latin capitals, so the rim and shade bands cover a larger fraction of each glyph and drag
# it down. Tuned against the measurement rather than by eye: with the body at the literal
# (228,177,58) the rendered Arabic came out (212,166,57) against retail's (228,179,63) --
# 7% dark. At (244,192,68) with the shade band at 0.55 it renders (224,178,65), a delta of
# (-4,-1,+2). Re-tune by measuring both, never by looking.
GOLD    = [(0.00, (255, 223, 108)), (0.12, (252, 204, 82)), (0.20, (244, 192, 68)),
           (0.78, (244, 192, 68)), (1.00, (226, 178, 62))]
OUTLINE = (58, 40, 8, 255)
SHADOW  = (0, 0, 0, 200)
RAMP_GAMMA = 1.0            # the measured body is flat; the ramp only lightens the top
RIM        = (255, 222, 112, 255)   # lit edge -- measured, amber and not white
RIM_A      = 0.8
RIM_W      = 1.1            # rim width in texels
SHADE      = (178, 128, 26, 255)    # shaded edge; measured as the fill's own p10
SHADE_A    = 0.55


# --- modern controller glyphs -------------------------------------------------------
# The retail face buttons are the 2001 Xbox look: glossy 32x32 orbs with a hard specular
# blob. These redraw them in the current Series X|S style -- a flat coloured disc, a thin
# darker rim, a white letter -- and the runtime swaps them by CONTENT, exactly the way the
# glyph sheet is swapped. Not a language feature: they apply whenever the pack is installed.
#
# BUTX/BUTT/BUTS/BUTC are the PlayStation names the PS2 build used, kept in the Xbox data:
# on this console they are A, Y, X and B respectively (verified by decoding them).
BUTTONS = [                    # xmf name, letter, disc colour
    ('BUTX', 'A', (61, 175, 63)),
    ('BUTC', 'B', (222, 62, 58)),
    ('BUTS', 'X', (40, 120, 205)),
    ('BUTT', 'Y', (241, 178, 34)),
]
BTN_FACE = 'C:/Windows/Fonts/segoeuib.ttf'


def render_button(letter, colour, size=32, ss=8):
    """One 32x32 controller glyph, drawn big and downsampled."""
    S2 = size * ss
    im = Image.new('RGBA', (S2, S2), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    pad = int(1.5 * ss)
    # soft drop shadow so the glyph reads on the game's bright backgrounds
    sh = Image.new('L', (S2, S2), 0)
    ImageDraw.Draw(sh).ellipse([pad, pad + ss, S2 - pad, S2 - pad + ss], fill=190)
    sh = sh.filter(ImageFilter.GaussianBlur(1.6 * ss))
    shim = Image.new('RGBA', (S2, S2), (0, 0, 0, 255)); shim.putalpha(sh)
    im.alpha_composite(shim)
    # the disc: a vertical gradient, light at the top, and a thin darker rim
    disc = Image.new('RGBA', (S2, S2), (0, 0, 0, 0))
    g = Image.new('RGBA', (S2, S2))
    gp = g.load()
    for y in range(S2):
        t = y / float(S2 - 1)
        k = 1.18 - 0.32 * t
        gp[0, y] = tuple(min(255, int(c * k)) for c in colour) + (255,)
        for x in range(1, S2):
            gp[x, y] = gp[0, y]
    m = Image.new('L', (S2, S2), 0)
    ImageDraw.Draw(m).ellipse([pad, pad, S2 - pad, S2 - pad], fill=255)
    disc.paste(g, (0, 0), m)
    rim = Image.new('L', (S2, S2), 0)
    ImageDraw.Draw(rim).ellipse([pad, pad, S2 - pad, S2 - pad], outline=255, width=int(0.9 * ss))
    rimim = Image.new('RGBA', (S2, S2), tuple(int(c * 0.62) for c in colour) + (255,))
    rimim.putalpha(rim)
    disc.alpha_composite(rimim)
    im.alpha_composite(disc)
    # the letter, optically centred on the disc rather than on the bitmap
    f = ImageFont.truetype(BTN_FACE, int(S2 * 0.60))
    tb = dr.textbbox((0, 0), letter, font=f)
    dr.text(((S2 - (tb[2] - tb[0])) / 2 - tb[0], (S2 - (tb[3] - tb[1])) / 2 - tb[1] - ss * 0.3),
            letter, font=f, fill=(255, 255, 255, 255))
    return im.resize((size, size), Image.LANCZOS)


def retail_sprite(name):
    """-> (w, h, xbox format code, the first SIG_LEN source bytes) for a FONT.xmf texture."""
    order = ['FONT', 'FONG', 'BUTX', 'BUTT', 'BUTS', 'BUTC', 'INFI', 'SPLR', 'DPAD']
    d = open(FONT_XMF, 'rb').read()
    i = order.index(name)
    _c, data, _p, fmtd, _s = struct.unpack_from('<5I', d, 0x30C + 0x94 + i * 0x14)
    w = 1 << ((fmtd >> 20) & 0xF)
    h = 1 << ((fmtd >> 24) & 0xF)
    return w, h, (fmtd >> 8) & 0xFF, d[FXMF_OFF + data: FXMF_OFF + data + SIG_LEN]


def retail_atlas(off):
    d = open(FONT_XMF, 'rb').read()
    px = d[FXMF_OFF + off: FXMF_OFF + off + 65536]
    rows = dxt(px, 0x0f, 256, 256)
    im = Image.new('RGBA', (256, 256))
    im.putdata([p for r in rows for p in r])
    return im


def ramp(t):
    for i in range(len(GOLD) - 1):
        a, ca = GOLD[i]
        b, cb = GOLD[i + 1]
        if a <= t <= b:
            u = 0.0 if b == a else (t - a) / (b - a)
            return tuple(int(ca[k] + (cb[k] - ca[k]) * u) for k in range(3))
    return GOLD[-1][1]


def form_class(cp):
    """0 isolated, 1 final, 2 initial, 3 medial."""
    for forms in FORMS.values():
        if cp in forms:
            return forms.index(cp)
    if 0xFEF5 <= cp <= 0xFEFC:
        return 1 if (cp - 0xFEF5) % 2 else 0        # lam-alef ligature: iso / final
    return 0


def fit_metrics(face, codepoints):
    """-> (type size in supersampled units, baseline offset within the cell).

    Sized from the TALLEST AND DEEPEST GLYPH ACTUALLY USED, never from the plain alef.
    Deriving it from the alef silently clipped everything taller than one: a hamza-carrying
    alef lost its hamza (\u0627\u0642\u0635\u0649 instead of \u0623\u0642\u0635\u0649) and high dots were shaved off. Faces also
    disagree wildly about what a point size means for Arabic, so nothing here trusts the
    nominal size either -- both numbers come from measured ink.
    """
    probe = 40 * SS
    f = ImageFont.truetype(face, probe)
    above = below = 1
    for cp in codepoints:
        im = Image.new('L', (probe * 4, probe * 4), 0)
        ImageDraw.Draw(im).text((probe, probe * 2), chr(cp), font=f, fill=255, anchor='ls')
        bb = im.getbbox()
        if bb is None:
            continue
        above = max(above, probe * 2 - bb[1])
        below = max(below, bb[3] - probe * 2)
    # THE WHOLE INK BAND FITS THE RETAIL CELL -- 34 texels, the same box a retail capital
    # occupies. The engine anchors a line by its TOP and hangs the \x08 button sprite off
    # that same top, so anything taller puts the Arabic lower in the line than the Latin it
    # replaces, and the highlight flare and the button glyph stay where retail put them
    # (reported: the text sat below the flare, the icon above it).
    #
    # ⚠ This does NOT align perfectly and cannot. Latin capitals fill their cell, so their
    # ink centroid is 44% down it; Arabic hangs a small body off tall ascenders with tails
    # below, so its centroid is lower whatever the cell. Measured against Latin's 15.0:
    # a 43-texel cell puts it at 26.1 (+11.1), and 34 puts it at 20.4 (+5.4) -- about 3px at
    # menu size. Closing the rest would mean shrinking the type to 57%, which is worse than
    # the misalignment. 34 also makes an Arabic quad IDENTICAL to a Latin one, so every
    # layout the game already tunes for retail keeps working, and a mixed line (a player
    # name on a LAN row) shares one baseline.
    usable = (RETAIL_BASELINE - 2 * MARGIN) * SS
    pt = max(1, int(probe * usable / float(above + below)))
    baseline = MARGIN * SS + int(round(above * usable / float(above + below)))
    return pt, baseline, RETAIL_BASELINE


def render_glyph(face, cp, joins_left, joins_right, pt, baseline, cell_h):
    """-> (RGBA cell image, cell width in atlas texels).  Cell height is CELL_H; the cell
    is cut through the tatweel on every joining edge so neighbours' strokes meet."""
    tat = '\u0640'
    s = (tat if joins_left else '') + chr(cp) + (tat if joins_right else '')
    f = ImageFont.truetype(face, pt)
    aT = f.getlength(tat)
    aL = aT if joins_left else 0.0
    aG = f.getlength(chr(cp))
    pen = 40 * SS
    W = pen * 2 + int(aL + aG + (aT if joins_right else 0))
    H = cell_h * SS + 40 * SS
    base = baseline + 20 * SS
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).text((pen, base), s, font=f, fill=255, anchor='ls')
    bb = mask.getbbox()
    if bb is None:
        raise SystemExit('glyph U+%04X rendered blank in %s' % (cp, face))
    bearing = int(2 * SS * XSTRETCH)
    left = pen + aL * (1.0 - JOIN_KEEP) if joins_left else bb[0] - bearing
    right = (pen + aL + aG + aT * JOIN_KEEP) if joins_right else (bb[2] + bearing)
    left, right = int(round(left)), int(round(right))
    # --- paint: drop shadow, outline, gold fill (the retail layer order) ---
    out = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    sh = mask.filter(ImageFilter.GaussianBlur(1.6 * SS))
    sh = sh.point(lambda v: min(255, int(v * 1.9)))
    shim = Image.new('RGBA', (W, H), SHADOW)
    shim.putalpha(sh)
    out.alpha_composite(shim, (int(2.2 * SS * XSTRETCH), int(2.2 * SS)))
    ol = mask.filter(ImageFilter.MaxFilter(int(OUTLINE_R * SS) * 2 + 1))
    olim = Image.new('RGBA', (W, H), OUTLINE)
    olim.putalpha(ol)
    out.alpha_composite(olim)
    # The ramp spans ink-top -> baseline so joined neighbours agree at the seam (a per-glyph
    # gradient would put a bright top on a short letter beside a tall one's mid-tone, and the
    # connecting stroke would visibly step). GAMMA pulls the bright half down over the body
    # band, where most Arabic letters actually live -- without it everything below the
    # ascenders sat in the dark end of the ramp and the whole line read as flat orange.
    top, bot = MARGIN * SS + 20 * SS, baseline + 20 * SS
    grad = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    gp = grad.load()
    for y in range(H):
        t = min(1.0, max(0.0, (y - top) / float(bot - top))) ** RAMP_GAMMA
        c = ramp(t)
        for x in range(W):
            gp[x, y] = (c[0], c[1], c[2], 255)
    grad.putalpha(mask)
    out.alpha_composite(grad)
    # --- the metallic read: a bright rim along the lit edge, a dark one along the shaded
    # edge. This is what the retail art has and a flat gradient does not; without it the
    # glyphs are the right colour and still look painted-on rather than moulded.
    # Rim = the part of the stroke the shifted copy no longer covers, i.e. its top-left band.
    def band(dx, dy):
        shifted = Image.new('L', (W, H), 0)
        shifted.paste(mask, (dx, dy))
        return ImageChops.subtract(mask, shifted)
    rim = band(int(RIM_W * SS * XSTRETCH), int(RIM_W * SS))
    rimim = Image.new('RGBA', (W, H), RIM)
    rimim.putalpha(rim.point(lambda v: int(v * RIM_A)))
    out.alpha_composite(rimim)
    shade = band(-int(RIM_W * SS * XSTRETCH), -int(RIM_W * SS))
    shim2 = Image.new('RGBA', (W, H), SHADE)
    shim2.putalpha(shade.point(lambda v: int(v * SHADE_A)))
    out.alpha_composite(shim2)
    cell = out.crop((left, 20 * SS, right, 20 * SS + cell_h * SS))
    w = max(1, int(round(cell.width / SS * XSTRETCH)))
    return cell.resize((w, cell_h), Image.LANCZOS), w


JOINL = {0: False, 1: False, 2: True, 3: True}     # by form class
JOINR = {0: False, 1: True, 2: False, 3: True}


ATLAS_W, ATLAS_H = 512, 1024      # power of two: the size fields are log2 (fmtd bits 20-27)


# Blank columns left between packed cells, filled by REPLICATING the cell's own edge column.
# Without this every glyph boundary showed a dark vertical line through the word (reported).
# The cause is bilinear filtering, not the art: the engine's UVs run x0/W .. x1/W, so the
# rightmost sampled texels blend with whatever sits at x1 -- and packed edge to edge, that is
# the NEXT GLYPH IN THE SHEET, an arbitrary neighbour. Clamp-to-edge would fix it, but the
# sampler state belongs to the game; replicating the edge into a gutter is the same thing
# baked into the atlas, and it costs 4 texels per glyph.
GUTTER = 2


def build(face, codepoints, w=ATLAS_W, h=ATLAS_H):
    atlas = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    atlas.paste(retail_atlas(FONT_TEX_OFF), (0, 0))          # retail block, verbatim
    pt, baseline, cell_h = fit_metrics(face, codepoints)
    # ⚠ START THE STRIP BELOW ROW 256, NOT ON IT. The retail quadrant's last rows carry ink
    # (measured: row 255 has 83 inked texels, maxA 142), and v0 = 256/1024 puts the bilinear
    # tap exactly between rows 255 and 256 -- so retail ink bleeds onto the TOP of every
    # first-row Arabic glyph. On screen that reads as tashkeel above letters that carry none,
    # which is exactly what the user reported. A two-texel gutter puts clear space between
    # the quadrant and the first cell; stripY stays 256 so the blank rows ship with the strip.
    glyphs, x, y, rowh = [], GUTTER, 256 + GUTTER * 2, 0
    for cp in codepoints:
        k = form_class(cp)
        cell, cw = render_glyph(face, cp, JOINL[k], JOINR[k], pt, baseline, cell_h)
        if x + cw + GUTTER > w:
            x = GUTTER
            y += rowh + GUTTER * 2
            rowh = 0
        if y + cell_h + GUTTER > h:
            raise SystemExit('atlas full at U+%04X (%d glyphs placed)' % (cp, len(glyphs)))
        atlas.alpha_composite(cell, (x, y))
        for k2 in range(1, GUTTER + 1):              # edge columns into the gutters
            atlas.paste(cell.crop((0, 0, 1, cell_h)), (x - k2, y))
            atlas.paste(cell.crop((cw - 1, 0, cw, cell_h)), (x + cw - 1 + k2, y))
        glyphs.append(dict(cp=cp, x0=x, y0=y, x1=x + cw, y1=y + cell_h))
        x += cw + GUTTER * 2
        rowh = max(rowh, cell_h)
    return atlas, glyphs


# the retail glyph table, straight out of the XBE (.data 0x0F6400, 124 x 5 bytes:
# charcode, x0, y0, x1, y1) -- so a preview can put Arabic beside the real Latin art.
RETAIL_TABLE_VA, RETAIL_TABLE_N = 0x0F6400, 124
EM, DEFAULT_ADV, XSCALE = 30, 10, 0.8      # font +0x900, +0x902, +0x908


def retail_glyphs():
    d = open(os.path.join(ROOT, 'extracted', 'default.xbe'), 'rb').read()
    off = 0xE5000 + (RETAIL_TABLE_VA - 0xF5700)          # .data: raw 0xE5000, VA 0xF5700
    out = {}
    for i in range(RETAIL_TABLE_N):
        c, x0, y0, x1, y1 = d[off + i * 5: off + i * 5 + 5]
        out[c] = dict(cp=c, x0=x0, y0=y0, x1=x1, y1=y1)
        out.setdefault(c ^ 0x20 if 0x41 <= (c & ~0x20) <= 0x5A else c, out[c])
    return out


def draw_run(atlas, cells, path=None, px=64, bg=(24, 26, 40), im=None, at=(20, 20)):
    """Simulate the engine's glyph loop: cells abut, width scaled by 0.8, height by 1.0,
    both against size/em -- and an unmapped byte advances by the font's default (0x902)."""
    k = px / float(EM)
    W = int(sum(((c['x1'] - c['x0']) if c else DEFAULT_ADV) * XSCALE * k for c in cells)) + 40
    H = int(max(c['y1']-c['y0'] for c in cells if c) * k) + 40
    if im is None:
        im = Image.new('RGBA', (W, H), bg + (255,))
    x = float(at[0])
    for c in cells:
        if c is None:
            x += DEFAULT_ADV * XSCALE * k
            continue
        w = (c['x1'] - c['x0']) * XSCALE * k
        h = (c['y1'] - c['y0']) * k
        sub = atlas.crop((c['x0'], c['y0'], c['x1'], c['y1'])).resize(
            (max(1, int(round(w))), max(1, int(round(h)))), Image.LANCZOS)
        im.alpha_composite(sub, (int(round(x)), at[1]))
        x += w
    if path:
        im.save(path)
    return im, int(x - at[0])


def preview_line(atlas, glyphs, text, path, px=64, bg=(24, 26, 40)):
    byid = {g['cp']: g for g in glyphs}
    return draw_run(atlas, [byid.get(cp) for cp, _ in shape(text)], path, px, bg)[0]


# --- the pack the runtime loads (see arabic.cpp for the reader) -------------------
# Arabic occupies bytes 0x80.. of a private codepage.  Those values can never collide
# with anything the engine treats specially: 0x00 terminator, 0x07/0x08/0x0B inline
# escapes, 0x0A newline (the game's own strtok delimiter) and 0x25 '%', which every
# displayed string passes through as a vsprintf FORMAT.
FIRST_BYTE = 0x80
# The runtime finds the sheet to replace by matching these bytes of its SOURCE pixels --
# no pointer walking. 64 is ample: FONT and FONG (the same letterforms in grey, which the
# game loads from the same file) already differ at offset 0x30.
SIG_OFF, SIG_LEN, SRC_FMT, SRC_W, SRC_H = 0, 64, 0x0F, 256, 256
# Retail characters the Arabic font ALWAYS keeps, whether or not a translated string happens
# to use one.  Text formatted at RUNTIME (a "%d" the game substitutes, the VIDEO screen's
# resolution, the audio sliders' percentages) can produce any digit, and a digit with no
# glyph silently becomes a default-advance blank -- a bug that would only show up for one
# particular value.
# '/' is here for a composed cell, not for any packed string: the LAN lobby builds
# "SKIN 1/5" at RUNTIME, and a line that mixes Arabic with a '/' draws through the ARABIC
# font -- where, before this, '/' had no record and silently became a blank advance. The
# user read the result as "مظهر 1 5" and asked what the 5 was. It costs a slot in a record
# array that is exactly full, which is paid for by the scan below no longer keeping the
# characters of a %-conversion (see there).
MANDATORY_KEEP = '0123456789:.,!?%X/'


def retail_src(off=FONT_TEX_OFF):
    """The retail sheet's DXT5 bytes exactly as the game loads them."""
    return open(FONT_XMF, 'rb').read()[FXMF_OFF + off: FXMF_OFF + off + 65536]


def encode(text, byte_of, where=''):
    """logical Arabic -> the visual-order byte string the engine's glyph loop draws.

    Used for the PREVIEW renderers only. Shipped strings go through logical_encode: the
    runtime shapes them, so a string can survive the game's own vsprintf (a pre-shaped
    "%d" would have been reversed to "d%" before the number was ever substituted).
    """
    enc = bytearray()
    for cp, src in shape(text):
        if cp == 'ESC':
            enc += src.encode('latin-1')          # \x07+selector or \x08+letter, verbatim
        elif cp in byte_of:
            enc.append(byte_of[cp])
        elif isinstance(cp, int) and cp < 0x80:
            enc.append(cp)                        # space, digits, ASCII punctuation
        else:
            raise SystemExit('U+%04X in %s has no glyph' % (cp, where))
    return bytes(enc)


# --- the LOGICAL codepage -------------------------------------------------------------
# Shipped strings are one byte per character, in READING order, and the runtime shapes
# them. Two reasons it is not UTF-8: the game's vsprintf writes into a 256-byte stack
# buffer, and two-byte characters would double every string toward smashing it; and one
# byte per character keeps a translated line the same length as the English it replaces.
#
# Logical bytes may reuse the same numeric range as glyph bytes. They never coexist: a
# string is entirely logical until the shaper runs, and entirely glyph bytes after.
LOGICAL_FIRST = 0x80


def logical_table(texts):
    """-> (logical byte per base character, ligature list) for everything the strings use."""
    base = []
    for t in texts:
        for ch in t:
            if ch >= '؀' and ch not in base:
                base.append(ch)
    base.sort()
    if LOGICAL_FIRST + len(base) > 0x100:
        raise SystemExit('%d base characters do not fit the logical codepage' % len(base))
    return {ch: LOGICAL_FIRST + i for i, ch in enumerate(base)}


def logical_encode(text, logical, where=''):
    """logical Arabic text -> the byte string the GAME stores and the runtime shapes."""
    out = bytearray()
    for ch in text:
        if ch in logical:
            out.append(logical[ch])
        elif ord(ch) < 0x80:
            out.append(ord(ch))                   # ASCII, the escapes, %d, newline
        else:
            raise SystemExit('U+%04X in %s has no logical byte' % (ord(ch), where))
    return bytes(out)


def emit_pack(path, atlas, glyphs, strings, stripy=256):
    """strings: {retail string index: logical Arabic text}."""
    byte_of = {g['cp']: FIRST_BYTE + i for i, g in enumerate(glyphs)}
    if FIRST_BYTE + len(glyphs) > 0x100:
        raise SystemExit('%d glyphs will not fit the codepage above 0x%02X' % (len(glyphs), FIRST_BYTE))
    striph = max(g['y1'] for g in glyphs) - stripy
    sig = retail_src()[SIG_OFF:SIG_OFF + SIG_LEN]
    assert len(sig) == SIG_LEN
    enc = {idx: encode(t, byte_of, 'string 0x%X' % idx) for idx, t in strings.items()}
    # Every NON-Arabic byte the Arabic strings still use -- digits, punctuation, the escape
    # selectors. The Arabic font keeps exactly these retail glyphs and frees all the rest,
    # which is what makes room for 101 Arabic forms in a 128-slot record array.
    # A %-CONVERSION IS NOT TEXT. The game's vsprintf substitutes the value long before the
    # shaper or the glyph loop ever see the line, so the 'd' of a "%d" is never drawn -- yet
    # scanning the raw bytes kept a record slot for it, in an array that is exactly full.
    # Dropping the conversion's letters is what buys the '/' added to MANDATORY_KEEP above.
    # ('%' itself stays kept, explicitly: the audio rows really do draw a literal per-cent.)
    # ⚠ SCAN THE LOGICAL TEXT, NOT `enc`. enc is SHAPED -- reversed -- so a "%d" in it has
    # already become "d%", and a conversion-stripping regex written for reading order silently
    # matches nothing. (encode()'s own docstring warns about exactly this reversal.)
    conv = re.compile(r'%[-+ #0-9.]*[a-zA-Z]')
    keep = set(MANDATORY_KEEP.encode('ascii'))
    for t in strings.values():
        t = conv.sub('', t)
        skip = False
        for ch in t:
            if skip: skip = False; continue       # an escape SELECTOR is consumed by the
            if ch in '\x07\x08': skip = True; continue  # escape handler, never a glyph
            b = ord(ch)
            if 0x20 < b < 0x80: keep.add(b)
    keep = sorted(keep)

    # --- the shaping tables the runtime needs -------------------------------------
    # Everything is resolved HERE, so the C++ shaper never touches Unicode: per logical
    # byte it gets a joining class and the four glyph bytes to emit, and lam-alef gets an
    # explicit pair list. A form the translation never produced is 0xFF and the shaper
    # falls back to the isolated one -- an unjoined letter rather than a blank.
    logical = logical_table(strings.values())
    JOINCLASS = {'U': 0, 'R': 1, 'D': 2}
    lg = []
    for ch, b in sorted(logical.items(), key=lambda kv: kv[1]):
        forms = FORMS.get(ch, ())
        # The runtime indexes form[] with the full four-way rule (pj/nj -> iso/fin/ini/med),
        # so a letter with FEWER forms must have its slots FOLDED, not padded with the
        # isolated one. A right-joining letter (alef, dal, reh, waw, teh marbuta...) has only
        # iso and final: "initial" is really iso and "medial" is really FINAL, because such a
        # letter still takes a join from its predecessor. Padding all the missing slots with
        # iso instead dropped the final form of every one of them -- 168 of 200 strings.
        if len(forms) >= 4:
            f = [byte_of.get(forms[k], 0xFF) for k in range(4)]
        elif len(forms) == 2:
            iso, fin = byte_of.get(forms[0], 0xFF), byte_of.get(forms[1], 0xFF)
            f = [iso, fin, iso, fin]
        else:                                      # bare hamza, punctuation: one shape
            cp = forms[0] if forms else ord(ch)
            f = [byte_of.get(cp, 0xFF)] * 4
        lg.append((b, JOINCLASS.get(JOIN.get(ch, 'U'), 0), f))
    ligs = []
    for alef, pair in LAM_ALEF.items():
        if 'ل' in logical and alef in logical:
            ligs.append((logical['ل'], logical[alef],
                         byte_of.get(pair[0], 0xFF), byte_of.get(pair[1], 0xFF)))

    lenc = {idx: logical_encode(t, logical, 'string 0x%X' % idx) for idx, t in strings.items()}
    blob = bytearray()
    blob += b'TJAR' + struct.pack('<6H', 5, atlas.width, atlas.height, stripy, striph,
                                  len(glyphs)) + struct.pack('<H', len(strings))
    blob += struct.pack('<HHBBH', SRC_W, SRC_H, SRC_FMT, SIG_LEN, SIG_OFF)
    blob += struct.pack('<BBHHH', FIRST_BYTE, len(keep), len(lg), len(ligs), LOGICAL_FIRST)
    blob += struct.pack('<H', len(BUTTONS)) + b'\0' * 2
    blob += sig + bytes(keep)
    for i, g in enumerate(glyphs):
        blob += struct.pack('<BBHHHH', FIRST_BYTE + i, 0, g['x0'], g['y0'], g['x1'], g['y1'])
    for b, join, f in lg:
        blob += struct.pack('<6B', b, join, f[0], f[1], f[2], f[3])
    for l in ligs:
        blob += struct.pack('<4B', *l)
    for idx in sorted(lenc):
        s = lenc[idx]
        blob += struct.pack('<HH', idx, len(s)) + s + b'\0'
    while len(blob) & 3:
        blob += b'\0'
    strip = atlas.crop((0, stripy, atlas.width, stripy + striph))
    for r, g, b, al in strip.getdata():
        blob += struct.pack('<I', (al << 24) | (r << 16) | (g << 8) | b)
    # the controller glyphs, each keyed by the first bytes of the RETAIL texture it replaces
    for name, letter, colour in BUTTONS:
        w, h, fmt, bsig = retail_sprite(name)
        blob += struct.pack('<HHBBH', w, h, fmt, len(bsig), 0) + bsig
        for r, g, b, al in render_button(letter, colour, w).getdata():
            blob += struct.pack('<I', (al << 24) | (r << 16) | (g << 8) | b)
    open(path, 'wb').write(blob)
    return dict(bytes=len(blob), glyphs=len(glyphs), strings=len(lenc), striph=striph,
                buttons=len(BUTTONS),
                logical=len(lg), ligatures=len(ligs),
                keep=''.join(chr(c) for c in keep),
                slots_needed=len(glyphs) + len(keep))


if __name__ == '__main__':
    from arabic_strings import ALL as _MENU
    from arabic_lan import LAN as _LAN          # the port's own LAN screens
    STRINGS = dict(_MENU); STRINGS.update(_LAN)
    ap = argparse.ArgumentParser()
    ap.add_argument('--face', default='C:/Windows/Fonts/tahomabd.ttf')
    # The canonical location the installer and make_dist.ps1 look in. It is a build
    # artifact, never committed: the pack carries signature bytes from the game's own sheet.
    ap.add_argument('--pack', nargs='?', const=os.path.join(ROOT, 'port', 'build-arabic',
                    'arabic_font.bin'), default=None, help='write the pack (default: port/build-arabic/)')
    ap.add_argument('--out', default='.', help='where preview PNGs go')
    a = ap.parse_args()
    # Everything non-ASCII that survives shaping needs a glyph -- that is the presentation
    # forms AND the Arabic punctuation the shaper leaves alone (U+061F question mark,
    # U+060C comma), which have no contextual forms and render isolated.
    cps = sorted({cp for t in STRINGS.values() for cp, _ in shape(t)
                  if cp != 'ESC' and isinstance(cp, int) and cp >= 0x80})
    atlas, glyphs = build(a.face, cps)
    tag = os.path.splitext(os.path.basename(a.face))[0]
    atlas.save(os.path.join(a.out, 'atlas_%s.png' % tag))
    preview_line(atlas, glyphs, STRINGS[0x2E], os.path.join(a.out, 'line_%s.png' % tag))
    print('%s: %d glyphs' % (tag, len(glyphs)))
    if a.pack:
        os.makedirs(os.path.dirname(os.path.abspath(a.pack)), exist_ok=True)
        info = emit_pack(a.pack, atlas, glyphs, STRINGS)
        print(info)
        if info['slots_needed'] > 128:
            raise SystemExit('%d slots needed but a font record array holds 128'
                             % info['slots_needed'])
