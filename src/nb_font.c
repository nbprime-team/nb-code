/*
 * nb_font.c -- glyph lookup over the generated Unifont table.
 *
 * Pure data lookup: no drawing (that is nb_text.c) and no file access.  Kept
 * separate so the host tests can check the codepoint arithmetic against the
 * generated tables without an emulator.
 */
#include "nb_font.h"

/* the face itself: port/nbcode_font_data.c (generated), one definition */
extern const nb_font nb_font_table;

/* binary search by codepoint over a sorted nb_latin_glyph table */
static short search(const nb_latin_glyph *tbl, int n, unsigned int cp)
{
	int lo = 0, hi = n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		unsigned int key = tbl[mid].cp;

		if (key == cp)
			return tbl[mid].glyph;
		if (key < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NB_GLYPH_NONE;
}

short nb_font_glyph(unsigned int cp)
{
	const nb_font *f = &nb_font_table;

	if (cp == 0)
		return NB_GLYPH_NONE;
	/* The table starts at U+0020, so ASCII is one subtract and one compare --
	 * the case every line of code takes.  The generated table is CONSECUTIVE
	 * over this range (gen_font.py writes every codepoint it has and upstream
	 * has all of 0x20..0x7E); nb_font_check() asserts that at startup, so a
	 * future table with a hole fails loudly instead of drawing wrong letters. */
	if (cp >= 0x20 && cp <= 0x7E) {
		const nb_latin_glyph *e = &f->latin[cp - 0x20];

		if (e->cp == cp)
			return e->glyph;
	}
	/* the western face first: it owns ASCII, Latin, Greek, Cyrillic and the
	 * punctuation, and the GB2312 face deliberately omits anything the two
	 * share, so a character can never resolve to two different glyphs */
	if (cp < 0x2000 || cp >= 0xFF00) {
		short g = search(f->latin, f->nlatin, cp);

		if (g != NB_GLYPH_NONE)
			return g;
	}
	return search(f->gb_uni, f->ngbuni, cp);
}

short nb_font_gb_glyph(unsigned int hi, unsigned int lo)
{
	const nb_font *f = &nb_font_table;

	if (hi < 0xA1 || hi > 0xFE || lo < 0xA1 || lo > 0xFE)
		return NB_GLYPH_NONE;
	return (short)f->gb_glyph[(hi - 0xA1) * 94 + (lo - 0xA1)];
}

unsigned int nb_font_gb_unicode(unsigned int hi, unsigned int lo)
{
	const nb_font *f = &nb_font_table;

	if (hi < 0xA1 || hi > 0xFE || lo < 0xA1 || lo > 0xFE)
		return 0xFFFD;
	return f->gb_map[(hi - 0xA1) * 94 + (lo - 0xA1)];
}

/* the same binary search as search(), returning the GB2312 SLOT (nb_gb_rev
 * stores the slot in the glyph field: there is exactly one slot per codepoint
 * and the two tables are generated together) */
static int search_slot(unsigned int cp)
{
	const nb_font *f = &nb_font_table;
	int lo = 0, hi = f->ngbrev - 1;

	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		unsigned int key = f->gb_rev[mid].cp;

		if (key == cp)
			return f->gb_rev[mid].glyph;
		if (key < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

int nb_font_gb_pair(unsigned int cp, unsigned int *hi, unsigned int *lo)
{
	int slot = search_slot(cp);

	if (slot < 0)
		return 0;
	*hi = (unsigned int)(slot / 94) + 0xA1;
	*lo = (unsigned int)(slot % 94) + 0xA1;
	return 1;
}

/* ---------------------------------------------------------------------------
 * the face is generated, so these are build-time facts that a wrong
 * regeneration would turn into wrong pixels.  Checked once at startup.
 * ------------------------------------------------------------------------- */
/* The generator writes the table's initializer in struct order.  A field added
 * on one side only would silently shift every pointer by four bytes -- a crash
 * far away from the cause -- so the two places that know the layout are pinned
 * to each other here. */
#define NB_OFF(field)	((long)__builtin_offsetof(nb_font, field))
_Static_assert(NB_OFF(gb_map) == (long)sizeof(nb_font) - 3 * (long)sizeof(void *),
	       "nb_font layout changed: update tools/gen_font.py in the same commit");
_Static_assert(NB_OFF(gb_uni) == NB_OFF(gb_map) - (long)sizeof(void *),
	       "nb_gb_uni and nb_gb_map must stay adjacent in nb_font");

int nb_font_check(void)
{
	const nb_font *f = &nb_font_table;
	int i;

	if (f->magic[0] != 'P' || f->magic[1] != 'C' || f->magic[2] != 'F' ||
	    f->magic[3] != 'N' || f->magic[4] != 'T')
		return 0;
	if (f->cell_h != 16 || f->half_w != 8 || f->cjk_w != 16)
		return 0;
	if (f->nglyphs <= 0 || f->nlatin < 95 || f->ngb != 94 * 94)
		return 0;
	/* ASCII must be consecutive from U+0020 (nb_font_glyph's fast path) */
	for (i = 0; i < 95; i++) {
		if (f->latin[i].cp != 0x20 + i)
			return 0;
	}
	/* every glyph's box must fit its cell and its stored rows must exist */
	for (i = 0; i < f->nglyphs; i++) {
		if ((int)f->glyph_x0[i] + (int)f->glyph_xw[i] > 16)
			return 0;
		if ((int)f->glyph_first[i] + (int)f->glyph_rows[i] > 16)
			return 0;
	}
	return 1;
}

/* ---------------------------------------------------------------------------
 * the one accessor the renderer uses
 *
 * The generated arrays are static in their own translation unit (a 360 KB face
 * belongs in exactly one object file), so this is how nb_text.c reaches a
 * glyph's geometry and its row words without naming those arrays.
 * ------------------------------------------------------------------------- */
int nb_font_lookup(int glyph, int *x0, int *xw, int *first, int *rows,
		   const unsigned short **bits, int *half);

int nb_font_lookup(int glyph, int *x0, int *xw, int *first, int *rows,
		   const unsigned short **bits, int *half)
{
	extern const nb_font nb_font_table;
	extern const unsigned char nb_glyph_x0[];
	extern const unsigned char nb_glyph_xw[];
	extern const unsigned char nb_glyph_first[];
	extern const unsigned char nb_glyph_rows[];
	extern const unsigned char nb_glyph_half[];
	extern const unsigned int nb_glyph_off[];
	extern const unsigned short nb_glyph_bits[];

	if (glyph < 0 || glyph >= nb_font_table.nglyphs)
		return 0;
	*x0 = nb_glyph_x0[glyph];
	*xw = nb_glyph_xw[glyph];
	*first = nb_glyph_first[glyph];
	*rows = nb_glyph_rows[glyph];
	if (half)
		*half = nb_glyph_half[glyph];
	if (bits)
		*bits = nb_glyph_bits + nb_glyph_off[glyph];
	return 1;
}
