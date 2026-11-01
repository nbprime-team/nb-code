#!/usr/bin/env python3
"""gen_font.py -- turn GNU Unifont into the one bitmap table nbCode draws with.

WHY A TABLE AND NOT A FONT ENGINE
    nbCode has no FreeType, no TTF and no firmware text call: the calculator's own
    font APIs are what reset the machine in the g1fm attempt, and a vector engine
    costs more image than there is heap.  So the glyphs are baked in at build
    time, exactly the way primetcc's fonts and g1fm's GB2312 tables are, and the
    on-device renderer is a bit-blit.

WHAT IS IN THE TABLE
    * WESTERN: printable ASCII, Latin-1 Supplement, Latin Extended A/B, the
      general punctuation / arrows / math / box drawing / geometric ranges a
      code editor actually shows, Greek and Cyrillic.  Only Unifont's 8x16
      HALF-WIDTH cells are kept -- western text advances 8 pixels per character,
      so 40 columns fit the 320 px screen and the glyphs are used exactly as
      rasterized (no scaling, no blur).
    * GB2312: the 94x94 codepage -- 7 445 characters including the 6 763 hanzi of
      level 1 + level 2 -- from Unifont's 16x16 full-width cells.  The GB2312
      byte pair maps straight to a slot in a flat 94*94 table, so decoding a
      GB2312 file is one lookup and no Unicode search.

    Glyphs that upstream stores as a 16x16 cell inside a "western" range (the
    combining marks, some fullwidth forms) are DROPPED rather than squeezed:
    drawing a 16 pixel glyph in an 8 pixel column either overlaps its neighbour
    or half-deletes the character, and a missing glyph draws the placeholder box
    -- visibly wrong instead of subtly wrong.

WHY THE SIZE MATTERS
    The image lives in the calculator's firmware heap (the shellcode loader
    mallocs it), so every KB of table is a KB an open file cannot use.  Two
    things keep it small:
      * only the rows an Unifont cell actually inks are stored (Unifont pads
        every cell to 16 rows, and a quarter of the table would be zeros);
      * each glyph is cropped to its ink bounding box in x as well (a hanzi
        rarely fills all 16 columns), and the renderer gets an explicit x origin.
    Identical bitmaps are interned, so the two faces share their overlap (the
    Greek letters, U+00B0, the arrows ... are in GB2312 as well).

OUTPUT (port/nbcode_font_data.c + port/nbcode_font_data.h)
    nb_font_table            -- magic "NBFNT", cell metrics, pointers to:
      nb_glyph_x0[]/xw[]     -- first inked column and width, in pixels
      nb_glyph_first[]/rows[]-- first stored row and row count
      nb_glyph_bits[]        -- one 16-bit word per stored row, bit 15 = x0
      nb_latin[]             -- sorted {codepoint, glyph} for the western face
      nb_gb_map[94*94]       -- GB2312 pair -> Unicode (0xFFFD = not a char)
      nb_gb_glyph[94*94]     -- GB2312 pair -> glyph (0xFFFF = no glyph)
      nb_gb_rev[NB_FONT_GBREV] -- Unicode -> GB2312 slot, sorted, for ENCODING

Usage:
    tools/gen_font.py [--out port/nbcode_font_data.h]
                      [--hex /usr/share/unifont/unifont.hex] [--no-cjk]
                      [--latin ascii|latin1|full]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(TOP, 'port', 'nbcode_font_data.c')
DEFAULT_HEX = '/usr/share/unifont/unifont.hex'
FALLBACK_HEX = ['/usr/share/unifont/unifont.hex',
                '/usr/share/unifont/unifont_jp.hex',
                '/usr/share/unifont/unifont_all.hex']

# The three western faces, cheapest first.  `full` is the default: a code editor
# shows curly quotes, dashes, arrows and box drawing, and a missing glyph is a
# visible box rather than a silent blank.
LATIN_TIERS = {
    'ascii': [(0x0020, 0x007E, 'printable ASCII')],
    'latin1': [(0x0020, 0x007E, 'printable ASCII'),
               (0x00A0, 0x00FF, 'Latin-1 Supplement')],
    'full': [
        (0x0020, 0x007E, 'printable ASCII'),
        (0x00A0, 0x00FF, 'Latin-1 Supplement'),
        (0x0100, 0x017F, 'Latin Extended-A'),
        (0x0370, 0x03FF, 'Greek'),
        (0x0400, 0x04FF, 'Cyrillic'),
        (0x2000, 0x206F, 'general punctuation'),
        (0x20A0, 0x20CF, 'currency symbols'),
        (0x2100, 0x214F, 'letterlike symbols'),
        (0x2190, 0x21FF, 'arrows'),
        (0x2200, 0x22FF, 'mathematical operators'),
        (0x2500, 0x257F, 'box drawing'),
        (0x25A0, 0x25FF, 'geometric shapes'),
    ],
}


def read_hex(path):
    """{codepoint: (rows, half)} for every glyph in a Unifont .hex file.

    A line is "CODEPOINT:HEX"; 16 bytes = an 8x16 half-width cell, 32 bytes = a
    16x16 full-width cell.  Half-width rows are widened to 16 columns here, with
    the ink in the HIGH byte, so every consumer sees one row type and a
    half-width glyph's pixel offset is simply x0 - 8.
    """
    out = {}
    with open(path, encoding='ascii', errors='ignore') as fh:
        for line in fh:
            if ':' not in line:
                continue
            left, data = line.split(':', 1)
            try:
                cp = int(left.strip(), 16)
            except ValueError:
                continue
            if cp in out:
                continue
            try:
                raw = bytes.fromhex(data.strip())
            except ValueError:
                continue
            if len(raw) == 16:
                out[cp] = ([b << 8 for b in raw], True)
            elif len(raw) == 32:
                out[cp] = ([(raw[i * 2] << 8) | raw[i * 2 + 1]
                            for i in range(16)], False)
    return out


def gb2312_pairs():
    """[(slot, unicode)] for every GB2312 pair, slot = (hi-0xA1)*94 + (lo-0xA1)."""
    out = []
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode('gb2312')
            except Exception:
                continue
            out.append(((hi - 0xA1) * 94 + (lo - 0xA1), ord(ch)))
    return out


def bbox(rows):
    """(x0, xw, first, n): the ink bounding box of a 16x16 cell.

    A blank cell gets (0, 0, 0, 0) -- the renderer draws nothing, which is the
    right answer for the space character and lets an empty glyph cost four zero
    bytes.
    """
    first, last = 16, -1
    left, right = 16, -1
    for i, r in enumerate(rows):
        if r:
            if first == 16:
                first = i
            last = i
            for c in range(16):
                if (r >> (15 - c)) & 1:
                    if c < left:
                        left = c
                    if c > right:
                        right = c
    if last < 0:
        return 0, 0, 0, 0
    return left, right - left + 1, first, last - first + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hex', default=DEFAULT_HEX)
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--header', default=None,
                    help='the generated header (default: --out with .h)')
    ap.add_argument('--latin', default='full', choices=sorted(LATIN_TIERS))
    ap.add_argument('--no-cjk', action='store_true',
                    help='western face only: a small image for bring-up')
    args = ap.parse_args()

    src = args.hex
    if not os.path.exists(src):
        for cand in FALLBACK_HEX:
            if os.path.exists(cand):
                src = cand
                break
    if not os.path.exists(src):
        raise SystemExit('gen_font: no Unifont .hex found (looked for %s)'
                         % ', '.join(FALLBACK_HEX))

    uni = read_hex(src)
    ranges = LATIN_TIERS[args.latin]

    # ---- western face: half-width cells only ----
    wanted = {}
    wide_dropped = 0
    want_count = 0
    for lo, hi, _name in ranges:
        want_count += hi - lo + 1
        for cp in range(lo, hi + 1):
            got = uni.get(cp)
            if got is None:
                continue
            rows, half = got
            if not half:
                wide_dropped += 1
                continue
            wanted[cp] = rows

    # ---- GB2312 codepage ----
    gb = gb2312_pairs()
    gb_uni = [0xFFFD] * (94 * 94)
    for slot, cp in gb:
        gb_uni[slot] = cp
    gb_missing = [] if args.no_cjk else [cp for _s, cp in gb if cp not in uni]

    # ---- lay the glyphs out, sharing identical bitmaps ----
    # A glyph record: (rows, x0, xw, first, nrows, half).  `half` is 1 for a
    # glyph that came from Unifont's 8x16 cell: that flag is what decides the
    # advance (8 px against 16), and it has to survive into the table because a
    # full-width glyph may have a narrow ink box (a CJK comma) and a half-width
    # one may be wide ('W', '@').
    glyphs = []
    index = {}

    def intern(rows, half):
        key = (tuple(rows), half)
        if key in index:
            return index[key]
        x0, xw, first, n = bbox(rows)
        idx = len(glyphs)
        glyphs.append((rows, x0, xw, first, n, 1 if half else 0))
        index[key] = idx
        return idx

    latin = []
    for cp in sorted(wanted):
        latin.append((cp, intern(wanted[cp], True)))
    cp_glyph = dict(latin)

    # A codepoint in BOTH faces (U+00B0, U+2190, the Greek letters ...) must
    # resolve to one glyph, or the two faces would disagree about a character.
    gb_glyph = [0xFFFF] * (94 * 94)
    if not args.no_cjk:
        for slot, cp in gb:
            got = uni.get(cp)
            if got is None:
                continue
            gi = cp_glyph.get(cp)
            if gi is None:
                gi = intern(got[0], got[1])
                cp_glyph[cp] = gi
            gb_glyph[slot] = gi
    gb_rev = sorted((cp, slot) for slot, cp in gb
                    if gb_glyph[slot] != 0xFFFF)

    # The CJK half of the face as a UNICODE lookup: a UTF-8 file full of Chinese
    # has to render without a GB2312 step in between.  Sorted by codepoint, and it
    # deliberately omits every codepoint the western face already covers, so a
    # lookup can prefer the western entry for those and stay consistent (the two
    # faces would otherwise disagree about, say, the Greek letters or U+00B0).
    gb_uni_tbl = []
    seen = set(cp for cp, _gi in latin)
    for _slot, cp in gb:
        if gb_glyph[_slot] == 0xFFFF or cp in seen:
            continue
        seen.add(cp)
        gb_uni_tbl.append((cp, gb_glyph[_slot]))
    gb_uni_tbl.sort()

    # ---- emit ----
    out = []
    w = out.append
    w('/* Generated by tools/gen_font.py from %s -- DO NOT EDIT. */' % src)
    w('/*')
    w(' * glyphs  : %d unique bitmaps' % len(glyphs))
    w(' * western : %d codepoints (%s face)' % (len(latin), args.latin))
    if wide_dropped:
        w(' * dropped : %d wide (16x16) cells inside the western ranges; they'
          % wide_dropped)
        w(' *           draw as the placeholder box, because a 16 px glyph in')
        w(' *           an 8 px column overlaps or clips (tools/gen_font.py)')
    w(' * gb2312  : %d slots mapped, %d with a glyph'
      % (sum(1 for v in gb_uni if v != 0xFFFD),
         sum(1 for g in gb_glyph if g != 0xFFFF)))
    w(' * Rendering: half-width cells are 8 px wide, full-width cells 16; each')
    w(' * glyph stores its ink bounding box and only the rows it uses.')
    w(' */')
    w('#include "nb_font.h"')
    w('')
    w('#define NB_FONT_GLYPHS %d' % len(glyphs))
    w('#define NB_FONT_LATIN  %d' % len(latin))
    w('#define NB_FONT_GBMAP  %d' % (94 * 94))
    w('')

    nglyph_bits = 0
    for g in glyphs:
        nglyph_bits += g[4]

    def emit_bytes(name, pick):
        w('const unsigned char %s[NB_FONT_GLYPHS] = {' % name)
        line = []
        for g in glyphs:
            line.append('%d' % g[pick])
            if len(line) == 24:
                w('  ' + ', '.join(line) + ',')
                line = []
        if line:
            w('  ' + ', '.join(line) + ',')
        w('};')
        w('')

    emit_bytes('nb_glyph_x0', 1)
    emit_bytes('nb_glyph_xw', 2)
    emit_bytes('nb_glyph_first', 3)
    emit_bytes('nb_glyph_rows', 4)
    emit_bytes('nb_glyph_half', 5)

    # The word offset of each glyph's rows inside nb_glyph_bits.  The generator
    # already knows it; without it the renderer would have to add up the row
    # counts of every glyph before the one it wants, on the device, per glyph.
    w('const unsigned int nb_glyph_off[NB_FONT_GLYPHS] = {')
    line, off = [], 0
    for g in glyphs:
        line.append('%d' % off)
        off += g[4]
        if len(line) == 12:
            w('  ' + ', '.join(line) + ',')
            line = []
    if line:
        w('  ' + ', '.join(line) + ',')
    w('};')
    w('')

    w('#define NB_FONT_WORDS %d' % nglyph_bits)
    w('const unsigned short nb_glyph_bits[NB_FONT_WORDS] = {')
    line = []
    for (rows, x0, xw, first, n, half) in glyphs:
        for r in range(first, first + n):
            word = (rows[r] >> (16 - x0 - xw)) & ((1 << xw) - 1)
            line.append('0x%04X' % word)
        if len(line) >= 12:
            w('  ' + ', '.join(line) + ',')
            line = []
    if line:
        w('  ' + ', '.join(line) + ',')
    w('};')
    w('')

    w('const nb_latin_glyph nb_latin[NB_FONT_LATIN] = {')
    for cp, gi in latin:
        w('  { 0x%04X, %d },' % (cp, gi))
    w('};')
    w('')

    w('const unsigned short nb_gb_map[NB_FONT_GBMAP] = {')
    line = []
    for v in gb_uni:
        line.append('0x%04X' % v)
        if len(line) == 16:
            w('  ' + ', '.join(line) + ',')
            line = []
    if line:
        w('  ' + ', '.join(line) + ',')
    w('};')
    w('')

    w('const unsigned short nb_gb_glyph[NB_FONT_GBMAP] = {')
    line = []
    for v in gb_glyph:
        line.append('0x%04X' % v)
        if len(line) == 16:
            w('  ' + ', '.join(line) + ',')
            line = []
    if line:
        w('  ' + ', '.join(line) + ',')
    w('};')
    w('')

    w('#define NB_FONT_GBREV %d' % len(gb_rev))
    w('#define NB_FONT_GBUNI %d' % len(gb_uni_tbl))
    w('const nb_latin_glyph nb_gb_uni[NB_FONT_GBUNI] = {')
    for cp, gi in gb_uni_tbl:
        w('  { 0x%04X, %d },' % (cp, gi))
    w('};')
    w('')
    w('const nb_latin_glyph nb_gb_rev[NB_FONT_GBREV] = {')
    for cp, slot in gb_rev:
        w('  { 0x%04X, %d },' % (cp, slot))
    w('};')
    w('')

    w('const nb_font nb_font_table = {')
    w("  { 'P', 'C', 'F', 'N', 'T', 1, 0, 0 },")
    w('  16, 16, 8, 16,')
    w('  NB_FONT_GLYPHS, NB_FONT_LATIN, NB_FONT_GBMAP, NB_FONT_GBUNI,')
    w('  NB_FONT_GBREV,')
    w('  nb_glyph_x0, nb_glyph_xw, nb_glyph_first, nb_glyph_rows,')
    w('  nb_glyph_half,')
    w('  nb_glyph_off, nb_glyph_bits, nb_latin, nb_gb_uni, nb_gb_map,')
    w('  nb_gb_glyph, nb_gb_rev')
    w('};')

    text = '\n'.join(out) + '\n'
    with open(args.out, 'w', encoding='utf-8') as fh:
        fh.write(text)

    # the header: no data, just the include guard, the sizes and a note.  The
    # generated arrays are static in the .c on purpose -- a face this size
    # belongs in exactly one translation unit.
    hdr = args.header
    if hdr is None:
        hdr = os.path.splitext(args.out)[0] + '.h'
    hlines = []
    hlines.append('/* Generated by tools/gen_font.py -- DO NOT EDIT. */')
    hlines.append('/*')
    hlines.append(' * The bitmap face is compiled once, in the matching .c file')
    hlines.append(' * (%s).  Include this header for the sizes and the font; every' %
                  os.path.basename(args.out))
    hlines.append(' * glyph lookup goes through nb_font.c (nb_font_glyph / nb_font_gb_glyph).')
    hlines.append(' */')
    hlines.append('#ifndef NBCODE_FONT_DATA_H')
    hlines.append('#define NBCODE_FONT_DATA_H')
    hlines.append('')
    hlines.append('#define NB_FONT_GLYPHS %d' % len(glyphs))
    hlines.append('#define NB_FONT_LATIN  %d' % len(latin))
    hlines.append('#define NB_FONT_GBMAP  %d' % (94 * 94))
    hlines.append('#define NB_FONT_GBUNI  %d' % len(gb_uni_tbl))
    hlines.append('#define NB_FONT_GBREV  %d' % len(gb_rev))
    hlines.append('#define NB_FONT_WORDS  %d' % nglyph_bits)
    hlines.append('#define NB_FONT_CELL_H 16')
    hlines.append('#define NB_FONT_HALF_W 8')
    hlines.append('#define NB_FONT_CJK_W  16')
    hlines.append('')
    hlines.append('#endif /* NBCODE_FONT_DATA_H */')
    with open(hdr, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(hlines) + '\n')

    rom = (nglyph_bits * 2                 # bitmap words
           + 4 * len(glyphs)               # x0/xw/first/rows
           + 4 * len(latin)                # nb_latin
           + 2 * (94 * 94) * 2             # gb_map + gb_glyph
           + 4 * len(gb_rev))              # nb_gb_rev
    print('gen_font: %s' % src)
    print('  western : %d codepoints (%s face, %d wanted, %d wide dropped)'
          % (len(latin), args.latin, want_count, wide_dropped))
    print('  gb2312  : %d slots mapped%s'
          % (sum(1 for v in gb_uni if v != 0xFFFD),
             ', %d have no Unifont glyph' % len(gb_missing) if gb_missing
             else ', every one has a glyph'))
    print('  glyphs  : %d unique bitmaps, %.1f rows each, %.1f KB of ROM'
          % (len(glyphs), float(nglyph_bits) / max(1, len(glyphs)),
             rom / 1024.0))
    print('  source  : %.1f KB of .c' % (len(text) / 1024.0))
    print('  -> %s' % args.out)
    print('  -> %s' % hdr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
