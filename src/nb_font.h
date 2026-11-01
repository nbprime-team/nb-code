/*
 * nb_font.h -- the on-device bitmap font: types and lookup, no rendering.
 *
 * The glyph data is generated (tools/gen_font.py -> port/nbcode_font_data.h) and
 * this header is what the renderer and the data file agree on.
 *
 * CELLS
 *     Unifont gives two cell shapes and nbCode keeps both, because that is what
 *     the upstream bitmaps actually are -- nothing is resampled anywhere:
 *
 *       half-width : 8x16, the ink in the LEFT 8 columns of the 16-bit rows
 *       full-width : 16x16, used for every GB2312 character
 *
 *     A half-width glyph therefore occupies one 8 pixel column and a full-width
 *     one two, so a line of mixed text advances 8 or 16 pixels per character.
 *
 * BITMAP STORAGE
 *     Four bytes per glyph: sixteen rows, one byte per row, bit 7 = leftmost
 *     pixel.  A glyph that does not use all sixteen rows carries its origin and
 *     height (nb_glyph_first/nb_glyph_rows) so the blank rows are not stored --
 *     Unifont pads every cell to sixteen rows and a quarter of the table would
 *     otherwise be zeros.
 */
#ifndef NB_FONT_H
#define NB_FONT_H

/* one western codepoint -> glyph index entry (sorted by codepoint) */
typedef struct nb_latin_glyph {
	unsigned short cp;
	short glyph;
} nb_latin_glyph;

/* the whole face, described once (the generated file defines nb_font_table) */
typedef struct nb_font {
	char magic[8];			/* "NBFNT\1\0\0" */
	short cell_h;			/* 16: the height of a full cell */
	short cjk_w;			/* 16: advance of a full-width glyph */
	short half_w;			/*  8: advance of a half-width glyph */
	short line_h;			/* 16: the editor's line spacing */
	int nglyphs;
	int nlatin;
	int ngb;			/* 94*94 GB2312 slots */
	int ngbuni;			/* GB2312 characters as Unicode */
	int ngbrev;
	const unsigned char *glyph_x0;	/* first inked column, cell relative */
	const unsigned char *glyph_xw;	/* inked width, in pixels (0 = blank) */
	const unsigned char *glyph_first;	/* first stored row (0..16) */
	const unsigned char *glyph_rows;	/* stored rows (0..16) */
	const unsigned char *glyph_half;	/* 1 = an 8 px cell, 0 = 16 px */
	const unsigned int *glyph_off;	/* word offset into glyph_bits */
	const unsigned short *glyph_bits;	/* one word per stored row */
	const nb_latin_glyph *latin;		/* sorted by codepoint */
	const nb_latin_glyph *gb_uni;		/* GB2312 characters as Unicode,
						 * sorted: the CJK half of the face */
	const unsigned short *gb_map;		/* 94*94 GB2312 -> Unicode */
	const unsigned short *gb_glyph;		/* 94*94 GB2312 -> glyph/0xFFFF */
	const nb_latin_glyph *gb_rev;		/* sorted by codepoint: -> GB slot */
} nb_font;

#define NB_GLYPH_NONE	((short)-1)

/* the generated face (port/nbcode_font_data.c defines it once) */
extern const nb_font nb_font_table;

/* sanity-check the generated tables (called once at startup; returns 0 and
 * refuses to draw when the face and the renderer disagree) */
int nb_font_check(void);

/* Glyph index for a Unicode codepoint, or NB_GLYPH_NONE.
 *
 * Two tables, because Unifont ships two faces and both are in this one:
 *   * the western face (half-width cells) covers ASCII, Latin, Greek, Cyrillic,
 *     punctuation, arrows and box drawing;
 *   * the GB2312 face (full-width cells) covers the 7 445 Chinese characters and
 *     symbols, keyed by Unicode so a UTF-8 file renders without a GB2312 step in
 *     between.
 * ASCII is a direct table lookup (the first entry is U+0020), everything else is
 * a binary search. */
short nb_font_glyph(unsigned int cp);

/* glyph index for a GB2312 byte pair (hi, lo both 0xA1..0xFE), or NB_GLYPH_NONE.
 * This is the fast path for Chinese source files: one multiply-free index into
 * a flat table, no Unicode step in between. */
short nb_font_gb_glyph(unsigned int hi, unsigned int lo);

/* the GB2312 slot -> Unicode value for a byte pair, or 0xFFFD when the pair is
 * not a GB2312 character.  Used when DECODING a GB2312 file. */
unsigned int nb_font_gb_unicode(unsigned int hi, unsigned int lo);

/* the GB2312 byte pair for a Unicode codepoint, or 0 when the character is not
 * in GB2312.  Used when ENCODING -- it is a search, but only for text that is
 * not pure ASCII, and only once per character written. */
int nb_font_gb_pair(unsigned int cp, unsigned int *hi, unsigned int *lo);

#endif /* NB_FONT_H */
