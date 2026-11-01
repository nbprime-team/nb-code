/*
 * nb_text.c -- text rendering with the generated Unifont face.
 *
 * THE MODEL, IN ONE PARAGRAPH
 * A line of a source file is BYTES (UTF-8, GB2312 or plain ASCII).  nb_text_decode
 * turns it ONCE into a small array of (codepoint, style, byte offset, frame
 * offset, advance) records, and everything afterwards -- drawing, the caret, hit
 * testing for a tap -- uses that array.  The editor therefore never re-derives
 * "where is the cursor on screen" from a byte index, and a Chinese character is
 * one record that occupies two frames, which is exactly what it is.
 *
 * FRAME COLUMNS vs BYTES
 * `frame` is a HALF-CELL column: one western character is 1, one GB2312 character
 * is 2.  Horizontal scrolling and the caret both live on that axis, so "scroll by
 * half a cell" is not a state the editor can reach.
 *
 * ALIGNMENT ON THE Y AXIS
 * Unifont pads every cell to 16 rows; the generated table stores only the rows
 * that carry ink plus the cell-relative row the ink starts on, so a glyph is
 * drawn at y + origin and a lowercase 'a' sits where upstream puts it inside its
 * cell -- which is what makes descenders line up with the line above.
 */
#include "nb_text.h"
#include "nb_gfx.h"
#include "nb_font.h"
#include "nb_codec.h"

/* The generated arrays are static inside port/nbcode_font_data.c (a face this
 * size belongs in one translation unit), so the renderer reaches them THROUGH
 * the font struct rather than by name.  nb_font_lookup() reads one glyph's
 * geometry and its row words in one call. */
int nb_text_line_h(void)
{
	return nb_font_table.line_h;
}

int nb_text_cell_w(void)
{
	return nb_font_table.half_w;
}

int nb_text_rows(void)
{
	return nb_font_table.cjk_w;	/* one line of code's vertical pitch */
}

int nb_text_is_halfwidth(unsigned int cp)
{
	short g;
	int x0, xw, first, rows, half = 1;
	const unsigned short *bits;

	if (cp < 0x80)
		return 1;
	g = nb_font_glyph(cp);
	if (g < 0)
		return 1;
	/* Which Unifont CELL the glyph came from, carried in the table.  Deriving
	 * it from the ink box would be wrong: a CJK full stop has a narrow box
	 * and 'W' has a wide one. */
	if (!nb_font_lookup(g, &x0, &xw, &first, &rows, &bits, &half))
		return 1;
	return half;
}

/* ---------------------------------------------------------------------------
 * glyph drawing
 * ------------------------------------------------------------------------- */
void nb_text_glyph(int x, int y, int glyph, nb_u32 colour)
{
	int x0, xw, first, rows, r, c, half = 1, ox;
	const unsigned short *bits;

	if (!nb_font_lookup(glyph, &x0, &xw, &first, &rows, &bits, &half))
		return;
	if (xw <= 0 || rows <= 0)
		return;			/* a blank glyph (space, NBSP, ...) */
	/* THE ORIGIN IS THE INK BOX, MEASURED INSIDE THE CELL, and it is the same
	 * rule for both widths: glyph_x0 is the inked column inside the source cell
	 * (0..7 in an 8 px cell, 0..15 in a 16 px one), glyph_xw is the inked
	 * width, and the row word carries its ink in the LOW bits ('A' row 0 is
	 * 0x000C: 6 px wide, x0 == 1).
	 *
	 * An earlier version subtracted 8 from the half-width case, believing a
	 * half-width glyph's ink sat in columns 8..15 of the 16 bit row.  It does
	 * not, so every western character was drawn EIGHT PIXELS TO THE LEFT of its
	 * own cell -- on the calculator, text sitting one character width left of
	 * where the cursor and the line numbers put it.  The host tests could not
	 * see it: they render text at x=0, where a negative origin is clipped away
	 * without a trace. */
	ox = x0;
	for (r = 0; r < rows; r++) {
		unsigned int word = bits[r];
		int py = y + first + r;

		if (word == 0)
			continue;
		for (c = 0; c < xw; c++) {
			if (word & (1u << (xw - 1 - c)))
				nb_gfx_pixel(x + ox + c, py, colour);
		}
	}
}

/* ---------------------------------------------------------------------------
 * decoding
 * ------------------------------------------------------------------------- */
static void decode_add(nb_text_decoded *d, unsigned int cp, int style,
		       int byte_off, int frame_off, int width, int advance)
{
	nb_text_char *c;

	if (d->n >= NB_TEXT_MAX_CHARS) {
		d->truncated = 1;
		return;
	}
	c = &d->ch[d->n++];
	c->cp = cp;
	c->style = (unsigned char)style;
	c->pad = 0;
	c->byte_off = (nb_u16)(byte_off > 0xFFFF ? 0xFFFF : byte_off);
	c->frame_off = (nb_u16)(frame_off > 0xFFFF ? 0xFFFF : frame_off);
	c->width = (nb_u16)width;
	c->advance = (nb_u16)advance;
}

void nb_text_decode(nb_text_decoded *d, const char *text, int len,
		    const unsigned char *styles, nb_encoding enc)
{
	int i = 0, frame = 0;

	nb_memset(d, 0, sizeof(*d));
	while (i < len) {
		unsigned char c = (unsigned char)text[i];
		unsigned int cp;
		int n, width, advance, half;
		short glyph;

		if (c == '\t') {
			/* a tab advances to the next multiple of four frames;
			 * the editor's tab_width controls how much a TAB KEY
			 * inserts, this is only how the result displays */
			int pad = 4 - (frame & 3);

			while (pad-- > 0) {
				decode_add(d, ' ', styles ? styles[i] : NB_HS_TEXT,
					   i, frame, 0, 8);
				frame++;
				if (d->truncated)
					break;
			}
			i++;
			continue;
		}
		if (c < 0x80) {
			cp = c;
			n = 1;
		} else if (enc == NB_ENC_GB2312 && c >= 0xA1 && c <= 0xFE &&
			   i + 1 < len && (unsigned char)text[i + 1] >= 0xA1 &&
			   (unsigned char)text[i + 1] <= 0xFE) {
			/* a GB2312 file: two bytes are one full-width character,
			 * and the codepoint is the table's answer (0xFFFD when
			 * the pair is unused, which draws the placeholder) */
			cp = nb_font_gb_unicode(c, (unsigned char)text[i + 1]);
			n = 2;
		} else {
			cp = nb_codec_utf8_decode((const unsigned char *)text + i,
						  &n);
		}

		glyph = nb_font_glyph(cp);
		half = nb_text_is_halfwidth(cp);
		advance = half ? nb_font_table.half_w : nb_font_table.cjk_w;
		if (glyph < 0)
			glyph = nb_font_glyph(0xFFFD);	/* the placeholder box */
		width = 0;
		if (glyph >= 0) {
			int gx0, gxw, gfirst, grows, ghalf;
			const unsigned short *gb;

			if (nb_font_lookup(glyph, &gx0, &gxw, &gfirst, &grows, &gb,
					   &ghalf))
				width = gxw;
		}
		if (width == 0 && cp != ' ')
			width = half ? 8 : 16;	/* an unknown glyph still has a
						 * visible box */
		decode_add(d, cp, styles ? styles[i] : NB_HS_TEXT, i, frame,
			   width, advance);
		frame += half ? 1 : 2;
		i += n;
		if (d->truncated)
			break;
	}
	d->frames = frame;
	d->bytes = i;
}

int nb_text_char_bytes(const nb_text_decoded *d, int i)
{
	if (i < 0 || i >= d->n)
		return 0;
	if (i + 1 < d->n)
		return d->ch[i + 1].byte_off - d->ch[i].byte_off;
	return d->bytes - d->ch[i].byte_off;
}

int nb_text_frame_of(const nb_text_decoded *d, int byte_off)
{
	int i;

	for (i = 0; i < d->n; i++) {
		int nb = nb_text_char_bytes(d, i);

		if (byte_off < d->ch[i].byte_off + nb)
			return d->ch[i].frame_off;
	}
	return d->frames;
}

int nb_text_byte_at(const nb_text_decoded *d, int frame)
{
	int i;

	for (i = 0; i < d->n; i++) {
		int fw = d->ch[i].advance / 8;

		if (frame < d->ch[i].frame_off + fw)
			return d->ch[i].byte_off;
	}
	return d->bytes;
}

/* ---------------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------------- */
void nb_text_draw_decoded(const nb_text_decoded *d, int x, int y, int from_frame,
			  int max_frames, const nb_theme *th,
			  const nb_u32 *class_colour, nb_u32 bg, int draw_bg)
{
	int i;

	(void)th;
	for (i = 0; i < d->n; i++) {
		const nb_text_char *c = &d->ch[i];
		int fw = c->advance / 8;
		int px = x + (c->frame_off - from_frame) * 8;
		nb_u32 col;

		if (c->frame_off + fw <= from_frame)
			continue;
		if (max_frames > 0 && c->frame_off - from_frame >= max_frames)
			break;
		if (px <= -16 || px >= nb_gfx_width())
			continue;
		col = class_colour ? class_colour[c->style] : th->text;
		if (draw_bg)
			nb_gfx_fill(px, y, fw * 8, nb_text_rows(), bg);
		if (c->cp == ' ')
			continue;
		nb_text_glyph(px, y, nb_font_glyph(c->cp) >= 0
				       ? nb_font_glyph(c->cp)
				       : nb_font_glyph(0xFFFD), col);
	}
}

int nb_text_draw(nb_text_decoded *scratch, int x, int y, const char *text,
		 nb_u32 colour, const nb_theme *th)
{
	nb_u32 classes[NB_HS_KIND];

	nb_text_classes(th, classes);
	{
		int i;

		for (i = 0; i < NB_HS_KIND; i++)
			classes[i] = colour;
	}
	nb_text_decode(scratch, text, nb_strlen(text), NB_NULL, NB_ENC_UTF8);
	nb_text_draw_decoded(scratch, x, y, 0, 0, th, classes, 0, 0);
	return scratch->frames * 8;
}

int nb_text_width(nb_text_decoded *scratch, const char *text)
{
	nb_text_decode(scratch, text, nb_strlen(text), NB_NULL, NB_ENC_UTF8);
	return scratch->frames * 8;
}

int nb_text_width_n(const char *text, int len, int max_frames)
{
	nb_text_decoded *d = nb_text_scratch();
	int w;

	nb_text_decode(d, text, len, NB_NULL, NB_ENC_UTF8);
	w = d->frames * 8;
	if (max_frames > 0 && w > max_frames * 8)
		w = max_frames * 8;
	return w;
}

/* ---------------------------------------------------------------------------
 * the shared decode buffer for UI strings
 *
 * Static, not stack: a nb_text_decoded is about 6 KB (512 records of 12 bytes)
 * and the UI's draw path nests several calls deep -- 6 KB of stack per level is
 * how a thread stack overflows on this machine.  The editor's per-line decode
 * owns its own instance in the UI state so a repaint cannot clobber it.
 * ------------------------------------------------------------------------- */
static nb_text_decoded ui_scratch;

nb_text_decoded *nb_text_scratch(void)
{
	return &ui_scratch;
}

nb_u32 nb_text_colour(const nb_theme *th, int style)
{
	switch (style) {
	case NB_HS_KEY:		return th->key;
	case NB_HS_CTRL:	return th->ctrl;
	case NB_HS_CONST:	return th->constant;
	case NB_HS_STRING:	return th->string;
	case NB_HS_OP:		return th->op;
	case NB_HS_LIB:		return th->lib;
	case NB_HS_COMMENT:	return th->comment;
	case NB_HS_PREPROC:	return th->preproc;
	default:		return th->text;
	}
}

void nb_text_classes(const nb_theme *th, nb_u32 *out)
{
	int i;

	for (i = 0; i < NB_HS_KIND; i++)
		out[i] = nb_text_colour(th, i);
}

/* ---------------------------------------------------------------------------
 * the glyph cache (see nb_text.h for why it exists)
 * ------------------------------------------------------------------------- */
void nb_gcache_reset(nb_gcache *c)
{
	nb_memset(c, 0, sizeof(*c));
}

int nb_glyph_put(nb_gcache *c, int glyph, nb_u32 colour)
{
	int i, x0, xw, first, rows, r, col, half = 1, ox;
	const unsigned short *bits;
	nb_u8 *dst;

	if (glyph < 0 || glyph >= nb_font_table.nglyphs)
		return -1;
	if (!nb_font_lookup(glyph, &x0, &xw, &first, &rows, &bits, &half))
		return -1;
	/* the same origin rule as nb_text_glyph(): the ink box is cell relative
	 * (see the comment there) and the packed bitmap starts at column 0, so the
	 * offset is applied once, in nb_glyph_draw() */
	ox = x0;
	/* Already here?  A line of code repeats spaces, digits and letters, and a
	 * 96 entry scan is far cheaper than re-packing a glyph.  The colour is
	 * part of the identity: the same letter in a comment and in a string are
	 * two entries. */
	for (i = 0; i < c->used; i++) {
		if (c->glyph[i] == glyph && c->colour[i] == colour) {
			c->hits++;
			return i;
		}
	}
	if (c->used >= NB_GCACHE_MAX)
		return -1;		/* full: the caller leaves the cell blank
					 * rather than re-packing per pixel */
	i = c->used++;
	c->misses++;
	c->glyph[i] = glyph;
	c->colour[i] = colour;
	c->x0[i] = (short)ox;
	c->xw[i] = (short)xw;
	c->first[i] = (short)first;
	c->rows[i] = (short)rows;
	nb_memset(c->pix[i], 0, NB_GCACHE_BYTES);
	if (xw <= 0 || rows <= 0)
		return i;		/* a blank glyph: nothing to blit */
	dst = c->pix[i];
	for (r = 0; r < rows; r++) {
		unsigned int word = bits[r];

		if (!word)
			continue;
		/* the row starts at the cell's column 0: the ink offset lives in
		 * c->x0 and is applied by nb_glyph_draw(), so a half-width glyph
		 * lands in its 8 pixel cell and a full-width one in its 16 */
		for (col = 0; col < xw; col++) {
			if (word & (1u << (xw - 1 - col)))
				dst[r * NB_GCACHE_PITCH + col] = 1;
		}
	}
	return i;
}

int nb_glyph_slot_w(const nb_gcache *c, int slot)
{
	if (slot < 0 || slot >= c->used)
		return 0;
	return c->xw[slot];
}

/* Blit one cached glyph.  `cell_x` is where the character's CELL starts; the ink
 * offset inside the cell is folded in here, and the cached row always begins at
 * cell column 0 (see nb_glyph_put), so a half-width glyph lands in its 8 pixel
 * cell and a full-width one in its 16. */
void nb_glyph_draw(const nb_gcache *c, int slot, int cell_x, int y)
{
	int r, col, xw, rows, ox;
	const nb_u8 *pix;
	nb_u32 colour;

	if (slot < 0 || slot >= c->used)
		return;
	xw = c->xw[slot];
	rows = c->rows[slot];
	if (xw <= 0 || rows <= 0)
		return;
	colour = c->colour[slot];
	ox = c->x0[slot];		/* the ink's column inside the cell */
	pix = c->pix[slot];
	for (r = 0; r < rows; r++) {
		const nb_u8 *row = pix + r * NB_GCACHE_PITCH;
		int py = y + c->first[slot] + r;

		if (py < 0 || py >= nb_gfx_height())
			continue;
		for (col = 0; col < xw; col++) {
			if (row[col])
				nb_gfx_pixel(cell_x + ox + col, py, colour);
		}
	}
}
