/*
 * nb_text.h -- text rendering: decode a line once, then draw or hit-test it.
 *
 * See nb_text.c for the model.  Two axes matter and they are different:
 *
 *   BYTE offset   what the editor's cursor columns are (nb_buffer)
 *   FRAME         what the screen is measured in: one western character is 1
 *                 frame (8 px), one GB2312 character is 2 (16 px)
 *
 * nb_text_decode() produces both for every character of a line, so the UI never
 * guesses.
 */
#ifndef NB_TEXT_H
#define NB_TEXT_H

#include "nb_common.h"
#include "nb_highlight.h"
#include "nb_font.h"

#define NB_TEXT_MAX_CHARS	512

typedef struct nb_text_char {
	unsigned int cp;
	unsigned char style;		/* NB_HS_* */
	unsigned char pad;
	nb_u16 byte_off;		/* where the character starts in the line */
	nb_u16 frame_off;		/* its left edge, in 8 px frames */
	nb_u16 width;			/* ink width in pixels (0 = blank) */
	nb_u16 advance;			/* 8 or 16 pixels */
} nb_text_char;

typedef struct nb_text_decoded {
	nb_text_char ch[NB_TEXT_MAX_CHARS];
	int n;
	int frames;			/* total frames the line occupies */
	int bytes;			/* bytes consumed (the line length) */
	int truncated;			/* the line was longer than 512 chars */
} nb_text_decoded;

/* metrics */
int nb_text_line_h(void);
int nb_text_rows(void);			/* pixels a line occupies on screen */
int nb_text_cell_w(void);

/* Decode `text` (len bytes) with optional per-byte styles.
 *
 * `enc` is how the bytes are meant: NB_ENC_GB2312 makes every pair in the GB2312
 * rows one full-width character, anything else is UTF-8.  The caller KNOWS --
 * the codec detected it when the file was opened -- and guessing here was a real
 * bug: in UTF-8, the bytes of U+4E2D are E4 B8 AD, and E4 B8 is a perfectly
 * valid GB2312 pair, so a "try GB2312 first" decoder rendered 中文 as a random
 * CJK glyph plus a placeholder box. */
void nb_text_decode(nb_text_decoded *d, const char *text, int len,
		    const unsigned char *styles, nb_encoding enc);

/* the byte index of the character at frame column `frame`, or the line length
 * when it is past the end (for a tap on the screen) */
int nb_text_byte_at(const nb_text_decoded *d, int frame);
/* the frame column a byte offset falls on (for the caret) */
int nb_text_frame_of(const nb_text_decoded *d, int byte_off);
/* how many bytes the i-th character occupies */
int nb_text_char_bytes(const nb_text_decoded *d, int i);
/* is this codepoint drawn in an 8 px cell? */
int nb_text_is_halfwidth(unsigned int cp);

/* draw a decoded line at (x,y), starting at frame `from_frame`, at most
 * `max_frames` frames wide (0 = no limit).  `class_colour` is the theme's
 * per-class colour table (nb_text_classes). */
void nb_text_draw_decoded(const nb_text_decoded *d, int x, int y, int from_frame,
			  int max_frames, const nb_theme *th,
			  const nb_u32 *class_colour, nb_u32 bg, int draw_bg);

/* one glyph's geometry and row words (implemented in nb_font.c: the arrays live
 * in the generated translation unit) */
int nb_font_lookup(int glyph, int *x0, int *xw, int *first, int *rows,
		   const unsigned short **bits, int *half);

/* one glyph, at its cell-relative origin */
void nb_text_glyph(int x, int y, int glyph, nb_u32 colour);

/* convenience for the UI's own strings (single colour, no scrolling) */
int nb_text_draw(nb_text_decoded *scratch, int x, int y, const char *text,
		 nb_u32 colour, const nb_theme *th);
int nb_text_width(nb_text_decoded *scratch, const char *text);
int nb_text_width_n(const char *text, int len, int max_frames);

/* ---------------------------------------------------------------------------
 * the glyph cache: pixel data for the glyphs a frame actually shows
 *
 * The generated table is 1 bit per pixel and every glyph is looked up twice per
 * frame (once to measure it, once to draw it), which on this CPU means a binary
 * search and a 16-iteration bit loop per character.  The cache turns a glyph into
 * 16 rows of PACKED NIBBLES once (2 pixels per byte, nibble value 0 or 15), so a
 * frame is a byte copy into the framebuffer instead of bit work.
 *
 * 256 bytes per glyph (one byte per pixel, 16x16), so the editor keeps a cache
 * sized to one screen (96 glyphs = 24 KB) and re-points it as the viewport
 * scrolls; a UI glyph (a header label, a status bar character) lives in the same
 * cache.  `colour` is folded in when the glyph is packed, so drawing a line is a
 * row-wise byte copy into the framebuffer with no bit arithmetic.
 * ------------------------------------------------------------------------- */
#define NB_GCACHE_MAX	96
#define NB_GCACHE_PITCH	16		/* bytes per pixel row */
#define NB_GCACHE_BYTES	(NB_GCACHE_PITCH * 16)

typedef struct nb_gcache {
	nb_u8 pix[NB_GCACHE_MAX][NB_GCACHE_BYTES];	/* 1 byte per pixel */
	int glyph[NB_GCACHE_MAX];
	nb_u32 colour[NB_GCACHE_MAX];
	short x0[NB_GCACHE_MAX], xw[NB_GCACHE_MAX];
	short first[NB_GCACHE_MAX], rows[NB_GCACHE_MAX];
	int used, hits, misses;
} nb_gcache;

void nb_gcache_reset(nb_gcache *c);
/* pack one glyph (with the colour burned in); returns its slot, or -1 when the
 * cache is full or the glyph does not exist */
int nb_glyph_put(nb_gcache *c, int glyph, nb_u32 colour);

/* Draw a cached glyph position `slot` of `c` at (x,y) for the cell whose frames
 * start at `cell_x`.  Both the ink bounding box and the cell-relative origin are
 * already folded in, so the caller passes the CELL's screen position. */
void nb_glyph_draw(const nb_gcache *c, int slot, int cell_x, int y);
int nb_glyph_slot_w(const nb_gcache *c, int slot);

/* the shared decode buffer for UI strings (not for the editor's line: that one
 * belongs to the UI state so a repaint cannot clobber a caret position) */
nb_text_decoded *nb_text_scratch(void);

/* theme -> per-class colours */
nb_u32 nb_text_colour(const nb_theme *th, int style);
void nb_text_classes(const nb_theme *th, nb_u32 *out);

#endif /* NB_TEXT_H */
