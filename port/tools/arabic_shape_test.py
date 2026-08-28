# -*- coding: utf-8 -*-
"""Gate: the RUNTIME shaper and the OFFLINE shaper must agree, for every shipped string.

arabic.cpp shapes at draw time from tables in the pack -- it never sees Unicode, only a
logical byte, a joining class and four glyph bytes. This re-implements that algorithm
reading ONLY the pack (no imports from arabic_shape beyond the reference encoder) and
compares it against the offline shaper the previews were checked against.

What it actually catches is table generation: a wrong joining class, a form resolved to the
wrong glyph byte, a missing lam-alef pair, an off-by-one in the logical codepage. Those
would otherwise show up as one wrong letter somewhere in the game, months later.

    python port/tools/arabic_shape_test.py [path/to/arabic_font.bin]
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arabic_font import ROOT, FIRST_BYTE, logical_table, logical_encode, encode, build
from arabic_shape import shape
from arabic_strings import ALL as _MENU
from arabic_lan import LAN as _LAN
ALL = dict(_MENU); ALL.update(_LAN)

def parse(path):
    d = open(path, 'rb').read()
    magic, ver = struct.unpack_from('<4sH', d, 0)
    assert magic == b'TJAR' and ver == 5, (magic, ver)
    aw, ah, sy, sh, ng, ns = struct.unpack_from('<6H', d, 6)
    srcW, srcH = struct.unpack_from('<2H', d, 18)
    srcFmt, sigLen = struct.unpack_from('<2B', d, 22)
    sigOff, = struct.unpack_from('<H', d, 24)
    first, keepLen = struct.unpack_from('<2B', d, 26)
    lgN, ligN, lgFirst = struct.unpack_from('<3H', d, 28)
    o = 38 + sigLen + keepLen   # +4: spriteCount and its pad
    glyphs = []
    for i in range(ng):
        b, _p, x0, y0, x1, y1 = struct.unpack_from('<BBHHHH', d, o); o += 10
        glyphs.append(b)
    logical = []
    for i in range(lgN):
        rec = struct.unpack_from('<6B', d, o); o += 6
        logical.append(dict(byte=rec[0], join=rec[1], form=list(rec[2:])))
    ligs = []
    for i in range(ligN):
        ligs.append(struct.unpack_from('<4B', d, o)); o += 4
    strings = {}
    for i in range(ns):
        idx, ln = struct.unpack_from('<HH', d, o); o += 4
        strings[idx] = d[o:o + ln]; o += ln + 1
    return dict(logical=logical, lgFirst=lgFirst, ligs=ligs, strings=strings)


def runtime_shape(s, P):
    """A transcription of arabic.cpp's Shape(), driven only by the pack's tables."""
    def lg_of(b):
        i = b - P['lgFirst']
        return P['logical'][i] if 0 <= i < len(P['logical']) else None

    def lig_for(a, b):
        for l in P['ligs']:
            if l[0] == a and l[1] == b:
                return l
        return None

    # Nothing hoists: colour is state (below), and a plain reversal already puts each button
    # glyph beside its own word with the author's separating space on the glyph's left.
    lead, i = [], 0
    toks = []
    while i < len(s):
        if s[i] in (7, 8) and i + 1 < len(s):
            toks.append(dict(out=s[i], esc=s[i + 1], lg=None, lig=None, ltr=True)); i += 2
            continue
        L = lig_for(s[i], s[i + 1]) if i + 1 < len(s) else None
        if L:
            toks.append(dict(out=0, esc=0, lg=None, lig=L, ltr=False)); i += 2
            continue
        g = lg_of(s[i])
        ltr = g is None and (48 <= s[i] <= 57 or 65 <= s[i] <= 90 or 97 <= s[i] <= 122)
        toks.append(dict(out=s[i], esc=0, lg=g, lig=None, ltr=ltr)); i += 1

    def joins_next(t): return bool(t['lg']) and t['lg']['join'] == 2
    def joins_prev(t): return bool(t['lig']) or (bool(t['lg']) and t['lg']['join'] != 0)

    for n, t in enumerate(toks):
        pj = n > 0 and joins_next(toks[n - 1])
        nj = n + 1 < len(toks) and joins_prev(toks[n + 1])
        if t['lig']:
            t['out'] = t['lig'][3] if pj else t['lig'][2]
        elif t['lg']:
            g = t['lg']['form'][3 if nj else 1] if pj else t['lg']['form'][2 if nj else 0]
            t['out'] = t['lg']['form'][0] if g == 0xFF else g

    # Colour in force at each token, in READING order; col 0 marks the escape itself.
    cur = 0x30
    for t in toks:
        if t['out'] == 7 and t['esc']:
            cur = t['esc']; t['col'] = 0
        else:
            t['col'] = cur

    out = bytearray(lead)
    i = len(toks)
    emitted = 0x30
    prev_out = 0            # previously EMITTED token, not the previous byte
    while i > 0:
        e, st = i, i - 1
        if toks[st]['ltr']:
            while st > 0 and toks[st - 1]['ltr']:
                st -= 1
        else:
            e = st + 1
        for k in range(st, e):
            if not toks[k]['col']:
                continue                       # a colour escape: state, not text
            if toks[k]['col'] != emitted:      # re-open the colour at this boundary
                out += bytes([7, toks[k]['col']])
                emitted = toks[k]['col']
            if toks[k]['out'] == 8 and prev_out:
                want = 3 if 0x41 <= toks[k]['esc'] <= 0x5A else 2
                have = 0
                while have < len(out) and out[len(out) - 1 - have] == 0x20:
                    have += 1
                out += bytes([0x20] * max(0, want - have))
            out.append(toks[k]['out'])
            if toks[k]['esc']:
                out.append(toks[k]['esc'])
            prev_out = toks[k]['out']
        i = st
    return bytes(out)


if __name__ == '__main__':
    pack = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, 'port', 'build-arabic', 'arabic_font.bin')
    P = parse(pack)
    # the reference: the offline shaper, over the same glyph assignment the pack used
    cps = sorted({cp for t in ALL.values() for cp, _ in shape(t)
                  if cp != 'ESC' and isinstance(cp, int) and cp >= 0x80})
    _atlas, glyphs = build('C:/Windows/Fonts/tahomabd.ttf', cps)
    byte_of = {g['cp']: FIRST_BYTE + i for i, g in enumerate(glyphs)}
    logical = logical_table(ALL.values())

    bad = 0
    for idx, text in sorted(ALL.items()):
        want = encode(text, byte_of, 'string 0x%X' % idx)   # offline: shaped directly
        stored = P['strings'][idx]
        assert stored == logical_encode(text, logical), 'pack string 0x%X is not the logical text' % idx
        got = runtime_shape(stored, P)                      # runtime: shaped from the tables
        if got != want:
            bad += 1
            print('MISMATCH 0x%02X %s' % (idx, text))
            print('   offline %s' % want.hex(' '))
            print('   runtime %s' % got.hex(' '))
    print('=== %d strings, %d mismatched ===' % (len(ALL), bad))

    # --- COMPOSED rows: label + value, the way the engine actually draws them ------------
    # A settings row is ONE line: the item renderer strcat's the VALUE onto its LABEL before
    # anything is shaped, so retail's leading 1 ends up in the MIDDLE. Standalone strings
    # cannot expose that -- both shapers agreed on all of them while the colour was landing on
    # the wrong half on screen -- so the property is asserted directly here instead: every
    # glyph that came from the VALUE must be drawn under colour 1, and no label glyph may be.
    def drawn(seq):
        """-> [(glyph byte, colour in force)], escapes consumed as the glyph loop does."""
        outp, cur, i = [], 0x30, 0
        while i < len(seq):
            if seq[i] == 7 and i + 1 < len(seq): cur = seq[i + 1]; i += 2; continue
            if seq[i] == 8 and i + 1 < len(seq): i += 2; continue
            outp.append((seq[i], cur)); i += 1
        return outp

    PAIRS = [(0x44, 0x46), (0x44, 0x45), (0x44, 0x47), (0x4B, 0x15), (0x4B, 0x16),
             (0xDC, 0x15), (0xDC, 0x16), (0x3B, 0x3F), (0x14, 0x15), (0x14, 0x16)]
    cbad = 0
    for lab, val in PAIRS:
        if lab not in ALL or val not in ALL:
            print('SKIP composed 0x%X+0x%X (not translated)' % (lab, val)); continue
        want_val = {b for b, _ in drawn(runtime_shape(logical_encode(ALL[val], logical), P))}
        want_lab = {b for b, _ in drawn(runtime_shape(logical_encode(ALL[lab], logical), P))}
        got = drawn(runtime_shape(logical_encode(ALL[lab] + ALL[val], logical), P))
        orange = {b for b, c in got if c == 0x31}
        plain  = {b for b, c in got if c != 0x31}
        # value glyphs the label does not also use must be orange, and vice versa
        miss = (want_val - want_lab) - orange
        leak = (want_lab - want_val) & orange
        if miss or leak:
            cbad += 1
            print('COMPOSED 0x%X+0x%X  value glyphs not orange: %s   label glyphs orange: %s'
                  % (lab, val, sorted(miss), sorted(leak)))
    print('=== %d composed rows, %d wrong ===' % (len(PAIRS), cbad))

    # --- ESCAPE PARITY WITH RETAIL ------------------------------------------------------
    # A button escape's SELECTOR CASE is the icon's SIZE, not a style: the glyph loop sizes
    # the sprite as 448*textScale/spriteH and doubles it for an UPPERCASE selector, so retail
    # picks the case per index to suit the scale that prompt is drawn at. Restoring uppercase
    # everywhere (session 35f) silently doubled four icons -- the user reported the corner
    # BACK button on the setup screen as "huge". A translation may change the WORDS; it may
    # never change the escapes, so require an exact per-index match against the retail table.
    xbe = os.path.join(ROOT, 'extracted', 'default.xbe')
    if os.path.exists(xbe):
        d = open(xbe, 'rb').read()
        base = 0x114C20 - 0xF5700 + 0xE5000          # retail string table, language 0
        def retail(idx):
            q = base + idx * 255
            return d[q:q + 255].split(b'\0')[0].decode('latin-1')
        def escapes(t):
            return [(t[i], t[i + 1]) for i in range(len(t) - 1) if t[i] in '\x07\x08']
        ebad = 0
        for idx, text in sorted(ALL.items()):
            if idx >= 227:                            # the retail table is 227 entries; every
                continue                              # index above it is a row this port ADDED
            ours, theirs = escapes(text), escapes(retail(idx))
            if ours != theirs:
                ebad += 1
                print('ESCAPE 0x%02X ours %r retail %r   (retail text %r)'
                      % (idx, ours, theirs, retail(idx)))
        print('=== escape parity: %d retail indices, %d differ ===' % (
            len([i for i in ALL if i < 227]), ebad))
    else:
        ebad = 0
        print('=== escape parity SKIPPED (no extracted/default.xbe) ===')

    sys.exit(1 if (bad or cbad or ebad) else 0)
