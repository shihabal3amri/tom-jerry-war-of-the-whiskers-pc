"""Arabic shaping for the Tom & Jerry font: logical Arabic text -> the ordered list of
Presentation-Forms-B glyphs the game's LEFT-TO-RIGHT glyph loop must draw.

The engine (FUN_00015EB0) walks bytes forward and advances +x, with no notion of
direction, so the shaper does BOTH jobs the renderer cannot: pick the contextual form
of every letter, and emit the result in VISUAL order (first byte = leftmost glyph).
"""

# joining class per Arabic letter:  D = dual (joins both sides), R = right-joining
# only (accepts a join from the previous letter but never joins to the next),
# U = non-joining, C = transparent (marks -- not handled, we ship unvocalised text).
JOIN = {
    '\u0621':'U','\u0622':'R','\u0623':'R','\u0624':'R','\u0625':'R','\u0626':'D',
    '\u0627':'R','\u0628':'D','\u0629':'R','\u062A':'D','\u062B':'D','\u062C':'D',
    '\u062D':'D','\u062E':'D','\u062F':'R','\u0630':'R','\u0631':'R','\u0632':'R',
    '\u0633':'D','\u0634':'D','\u0635':'D','\u0636':'D','\u0637':'D','\u0638':'D',
    '\u0639':'D','\u063A':'D','\u0640':'D','\u0641':'D','\u0642':'D','\u0643':'D',
    '\u0644':'D','\u0645':'D','\u0646':'D','\u0647':'D','\u0648':'R','\u0649':'D',
    '\u064A':'D',
}
# base codepoint in Presentation Forms-B; entries are (isolated, final, initial, medial)
# -- R-class letters only have the first two.
FORMS = {
    '\u0621':(0xFE80,),
    '\u0622':(0xFE81,0xFE82),          '\u0623':(0xFE83,0xFE84),
    '\u0624':(0xFE85,0xFE86),          '\u0625':(0xFE87,0xFE88),
    '\u0626':(0xFE89,0xFE8A,0xFE8B,0xFE8C),
    '\u0627':(0xFE8D,0xFE8E),
    '\u0628':(0xFE8F,0xFE90,0xFE91,0xFE92),
    '\u0629':(0xFE93,0xFE94),
    '\u062A':(0xFE95,0xFE96,0xFE97,0xFE98),
    '\u062B':(0xFE99,0xFE9A,0xFE9B,0xFE9C),
    '\u062C':(0xFE9D,0xFE9E,0xFE9F,0xFEA0),
    '\u062D':(0xFEA1,0xFEA2,0xFEA3,0xFEA4),
    '\u062E':(0xFEA5,0xFEA6,0xFEA7,0xFEA8),
    '\u062F':(0xFEA9,0xFEAA),          '\u0630':(0xFEAB,0xFEAC),
    '\u0631':(0xFEAD,0xFEAE),          '\u0632':(0xFEAF,0xFEB0),
    '\u0633':(0xFEB1,0xFEB2,0xFEB3,0xFEB4),
    '\u0634':(0xFEB5,0xFEB6,0xFEB7,0xFEB8),
    '\u0635':(0xFEB9,0xFEBA,0xFEBB,0xFEBC),
    '\u0636':(0xFEBD,0xFEBE,0xFEBF,0xFEC0),
    '\u0637':(0xFEC1,0xFEC2,0xFEC3,0xFEC4),
    '\u0638':(0xFEC5,0xFEC6,0xFEC7,0xFEC8),
    '\u0639':(0xFEC9,0xFECA,0xFECB,0xFECC),
    '\u063A':(0xFECD,0xFECE,0xFECF,0xFED0),
    '\u0640':(0x0640,0x0640,0x0640,0x0640),
    '\u0641':(0xFED1,0xFED2,0xFED3,0xFED4),
    '\u0642':(0xFED5,0xFED6,0xFED7,0xFED8),
    '\u0643':(0xFED9,0xFEDA,0xFEDB,0xFEDC),
    '\u0644':(0xFEDD,0xFEDE,0xFEDF,0xFEE0),
    '\u0645':(0xFEE1,0xFEE2,0xFEE3,0xFEE4),
    '\u0646':(0xFEE5,0xFEE6,0xFEE7,0xFEE8),
    '\u0647':(0xFEE9,0xFEEA,0xFEEB,0xFEEC),
    '\u0648':(0xFEED,0xFEEE),
    '\u0649':(0xFEEF,0xFEF0,0xFBE8,0xFBE9),
    '\u064A':(0xFEF1,0xFEF2,0xFEF3,0xFEF4),
}
# LAM + ALEF must become one ligature glyph -- an unligated lam-alef is not merely ugly
# in Arabic, it is wrong.  (isolated, final) per alef variant.
LAM_ALEF = {
    '\u0622':(0xFEF5,0xFEF6), '\u0623':(0xFEF7,0xFEF8),
    '\u0625':(0xFEF9,0xFEFA), '\u0627':(0xFEFB,0xFEFC),
}
ISO, FIN, INI, MED = 0, 1, 2, 3

def _joins_prev(ch):      # can this letter be joined FROM the letter before it?
    return JOIN.get(ch) in ('D', 'R')
def _joins_next(ch):      # does this letter join TO the letter after it?
    return JOIN.get(ch) == 'D'

def shape(text):
    """logical Arabic -> list of (codepoint, source_char) in VISUAL order.

    Two engine escapes are ATOMIC and must survive intact, in order:
      \\x07 + selector  inline colour switch (FUN_00015EB0 case 7: '1' = the orange used
                        for every settings VALUE, '0' = back to the item's own colour)
      \\x08 + letter    button glyph (X/O/S/T/D -- a sprite, not a glyph)
    Reversing a string blindly would put the selector before its escape and turn both into
    stray characters.  Digit and Latin runs are atomic for the same reason in the other
    direction: "10" reversed is "01", which is simply a different number.
    """
    # 0. NOTHING IS HOISTED. A leading escape used to be lifted to the output head so a
    #    button sprite's overhang fell into the margin -- but the output head is the visual
    #    LEFT, which on an RTL line is the END of the sentence, so on a two-prompt line the
    #    first glyph jumped past everything and landed beside the OTHER prompt's word. A
    #    plain reversal is correct: [esc][sp][WORD] -> [WORD][sp][esc] puts the glyph at the
    #    read-first end of its own word with the separating space on its left, which is
    #    where the overhang needs it.
    # 1. fold lam+alef into single ligature characters before form selection
    chars, i = [], 0
    while i < len(text):
        if text[i] in ('\x07', '\x08') and i + 1 < len(text):
            chars.append(('ESC', text[i:i+2])); i += 2
        elif text[i] == '\u0644' and i + 1 < len(text) and text[i+1] in LAM_ALEF:
            chars.append(('LA', text[i+1])); i += 2
        else:
            chars.append((text[i], None)); i += 1
    # 2. contextual form per character
    out = []
    for n, (ch, alef) in enumerate(chars):
        prev = chars[n-1] if n else None
        nxt  = chars[n+1] if n + 1 < len(chars) else None
        # A lam-alef ligature ENDS IN ALEF, so it never joins to what follows -- but it does
        # accept a join from what precedes, because it BEGINS with lam. Treating it as
        # joining onward made the next letter medial/final when it should be initial (the
        # ain in لاعبون); the runtime shaper caught it against this one.
        pj = bool(prev) and _joins_next(prev[0])                        # previous joins onward
        nj = bool(nxt)  and (nxt[0] == 'LA' or _joins_prev(nxt[0]))     # next accepts a join
        if ch == 'ESC':
            out.append(('ESC', alef)); continue
        if ch == 'LA':
            # the ligature behaves like alef: takes a join from the previous letter only
            out.append((LAM_ALEF[alef][1 if pj else 0], 'LA' + alef)); continue
        if ch not in FORMS:
            out.append((ord(ch), ch)); continue          # space, digits, latin, punctuation
        f = FORMS[ch]
        if len(f) == 1:                                   # non-joining (bare hamza): one form
            out.append((f[ISO], ch))                      # even after a letter that joins on
        elif len(f) == 2:                                 # right-joining: iso / final only
            out.append((f[FIN] if pj else f[ISO], ch))
        else:
            idx = (MED if nj else FIN) if pj else (INI if nj else ISO)
            out.append((f[idx], ch))
    # 3. VISUAL order. The engine draws left to right and Arabic reads right to left, so the
    #    token list is reversed -- but a run of digits/Latin (and an escape pair) is one
    #    left-to-right token that keeps its own internal order: "10" must not become "01".
    runs, cur = [], []
    for tok in out:
        cp = tok[0]
        ltr = cp == 'ESC' or (isinstance(cp, int) and (
              48 <= cp <= 57 or 65 <= cp <= 90 or 97 <= cp <= 122))
        if ltr:
            cur.append(tok)
        else:
            if cur: runs.append(cur); cur = []
            runs.append([tok])
    if cur: runs.append(cur)
    runs.reverse()
    # COLOUR IS STATE. \x07+sel recolours everything drawn AFTER it, and the engine strcat's
    # a settings VALUE onto its LABEL before anything is shaped -- so reversing the escape as
    # a token moves the orange onto the label. Record the colour each token had in READING
    # order and re-open it at the boundaries of the reversed stream.
    colour, cur = {}, '0'
    for n, tok in enumerate(out):
        if tok[0] == 'ESC' and tok[1][0] == '\x07':
            cur = tok[1][1]; colour[id(tok)] = None
        else:
            colour[id(tok)] = cur
    flat, emitted, prev = [], '0', None
    for r in runs:
        for t in r:
            c = colour.get(id(t))
            if c is None:
                continue                      # a colour escape: state, not text
            if c != emitted:
                flat.append(('ESC', '\x07' + c)); emitted = c
            # clearance for the sprite's LEFT overhang: two spaces for a full-size
            # (uppercase) icon, one for a half-size one, none when nothing precedes it
            if t[0] == 'ESC' and t[1][0] == '\x08' and prev is not None:
                want = 3 if t[1][1].isupper() else 2
                have = 0
                while have < len(flat) and flat[len(flat) - 1 - have] == (0x20, ' '):
                    have += 1
                for _ in range(max(0, want - have)):
                    flat.append((0x20, ' '))
            flat.append(t)
            prev = t
    return flat

if __name__ == '__main__':
    import sys
    s = sys.argv[1] if len(sys.argv) > 1 else '\u0627\u0636\u063A\u0637 \u0632\u0631 \u0627\u0644\u0628\u062F\u0621'
    for cp, src in shape(s):
        print('U+%04X  %s' % (cp, chr(cp)))
