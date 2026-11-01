/*
 * nb_ui_overlay.c -- the panels that sit ON TOP of the editor, and the scrollbar.
 *
 * WHY THEY ARE TOGETHER: they all answer one question, "who has the finger?".
 * nb_ui.c routes every touch and every key through nb_ui_overlay_touch() and
 * nb_ui_overlay_key() first, and a panel that answers 1 has EATEN the event, so
 * the editor below cannot also act on it.  A tap that both picks a character out
 * of the table and moves the caret is a bug that is easy to write and hard to
 * see; one place that decides is how it stays fixed.
 *
 *   THE CHARACTER TABLE (shift+A)  ASCII 0x20..0x7E plus the symbols a C file, a
 *   note or a Chinese sentence actually wants, in a grid the user taps.  The
 *   insertion goes through nb_codec_char_to_bytes with the BUFFER's encoding, so
 *   a GB2312 file gets the GB2312 pair and a UTF-8 file gets UTF-8: a table that
 *   always inserted UTF-8 would corrupt every file that is not.
 *
 *   THE HAMBURGER MENU (the icon in the header)  save, save as, file
 *   information, go to line.  Four things that had no key of their own: the
 *   calculator's keyboard has 32 character keys and they are all spoken for.
 *
 *   THE FILE INFORMATION PANEL   the facts about the open file that a user asks
 *   for when a save went somewhere unexpected.
 *
 *   THE SCROLLBAR   drawn OVER the text, see-through (nb_gfx_blend: the panel has
 *   no hardware alpha), and dragged for pixel-level scrolling -- the same
 *   set_scroll_px() the finger drag uses, so the bar and the gesture cannot
 *   disagree about where the window is.
 */
#include "nb_ui.h"
#include "nb_clip.h"
#include "nb_codec.h"
#include "nb_gfx.h"
#include "nb_port.h"

/* ---------------------------------------------------------------------------
 * which panel is up
 * ------------------------------------------------------------------------- */
static int over;

int nb_ui_overlay(void)
{
	return over;
}

static void palette_open(void);		/* the table's own bookkeeping */

void nb_ui_overlay_open(int which)
{
	nb_ui *u = nb_ui_get();

	over = which;
	if (which == NB_OVER_PALETTE)
		palette_open();
	/* the panel gets a few frames of guaranteed redraw when it opens: see the
	 * note on over_repaint in nb_ui.h */
	u->over_repaint = 3;
	u->dirty = 1;
}

void nb_ui_overlay_close(void)
{
	if (over) {
		over = NB_OVER_NONE;
		nb_ui_get()->dirty = 1;
	}
}

/* ---------------------------------------------------------------------------
 * the character table (shift+A)
 *
 * A CURSOR, NOT A TAP: the table is where the symbols a C file wants live, and a
 * finger is not precise enough to pick `《` out of a grid without seeing what it
 * picked -- an immediate insert on touch means a wrong character is in the file
 * before it has been read.  So a tap (or an arrow key) MOVES the cursor, the
 * title row shows the character and its CODEPOINT, and ENTER inserts it and
 * closes the table.
 *
 * THE LIST IS CODEPOINTS, and the first 95 are printable ASCII in order.  The
 * rest is the "common symbols" half: the arithmetic and comparison signs a
 * calculator user expects, the money and the typography of a note, the Greek a
 * formula is written in, the circled numbers, and the CJK punctuation a Chinese
 * sentence needs.  A codepoint the font cannot draw is skipped when the grid is
 * built, so the table never offers a character that would come out as a box.
 * ------------------------------------------------------------------------- */
static const unsigned int palette_extra[] = {
	/* Latin-1: the marks, the money and the arithmetic a note actually uses.
	 * (The accented LETTERS are not here: a calculator keyboard has 32
	 * character keys and the table is for symbols.) */
	0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8,
	0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AE, 0x00AF, 0x00B0, 0x00B1,
	0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9,
	0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00D7, 0x00F7,
	/* Greek: the lower case (physics and maths are written in it) and the
	 * capitals that are not shaped like Latin letters */
	0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8,
	0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0,
	0x03C1, 0x03C2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8,
	0x03C9, 0x0393, 0x0394, 0x0398, 0x039B, 0x039E, 0x03A0, 0x03A3,
	0x03A6, 0x03A8, 0x03A9,
	/* stars and the circled numbers: what a list in a note looks like */
	0x2605, 0x2606, 0x2460, 0x2461, 0x2462, 0x2463, 0x2464, 0x2465,
	0x2466, 0x2467, 0x2468,
	/* CJK punctuation: a Chinese sentence needs these and the keypad has
	 * none of them */
	0x3000, 0x3001, 0x3002, 0x3005, 0x3008, 0x3009, 0x300A, 0x300B,
	0x300C, 0x300D, 0x300E, 0x300F, 0x3010, 0x3011, 0x3014, 0x3015,
	/* ... and the full-width forms of the marks that are otherwise ASCII */
	0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF5E
};

#define PALETTE_CELL	16
#define PALETTE_COLS	19
#define PALETTE_ROWS	12	/* 12 * 16 px fits the body under the title */
#define PALETTE_MAX	(PALETTE_COLS * PALETTE_ROWS)
#define PALETTE_X0	((NB_SCREEN_W - PALETTE_COLS * PALETTE_CELL) / 2)
/* THE GRID HAS TO END AT THE BODY'S BOTTOM EDGE.  Twelve rows of 16 px is 192,
 * and the body is 206 px tall (the header is 18 -- see NB_HEAD_H), so the title
 * row gets 14 and the grid starts there.  The title is DRAWN at +1, in a 16 px
 * cell whose ink starts four rows down: its ink and the grid's first row do not
 * meet, and the last row of characters stays clear of the status bar instead of
 * running two pixels under it (which is where it was until the header grew). */
#define PALETTE_TITLE_Y	(NB_BODY_TOP + 1)	/* the title row */
#define PALETTE_Y0	(NB_BODY_TOP + 14)	/* ... and the grid under it */
#if PALETTE_Y0 + PALETTE_ROWS * PALETTE_CELL > NB_BODY_BOT
#error "the character table no longer fits between the header and the status bar"
#endif

/* the table, built once: ASCII, then the symbols the font can draw.  The order is
 * the display order, so the grid can be indexed directly. */
static unsigned int palette_cp[PALETTE_MAX];
static int palette_n;
static int palette_built;

static void palette_build(void)
{
	unsigned int cp;

	if (palette_built)
		return;
	palette_n = 0;
	for (cp = 0x20; cp < 0x7F && palette_n < PALETTE_MAX; cp++)
		palette_cp[palette_n++] = cp;
	for (cp = 0; cp < sizeof(palette_extra) / sizeof(palette_extra[0]) &&
	     palette_n < PALETTE_MAX; cp++) {
		unsigned int c = palette_extra[cp];

		/* a character the font cannot draw is not offered: it would come
		 * out as a box, and a table of boxes is worse than a shorter table */
		if (nb_font_glyph(c) >= 0)
			palette_cp[palette_n++] = c;
	}
	palette_built = 1;
}

int nb_ui_palette_count(void)
{
	palette_build();
	return palette_n;
}

unsigned int nb_ui_palette_cp(int index)
{
	palette_build();

	if (index < 0 || index >= palette_n)
		return 0;
	return palette_cp[index];
}

/* where a codepoint's cell is: the grid is drawn and hit tested from the same
 * two numbers, which is why they are a function */
static void palette_cell(int index, int *x, int *y)
{
	if (index < 0)
		index = 0;
	*x = PALETTE_X0 + (index % PALETTE_COLS) * PALETTE_CELL;
	*y = PALETTE_Y0 + (index / PALETTE_COLS) * PALETTE_CELL;
}

/* the selection: the cursor and the one character it is on */
int nb_ui_palette_cursor(void)
{
	return nb_ui_get()->pal_cursor;
}

void nb_ui_palette_move(int dx, int dy)
{
	nb_ui *u = nb_ui_get();
	int idx, col, row;

	palette_build();
	if (palette_n <= 0)
		return;
	idx = u->pal_cursor;
	if (idx >= palette_n)
		idx = palette_n - 1;
	col = (idx % PALETTE_COLS) + dx;
	row = (idx / PALETTE_COLS) + dy;
	if (col < 0) {
		if (row > 0) {
			row--;
			col = PALETTE_COLS - 1;
		} else {
			col = 0;
		}
	}
	if (col >= PALETTE_COLS) {
		if (row + 1 < PALETTE_ROWS) {
			row++;
			col = 0;
		} else {
			col = PALETTE_COLS - 1;
		}
	}
	if (row < 0)
		row = 0;
	if (row >= PALETTE_ROWS)
		row = PALETTE_ROWS - 1;
	idx = row * PALETTE_COLS + col;
	if (idx >= palette_n)
		idx = palette_n - 1;
	u->pal_cursor = idx;
	u->dirty = 1;
}

/* ENTER: insert what the cursor is on, and close */
void nb_ui_palette_choose(void)
{
	nb_ui *u = nb_ui_get();
	unsigned int cp = nb_ui_palette_cp(u->pal_cursor);

	nb_ui_overlay_close();
	if (cp)
		nb_ui_editor_insert_cp(cp);
}

/* the cursor may be stale from a previous session (or from a table that shrank
 * when the font changed): clamp it when the table opens */
static void palette_open(void)
{
	nb_ui *u = nb_ui_get();

	palette_build();
	if (u->pal_cursor >= palette_n)
		u->pal_cursor = 0;
}

static void palette_draw(void)
{
	nb_ui *u = nb_ui_get();
	int i;
	char info[24];
	int cp = (int)nb_ui_palette_cp(u->pal_cursor);

	palette_build();
	nb_ui_panel(0, NB_BODY_TOP, NB_SCREEN_W, NB_BODY_BOT - NB_BODY_TOP);
	/* THE TITLE ROW SAYS TWO THINGS AND NOTHING ELSE: what this is, and what
	 * the cursor is on -- the character and its codepoint (the report asked
	 * for exactly that, and no hints) */
	/* THE TITLE USES THE BODY'S TEXT COLOUR, not the header's: head_fg is the
	 * colour that goes with head_bg (a dark bar with white text), and on the
	 * light panel this is drawn on it was invisible.  Every panel in this file
	 * is careful about which pair of theme colours it uses. */
	nb_ui_puts(6, PALETTE_TITLE_Y, "字符表", u->th->text);
	if (cp > 0) {
		char utf8[4];
		int n = nb_codec_encode((unsigned int)cp, utf8, 4);
		int w;

		utf8[n > 0 ? n : 0] = 0;
		static const char *hex = "0123456789ABCDEF";
		char one[2];
		int shift;

		nb_str_copy(info, sizeof(info), "U+", -1);
		one[1] = 0;
		for (shift = 12; shift >= 0; shift -= 4) {
			one[0] = hex[(cp >> shift) & 0xF];
			nb_str_append(info, sizeof(info), one);
		}
		nb_str_append(info, sizeof(info), "  ");
		nb_str_append(info, sizeof(info), utf8);
		w = nb_ui_puts_w(info);
		nb_ui_puts(NB_SCREEN_W - 6 - w, PALETTE_TITLE_Y, info,
			   u->th->text);
	}
	for (i = 0; i < palette_n; i++) {
		char utf8[4];
		int n = nb_codec_encode(palette_cp[i], utf8, 4);
		int x, y, w;
		int sel = (i == u->pal_cursor);

		if (n <= 0)
			continue;
		utf8[n] = 0;
		palette_cell(i, &x, &y);
		if (y + PALETTE_CELL > NB_BODY_BOT)
			break;
		/* the cursor cell is a plain tint: the accent edge belongs to rows
		 * that are a LINE (a menu, a list), where it marks "this is the one
		 * you are on" across the whole width -- inside a grid of characters it
		 * just covers half of the glyph */
		if (sel)
			nb_gfx_fill(x, y, PALETTE_CELL, PALETTE_CELL,
				    u->th->sel_bg);
		w = nb_ui_puts_w(utf8);
		nb_ui_puts(x + (PALETTE_CELL - w) / 2, y, utf8,
			   sel ? u->th->sel_fg : u->th->text);
	}
}

static int symbols_digit(int what, int ch);	/* the digit shortcuts */

/* ---------------------------------------------------------------------------
 * the quick symbol panel (shift+6)
 *
 * The comparison and logic operators are what a keypad cannot type and what every
 * line of code needs, so they get a panel of their own: nine cells, each with a
 * DIGIT SHORTCUT (the number keys are unshifted, so it is one key) and a tap
 * target.  The list follows the LANGUAGE -- `&&` is C, `and` is Lua and Python,
 * and PPL spells its comparison as `>= <=` with `AND OR NOT` for logic -- which is
 * the point: the same key gives what the file in front of the user actually needs.
 *
 * It closes after inserting: this is a quick panel, not a keyboard.  Shift+6 opens
 * it again.
 * ------------------------------------------------------------------------- */
static const char *const sym_c[NB_SYM_CELLS] = {
	">", "<", "=", "==", "!=", ">=", "<=", "&&", "||"
};
static const char *const sym_lua[NB_SYM_CELLS] = {
	"==", "~=", ">=", "<=", "and", "or", "not", "..", "#"
};
static const char *const sym_py[NB_SYM_CELLS] = {
	"==", "!=", ">=", "<=", "and", "or", "not", "**", "//"
};
static const char *const sym_ppl[NB_SYM_CELLS] = {
	":=", "==", "\xe2\x89\xa0", "\xe2\x89\xa5", "AND", "OR", "NOT", "MOD", "\xe2\x86\x92"
};
static const char *const sym_txt[NB_SYM_CELLS] = {
	">", "<", "=", "==", "!=", ">=", "<=", "->", "=>"
};

#define SYM_COLS	3
#define SYM_CELL_W	100
#define SYM_CELL_H	30
#define SYM_X0		10
#define SYM_Y0		(NB_BODY_TOP + 22)

/* THE CELLS ARE NUMBERED LIKE THE CALCULATOR'S KEYPAD.
 *
 * The digit keys on this keyboard are 7 8 9 on the TOP row and 1 2 3 on the
 * bottom one -- not the phone layout that a printed 1..9 grid assumes.  Numbering
 * the cells 1..9 in reading order sent the finger to the wrong cell every time:
 * the top left key is a 7, and it picked the first symbol instead of the seventh.
 * A shortcut belongs where the KEY is; that is what makes it one. */
static const char sym_key[NB_SYM_CELLS] = {
	'7', '8', '9',
	'4', '5', '6',
	'1', '2', '3'
};

/* the cell a digit key picks, or -1 when that key is not one of the nine */
int nb_ui_symbol_cell_of_key(int ch)
{
	int i;

	for (i = 0; i < NB_SYM_CELLS; i++) {
		if (sym_key[i] == ch)
			return i;
	}
	return -1;
}

/* the digit shown in a cell (and the key that picks it) */
int nb_ui_symbol_key(int index)
{
	if (index < 0 || index >= NB_SYM_CELLS)
		return 0;
	return sym_key[index];
}

const char *nb_ui_symbol_token(nb_lang lang, int index)
{
	const char *const *tab = sym_c;

	if (index < 0 || index >= NB_SYM_CELLS)
		return NB_NULL;
	if (lang == NB_LANG_LUA)
		tab = sym_lua;
	else if (lang == NB_LANG_PY)
		tab = sym_py;
	else if (lang == NB_LANG_PPL)
		tab = sym_ppl;
	else if (lang == NB_LANG_TXT)
		tab = sym_txt;
	return tab[index];
}

/* the language the buffer is highlighted as (the panel follows the file) */
static nb_lang symbols_lang(void)
{
	nb_ui *u = nb_ui_get();

	return u->buf ? nb_hl_lang(&u->hl) : NB_LANG_C;
}

void nb_ui_symbols_open(void)
{
	nb_ui_overlay_open(NB_OVER_SYMBOLS);
}

static void symbols_draw(void)
{
	nb_ui *u = nb_ui_get();
	nb_lang lang = symbols_lang();
	int i;

	nb_ui_panel(4, NB_BODY_TOP, NB_SCREEN_W - 8,
		    SYM_Y0 - NB_BODY_TOP + 3 * SYM_CELL_H + 6);
	nb_ui_puts(8, NB_BODY_TOP + 3, nb_lang_name(lang), u->th->muted);
	for (i = 0; i < NB_SYM_CELLS; i++) {
		const char *tok = nb_ui_symbol_token(lang, i);
		int cx = SYM_X0 + (i % SYM_COLS) * SYM_CELL_W;
		int cy = SYM_Y0 + (i / SYM_COLS) * SYM_CELL_H;
		char num[4];

		if (!tok || !tok[0])
			continue;
		nb_gfx_frame(cx, cy, SYM_CELL_W - 6, SYM_CELL_H - 4,
			     u->th->line);
		num[0] = (char)sym_key[i];
		num[1] = 0;
		nb_ui_puts(cx + 6, cy + 7, num, u->th->muted);
		nb_ui_puts(cx + 22, cy + 7, tok, u->th->text);
	}
}

/* a tap on a cell inserts its token */
static int symbols_touch(int action, int x, int y)
{
	int col, row, idx;
	const char *tok;

	if (action != 1)
		return 1;
	col = (x - SYM_X0) / SYM_CELL_W;
	row = (y - SYM_Y0) / SYM_CELL_H;
	if (x < SYM_X0 || y < SYM_Y0 || col < 0 || col >= SYM_COLS ||
	    row < 0 || row >= 3)
		return 1;
	idx = row * SYM_COLS + col;
	if (idx < 0 || idx >= NB_SYM_CELLS)
		return 1;
	tok = nb_ui_symbol_token(symbols_lang(), idx);
	nb_ui_overlay_close();
	if (tok)
		nb_ui_editor_ins(tok);
	return 1;
}

/* the digit keys, while the panel is up: the nine keys pick the nine cells, in
 * the keypad's own layout (see sym_key) */
static int symbols_digit(int what, int ch)
{
	const char *tok;
	int idx;

	if (what != NB_KEY_CHAR)
		return 0;
	idx = nb_ui_symbol_cell_of_key(ch);
	if (idx < 0)
		return 0;
	tok = nb_ui_symbol_token(symbols_lang(), idx);
	nb_ui_overlay_close();
	if (tok)
		nb_ui_editor_ins(tok);
	return 1;
}

/* ---------------------------------------------------------------------------
 * THE GLOBAL CLIPBOARD (shift+MENU)
 *
 * Ten entries, newest first, shared by every file and kept on the disk
 * (nb_clip.h).  This panel is how they come back: tap one to insert it at the
 * caret, DEL to throw it away.
 *
 * IT IS THE ONE PANEL THAT OUTLIVES THE FILE IT WAS OPENED IN, which is the
 * whole point -- copying a function out of one file and into another is the job
 * it exists for.
 * ------------------------------------------------------------------------- */
#define CLIP_Y0		(NB_BODY_TOP + 16)	/* the title row is the first 16 */
#define CLIP_ROW	18			/* 16 px of text and 2 of air */
/* ALL TEN ENTRIES HAVE TO FIT: the body is 206 px, the title takes 16, and
 * 10 * 18 = 180 puts the last row at y = 214, above the status bar at 224.  A
 * header that grows again (see NB_HEAD_H) would silently cut the oldest entry
 * off the panel, so the arithmetic is checked when the image is built. */
#if CLIP_Y0 + NB_CLIP_MAX * CLIP_ROW > NB_BODY_BOT
#error "the clipboard's ten rows no longer fit between the header and the status bar"
#endif

static int clip_sel;

void nb_ui_clip_open(void)
{
	clip_sel = 0;
	nb_ui_overlay_open(NB_OVER_CLIP);
}

/* the largest prefix of `n` bytes that does not end inside a character: cutting a
 * UTF-8 sequence in half draws a box on the calculator */
static int whole_utf8(const char *t, int n)
{
	int i = n, back = 0;

	while (i > 0 && back < 3 && ((unsigned char)t[i - 1] & 0xC0) == 0x80) {
		i--;
		back++;
	}
	if (i == 0)
		return n;
	{
		unsigned char lead = (unsigned char)t[i - 1];
		int need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3
			: (lead >= 0xC0) ? 2 : 1;

		if (1 + back < need)
			return i - 1;
	}
	return n;
}

/* how many rows fit between the title and the status bar */
static int clip_rows(void)
{
	int fit = (NB_BODY_BOT - CLIP_Y0) / CLIP_ROW;

	if (fit > NB_CLIP_MAX)
		fit = NB_CLIP_MAX;
	return fit;
}

/* ONE ROW OF AN ENTRY: the FIRST LINE of it (an entry can be a whole line copied
 * with its newline, or several lines from a selection), with a marker when there
 * is more.  The text is copied into a buffer of its own because the drawing
 * helpers take NUL terminated strings and an entry can hold a newline. */
static void clip_row_text(int i, char *out, int cap)
{
	const char *t = nb_clip_text(i);
	int n = nb_clip_len(i), k = 0, more = 0;

	while (k < n && k < cap - 4) {
		if (t[k] == '\n') {
			more = 1;
			break;
		}
		if (t[k] == '\r' || t[k] == '\t')
			break;
		k++;
	}
	if (k < n)
		more = 1;
	/* never cut half a character: a truncated UTF-8 tail draws as a box */
	k = whole_utf8(t, k);
	nb_memcpy(out, t, (unsigned int)k);
	out[k] = 0;
	if (more)
		nb_str_append(out, cap, " \xe2\x86\xb5");	/* U+21B5, the ↵ in
							 * the help text */
}

static void clip_draw(void)
{
	nb_ui *u = nb_ui_get();
	int count = nb_clip_count();
	int i, fit;

	nb_ui_panel(0, NB_BODY_TOP, NB_SCREEN_W, NB_BODY_BOT - NB_BODY_TOP);
	nb_ui_puts(6, NB_BODY_TOP + 1, "剪贴板", u->th->text);
	{
		const char *hint = "del 删除   ↵ 插入";
		int w = nb_ui_puts_w(hint);

		nb_ui_puts(NB_SCREEN_W - 6 - w, NB_BODY_TOP + 1, hint,
			   u->th->sub_fg);
	}
	if (count == 0) {
		nb_ui_puts(8, CLIP_Y0 + 6, "剪贴板是空的", u->th->text);
		nb_ui_puts(8, CLIP_Y0 + 26, "shift+VIEW 复制选区或整行",
			   u->th->muted);
		return;
	}
	fit = clip_rows();
	for (i = 0; i < count && i < fit; i++) {
		int y = CLIP_Y0 + i * CLIP_ROW;
		char line[NB_CLIP_ENTRY / 2];
		nb_u32 col = (i == clip_sel) ? u->th->sel_fg : u->th->text;

		if (i == clip_sel) {
			nb_gfx_fill(0, y, NB_SCREEN_W, CLIP_ROW, u->th->sel_bg);
			nb_gfx_fill(0, y, 2, CLIP_ROW, u->th->accent);
		}
		clip_row_text(i, line, (int)sizeof(line));
		nb_ui_puts_clip(8, y + 1, line, col, NB_SCREEN_W - 12);
	}
}

/* insert entry `i` at the caret and close: the panel is a picker, not a screen */
static void clip_insert(int i)
{
	int len = nb_clip_len(i);
	const char *t = nb_clip_text(i);

	nb_ui_overlay_close();
	if (len <= 0)
		return;
	if (nb_ui_editor_insert_text(t, len))
		nb_ui_message("已粘贴", 0);
	else
		nb_ui_message("无法粘贴（只读文件？）", 1);
}

static int clip_touch(int action, int x, int y)
{
	int row, count = nb_clip_count();

	if (action != 1)
		return 1;
	if (y < CLIP_Y0)
		return 1;			/* the title row: nothing */
	row = (y - CLIP_Y0) / CLIP_ROW;
	if (row >= 0 && row < count && row < clip_rows()) {
		clip_sel = row;
		clip_insert(row);
	}
	return 1;
}

static int clip_key(int what)
{
	int count = nb_clip_count();

	switch (what) {
	case NB_KEY_UP:
		if (count > 0 && clip_sel > 0)
			clip_sel--;
		return 1;
	case NB_KEY_DOWN:
		if (count > 0 && clip_sel + 1 < count)
			clip_sel++;
		return 1;
	case NB_KEY_ENTER:
		if (count > 0)
			clip_insert(clip_sel);
		return 1;
	case NB_KEY_BACKSPACE:		/* the Del key, as the browser uses it */
	case NB_KEY_DELETE:
		if (count > 0 && nb_clip_delete(clip_sel)) {
			nb_clip_save();	/* THE FILE IS THE POINT: a deletion has
					 * to survive the session */
			if (clip_sel >= nb_clip_count() && clip_sel > 0)
				clip_sel--;
			nb_ui_message("已删除该条", 0);
		}
		return 1;
	default:
		return 0;
	}
}

/* ---------------------------------------------------------------------------
 * the popup menu (the MENU key, and the hamburger in the editor's header)
 *
 * ONE WIDGET FOR ALL THREE SCREENS: each screen hands over its own list of items
 * and action ids, and the menu is navigated with the arrow keys or a finger and
 * fired with ENTER or a tap.  This is where the old F1..F6 actions live now --
 * the calculator has no F-key row, and a menu the user can read is worth more
 * than a key chord they have to remember.
 * ------------------------------------------------------------------------- */
#define MENU_W		150
#define MENU_ROW	20
#define MENU_X		(NB_SCREEN_W - 6 - MENU_W)
#define MENU_Y		(NB_BODY_TOP + 2)

static const char *const *menu_items;
static const int *menu_actions;
static const char *menu_title;
static int menu_n;

void nb_ui_menu_open(const char *title, const char *const *items,
		     const int *actions, int n)
{
	nb_ui *u = nb_ui_get();

	if (n <= 0 || n > 12)
		return;
	menu_title = title;
	menu_items = items;
	menu_actions = actions;
	menu_n = n;
	u->menu_cursor = 0;
	nb_ui_overlay_open(NB_OVER_MENU);
}

int nb_ui_menu_active(void)
{
	return over == NB_OVER_MENU;
}

static int menu_height(void)
{
	return MENU_ROW * menu_n + (menu_title ? 18 : 4);
}

/* the icon in the header: the hit box is the whole right hand end of the bar */
int nb_ui_menu_icon_hit(int x, int y)
{
	return y < NB_BODY_TOP && x >= NB_SCREEN_W - 26;
}

void nb_ui_menu_icon_draw(void)
{
	nb_ui *u = nb_ui_get();
	int i;

	for (i = 0; i < 3; i++)
		nb_gfx_fill(NB_SCREEN_W - 22, 4 + i * 4, 16, 2, u->th->head_fg);
}

static void menu_draw(void)
{
	nb_ui *u = nb_ui_get();
	int h = menu_height();
	int top = MENU_Y + (menu_title ? 18 : 0);
	int i;

	nb_ui_panel(MENU_X, MENU_Y, MENU_W, h);
	if (menu_title)
		nb_ui_puts(MENU_X + 8, MENU_Y + 3, menu_title, u->th->muted);
	for (i = 0; i < menu_n; i++) {
		int y = top + i * MENU_ROW;

		if (i == u->menu_cursor) {
			nb_gfx_fill(MENU_X + 2, y, MENU_W - 4, MENU_ROW,
				    u->th->sel_bg);
			nb_gfx_fill(MENU_X + 2, y, 2, MENU_ROW,
				    u->th->accent);
		}
		nb_ui_puts(MENU_X + 10, y + 2, menu_items[i],
			   i == u->menu_cursor ? u->th->sel_fg : u->th->text);
	}
}

static void menu_choose(int item)
{
	int action = NB_MENU_NONE;

	if (item >= 0 && item < menu_n)
		action = menu_actions[item];
	nb_ui_overlay_close();
	if (action != NB_MENU_NONE)
		nb_ui_menu_action(action);
}

/* the row under a point, or -1 */
static int menu_row_at(int x, int y)
{
	int top = MENU_Y + (menu_title ? 18 : 0);

	if (x < MENU_X || x >= MENU_X + MENU_W || y < top)
		return -1;
	{
		int row = (y - top) / MENU_ROW;

		return (row >= 0 && row < menu_n) ? row : -1;
	}
}

static void menu_key(int what)
{
	nb_ui *u = nb_ui_get();
	int moved = 0;

	switch (what) {
	case NB_KEY_UP:
		u->menu_cursor = (u->menu_cursor + menu_n - 1) % menu_n;
		moved = 1;
		break;
	case NB_KEY_DOWN:
		u->menu_cursor = (u->menu_cursor + 1) % menu_n;
		moved = 1;
		break;
	case NB_KEY_ENTER:
	case NB_KEY_RIGHT:
		menu_choose(u->menu_cursor);
		return;
	default:
		break;
	}
	if (moved)
		u->dirty = 1;
}

/* ---------------------------------------------------------------------------
 * the file information panel
 * ------------------------------------------------------------------------- */
/* THE LINES ARE STATIC, NOT A 512-BYTE FRAME: the editor draws on the Python
 * application's thread stack (nb_app.c has the whole story), and the information
 * panel is the last place that should be spending 576 bytes of it -- it is drawn
 * once per frame while it is open, single-threaded, like everything here. */
static char info_lines[8][64];

static void info_draw(void)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	char (*lines)[64] = info_lines;
	int n = 0, i;
	int h, y;

	if (b) {
		char num[24];

		nb_str_copy(lines[n], sizeof(lines[0]), "文件  ", -1);
		nb_str_append(lines[n], sizeof(lines[0]), b->name);
		n++;
		nb_str_copy(lines[n], sizeof(lines[0]), "路径  ", -1);
		nb_str_append(lines[n], sizeof(lines[0]), b->path);
		n++;
		nb_str_copy(lines[n], sizeof(lines[0]), "语言  ", -1);
		nb_str_append(lines[n], sizeof(lines[0]),
			      nb_lang_name(nb_hl_lang(&u->hl)));
		nb_str_append(lines[n], sizeof(lines[0]), "    编码  ");
		nb_str_append(lines[n], sizeof(lines[0]),
			      b->encoding == NB_ENC_GB2312 ? "GB2312"
			      : b->encoding == NB_ENC_LATIN1 ? "Latin-1"
			      : "UTF-8");
		nb_str_append(lines[n], sizeof(lines[0]),
			      b->readonly ? "    只读" : "");
		n++;
		nb_str_copy(lines[n], sizeof(lines[0]), "行数  ", -1);
		nb_fmt_int(num, sizeof(num), b->nlines);
		nb_str_append(lines[n], sizeof(lines[0]), num);
		nb_str_append(lines[n], sizeof(lines[0]), "    字节  ");
		nb_fmt_int(num, sizeof(num), (int)b->nbytes);
		nb_str_append(lines[n], sizeof(lines[0]), num);
		nb_str_append(lines[n], sizeof(lines[0]),
			      b->modified ? "    有未保存的修改" : "    已保存");
		n++;
	}
	{
		char num[24];

		nb_str_copy(lines[n], sizeof(lines[0]), "光标  ", -1);
		nb_fmt_int(num, sizeof(num), nb_ui_get()->buf ?
			   nb_ui_get()->buf->cur_line + 1 : 0);
		nb_str_append(lines[n], sizeof(lines[0]), num);
		nb_str_append(lines[n], sizeof(lines[0]), "    视图 ");
		nb_fmt_int(num, sizeof(num), nb_ui_scroll_px());
		nb_str_append(lines[n], sizeof(lines[0]), num);
		nb_str_append(lines[n], sizeof(lines[0]), "/");
		nb_fmt_int(num, sizeof(num), nb_ui_scroll_max_px());
		nb_str_append(lines[n], sizeof(lines[0]), num);
		n++;
	}
	h = (n + 2) * 16;
	y = NB_BODY_TOP + 8;
	nb_ui_panel(6, y, NB_SCREEN_W - 12, h);
	nb_ui_puts(12, y + 4, "文件信息", u->th->text);
	nb_ui_puts(NB_SCREEN_W - 12 - nb_ui_puts_w("点按关闭"), y + 4,
		   "点按关闭", u->th->sub_fg);
	for (i = 0; i < n; i++)
		nb_ui_puts_clip(12, y + 24 + i * 16, lines[i], u->th->text,
				NB_SCREEN_W - 24);
}

/* ---------------------------------------------------------------------------
 * the scrollbar
 *
 * See-through on purpose (the text under it stays readable), and its geometry is
 * derived from the SAME scroll number the gestures use: the thumb cannot drift
 * away from the text.
 * ------------------------------------------------------------------------- */
#define BAR_W		4
/* FLUSH WITH THE SCREEN'S EDGE: it was one pixel in, which reads as a mistake
 * rather than as a margin (the user's report), and the file size column in the
 * browser now stops one pixel short of it (nb_ui_browser.c) */
#define BAR_X		(NB_SCREEN_W - BAR_W)
#define BAR_HIT		(NB_SCREEN_W - 14)	/* the touch target is wider
						 * than the pixels: a 4 px bar is
						 * not a finger */

static int thumb_h(void)
{
	int body = nb_ui_editor_body_px();
	int max = nb_ui_scroll_max_px();

	if (max <= 0)
		return body;
	{
		int content = body + max;
		int h = body * body / content;

		return h < 20 ? 20 : h;
	}
}

static int thumb_y(void)
{
	int max = nb_ui_scroll_max_px();
	int body = nb_ui_editor_body_px();
	int h = thumb_h();

	if (max <= 0)
		return NB_BODY_TOP;
	return NB_BODY_TOP + (body - h) * nb_ui_scroll_px() / max;
}

int nb_ui_scrollbar_visible(void)
{
	nb_mode m = nb_ui_get()->mode;

	if (m != NB_MODE_EDITOR && m != NB_MODE_BROWSER &&
	    m != NB_MODE_WORKSPACE)
		return 0;
	return nb_ui_scroll_max_px() > 0;
}

int nb_ui_scrollbar_hit(int x, int y)
{
	return nb_ui_scrollbar_visible() && x >= BAR_HIT && y >= NB_BODY_TOP &&
		y < NB_BODY_BOT;
}

void nb_ui_scrollbar_draw(void)
{
	nb_ui *u = nb_ui_get();

	if (!nb_ui_scrollbar_visible())
		return;
	/* the track: a hint that there is more text, not a frame.  The thumb is the
	 * ACCENT colour (the same one the caret and the selected row's edge use), so
	 * "this is the scroll position" reads at a glance. */
	nb_gfx_blend(BAR_X, NB_BODY_TOP, BAR_W, NB_BODY_BOT - NB_BODY_TOP,
		     u->th->sub_fg, 60);
	nb_gfx_blend(BAR_X, thumb_y(), BAR_W, thumb_h(), u->th->accent, 190);
}

/* put the scroll where a finger at y wants it: the thumb's MIDDLE follows the
 * finger, which is what makes a short drag near either end feel right */
static void scrollbar_to(int y)
{
	int body = nb_ui_editor_body_px();
	int max = nb_ui_scroll_max_px();
	int h = thumb_h();
	int span = body - h;
	int px;

	if (max <= 0 || span <= 0)
		return;
	px = (y - NB_BODY_TOP - h / 2) * max / span;
	nb_ui_scroll_set_px(px);
}

void nb_ui_scrollbar_touch(int action, int x, int y)
{
	nb_ui *u = nb_ui_get();

	(void)x;
	switch (action) {
	case 1:
		u->scrub = 1;
		scrollbar_to(y);
		break;
	case 2:
		if (u->scrub)
			scrollbar_to(y);
		break;
	case 8:
		u->scrub = 0;
		break;
	default:
		break;
	}
	u->dirty = 1;
}

/* ---------------------------------------------------------------------------
 * the routing
 * ------------------------------------------------------------------------- */
void nb_ui_overlay_draw(void)
{
	switch (over) {
	case NB_OVER_PALETTE:
		palette_draw();
		break;
	case NB_OVER_MENU:
		menu_draw();
		break;
	case NB_OVER_INFO:
		info_draw();
		break;
	case NB_OVER_SYMBOLS:
		symbols_draw();
		break;
	case NB_OVER_CLIP:
		clip_draw();
		break;
	default:
		break;
	}
}

/* 1 = the event was eaten.  While a panel is up EVERY touch goes to it: a tap
 * that fell through to the editor would move the caret behind the panel. */
int nb_ui_overlay_touch(int action, int x, int y)
{
	nb_ui *u = nb_ui_get();

	if (!over)
		return 0;
	u->dirty = 1;
	/* A PRESS THAT A PANEL EATS ENDS THE GESTURE: the release usually arrives
	 * after the panel has closed (choosing a menu item closes it), and a
	 * release the editor saw no press for must not become a TAP -- that would
	 * move the caret behind the panel the user was looking at. */
	if (action == 1) {
		/* ... AND THE FINGER IS EATEN UNTIL IT COMES UP: a move after a press
		 * that a panel took must not drag the text behind the panel (the
		 * editor's drag path reads touch_eaten, see nb_ui_editor.c) */
		u->touch_down = 0;
		u->touch_moved = 1;
		u->touch_eaten = 1;
	}
	if (over == NB_OVER_PALETTE) {
		if (action == 1) {
			int col = (x - PALETTE_X0) / PALETTE_CELL;
			int row = (y - PALETTE_Y0) / PALETTE_CELL;
			int idx;

			palette_build();
			/* A TAP ONLY MOVES THE CURSOR: the title row then shows
			 * what was picked, and ENTER inserts it */
			if (x >= PALETTE_X0 && col >= 0 && col < PALETTE_COLS &&
			    y >= PALETTE_Y0 && row >= 0 && row < PALETTE_ROWS) {
				idx = row * PALETTE_COLS + col;
				if (idx >= 0 && idx < palette_n) {
					nb_ui_get()->pal_cursor = idx;
					nb_ui_get()->dirty = 1;
				}
			}
		}
		return 1;
	}
	if (over == NB_OVER_SYMBOLS)
		return symbols_touch(action, x, y);
	if (over == NB_OVER_CLIP)
		return clip_touch(action, x, y);
	if (over == NB_OVER_MENU) {
		if (action == 1) {
			int row = menu_row_at(x, y);

			/* A TAP PICKS AND FIRES, because the row is highlighted
			 * under the finger: there is nothing to confirm */
			if (row >= 0)
				menu_choose(row);
			else
				nb_ui_overlay_close();
		}
		return 1;
	}
	/* the information panel: any tap closes it */
	if (action == 1)
		nb_ui_overlay_close();
	return 1;
}

/* 1 = the key was eaten */
int nb_ui_overlay_key(int what, int ch)
{
	if (!over)
		return 0;
	/* the quick symbol panel: a digit key inserts its cell */
	if (over == NB_OVER_SYMBOLS && symbols_digit(what, ch))
		return 1;
	if (over == NB_OVER_CLIP && clip_key(what))
		return 1;
	if (over == NB_OVER_PALETTE) {
		switch (what) {
		case NB_KEY_LEFT:
			nb_ui_palette_move(-1, 0);
			return 1;
		case NB_KEY_RIGHT:
			nb_ui_palette_move(1, 0);
			return 1;
		case NB_KEY_UP:
			nb_ui_palette_move(0, -1);
			return 1;
		case NB_KEY_DOWN:
			nb_ui_palette_move(0, 1);
			return 1;
		case NB_KEY_ENTER:
			nb_ui_palette_choose();
			return 1;
		default:
			break;
		}
	}
	if (over == NB_OVER_MENU && what == NB_KEY_HELP) {
		nb_ui_overlay_close();
		return 0;			/* HELP still opens the help */
	}
	if (over == NB_OVER_MENU && (what == NB_KEY_UP || what == NB_KEY_DOWN ||
				     what == NB_KEY_ENTER ||
				     what == NB_KEY_RIGHT)) {
		menu_key(what);
		return 1;
	}
	if (what == NB_KEY_ESC || what == NB_KEY_ON || what == NB_KEY_MENU)
		nb_ui_overlay_close();
	return 1;
}
