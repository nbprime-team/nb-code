/*
 * nb_ui_editor.c -- the editor screen: open, draw, edit, save.
 *
 * THE VIEWPORT
 * Screen rows: an 18 px header, twelve code lines at 16 px with a thirteenth
 * peeking in at the bottom, and a 16 px status line.  A line of code therefore
 * starts every 16 pixels and a 40 column western line fills the window exactly.  Both scroll axes are in PIXELS (a drag moves the text with the
 * finger, see "THE VIEWPORT, IN PIXELS" below); a full-width character is two
 * 8 px frames, which is what the drawing and the selection geometry have to
 * respect.  The line number gutter costs 4 columns.
 *
 * WHAT IS HIGHLIGHTED WHEN
 * An edit re-colours from the line it touched, and keeps going while the
 * multi-line state changes (typing `--[[` repaints the rest of the file; typing a
 * letter repaints one line).  nb_highlight.h explains the state cache; this file
 * is the only caller and it is where the policy lives.
 *
 * THE STYLE CACHE
 * One byte per byte of every line, allocated on demand and freed when the file
 * closes: a 2000 line file of 40 column lines costs 80 KB, which is a rounding
 * error next to the file itself, and it means a repaint never re-tokenizes a line
 * that did not change.
 */
#include "nb_ui.h"
#include "nb_clip.h"
#include "nb_find.h"
#include "nb_codec.h"
#include "nb_gfx.h"
#include "nb_port.h"

#define GUTTER_MIN	36	/* 4 digits + a margin, when line numbers are on */
#define GUTTER_MARK	11	/* the fold marker owns x = 3..10 */
/* the bands come from nb_ui.h (NB_BODY_TOP/NB_BODY_BOT): the overlays are drawn
 * from another file and they need the same numbers */
#define BODY_TOP	NB_BODY_TOP
#define BODY_BOT	NB_BODY_BOT
#define LINE_H		16

/* THE BODY IS A WHOLE NUMBER OF ROWS, AND THE STATUS BAR OVERLAPS ITS LAST TWO
 * PIXELS.
 *
 * The screen is 240 px, the header is 18 and the status bar is 16, so the space
 * between them is 206 -- twelve rows and fourteen pixels.  Drawing only twelve
 * rows (and letting the extra row come and go with the pixel scroll) is what made
 * the line above the status bar appear and disappear as the user dragged ("the
 * bottom line sometimes shows and sometimes does not").
 *
 * So the rows go down to the SCREEN's edge: thirteen whole rows, 208 px, from
 * y = 18 to 226 -- and the status bar is painted over the last two pixels of the
 * thirteenth, because nb_ui_status() runs AFTER the text.  Every row has the same
 * visible height at every sub-row offset, which is what "keep it shown and draw
 * the bar over it" asks for. */
#define ROWS_FULL	((BODY_BOT - BODY_TOP) / LINE_H)	/* 12: clear of the bar */
#define ROWS_DRAWN	((NB_SCREEN_H - BODY_TOP) / LINE_H)	/* 13: to the edge */

static int visible_rows(void)
{
	return ROWS_DRAWN;
}

/* THE GUTTER WIDENS WITH THE FILE.  The fold marker lives in its left edge
 * (GUTTER_MARK) and the line number is right aligned six pixels short of the
 * divider, so a FIXED 36 pixel column put the two on top of each other from line
 * 1000 on: the number grew to the left, straight through the marker, and at
 * 10000 it started at a negative x and got clipped by the screen edge as well.
 * 8 px per digit of the number, the marker's 11, and a 6 px margin. */
static int gutter_width(void)
{
	nb_ui *u = nb_ui_get();
	int digits = 1, n, w;

	if (!(u->cfg && !nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOLINENUM)))
		return 2;
	if (u->buf && u->buf->nlines > 0) {
		n = u->buf->nlines;
		while (n >= 10) {
			n /= 10;
			digits++;
		}
	}
	w = GUTTER_MARK + 6 + digits * 8;
	return w < GUTTER_MIN ? GUTTER_MIN : w;
}

static int text_cols(void)
{
	return (NB_SCREEN_W - gutter_width()) / 8;
}

/* ---------------------------------------------------------------------------
 * the style cache
 * ------------------------------------------------------------------------- */
static int styles_reserve(nb_ui *u, int lines)
{
	int i;

	if (u->styles_cap >= lines)
		return 1;
	{
		int cap = u->styles_cap ? u->styles_cap : 64;
		unsigned char **n;
		int *c;

		while (cap < lines)
			cap *= 2;
		n = (unsigned char **)nb_realloc(u->styles,
						  (unsigned int)cap *
						  sizeof(unsigned char *));
		if (!n)
			return 0;
		c = (int *)nb_realloc(u->style_cap,
				      (unsigned int)cap * sizeof(int));
		if (!c)
			return 0;
		for (i = u->styles_cap; i < cap; i++) {
			n[i] = NB_NULL;
			c[i] = 0;		/* no buffer, no room */
		}
		u->styles = n;
		u->style_cap = c;
		u->styles_cap = cap;
	}
	return 1;
}

/* free one line's styles (and say so in the caps array) */
static void styles_drop(nb_ui *u, int line)
{
	if (!u->styles || line < 0 || line >= u->styles_cap)
		return;
	if (u->styles[line]) {
		nb_free(u->styles[line]);
		u->styles[line] = NB_NULL;
	}
	if (u->style_cap)
		u->style_cap[line] = 0;
}

/* ---------------------------------------------------------------------------
 * touch: where is the finger, and what does that mean
 *
 * The view is scrolled by PIXELS on both axes (top_line + yoff vertically, left
 * + xoff horizontally), so a point on the glass maps to a line and a byte column
 * like this.  Two constants decide the gestures: a finger that moves more than
 * TAP_SLOP is dragging (and a drag scrolls), and one that stays put for
 * LONG_PRESS_FRAMES is selecting.
 * ------------------------------------------------------------------------- */
#define TAP_SLOP	5

/* ---------------------------------------------------------------------------
 * THE VIEWPORT, IN PIXELS
 *
 * The scroll position is ONE number per axis: how many pixels of the file are
 * off the top of the body, and how many are off its left edge.  It is STORED as
 * two fields each -- `top_line` (which the buffer owns, and which every line
 * number is drawn from) plus `yoff` for the remainder, `left` plus `xoff` the
 * same way -- but every rule about it is a statement about the pixel number:
 * the window never shows blank space past either end of the file, and the caret
 * is always fully visible.
 *
 * Keeping the two fields in step by hand is what let a drag push the text past
 * the end of the file: the line number stayed clamped while the pixel offset
 * walked away, and after that the caret could sit inside the window's LINE RANGE
 * but outside the glass (the follow logic compares line numbers), so the cursor
 * looked stuck at the bottom of the file and the view kept moving at the top.
 * ------------------------------------------------------------------------- */
/* WHAT THE SCROLL CLAMPS TO is the space between the bars (206 px): that leaves
 * the file's LAST line two pixels clear of the status bar instead of under it.
 * What the RENDERER walks is ROWS_DRAWN (13 whole rows, 208 px), so the row at the
 * bottom edge is painted at every sub-row offset and the bar covers the two pixels
 * it overlaps -- see visible_rows(). */
#define BODY_PX		(BODY_BOT - BODY_TOP)
#define LEFT_MAX	240		/* frames the horizontal drag may reach */

/* ---------------------------------------------------------------------------
 * THE VERTICAL SCROLL IS COUNTED IN VISIBLE ROWS, NOT IN LINE NUMBERS
 *
 * THE BUG THIS FIXES: with a folded block the two are different numbers.  The
 * window used to be positioned by writing a LINE NUMBER into a pixel scroll
 * ("put the caret's line at the top" was `set_scroll_px(line * LINE_H)`), and
 * every hidden line between the top of the file and the caret made that number
 * wrong -- fold a block above the caret and the whole view jumped, because the
 * rows the drawing loop laid out and the pixels the gestures used were counted
 * from two different origins.  The scrollbar was wrong the same way: it promised
 * a position in the UNFOLDED file, so its thumb no longer matched the text.
 *
 * The rules are now stated once, in rows:
 *   * row 0 is the first VISIBLE line of the file;
 *   * `vis_row_of(line)` is that line's row (a hidden line reports the row of
 *     the folded block it is inside of, which is what "the caret is there"
 *     should mean);
 *   * the window's top is always a VISIBLE line, so `scroll_px()` and the
 *     drawing loop agree about which row is at y = BODY_TOP.
 *
 * The counts are O(number of folds), not O(number of lines): a fold is a range,
 * and what matters is how much of it lies above the line being asked about.
 * ------------------------------------------------------------------------- */

/* how many lines of [0, line) a closed fold hides.  A fold's OWN line stays
 * visible: it is what carries the marker. */
static int hidden_before(nb_ui *u, int line)
{
	const nb_fold *f = &u->fold;
	int i, n = 0;

	if (line <= 0)
		return 0;
	for (i = 0; i < f->n; i++) {
		int a = f->line[i] + 1;
		int b = f->end[i];

		if (b >= line)
			b = line - 1;
		if (a <= b)
			n += b - a + 1;
	}
	return n;
}

/* the visible row index of a buffer line, counting from the first line of the
 * file (which is row 0) */
static int vis_row_of(nb_ui *u, int line)
{
	nb_buf *b = u->buf;

	if (!b || line <= 0)
		return 0;
	if (line > b->nlines)
		line = b->nlines;
	if (u->fold.n == 0)
		return line;
	return line - hidden_before(u, line);
}

/* how many rows the file has when the closed folds are taken out */
static int vis_total(nb_ui *u)
{
	nb_buf *b = u->buf;

	if (!b)
		return 0;
	if (u->fold.n == 0)
		return b->nlines;
	return b->nlines - hidden_before(u, b->nlines);
}

/* the line a row belongs to (the last visible line when the row is past the end) */
/* THE INVERSE OF vis_row_of(), IN O(FOLDS) AND NOT O(LINES).
 *
 * This is on the DRAG path (set_scroll_px is called for every touch move), and
 * walking the file line by line while asking `nb_fold_hidden` about each one is
 * O(lines x folds) -- 45,000 fold comparisons per finger move for a 2000 line file
 * with a couple of dozen folds, which on a 100 MHz calculator is exactly the
 * "strange stutter when the code has a lot of folds" a user reported.
 *
 * The folds are kept SORTED (nb_fold_toggle inserts in order), so the mapping is
 * the identity shifted by the hidden lines passed so far, and only the folds
 * themselves have to be walked:
 *
 *     row 0..f.line-hidden      line = row + hidden
 *     the fold's own line       ... and the lines it hides all report that row
 *     after it                  hidden grows by (end - line)
 */
static int line_of_row(nb_ui *u, int row)
{
	const nb_fold *f = &u->fold;
	nb_buf *b = u->buf;
	int i, hidden = 0;

	if (!b || b->nlines <= 0 || row <= 0)
		return 0;
	if (f->n == 0)
		return row < b->nlines ? row : b->nlines - 1;
	for (i = 0; i < f->n; i++) {
		int opener_row = f->line[i] - hidden;

		if (row < opener_row)
			break;			/* before this fold */
		if (row == opener_row)
			return f->line[i] < b->nlines ? f->line[i]
						     : b->nlines - 1;
		hidden += f->end[i] - f->line[i];
	}
	row += hidden;
	return row < b->nlines ? row : b->nlines - 1;
}

/* the first VISIBLE line at or after `line`: the window's top has to be one, or
 * the row it is counted at and the row it is drawn at disagree by the length of
 * the folded block it is inside of */
static int line_visible_from(nb_ui *u, int line)
{
	nb_buf *b = u->buf;

	if (!b || b->nlines <= 0)
		return 0;
	if (line < 0)
		line = 0;
	while (line < b->nlines - 1 && nb_fold_hidden(&u->fold, line))
		line++;
	return line;
}

static int max_scroll_px(void)
{
	int total = vis_total(nb_ui_get()) * LINE_H;

	return total > BODY_PX ? total - BODY_PX : 0;
}

static int scroll_px(void)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;

	if (!b)
		return 0;
	return vis_row_of(u, b->top_line) * LINE_H + u->yoff;
}

static void set_scroll_px(int px)
{
	nb_ui *u = nb_ui_get();
	int max = max_scroll_px();

	if (!u->buf)
		return;
	if (px < 0)
		px = 0;
	else if (px > max)
		px = max;
	u->buf->top_line = line_visible_from(u, line_of_row(u, px / LINE_H));
	u->yoff = px % LINE_H;
}

static int scroll_left_px(void)
{
	nb_ui *u = nb_ui_get();

	return u->left * 8 + u->xoff;
}

/* how far right it is worth scrolling: far enough to bring the end of the
 * longest visible line into view, and no further.  `left_max` is measured by the
 * draw (it decodes every visible line anyway); a drag past it would only uncover
 * blank space, which reads as a bug rather than as a scroll. */
static int left_limit_px(void)
{
	nb_ui *u = nb_ui_get();
	int px = (u->left_max + 1) * 8 - text_cols() * 8;

	return px > 0 ? px : 0;
}

static void set_scroll_left_px(int px)
{
	nb_ui *u = nb_ui_get();
	int limit = left_limit_px();

	if (px < 0)
		px = 0;
	else if (px > limit)
		px = limit;
	if (px > LEFT_MAX * 8)
		px = LEFT_MAX * 8;
	u->left = px / 8;
	u->xoff = px % 8;
}

static void find_draw(void);
static int find_at(int x, int y);
/* the scratch the per-line highlight spans are written into (see the note in
 * nb_ui_editor_draw): never on the stack, because this runs per line per frame */
#define FIND_SPANS	24
static int find_span_off[FIND_SPANS];
static int find_span_len[FIND_SPANS];
/* the copy and paste scratch buffers (static, see the notes at their uses).  THE
 * SIZES ARE MACROS because a `sizeof` on the pointer that names them is 4. */
#define COPY_MAX	(NB_CLIP_ENTRY + 2)
#define PASTE_MAX	(NB_CLIP_ENTRY * 3 + 4)
static int tab_w(nb_ui *u);
static int line_indent_cols(nb_buf *b, int line, int tw);
static int indent_guides_on(nb_ui *u);
static void scroll_into_view(void);
/* a fold makes the file SHORTER in rows: the window may not stay past the new
 * end of it, or the last lines float above a band of blank glass */
static void scroll_clamp(void);
/* ... and the window does NOT move when something is folded (see view_anchor) */
static void view_anchor(void);
static void find_match(nb_ui *u);		/* the bracket pair (nb_ui_editor.c) */
static nb_lang nb_lang_of(nb_ui *u);
static void comp_draw(void);		/* the completion popup */
static int comp_touch(int x, int y);
static int char_frames(nb_ui *u, int col, int *frame);

/* the same numbers, for the overlays: the scrollbar has to be drawn and dragged
 * with EXACTLY the value the gestures use, or the thumb and the text disagree
 * about where the window is (see nb_ui_overlay.c) */
int nb_ui_editor_scroll_px(void)
{
	return scroll_px();
}

int nb_ui_editor_scroll_left_px(void)
{
	return scroll_left_px();
}

void nb_ui_editor_set_scroll_left_px(int px)
{
	set_scroll_left_px(px);
}

void nb_ui_editor_set_scroll_px(int px)
{
	set_scroll_px(px);
}

int nb_ui_editor_max_scroll_px(void)
{
	return max_scroll_px();
}

int nb_ui_editor_body_px(void)
{
	return BODY_PX;
}

/* THE SCREEN ROW OF A BUFFER LINE, counted in VISIBLE lines: folding means a line
 * number is no longer a row, and everything that has to keep the caret on the
 * glass has to ask this.  A negative answer is "that many lines above the
 * window". */
static int row_of_visible(int line)
{
	nb_ui *u = nb_ui_get();

	if (!u->buf)
		return 0;
	/* both ends go through the same row counter, so a fold above the window
	 * cancels out instead of being counted twice */
	return vis_row_of(u, line) - vis_row_of(u, u->buf->top_line);
}

int nb_ui_editor_row_of(int line)
{
	return row_of_visible(line);
}

int nb_ui_editor_gutter_w(void)
{
	return gutter_width();
}

int nb_ui_editor_rows(void)
{
	return visible_rows();
}

/* how many rows the last draw painted (see rows_drawn in nb_ui.h) */
int nb_ui_editor_rows_drawn(void)
{
	return nb_ui_get()->rows_drawn;
}

/* which line is at screen y, given the pixel scroll.  The answer is always a
 * VISIBLE line: pointing at a line inside a folded block would put the caret
 * somewhere the user cannot see. */
static int line_at_y(int y)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int rel, row;

	if (!b)
		return 0;
	if (y < BODY_TOP)
		y = BODY_TOP;
	rel = y - BODY_TOP + u->yoff;
	row = vis_row_of(u, b->top_line) + rel / LINE_H;
	return line_visible_from(u, line_of_row(u, row));
}

/* which byte column of `line` is at screen x */
static int col_at_x(int line, int x)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int frame, gw = gutter_width();

	if (!b || line < 0 || line >= b->nlines)
		return 0;
	if (x < gw)
		return 0;
	/* the text starts at gw - xoff, so a point maps through the PIXEL scroll:
	 * rounding here instead of using xoff is what made a tap land one cell
	 * off after a horizontal drag */
	frame = u->left + (x - gw + u->xoff) / 8;
	if (frame < 0)
		frame = 0;
	nb_text_decode(&u->dec, nb_buf_line(b, line), nb_buf_linelen(b, line),
		       NB_NULL, b->encoding);
	return nb_text_byte_at(&u->dec, frame);
}

/* the selection, with its ends sorted (the anchor may be either one) */
static void selection_span(int *l0, int *c0, int *l1, int *c1)
{
	nb_ui *u = nb_ui_get();

	if (u->sel_l0 < u->sel_l1 ||
	    (u->sel_l0 == u->sel_l1 && u->sel_c0 <= u->sel_c1)) {
		*l0 = u->sel_l0; *c0 = u->sel_c0;
		*l1 = u->sel_l1; *c1 = u->sel_c1;
	} else {
		*l0 = u->sel_l1; *c0 = u->sel_c1;
		*l1 = u->sel_l0; *c1 = u->sel_c0;
	}
}

void nb_ui_selection_clear(void)
{
	nb_ui *u = nb_ui_get();

	if (u->sel_active) {
		u->sel_active = 0;
		u->dirty = 1;
	}
}

int nb_ui_selection_active(void)
{
	return nb_ui_get()->sel_active;
}

/* IS THIS BYTE OF THIS LINE INSIDE THE SELECTION?  The renderer asks, once per
 * drawn line, and the answer is a half-open range on each line: from the anchor to
 * the end of its line, everything on the lines between, and from the start of the
 * last line to its end.
 *
 * HALF OPEN, [c0, c1), because that is exactly what nb_buf_delete_range takes
 * away -- with an inclusive end the highlight showed one character more than the
 * delete removed. */
int nb_ui_selection_on(int line, int byte_off)
{
	nb_ui *u = nb_ui_get();
	int l0, c0, l1, c1;

	if (!u->sel_active)
		return 0;
	selection_span(&l0, &c0, &l1, &c1);
	if (line < l0 || line > l1)
		return 0;
	if (l0 == l1)
		return byte_off >= c0 && byte_off < c1;
	if (line == l0)
		return byte_off >= c0;
	if (line == l1)
		return byte_off < c1;
	return 1;
}

/* delete the selection and leave the cursor where it started */
int nb_ui_selection_delete(void)
{
	nb_ui *u = nb_ui_get();
	int l0, c0, l1, c1, rc;

	if (!u->sel_active)
		return 0;
	selection_span(&l0, &c0, &l1, &c1);
	u->sel_active = 0;
	rc = nb_buf_delete_range(u->buf, l0, c0, l1, c1);
	if (rc == 0)
		return 0;
	if (rc == 2)
		nb_ui_message("已删除（内容过大，无法撤销）", 1);
	nb_ui_editor_changed(u->buf->cur_line);
	return 1;
}

/* ---------------------------------------------------------------------------
 * the gestures
 * ------------------------------------------------------------------------- */

/* A PRESS THIS SCREEN CONSUMED OWNS THE WHOLE GESTURE: the menu icon, the
 * gutter, the completion popup and anything outside the body eat the finger until
 * it comes up.  `touch_down = 0` stops the RELEASE from becoming a tap, and
 * `touch_eaten` stops the MOVES from becoming a drag -- which is the bug a user
 * reported twice ("I tap a line number to fold and the code runs around, left and
 * right most noticeably"): the drag path only asked whether the finger had moved,
 * and the tap itself set that flag, so the finger's own jitter scrolled the view. */
static void touch_eat(nb_ui *u)
{
	u->touch_down = 0;
	u->touch_moved = 1;
	u->touch_eaten = 1;
}

void nb_ui_editor_touch(int action, int x, int y)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;

	if (!b)
		return;
	/* THE SCROLLBAR HAS THE FINGER FIRST: it is drawn over the text, so a press
	 * on it is a press on the bar and not on the line underneath (the hit box is
	 * wider than the 4 px of ink -- a 4 px bar is not a finger) */
	if (u->scrub || (action == 1 && nb_ui_scrollbar_hit(x, y))) {
		nb_ui_scrollbar_touch(action, x, y);
		return;
	}
	/* A TAP ON THE GUTTER FOLDS OR UNFOLDS THE BLOCK AT THAT LINE: the line
	 * numbers are the margin, and a margin is where a fold marker belongs.  It
	 * does not move the caret -- the line the user is working on is none of the
	 * fold's business. */
	if (action == 1 && x < gutter_width() && y >= BODY_TOP && y < BODY_BOT) {
		/* the row under the finger, through the same arithmetic the drawing
		 * and the caret use: with a folded block above, counting lines from
		 * `top_line` put the toggle on the wrong block */
		int line = line_at_y(y);

		if (line < b->nlines &&
		    nb_fold_toggle(&u->fold, b, line, nb_lang_of(u))) {
			/* NOTHING MOVES BUT THE FOLD: not the caret (it is the user's,
			 * and a fold is not a place to go), and not the window (this
			 * used to say `view_line = -1`, i.e. "fit the window to the
			 * caret on the next draw" -- and a caret the user had scrolled
			 * AWAY from, still on line 1 while they read line 1000, then
			 * yanked the whole view back to the top).  A caret that ends
			 * up inside a fold is handled where it matters: the first edit
			 * unfolds it again (nb_ui_editor_changed). */
			view_anchor();
			u->dirty = 1;
		}
		touch_eat(u);
		return;
	}
	/* the completion popup has the finger first */
	if (action == 1 && comp_touch(x, y)) {
		touch_eat(u);
		return;
	}
	/* THE SEARCH BUTTONS ARE IN THE HEADER TOO, and they get the finger before
	 * the header is rejected as "not text" */
	if (action == 1) {
		int fb = find_at(x, y);

		if (fb >= 0) {
			if (fb == 2) {
				nb_find_clear();
				nb_ui_message("已关闭查找", 0);
			} else {
				nb_ui_editor_find_step(fb == 1 ? 1 : -1);
			}
			touch_eat(u);
			return;
		}
	}
	/* THE HAMBURGER IS IN THE HEADER, and the header is the one part of the
	 * screen that is not text, so its press never reaches the caret */
	if (action == 1 && nb_ui_menu_icon_hit(x, y)) {
		/* the same menu the MENU key opens (the items are the editor's) */
		nb_ui_editor_menu();
		touch_eat(u);
		return;
	}
	/* ONLY THE BODY IS TEXT.  A press on the header (a tap that missed the menu
	 * icon) or on the status bar is not a press on a line, and the tap handler
	 * CLAMPS Y into the body -- so it used to put the caret on the first visible
	 * line and the follow then scrolled the window to it.  "Tapping a menu item
	 * moved my code" is this. */
	if (y < BODY_TOP || y >= BODY_BOT) {
		if (action == 1) {
			touch_eat(u);		/* nothing may become a tap or a drag */
			u->dirty = 1;
		}
		return;
	}
	if (action == 1) {			/* press */
		u->touch_down = 1;
		u->touch_moved = 0;
		u->touch_eaten = 0;
		u->touch_frames = 0;
		u->touch_x = u->touch_last_x = x;
		u->touch_y = u->touch_last_y = y;
		/* a tap outside the selection starts a new gesture: the selection
		 * goes, and a long press will make a new one */
		if (u->sel_active &&
		    !nb_ui_selection_on(line_at_y(y), col_at_x(line_at_y(y), x)))
			nb_ui_selection_clear();
		u->dirty = 1;
		return;
	}
	if (action == 2) {			/* move */
		int dx = x - u->touch_last_x, dy = y - u->touch_last_y;

		u->touch_x = x;
		u->touch_y = y;
		if (u->touch_eaten) {
			/* the gesture belongs to a panel, the gutter or the header:
			 * the finger's jitter is NOT a drag of the text */
			u->touch_last_x = x;
			u->touch_last_y = y;
			return;
		}
		if (!u->touch_moved &&
		    (dx * dx + dy * dy) > (TAP_SLOP * TAP_SLOP))
			u->touch_moved = 1;
		if (!u->touch_moved) {
			u->touch_last_x = x;
			u->touch_last_y = y;
			return;
		}
		if (u->sel_active) {
			/* DRAGGING A SELECTION EXTENDS IT: no scrolling while the
			 * user is marking text */
			u->sel_l1 = line_at_y(y);
			u->sel_c1 = col_at_x(u->sel_l1, x);
			u->dirty = 1;
		} else {
			/* DRAGGING SCROLLS, IN PIXELS, IN BOTH DIRECTIONS: the
			 * text follows the finger -- a pixel at a time vertically
			 * instead of a line, and a pixel at a time horizontally
			 * instead of an 8 px cell, so a line of Chinese can be
			 * positioned exactly.  Both are clamped to the content by
			 * set_scroll_px/set_scroll_left_px: dragging at either end
			 * of the file must not walk the text off the glass. */
			set_scroll_px(scroll_px() - dy);
			set_scroll_left_px(scroll_left_px() - dx);
			u->dirty = 1;
		}
		u->touch_last_x = x;
		u->touch_last_y = y;
		return;
	}
	if (action == 8) {			/* release */
		int was_tap = u->touch_down && !u->touch_moved &&
			u->touch_frames < 12;

		u->touch_down = 0;
		u->touch_moved = 0;
		u->touch_eaten = 0;		/* the next gesture starts clean */
		u->touch_frames = 0;
		if (was_tap && !u->sel_active) {
			/* A TAP PUTS THE CARET WHERE THE FINGER IS */
			int line = line_at_y(y);

			nb_buf_goto(b, line, col_at_x(line, x));
			nb_ui_selection_clear();
			scroll_into_view();
			u->dirty = 1;
		}
		return;
	}
}

/* ---------------------------------------------------------------------------
 * AUTO-SCROLL WHILE A SELECTION IS BEING DRAGGED
 *
 * A finger that reaches the edge of the glass -- or LEAVES it: the digitizer
 * keeps reporting the last position it had and the input hook drops the contact
 * once it is marked invalid -- has to keep the text moving, or a selection can
 * never be longer than one screenful.  The caret-follow does not do this (it
 * follows the CARET, and a selection drag does not move the caret), so it is a
 * scroll of its own, driven per frame from the finger's last known position.
 *
 * Two guards, both deliberate: the scroll stops at the ends of the file (like
 * every other scroll here), and it stops if no touch event has arrived for
 * EDGE_STALE frames -- a finger lifted off the glass outside the screen can take
 * its release event with it, and scrolling forever is worse than stopping.
 * ------------------------------------------------------------------------- */
#define EDGE_BAND	12		/* px from the body's edge that scrolls */
#define EDGE_STEP	8		/* px per frame (the loop runs at ~30 Hz) */
#define EDGE_STALE	150		/* frames without an event: give up */

void nb_ui_editor_touch_tick(void)
{
	nb_ui *u = nb_ui_get();
	int edge = 0, hedge = 0, px, moved = 0, y, x;

	if (!u->touch_down || !u->sel_active || u->scrub || u->touch_eaten)
		return;
	if (u->touch_idle > EDGE_STALE)
		return;
	/* BOTH AXES.  The vertical bands were the whole of this, so a finger that
	 * left the screen to the SIDE stopped extending the selection: a selection
	 * could never be longer than one line, which is exactly what a user
	 * reported ("dragging a selection past the left or right edge does not
	 * follow").  The horizontal step is one 8 px column, because that is the
	 * unit the text moves in (see the horizontal scroll). */
	if (u->touch_y <= NB_BODY_TOP + EDGE_BAND)
		edge = -1;
	else if (u->touch_y >= NB_BODY_BOT - EDGE_BAND)
		edge = 1;
	if (u->touch_x <= gutter_width() + EDGE_BAND)
		hedge = -1;
	else if (u->touch_x >= NB_SCREEN_W - EDGE_BAND)
		hedge = 1;
	if (!edge && !hedge)
		return;
	if (edge) {
		px = scroll_px() + edge * EDGE_STEP;
		if (px < 0)
			px = 0;
		if (px > max_scroll_px())
			px = max_scroll_px();
		if (px != scroll_px()) {
			set_scroll_px(px);
			moved = 1;
		}
	}
	if (hedge) {
		px = scroll_left_px() + hedge * 8;
		if (px < 0)
			px = 0;
		if (px > left_limit_px())
			px = left_limit_px();
		if (px != scroll_left_px()) {
			set_scroll_left_px(px);
			moved = 1;
		}
	}
	if (!moved)
		return;			/* already at that end of the file */
	/* the selection end follows the VIEW: the line and the column under the
	 * finger are read again after the scroll (clamped into the body, because the
	 * finger is past its edge by definition here) */
	y = u->touch_y;
	if (y < NB_BODY_TOP)
		y = NB_BODY_TOP;
	if (y >= NB_BODY_BOT)
		y = NB_BODY_BOT - 1;
	x = u->touch_x;
	if (x < gutter_width())
		x = gutter_width();
	if (x >= NB_SCREEN_W)
		x = NB_SCREEN_W - 1;
	u->sel_l1 = line_at_y(y);
	u->sel_c1 = col_at_x(u->sel_l1, x);
	u->dirty = 1;
}

/* the finger has been still for LONG_PRESS_FRAMES: start a selection here */
void nb_ui_editor_long_press(int x, int y)
{
	nb_ui *u = nb_ui_get();

	if (u->mode != NB_MODE_EDITOR || !u->buf || u->touch_eaten)
		return;
	u->sel_l0 = line_at_y(y);
	u->sel_c0 = col_at_x(u->sel_l0, x);
	u->sel_l1 = u->sel_l0;
	u->sel_c1 = u->sel_c0;
	u->sel_active = 1;
	nb_ui_message("已选中：拖动可扩选，MENU 剪切", 0);
	u->dirty = 1;
}

void nb_ui_editor_changed(int line)
{
	nb_ui *u = nb_ui_get();
	int i;

	if (!u->buf)
		return;
	if (line < 0)
		line = 0;
	/* AN EDIT INSIDE A FOLD REVEALS IT.  Folding a block does not move the caret
	 * (a tap on a line number is not a request to go anywhere), so the caret can
	 * be on a line the fold hides -- and then the user would be typing into
	 * something they cannot see.  The first edit unfolds that block, which is
	 * also the moment the user finds out where they were. */
	if (nb_fold_reveal(&u->fold, u->buf->cur_line)) {
		nb_ui_message("已展开：光标在折叠块内", 0);
		u->view_line = -1;		/* fit the window to the caret again */
	}
	if (!styles_reserve(u, u->buf->nlines + 2))
		return;

	/* TWO THINGS ARE INVALIDATED BY AN EDIT, and both were once wrong here:
	 *
	 *   * the style buffers from `line` down -- nb_hl_lines() grows a buffer that
	 *     is too small (caps[] is what it checks), but a buffer below the edit
	 *     may belong to a different line now;
	 *   * the per-line BLOCK STATES below `line` when the LINE NUMBERING changed
	 *     (a newline went in or came out): a state recorded for old line 7 is
	 *     meaningless for new line 7.
	 *
	 * Editing one line in the middle of a file (no newline) keeps the states and
	 * re-colours from there, which is the case that has to stay cheap. */
	for (i = line; i < u->styles_cap; i++)
		styles_drop(u, i);
	{
		int structural = (u->hl_nlines != u->buf->nlines);

		if (structural) {
			u->hl_nlines = u->buf->nlines;
			nb_hl_shift(&u->hl, line);
			/* A NEWLINE MOVES EVERY LINE BELOW IT, and a fold is stored as
			 * a line number: rather than renumber a list of ranges on every
			 * keystroke (and get it wrong once), the folds are dropped.  One
			 * tap brings them back, and hiding the WRONG lines is a bug the
			 * user cannot even see. */
			nb_fold_clear(&u->fold);
		}
		if (u->cfg && !nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOHIGHLIGHT)) {
			/* the early stop is only sound when the TEXT below did not move:
			 * after a newline every line below is a different line, with its
			 * own colours to work out */
			nb_hl_lines(&u->hl, u->buf, u->styles, u->style_cap, line,
				    u->buf->nlines - 1,
				    !structural && line == u->buf->cur_line);
		}
	}
	u->dirty = 1;
}

/* the style byte the editor is holding for (line, byte), or -1: the host tests use
 * this to check that an edit keeps the colours lined up with the text */
int nb_ui_style_at(int line, int byte_off)
{
	nb_ui *u = nb_ui_get();

	if (!u->styles || line < 0 || line >= u->styles_cap || !u->styles[line])
		return -1;
	if (!u->buf || line >= u->buf->nlines || byte_off < 0 ||
	    byte_off >= nb_buf_linelen(u->buf, line))
		return -1;
	return u->styles[line][byte_off];
}

/* how many style bytes the editor REALLY has room for on this line, and the
 * buffer: a line whose text needs more bytes than this cannot be coloured, which
 * is the bug the tests look for */
const unsigned char *nb_ui_style_buf(int line, int *cap)
{
	nb_ui *u = nb_ui_get();

	if (cap)
		*cap = (u->style_cap && line >= 0 && line < u->styles_cap)
			? u->style_cap[line] : 0;
	if (!u->styles || line < 0 || line >= u->styles_cap)
		return NB_NULL;
	return u->styles[line];
}

/* ---------------------------------------------------------------------------
 * open / close / save
 * ------------------------------------------------------------------------- */
/* WHICH LANGUAGE THIS BUFFER IS, AND COLOUR IT AS THAT.
 *
 * setting.json wins when it names one ("language": "c"); otherwise the file name
 * decides.  Called when a file is opened and after a save-as, because a name that
 * changed extension changes the language: "note.c" saved as "note.py" should be
 * coloured as Python from then on, and the status line says which one it is. */
static void editor_relanguage(nb_ui *u)
{
	nb_lang want;

	if (!u->buf)
		return;
	want = (u->cfg && u->cfg->language != NB_LANG_AUTO)
		? (nb_lang)u->cfg->language
		: nb_lang_from_name(u->buf->name);
	if (nb_hl_lang(&u->hl) == want)
		return;
	nb_hl_open(&u->hl, nb_lang_get(want), u->buf);
	u->hl_nlines = u->buf->nlines;
	if (u->cfg && !nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOHIGHLIGHT))
		nb_hl_lines(&u->hl, u->buf, u->styles, u->style_cap, 0,
			    u->buf->nlines - 1, 0);
	nb_ui_message(nb_lang_name(want), 0);
}

/* A FILE THAT IS NOT TEXT GOES TO THE HEX VIEW (nb_hex.h).  `force_text` is the
 * user saying "this IS text": the hex view's menu hands it here, and a Latin-1
 * source file (a real thing) is then edited like any other. */
static int open_file_enc(const char *dir, const char *name, int force_text);

int nb_ui_open_file(const char *dir, const char *name)
{
	return open_file_enc(dir, name, 0);
}

int nb_ui_open_file_text(const char *dir, const char *name)
{
	return open_file_enc(dir, name, 1);
}

static int open_file_enc(const char *dir, const char *name, int force_text)
{
	nb_ui *u = nb_ui_get();
	char path[NB_PATH_MAX];
	const char *err = "";
	char *raw;
	int len = 0;
	int bom = 0;
	nb_encoding enc;
	nb_eol eol = NB_EOL_LF;
	int i;

	nb_str_copy(path, sizeof(path), dir ? dir : "", -1);
	if (nb_strlen(path) < sizeof(path) - 2) {
		int n = nb_strlen(path);

		if (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') {
			path[n] = '\\';
			path[n + 1] = 0;
		}
	}
	nb_str_append(path, sizeof(path), name);

	raw = nb_file_slurp(path, &len, &err);
	if (!raw) {
		nb_ui_message(err && *err ? err : "无法打开", 2);
		return 0;
	}
	if (len > NB_FILE_MAX) {
		nb_free(raw);
		nb_ui_message("文件过大", 2);
		return 0;
	}
	enc = nb_codec_detect(raw, len, &bom);
	/* THE OPENING DECISION, BEFORE A LINE IS BUILT: Latin-1 is the codec's
	 * "everything else" (neither UTF-8 nor GB2312), so it is what a binary, a
	 * compressed blob or a file in some third encoding comes out as.  Drawing
	 * that as mojibake helps nobody; the hex view shows what the bytes ARE, and
	 * its menu offers the editor for the case where the guess was wrong. */
	if (enc == NB_ENC_LATIN1 && !force_text) {
		nb_free(raw);
		if (nb_hex_open(path, name)) {
			u->mode = NB_MODE_HEX;
			u->dirty = 1;
			nb_ui_message("非文本文件：HEX 视图（MENU 可转文本）", 0);
			return 1;
		}
		nb_ui_message("无法打开", 2);
		return 0;
	}
	/* the line ending the file uses is preserved on save */
	for (i = 0; i + 1 < len && i < 4096; i++) {
		if (raw[i] == '\r' && raw[i + 1] == '\n') {
			eol = NB_EOL_CRLF;
			break;
		}
		if (raw[i] == '\n')
			break;
	}

	if (!u->buf) {
		u->buf = nb_buf_new();
		if (!u->buf) {
			nb_free(raw);
			nb_ui_message("内存不足：缓冲区", 2);
			return 0;
		}
	}
	if (!nb_buf_from_raw(u->buf, raw, len, enc, eol)) {
		nb_free(raw);
		nb_ui_message("内存不足：文件", 2);
		return 0;
	}
	nb_free(raw);
	nb_buf_set_name(u->buf, name, path);
	/* the browser follows: F1 from here shows the folder the file is in */
	nb_file_show(dir);
	u->buf->readonly = (u->cfg && nb_cfg_flag(u->cfg, NB_CFG_FLAG_READONLY)) ? 1 : 0;

	/* the highlighter follows the file name, unless setting.json pinned one */
	{
		nb_lang lang = (u->cfg && u->cfg->language != NB_LANG_AUTO)
			? (nb_lang)u->cfg->language : nb_lang_from_name(name);

		/* one call, so the state array survives from file to file (and the
		 * first call after a zeroed nb_ui initialises it) */
			nb_hl_open(&u->hl, nb_lang_get(lang), u->buf);
			u->hl_nlines = u->buf->nlines;
	}
	/* free the previous file's styles */
	for (i = 0; i < u->styles_cap; i++)
		styles_drop(u, i);
	styles_reserve(u, u->buf->nlines + 2);
	if (u->cfg && !nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOHIGHLIGHT))
		nb_hl_lines(&u->hl, u->buf, u->styles, u->style_cap, 0,
			    u->buf->nlines - 1, 0);
	u->mode = NB_MODE_EDITOR;
	u->left = 0;
	u->xoff = 0;
	u->top = 0;
	u->yoff = 0;
	nb_complete_reset(&u->comp);
	nb_fold_clear(&u->fold);	/* a new file, no folds */
	u->view_line = -1;		/* fit the window to the caret on the
					 * first draw of the new file */
	u->sel_active = 0;
	u->touch_down = 0;
	u->touch_eaten = 0;
	u->sel = u->sel;		/* the browser keeps its place for esc */
	u->match_line = u->match_col = -1;
	{
		char msg[48];
		char sz[16];

		nb_str_copy(msg, sizeof(msg), "已打开 ", -1);
		nb_str_append(msg, sizeof(msg), name);
		nb_str_append(msg, sizeof(msg), "  ");
		nb_fmt_size(sz, sizeof(sz), (unsigned int)len);
		nb_str_append(msg, sizeof(msg), sz);
		nb_ui_message(msg, 0);
	}
	u->dirty = 1;
	return 1;
}

void nb_ui_editor_close(void)
{
	nb_ui *u = nb_ui_get();

	u->sel_active = 0;
	u->touch_down = 0;
	u->touch_eaten = 0;
	u->yoff = 0;
	u->xoff = 0;

	if (u->buf) {
		nb_buf_free(u->buf);
		u->buf = NB_NULL;
	}
}

int nb_ui_editor_save(void)
{
	nb_ui *u = nb_ui_get();
	char *raw = NB_NULL;
	int len = 0, lost = 0;
	const char *err = "";

	if (!u->buf || u->buf->readonly) {
		nb_ui_message(u->buf ? "只读" : "无文件", 1);
		return 0;
	}
	if (!nb_buf_to_raw(u->buf, &raw, &len, &lost)) {
		nb_ui_message("内存不足：无法保存", 2);
		return 0;
	}
	if (!nb_file_spit(u->buf->path, raw, len, &err)) {
		nb_free(raw);
		nb_ui_message(err && *err ? err : "写入失败", 2);
		return 0;
	}
	nb_free(raw);
	u->buf->modified = 0;
	{
		char msg[48];
		char sz[16];

		nb_str_copy(msg, sizeof(msg), "已保存 ", -1);
		nb_str_append(msg, sizeof(msg), u->buf->name);
		nb_str_append(msg, sizeof(msg), "  ");
		nb_fmt_size(sz, sizeof(sz), (unsigned int)len);
		nb_str_append(msg, sizeof(msg), sz);
		if (lost > 0) {
			nb_str_append(msg, sizeof(msg), "  ");
			{
				char n[12];

				nb_fmt_int(n, sizeof(n), lost);
				nb_str_append(msg, sizeof(msg), n);
				nb_str_append(msg, sizeof(msg), " lost");
			}
		}
		nb_ui_message(msg, lost > 0 ? 1 : 0);
	}
	u->dirty = 1;
	return 1;
}

/* ---------------------------------------------------------------------------
 * scrolling
 * ------------------------------------------------------------------------- */
static void scroll_clamp(void)
{
	int max;

	if (!nb_ui_get()->buf)
		return;
	max = max_scroll_px();
	if (scroll_px() > max)
		set_scroll_px(max);
}

/* RE-ANCHOR THE WINDOW AFTER A FOLD: keep the line it is showing at the top, make
 * sure that line is a visible one, pull the window back in if the file got shorter
 * than it -- and tell the caret-follow that this view IS the caret's, so the next
 * draw does not fit the window to a caret the user scrolled away from. */
static void view_anchor(void)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;

	if (!b)
		return;
	b->top_line = line_visible_from(u, b->top_line);
	scroll_clamp();
	u->view_line = b->cur_line;
	u->view_col = b->cur_col;
	u->view_nlines = b->nlines;
}

static void scroll_into_view(void)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;

	if (!b)
		return;
	/* THE WINDOW FOLLOWS THE CARET, IT DOES NOT TETHER IT.
	 *
	 * This runs from every draw, and an unconditional "make the caret visible"
	 * makes dragging impossible: the next frame pulls the text straight back
	 * to wherever the caret is, which is why a drag with the caret on the first
	 * line did nothing at all -- and why, after the pixel offset had escaped,
	 * the file looked stuck at the bottom.  The follow is for a caret that has
	 * MOVED (a key, a tap, an edit, a new file); a view the user dragged is
	 * left exactly where they put it. */
	if (u->view_line == b->cur_line && u->view_col == b->cur_col &&
	    u->view_nlines == b->nlines)
		return;
	u->view_line = b->cur_line;
	u->view_col = b->cur_col;
	u->view_nlines = b->nlines;
	/* VERTICAL, IN ROWS OF VISIBLE LINES (with folding, a line number and a row
	 * are two different things): put the caret's line inside the window.
	 *
	 * THE TWO ANSWERS ARE ROW COUNTS, NOT LINE NUMBERS -- this is what used to
	 * make the view jump when something was folded: it searched for a line
	 * number whose ROW was right (a loop that walked `top_line` forward while
	 * the caret stayed below the window), and then handed that line number to a
	 * pixel scroll.  One row of arithmetic instead. */
	{
		int row = row_of_visible(b->cur_line);
		/* ROWS_FULL, not ROWS_DRAWN: the caret is kept clear of the status
		 * bar, which covers the bottom two pixels of the last drawn row */
		int rows = ROWS_FULL;
		int caret_row = vis_row_of(u, b->cur_line);

		if (row < 0 || (row == 0 && u->yoff > 0)) {
			/* the caret's line at the top of the window */
			set_scroll_px(caret_row * LINE_H);
		} else if (row >= rows) {
			/* ... or on the last full row of it */
			set_scroll_px((caret_row - rows + 1) * LINE_H);
		}
	}

	{
		/* the caret's frame column, from the decoded line */
		int caret_frame, lpx, width = NB_SCREEN_W - gutter_width();

		nb_text_decode(&u->dec, nb_buf_line(b, b->cur_line),
			       nb_buf_linelen(b, b->cur_line),
			       (u->styles && b->cur_line < u->styles_cap)
			       ? u->styles[b->cur_line] : NB_NULL,
			       b->encoding);
		caret_frame = nb_text_frame_of(&u->dec, b->cur_col);
		lpx = scroll_left_px();
		if (caret_frame * 8 < lpx)
			lpx = caret_frame * 8;
		else if (caret_frame * 8 + 8 > lpx + width)
			lpx = (caret_frame + 2) * 8 - width;
		set_scroll_left_px(lpx);
	}
}

/* ---------------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------------- */
static void draw_gutter(int row_y, int line)
{
	nb_ui *u = nb_ui_get();
	char num[12];
	nb_text_decoded *d = nb_text_scratch();

	if (!(u->cfg && !nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOLINENUM)))
		return;
	/* THE GUTTER OWNS ITS COLUMN.  It is drawn after the text -- which is
	 * positioned by a pixel offset and can spill under it -- so it fills its own
	 * background before writing the number. */
	nb_gfx_fill(0, row_y, gutter_width(), LINE_H, u->th->gutter_bg);
	/* THE FOLD MARKER, in the gutter's left edge: a small block for a line that
	 * CAN be folded, with a hole in it when it IS folded (the classic [+]/[-]
	 * pair, drawn as pixels so it fits in 7 px).  A tap on the gutter toggles
	 * it -- see nb_ui_editor_touch. */
	if (nb_fold_open_at(&u->fold, line)) {
		nb_gfx_fill(3, row_y + 4, 7, 7, u->th->sub_fg);
		nb_gfx_fill(4, row_y + 7, 5, 1, u->th->gutter_bg);
	} else if (nb_fold_region_end(u->buf, line, nb_lang_of(u)) >= 0) {
		nb_gfx_fill(3, row_y + 4, 7, 1, u->th->sub_fg);
		nb_gfx_fill(3, row_y + 7, 7, 1, u->th->sub_fg);
		nb_gfx_fill(3, row_y + 10, 7, 1, u->th->sub_fg);
	}
	nb_fmt_int(num, sizeof(num), line + 1);
	{
		int w = nb_text_width(d, num);

		nb_text_draw(d, gutter_width() - 6 - w, row_y, num,
			     (line == u->buf->cur_line) ? u->th->text
							: u->th->gutter_fg,
			     u->th);
	}
}

/* THE RIGHT HAND END OF THE STATUS ROW: the file type, then the keyboard layer.
 *
 * A function of its own so it can be READ (the tests ask for the string) instead
 * of measured as pixels -- which is how the labels below are checked to be the
 * keypad's words and the type to be abbreviated.
 *
 * THE FILE TYPE IS ABBREVIATED (py, c, lua, ppl, txt): the row also carries the
 * file size, the caret's position and the keyboard layer, and "Python" is two
 * thirds of the room the message beside it needs.  The full name is still on the
 * information panel (文件信息), where there is room for it. */
int nb_ui_editor_status_label(char *out, int n)
{
	nb_ui *u = nb_ui_get();
	char mod[24];
	int st = nb_keymap_alpha_state();
	int up = nb_keymap_alpha_upper();

	if (!out || n <= 0)
		return 0;
	/* THE KEYBOARD LAYER COMES FIRST, THE FILE TYPE AFTER IT: the layer is what
	 * changes under the user's fingers, and the right hand end of a row is where
	 * the eye lands first (the user's report). */
	nb_str_copy(mod, sizeof(mod), "", -1);
	if (st == NB_ALPHA_ONCE)
		nb_str_append(mod, sizeof(mod), up ? "Alpha" : "alpha");
	else if (st == NB_ALPHA_LOCK)
		nb_str_append(mod, sizeof(mod), up ? "Alock" : "alock");
	else if (nb_keymap_held_shift())
		nb_str_append(mod, sizeof(mod), "shift");
	if (mod[0])
		nb_str_append(mod, sizeof(mod), "  ");
	nb_str_append(mod, sizeof(mod), nb_lang_abbrev(nb_hl_lang(&u->hl)));
	nb_str_copy(out, n, mod, -1);
	return 1;
}

void nb_ui_editor_draw(void)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int rows = visible_rows();
	int gw = gutter_width();
	int i, vi;
	char left[40];
	char header[48];
	if (!b) {
		nb_ui_header("nbCode", NB_NULL);
		nb_ui_status("no file", NB_NULL, u->th->foot_bg);
		return;
	}
	scroll_clamp();		/* a fold may have shortened the file */
	scroll_into_view();
	find_match(u);			/* the bracket pair, if the caret is on one */

	{
		char title[48];

		nb_str_copy(title, sizeof(title), b->name[0] ? b->name : "(new)",
			    -1);
		if (b->modified)
			nb_str_append(title, sizeof(title), " *");
		/* THE HEADER IS DRAWN LAST (below the text loop): with the pixel
		 * scroll a line can be half way off the top of the body, and a
		 * header drawn first would have the code run over it.  The right
		 * hand end of the bar is the hamburger, which is why the language
		 * name is on the status line instead. */
		nb_str_copy(header, sizeof(header), title, -1);
	}

	/* the body */
	nb_gfx_fill(0, BODY_TOP, gw, BODY_BOT - BODY_TOP, u->th->gutter_bg);
	nb_gfx_hline(gw - 1, BODY_TOP, 1, u->th->line);
	nb_gfx_fill(gw, BODY_TOP, NB_SCREEN_W - gw, BODY_BOT - BODY_TOP,
		    u->th->bg);

	if (u->cfg && !nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOHIGHLIGHT)) {
		/* COLOUR EVERY VISIBLE LINE, not just the first one whose block state
		 * stopped changing: `stop_when_stable` means "the LATER lines' colours
		 * are still correct", which is only true if they were coloured before.
		 * A screenful of lines inside one long comment therefore came out with
		 * its first line coloured and the rest in the plain text colour.  The
		 * visible range is a dozen lines; colouring all of them costs nothing
		 * next to drawing them. */
		nb_hl_lines(&u->hl, b, u->styles, u->style_cap, b->top_line,
			    b->top_line + rows + 2, 0);
	}

	/* THE ROWS GO DOWN TO THE SCREEN'S EDGE, NOT TO THE STATUS BAR.  The pixel
	 * scroll (u->yoff) slides the text up, so the row at the bottom edge is
	 * PARTLY visible -- and it used to be dropped entirely whenever its top fell
	 * below the body's edge, which made the line above the status bar come and
	 * go as the user dragged ("the bottom line sometimes shows and sometimes
	 * does not").  It is drawn here and the STATUS BAR IS PAINTED OVER THE
	 * OVERLAP below (nb_ui_status is called after this loop), so what shows is
	 * always the same: every row that has any pixel above the bar.  */
	u->left_max = 0;
	u->rows_drawn = 0;
	/* A SCREEN ROW IS NOT A LINE NUMBER once something is folded: this walks the
	 * BUFFER's lines and places the visible ones on successive rows, which is the
	 * only way a hidden range can look like a single line */
	for (i = 0, vi = b->top_line; vi < b->nlines; vi++) {
		int line = vi;
		int y = BODY_TOP + i * LINE_H - u->yoff;

		if (nb_fold_hidden(&u->fold, line))
			continue;
		/* ROWS_DRAWN + 1: with a pixel offset the row after the last FULL one
		 * is partly on the glass (its top is above the screen's edge), and
		 * stopping at ROWS_DRAWN left that space empty -- "scroll half a line
		 * and the line below the last one does not appear". */
		if (i >= ROWS_DRAWN + 1)
			break;
		i++;
		if (y <= BODY_TOP - LINE_H || y >= BODY_TOP + LINE_H * ROWS_DRAWN)
			continue;
		u->rows_drawn++;
		if (line == b->cur_line) {
			/* the current line gets a tint: the caret is one pixel
			 * wide and this is what makes it findable */
			nb_gfx_fill(gw, y, NB_SCREEN_W - gw, LINE_H,
				    u->th->curline);
		}
		/* THE INDENTATION GUIDES: a hairline at every indentation level of the
		 * line, drawn BEFORE the text so the code sits on top of them.  The
		 * levels come from the line's own leading whitespace (a tab counts as
		 * `tab_width` columns, because that is what the editor inserts for one),
		 * and only the levels the line actually uses are drawn -- a guide to the
		 * right of the text would be a line through the middle of it. */
		if (indent_guides_on(u)) {
			int ind = line_indent_cols(b, line, tab_w(u));
			int lv;

			/* STRICTLY LESS THAN THE LINE'S OWN INDENT: the guide marks the
			 * BLOCK a level belongs to, and a guide at the line's own indent
			 * would be drawn through the first character of its text (the
			 * user's report).  One level of indentation therefore has no
			 * guide at all, which is right: there is nothing to line up
			 * with. */
			for (lv = 1; lv * tab_w(u) < ind; lv++) {
				int col = lv * tab_w(u);


				int gx = gw + (col - u->left) * 8 - u->xoff;

				if (gx < gw || gx >= NB_SCREEN_W)
					continue;
				nb_gfx_fill(gx, y, 1, LINE_H, u->th->line);
			}
		}
		{
			const unsigned char *st =
				(u->styles && line < u->styles_cap)
				? u->styles[line] : NB_NULL;

			nb_text_decode(&u->dec, nb_buf_line(b, line),
				       nb_buf_linelen(b, line), st,
				       b->encoding);
			/* the horizontal drag's limit comes from what is on
			 * screen (see left_limit_px) */
			if (u->dec.frames > u->left_max)
				u->left_max = u->dec.frames;
			/* EVERY SEARCH MATCH GETS A TINT (the user's report: "find
			 * `pr` and give every `pr` a highlight").  It goes under the
			 * selection, so a selected match still reads as selected, and
			 * the spans come from the matcher line by line -- the panel's
			 * count and the highlight therefore cannot disagree. */
			if (nb_find_active()) {
				/* THE SPANS LIVE IN A SCRATCH BUFFER, not on the stack:
				 * this runs for every visible line of every frame, and the
				 * editor is drawing on the Python application's own thread
				 * stack (see nb_app.c) -- two 24 entry arrays in the frame
				 * of the busiest function in the image is 120 bytes of
				 * headroom given away for nothing.  The editor is
				 * single-threaded and this is used and finished inside the
				 * line, so one buffer is enough. */
				int *offs = find_span_off;
				int *lens = find_span_len;
				int nm = nb_find_line(b, line, offs, lens,
						      FIND_SPANS);
				int k;

				for (k = 0; k < nm; k++) {
					int f0 = nb_text_frame_of(&u->dec,
								  offs[k]);
					int f1 = nb_text_frame_of(&u->dec,
								  offs[k] + lens[k]);
					int x0, x1;

					if (f1 <= f0)
						f1 = f0 + 1;
					x0 = gw + (f0 - u->left) * 8 - u->xoff;
					x1 = gw + (f1 - u->left) * 8 - u->xoff;
					if (x0 < gw)
						x0 = gw;
					if (x1 > NB_SCREEN_W)
						x1 = NB_SCREEN_W;
					if (x1 > x0)
						nb_gfx_fill(x0, y, x1 - x0,
							    LINE_H,
							    u->th->find);
				}
			}
			/* THE SELECTION IS DRAWN BEHIND THE TEXT, AND A
			 * CHARACTER IS THE UNIT.  Measuring it in frames times 8
			 * highlighted only the left half of the last Chinese
			 * character: a full-width character is TWO frames wide.
			 * A character counts as selected when any of its bytes
			 * is, so an endpoint inside one still shows whole. */
			if (u->sel_active) {
				int len = nb_buf_linelen(b, line);
				int ci, f0 = -1, f1 = -1;

				for (ci = 0; ci < u->dec.n; ci++) {
					const nb_text_char *c = &u->dec.ch[ci];
					int nb = nb_text_char_bytes(&u->dec, ci);
					int k, hit = 0;

					for (k = 0; k < nb && !hit; k++)
						hit = nb_ui_selection_on(
							line, c->byte_off + k);
					if (!hit)
						continue;
					if (f0 < 0 || c->frame_off < f0)
						f0 = c->frame_off;
					if (c->frame_off + c->advance / 8 > f1)
						f1 = c->frame_off +
							c->advance / 8;
				}
				/* the anchor can sit past the last character
				 * ("select to the end of the line"), and that
				 * has to be shown as well */
				if (nb_ui_selection_on(line, len)) {
					if (f0 < 0)
						f0 = u->dec.frames;
					if (u->dec.frames > f1)
						f1 = u->dec.frames;
				}
				if (f0 >= 0 && f1 > f0) {
					int x0 = gw + (f0 - u->left) * 8 -
						u->xoff;
					int x1 = gw + (f1 - u->left) * 8 -
						u->xoff;

					/* the gutter owns x < gw: the fill is
					 * clipped there (the text below spills
					 * under it on purpose, and the gutter is
					 * painted over the spill below) */
					if (x0 < gw)
						x0 = gw;
					if (x1 > NB_SCREEN_W)
						x1 = NB_SCREEN_W;
					if (x1 > x0)
						nb_gfx_fill(x0, y, x1 - x0,
							    LINE_H,
							    u->th->sel_bg);
				}
			}
			/* THE MATCHING BRACKETS ARE BOXED: the box goes behind the
			 * text (here, with the selection), the glyphs on top of it
			 * below */
			if (u->match_line >= 0) {
				int k;

				for (k = 0; k < 2; k++) {
					int ml = k ? u->match_line : u->brace_line;
					int mc = k ? u->match_col : u->brace_col;
					int f = 0, fw;

					if (ml != line || mc < 0)
						continue;
					fw = char_frames(u, mc, &f);
					if (fw <= 0)
						continue;
					{
						int bx = gw + (f - u->left) * 8 -
							u->xoff;

						if (bx + fw * 8 > gw &&
						    bx < NB_SCREEN_W) {
							if (bx < gw)
								bx = gw;
							nb_gfx_fill(bx, y,
								    fw * 8,
								    LINE_H,
								    u->th->match);
						}
					}
				}
			}
			/* the text, shifted by the PIXEL scroll: the frame grid
			 * starts at gw - xoff, and one frame more than fits is
			 * asked for so the partly visible column on the right is
			 * drawn (nb_gfx clips the rest) */
			nb_text_draw_decoded(&u->dec, gw - u->xoff, y, u->left,
					     text_cols() + 2, u->th, u->classes,
					     0, 0);
			/* a folded line says so where its text ends */
			if (nb_fold_open_at(&u->fold, line)) {
				int fx = gw + (u->dec.frames - u->left) * 8 -
					u->xoff;

				if (fx >= gw - 8 && fx < NB_SCREEN_W - 16)
					nb_ui_puts(fx + 4, y, "...",
						   u->th->muted);
			}
			/* THE MATCHING BRACKETS, ON TOP: their glyphs in the
			 * accent colour, so the pair is told apart from every other
			 * bracket on the line (the box behind them was filled with
			 * the text).
			 *
			 * NOT nb_text_draw_decoded: that draws each character in the
			 * colour its STYLE byte names, which is the whole point of it
			 * -- and exactly what has to be overridden here.  A one
			 * character string through nb_text_draw takes the colour it is
			 * given. */
			if (u->match_line >= 0) {
				int k;

				for (k = 0; k < 2; k++) {
					int ml = k ? u->match_line : u->brace_line;
					int mc = k ? u->match_col : u->brace_col;
					int f = 0, fw, len;

					if (ml != line || mc < 0)
						continue;
					fw = char_frames(u, mc, &f);
					if (fw <= 0)
						continue;
					len = nb_buf_linelen(b, line);
					if (mc < len) {
						char one[2];

						one[0] = nb_buf_line(b, line)[mc];
						one[1] = 0;
						nb_text_draw(nb_text_scratch(),
							     gw + (f - u->left) * 8 -
							     u->xoff, y, one,
							     u->th->accent, u->th);
					}
				}
			}
		}
		/* the caret */
		if (line == b->cur_line) {
			int frame = nb_text_frame_of(&u->dec, b->cur_col);
			int cx = gw + (frame - u->left) * 8 - u->xoff;

			if (cx >= gw - 8 && cx < NB_SCREEN_W)
				nb_gfx_fill(cx, y, 2, LINE_H, u->th->cursor);
		}
		/* THE GUTTER LAST: with the horizontal pixel offset the text can
		 * spill into the gutter column, and draw_gutter fills its own
		 * background, so the line numbers stay readable */
		draw_gutter(y, line);
	}
	/* the completion popup, over the text */
	comp_draw();
	/* the scrollbar, over the text and see-through (nb_ui_overlay.c) */
	nb_ui_scrollbar_draw();
	/* AND THE HEADER AFTER THE BODY: the top line is drawn even when it is
	 * half scrolled out (that sliver is what makes the pixel scroll look
	 * right), so the header has to be painted over it -- otherwise the code
	 * runs into the title bar. */
	nb_ui_header(header, NB_NULL);
	nb_ui_menu_icon_draw();
	/* the search chrome goes over the header, to the left of the hamburger */
	find_draw();
	if (u->input_active)
		nb_ui_input_draw();

	/* the status line: position, encoding, read-only state, and the modifier
	 * state on the right where there is room for it */
	{
		char pos[24], col[12];

		nb_fmt_int(pos, sizeof(pos), b->cur_line + 1);
		nb_str_append(pos, sizeof(pos), ":");
		nb_fmt_int(col, sizeof(col), b->cur_col + 1);
		nb_str_append(pos, sizeof(pos), col);
		nb_str_append(pos, sizeof(pos), " ");
		nb_str_append(pos, sizeof(pos),
			      b->encoding == NB_ENC_GB2312 ? "GB"
			      : b->encoding == NB_ENC_LATIN1 ? "L1" : "U8");
		if (b->readonly)
			nb_str_append(pos, sizeof(pos), " RO");
		nb_str_copy(left, sizeof(left), pos, -1);
	}
	{
		char sz[16];

		nb_fmt_size(sz, sizeof(sz), b->nbytes);
		nb_str_append(left, sizeof(left), "  ");
		nb_str_append(left, sizeof(left), sz);
	}
	/* WHAT THE NEXT KEY WILL TYPE.  ALPHA and SHIFT change every other key on
	 * the keyboard, so the state has to be visible: an alpha layer that is
	 * locked and unmarked is a user typing a '7' and getting a 'q'.  The text
	 * names the layer, not the key, because that is what the user is
	 * choosing.
	 *
	 * THE LABELS ARE THE KEYBOARD'S OWN WORDS: shift / alpha / alock / Alock,
	 * the same four names primeCode's keyboard uses (and the same convention --
	 * a capital A means the letters come out in CAPITALS).  The Chinese words
	 * that used to be here ("大写", "字母锁") were neither shorter nor the
	 * name printed on the key the user has to press. */
	{
		char mod[24];

		nb_ui_editor_status_label(mod, sizeof(mod));
		nb_ui_status(left, NB_NULL, u->th->foot_bg);
		/* the message takes the row over, so the indicator goes on top of
		 * it: which layer the keyboard is in is not a message */
		nb_ui_message_line();
		nb_ui_status_right(mod);
	}

	if (u->show_help)
		nb_ui_help_draw();
}

/* the key help overlay: the calculator's keyboard has no room for a legend, and
 * this is how a user finds out that MENU saves */
/* THE KEY HELP, FOR EVERY SCREEN (it is a reference, not a mode): MENU opens the
 * popup, HELP opens this, and the two are the only function keys nbCode uses --
 * the calculator has no F1..F6 row (see nb_ui.h). */
void nb_ui_help_draw(void)
{
	nb_ui *u = nb_ui_get();
	/* THIRTEEN LINES, AND THAT IS THE WHOLE BUDGET: the panel is the screen
	 * (a 240 pixel display does not scroll a help page) and the title takes the
	 * first 20, so (240 - 20) / 16 = 13 lines of text fit and the fourteenth is
	 * lost.  Every line here has to earn its place, which is why the chords are
	 * grouped: the two shift+key pairs share a line, and the version line --
	 * which used to be the fourteenth and therefore never drawn -- is now the
	 * thirteenth. */
	static const char *lines[] = {
		"MENU 菜单  HELP 帮助",
		"esc 返回  ON 退出",
		"方向键 移动  ↵ 换行/打开",
		"退格 删除字符/文件",
		"shift+退格 前删  shift+←/→ 行首尾",
		"shift 显示 shift，用完松开",
		"alpha 一/二/三次：alpha/alock/Alock",
		"SYMB 查找  PLOT 替换  shift+A 字符表",
		"shift+6 符号面板  shift+VIEW 复制",
		"shift+MENU 剪贴板  点击行号 折叠",
		"两个字母出补全，↵ 选中",
		"触摸 拖动滚动 · 长按框选",
		"nbCode " NB_VERSION "  Unifont+GB2312",
	};
	int y = 20;			/* below the panel's title row */
	int i;
	const int nlines = (int)(sizeof(lines) / sizeof(lines[0]));

	/* THE PANEL IS THE WHOLE SCREEN, and that is deliberate: a 240 pixel
	 * display does not scroll a help page, it loses the lines that do not fit,
	 * and the list has grown to thirteen.  Covering the editor (and its status
	 * bar) for as long as the help is up is what a help screen is for; the
	 * alternative -- a box that has to be shortened every time a key is added
	 * -- is how the last line ends up drawn behind the status bar. */
	nb_ui_panel(0, 0, NB_SCREEN_W, NB_SCREEN_H);
	nb_ui_puts(6, 3, "按键帮助", u->th->text);
	nb_ui_puts(NB_SCREEN_W - 6 - nb_ui_puts_w("esc 关闭"), 3, "esc 关闭",
		   u->th->sub_fg);
	for (i = 0; i < nlines; i++) {
		if (y + i * 16 + 16 > NB_SCREEN_H)
			break;
		nb_ui_puts(8, y + i * 16, lines[i], u->th->text);
	}
}

/* INSERT BYTES AT THE CARET, replacing a selection if there is one.
 *
 * One function because typed characters and the character table have to behave
 * identically -- a table insert that did not replace the selection would leave
 * the highlighted text behind the new character. */
static int editor_insert(nb_buf *b, const char *bytes, int n)
{
	nb_ui *u = nb_ui_get();

	if (!b || n <= 0)
		return 0;
	if (nb_ui_selection_active()) {
		int line0 = u->sel_l0;

		nb_ui_selection_delete();
		nb_buf_goto(b, line0, nb_buf_line(b, line0) ? b->cur_col : 0);
	}
	return nb_buf_insert(b, bytes, n);
}

/* the tab width the editor uses (the config's, or four) */
static int tab_w(nb_ui *u)
{
	return (u->cfg && u->cfg->tab_width > 0) ? u->cfg->tab_width : 4;
}

/* how far a line is indented, in COLUMNS (a tab is a tab stop) */
static int line_indent_cols(nb_buf *b, int line, int tw)
{
	const char *t = nb_buf_line(b, line);
	int n = nb_buf_linelen(b, line);
	int i, col = 0;

	for (i = 0; i < n; i++) {
		if (t[i] == ' ') {
			col++;
		} else if (t[i] == '\t') {
			col += tw - (col % tw);
		} else {
			break;
		}
	}
	return col;
}

/* are the indentation guides on? (they are, unless the setting says otherwise) */
static int indent_guides_on(nb_ui *u)
{
	return u->cfg ? !nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOGUIDES) : 1;
}

/* HOW MANY SPACES A BACKSPACE SHOULD TAKE OUT, 0 for the ordinary one character:
 * only inside the line's LEADING indentation (everything before the caret is a
 * space) and only up to the previous tab stop. */
static int indent_backspace(nb_buf *b)
{
	nb_ui *u = nb_ui_get();
	const char *t;
	int tw, col, back, spaces = 0;

	if (!b || b->readonly || b->cur_col <= 0)
		return 0;
	tw = tab_w(u);
	if (tw <= 0)
		tw = 4;
	col = b->cur_col;
	t = nb_buf_line(b, b->cur_line);
	/* the whole prefix has to be spaces: a backspace just after a word is the
	 * ordinary one, even if the line is indented */
	while (spaces < col) {
		if (t[spaces] != ' ' && t[spaces] != '\t')
			return 0;
		spaces++;
	}
	back = col % tw;
	if (back == 0)
		back = tw;
	return back > col ? col : back;
}

/* ---------------------------------------------------------------------------
 * THE GLOBAL CLIPBOARD (nb_clip.h): copy here, and the panel opened by
 * shift+MENU lists it -- across files and across sessions, because nb_clip.c
 * writes the list to a file.
 * ------------------------------------------------------------------------- */

/* the selected text, newlines between the lines included, into `out`; returns
 * its length (0 when nothing is selected).  The ends are sorted first: the anchor
 * may be either end of the selection. */
static int selection_text(char *out, int cap)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int l0, c0, l1, c1, l, n = 0;

	if (!u->sel_active || !b)
		return 0;
	selection_span(&l0, &c0, &l1, &c1);
	for (l = l0; l <= l1 && n < cap; l++) {
		const char *t = nb_buf_line(b, l);
		int len = nb_buf_linelen(b, l);
		int from = (l == l0) ? c0 : 0;
		int to = (l == l1) ? c1 : len;

		while (from < to && n < cap)
			out[n++] = t[from++];
		if (l < l1 && n < cap)
			out[n++] = '\n';
	}
	out[n < cap ? n : cap - 1] = 0;
	return n;
}

/* COPY: the selection when there is one, otherwise the caret's line (with its
 * newline, the same thing the menu's "copy line" puts in the buffer's clipboard,
 * so a paste from the panel is line-shaped).  Returns 1 for a selection, 2 for a
 * line and 0 when there was nothing to copy. */
static char copy_text[COPY_MAX];

int nb_ui_editor_copy(void)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	char *text = copy_text;
	int n, kind, full = 0;

	if (!b)
		return 0;
	if (u->sel_active) {
		n = selection_text(text, COPY_MAX);
		kind = 1;
	} else {
		const char *t = nb_buf_line(b, b->cur_line);
		int len = nb_buf_linelen(b, b->cur_line);

		if (len > COPY_MAX - 2)
			len = COPY_MAX - 2;
		nb_memcpy(text, t, (unsigned int)len);
		text[len] = '\n';		/* the line, as a line */
		text[len + 1] = 0;
		n = len + 1;
		kind = 2;
	}
	if (n <= 0)
		return 0;
	if (n > NB_CLIP_ENTRY) {
		n = NB_CLIP_ENTRY;		/* nb_clip_add truncates the same way */
		text[n] = 0;
		full = 1;
	}
	/* THE BUFFER'S OWN CLIPBOARD GETS IT TOO, so a paste right after a copy
	 * works even if the panel is never opened */
	nb_buf_clip_set(b, text, n);
	if (!nb_clip_add(text, n)) {
		/* already the newest entry: say the copy happened, not that the list
		 * changed */
		nb_ui_message(kind == 1 ? "已复制选区" : "已复制该行", 0);
		return kind;
	}
	nb_clip_save();
	if (full)
		nb_ui_message("已复制（过长，只保留开头）", 1);
	else
		nb_ui_message(kind == 1 ? "已复制选区" : "已复制该行", 0);
	return kind;
}

/* INSERT A CLIPBOARD ENTRY AT THE CARET.  The text goes in the BUFFER's encoding
 * (a GB2312 file has to stay GB2312) and its newlines split into real lines --
 * nb_buf_insert does that splitting, so a pasted block behaves like a typed one
 * (and one undo record undoes the whole paste). */
/* THE PASTE BUFFER IS STATIC, for the reason above: 580 bytes in the frame of the
 * function that inserts a clipboard entry is the largest single frame in the image,
 * and it is single-threaded scratch either way. */
static char paste_bytes[PASTE_MAX];

int nb_ui_editor_insert_text(const char *text, int len)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	char *bytes = paste_bytes;
	int n = 0, i;

	if (!b || b->readonly || !text || len <= 0)
		return 0;
	if (len > NB_CLIP_ENTRY)
		len = NB_CLIP_ENTRY;
	if (b->encoding == NB_ENC_UTF8) {
		n = len;
		if (n > PASTE_MAX)
			n = PASTE_MAX;
		nb_memcpy(bytes, text, (unsigned int)n);
	} else {
		/* the entry is UTF-8 (that is what the editor holds and what the
		 * clipboard file is written in); the file being edited may not be */
		for (i = 0; i < len && n < PASTE_MAX - 4; ) {
			int used = 0, k;
			unsigned int cp = nb_codec_utf8_decode(
				(const unsigned char *)text + i, &used);

			if (used <= 0)
				break;
			i += used;
			if (cp == 0xFFFD)
				cp = '?';
			k = nb_codec_char_to_bytes(cp, b->encoding, bytes + n,
						  PASTE_MAX - n);
			if (k > 0)
				n += k;
		}
	}
	if (n <= 0 || !editor_insert(b, bytes, n))
		return 0;
	nb_ui_editor_changed(b->cur_line);
	u->dirty = 1;
	return 1;
}

/* ONE CODEPOINT FROM THE CHARACTER TABLE (nb_ui_overlay.c).  The bytes are in the
 * BUFFER's encoding, not always UTF-8: typing into a GB2312 file has to write
 * GB2312, or the file becomes one no other tool on the calculator can read. */
int nb_ui_editor_insert_cp(unsigned int cp)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	char bytes[4];
	int n;

	if (!b || b->readonly)
		return 0;
	n = nb_codec_char_to_bytes(cp, b->encoding, bytes, 4);
	if (n <= 0 || !editor_insert(b, bytes, n))
		return 0;
	nb_ui_editor_changed(b->cur_line);
	u->dirty = 1;
	return 1;
}

/* INSERT A KEY'S SNIPPET ("sin()", "()", "{}" ...), in the BUFFER's encoding,
 * and leave the caret INSIDE a pair -- primeCode's rule (keys.py: `if adde[-1] in
 * ')]}"': gb -= 1`), which is the difference between a bracket key that is useful
 * and one that has to be corrected every time. */
void nb_ui_editor_ins(const char *text)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	char bytes[32];
	int lost = 0, len, n;

	if (!b || b->readonly || !text || !text[0])
		return;
	len = nb_strlen(text);
	n = nb_codec_line_to_bytes(text, len, b->encoding, bytes,
				   (int)sizeof(bytes), &lost);
	if (n <= 0 || !editor_insert(b, bytes, n))
		return;
	if (nb_key_wants_caret_inside(text, len))
		b->cur_col = nb_buf_prev_char(b, b->cur_line, b->cur_col);
	u->dirty = 1;
	if (lost > 0) {
		char msg[40], num[12];

		nb_str_copy(msg, sizeof(msg), "该编码无法表示：", 1);
		nb_fmt_int(num, sizeof(num), lost);
		nb_str_append(msg, sizeof(msg), num);
		nb_ui_message(msg, 1);
		return;
	}
	nb_ui_editor_changed(b->cur_line);
}

/* PUT THE CARET ON A LINE AND SHOW IT (the menu's "go to line") */
void nb_ui_editor_goto(int line)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	char msg[32], num[12];

	if (!b)
		return;
	if (line < 1)
		line = 1;
	if (line > b->nlines)
		line = b->nlines;
	nb_buf_goto(b, line - 1, 0);
	nb_ui_selection_clear();
	u->view_line = -1;		/* force the window to follow the caret */
	nb_str_copy(msg, sizeof(msg), "第 ", -1);
	nb_fmt_int(num, sizeof(num), line);
	nb_str_append(msg, sizeof(msg), num);
	nb_str_append(msg, sizeof(msg), " 行");
	nb_ui_message(msg, 0);
	u->dirty = 1;
}

/* SAVE AS: the same folder, a new name.  The language follows the new extension
 * when the config lets the file name decide, because a "note.c" saved as
 * "note.py" should be coloured as Python from then on. */
int nb_ui_editor_save_as(const char *name)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	char path[NB_PATH_MAX];
	int i, cut = -1;

	if (!b || !name || !*name)
		return 0;
	for (i = 0; name[i]; i++) {
		if (name[i] == ':' || name[i] == '\\' || name[i] == '/') {
			nb_ui_message("文件名不能带路径", 2);
			return 0;
		}
	}
	nb_str_copy(path, sizeof(path), b->path, -1);
	for (i = 0; path[i]; i++) {
		if (path[i] == '\\' || path[i] == '/')
			cut = i;
	}
	if (cut < 0)
		path[0] = 0;
	else
		path[cut + 1] = 0;
	nb_str_append(path, sizeof(path), name);
	nb_str_copy(b->name, sizeof(b->name), name, -1);
	nb_str_copy(b->path, sizeof(b->path), path, -1);
	editor_relanguage(u);
	return nb_ui_editor_save();
}

/* ---------------------------------------------------------------------------
 * FIND AND REPLACE (SYMB / PLOT, nb_find.c does the matching)
 *
 * The header's right hand end carries `k/n` -- which match of how many -- and two
 * buttons, `上` and `下`, that step to the previous and the next one.  Both are
 * inside the header, so the tap that presses one is a tap on the header and not on
 * the text (nb_ui_editor_touch checks these BEFORE it decides a press is not
 * text); a 320 pixel screen has no room for a floating toolbar, and the top right
 * is where the user's eye already goes for the count.
 * ------------------------------------------------------------------------- */
/* THE FIND WINDOW IS A FLOATING PANEL, and it was ugly as header furniture: two
 * labels squeezed into the title bar next to the hamburger, with the count between
 * them, on a bar that has a file name to show.  It is a small panel now, drawn
 * over the text at the top right while a search is active:
 *
 *     +-------------+
 *     | 3/17      x |
 *     | [ 上 ] [ 下 ]|
 *     +-------------+
 *
 * It is NOT a modal overlay: a tap outside it is still a tap on the code (only the
 * taps inside it belong to it), because a user who is stepping through matches is
 * also reading the line they landed on.
 */
#define FIND_W		104
#define FIND_H		48
#define FIND_X		(NB_SCREEN_W - 6 - FIND_W)
#define FIND_Y		(NB_BODY_TOP + 3)
#define FIND_BTN_H	20
#define FIND_BTN_Y	(FIND_Y + FIND_H - FIND_BTN_H - 5)

static void find_btn_rect(int which, int *x, int *y, int *w, int *h)
{
	int bw = (FIND_W - 12 - 4) / 2;

	*x = FIND_X + 6 + which * (bw + 4);
	*y = FIND_BTN_Y;
	*w = bw;
	*h = FIND_BTN_H;
}

static void find_button(int which, const char *label, int on)
{
	nb_ui *u = nb_ui_get();
	int x, y, w, h, tw;

	find_btn_rect(which, &x, &y, &w, &h);
	nb_gfx_fill(x, y, w, h, on ? u->th->sel_bg : u->th->bg);
	nb_gfx_frame(x, y, w, h, on ? u->th->accent : u->th->line);
	tw = nb_ui_puts_w(label);
	nb_ui_puts(x + (w - tw) / 2, y + (h - 16) / 2, label,
		   on ? u->th->sel_fg : u->th->text);
}

/* which part of the window is at a point: 0 previous, 1 next, 2 close, -1 none */
static int find_at(int x, int y)
{
	int i;

	if (!nb_find_active() || !nb_ui_get()->buf)
		return -1;
	if (x < FIND_X || x >= FIND_X + FIND_W || y < FIND_Y ||
	    y >= FIND_Y + FIND_H)
		return -1;
	for (i = 0; i < 2; i++) {
		int bx, by, bw, bh;

		find_btn_rect(i, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by && y < by + bh)
			return i;
	}
	if (y < FIND_BTN_Y && x >= FIND_X + FIND_W - 22)
		return 2;			/* the close box */
	return -1;
}

void nb_ui_editor_find_window(int *x, int *y, int *w, int *h)
{
	int on = nb_find_active() && nb_ui_get()->buf != NB_NULL;

	if (x)
		*x = on ? FIND_X : 0;
	if (y)
		*y = on ? FIND_Y : 0;
	if (w)
		*w = on ? FIND_W : 0;
	if (h)
		*h = on ? FIND_H : 0;
}

static void find_draw(void)
{
	nb_ui *u = nb_ui_get();
	int total, index = 0;
	char num[16];

	if (!nb_find_active() || !u->buf)
		return;
	total = nb_find_count(u->buf);
	if (total >= NB_FIND_COUNT_MAX)
		nb_str_copy(num, sizeof(num), "999+", -1);
	else
		nb_fmt_int(num, sizeof(num), total);
	if (nb_find_current(u->buf, NB_NULL, NB_NULL, &index))
		index++;
	else
		index = 0;
	nb_ui_panel(FIND_X, FIND_Y, FIND_W, FIND_H);
	{
		char line[28];
		int w;

		nb_fmt_int(line, sizeof(line), index);
		nb_str_append(line, sizeof(line), "/");
		nb_str_append(line, sizeof(line), num);
		w = nb_ui_puts_w(line);
		nb_ui_puts(FIND_X + (FIND_W - 22 - w) / 2, FIND_Y + 4, line,
			   total > 0 ? u->th->text : u->th->muted);
	}
	/* the close box: a search that stays on the screen for good is a search the
	 * user cannot get rid of without knowing which key to press */
	nb_ui_puts(FIND_X + FIND_W - 20, FIND_Y + 3, "x", u->th->sub_fg);
	find_button(0, "上", total > 0);
	find_button(1, "下", total > 0);
}

/* jump to the next (dir > 0) or previous match, and say where the user is */
int nb_ui_editor_find_step(int dir)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int line = 0, col = 0, index = 0;

	if (!b || !nb_find_active())
		return 0;
	if (!nb_find_step(b, dir, &line, &col, &index)) {
		nb_ui_message("没有找到", 1);
		return 0;
	}
	nb_buf_goto(b, line, col);
	/* the view follows the caret: view_line = -1 is "fit the window to it on
	 * the next draw", which is what a jump needs (unlike a fold, see
	 * view_anchor) */
	u->view_line = -1;
	u->sel_active = 0;
	u->dirty = 1;
	{
		int total = nb_find_count(b);

		if (total >= NB_FIND_COUNT_MAX)
			nb_ui_message(dir > 0 ? "下一个（999+）" : "上一个（999+）", 0);
		else {
			char msg[32];

			nb_fmt_int(msg, sizeof(msg), index + 1);
			nb_str_append(msg, sizeof(msg), "/");
			{
				char tot[12];

				nb_fmt_int(tot, sizeof(tot), total);
				nb_str_append(msg, sizeof(msg), tot);
			}
			nb_ui_message(msg, 0);
		}
	}
	return 1;
}

/* SYMB: ask for the search text, remembering the last one */
void nb_ui_editor_find_begin(void)
{
	nb_ui *u = nb_ui_get();

	if (!u->buf)
		return;
	nb_ui_input_begin("查找: ", nb_find_text());
	u->input_then = NB_INPUT_FIND;
}

/* the search text has been typed: count it and jump to the first match at or
 * after the caret */
void nb_ui_editor_find_commit(const char *text)
{
	nb_ui *u = nb_ui_get();
	int line = 0, col = 0, index = 0, total;

	if (!u->buf)
		return;
	if (!nb_find_set(text)) {
		nb_ui_message("已清除查找", 0);
		u->dirty = 1;
		return;
	}
	total = nb_find_count(u->buf);
	if (total <= 0) {
		nb_ui_message("没有找到", 1);
		u->dirty = 1;
		return;
	}
	if (nb_find_current(u->buf, &line, &col, &index)) {
		nb_buf_goto(u->buf, line, col);
		u->view_line = -1;
	}
	{
		char msg[40];

		nb_str_copy(msg, sizeof(msg), "找到 ", -1);
		if (total >= NB_FIND_COUNT_MAX)
			nb_str_append(msg, sizeof(msg), "999+");
		else {
			char tot[12];

			nb_fmt_int(tot, sizeof(tot), total);
			nb_str_append(msg, sizeof(msg), tot);
		}
		nb_str_append(msg, sizeof(msg), " 处");
		nb_ui_message(msg, 0);
	}
	u->dirty = 1;
}

/* PLOT: ask what to put in place of the matches (an empty answer deletes them) */
void nb_ui_editor_replace_begin(void)
{
	nb_ui *u = nb_ui_get();

	if (!u->buf)
		return;
	if (!nb_find_active()) {
		/* nothing to replace yet: the same question as SYMB, and the user can
		 * press PLOT again once the count is on the screen */
		nb_ui_editor_find_begin();
		return;
	}
	nb_ui_input_begin("替换为: ", "");
	u->input_then = NB_INPUT_REPLACE;
}

/* the replacement has been typed: replace the match under the caret and go to the
 * next one (and on, so that pressing PLOT again continues down the file) */
void nb_ui_editor_replace_commit(const char *rep)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int line = 0, col = 0;

	if (!b)
		return;
	if (!nb_find_replace(b, rep, &line, &col)) {
		/* THE CARET IS NOT ON A MATCH: go to one first, so the answer to
		 * "why did nothing happen" is on the screen */
		nb_ui_editor_find_step(1);
		nb_ui_message("光标不在匹配处：已跳到下一个", 1);
		return;
	}
	nb_ui_editor_changed(line);
	nb_buf_goto(b, line, col);
	u->view_line = -1;
	u->dirty = 1;
	nb_ui_message("已替换", 0);
}

/* PLOT's "replace all": the menu asks for the replacement, and every match the
 * search text finds is replaced (one undo record each) */
void nb_ui_editor_replace_all_commit(const char *rep)
{
	nb_ui *u = nb_ui_get();
	int n;

	if (!u->buf || !nb_find_active()) {
		nb_ui_message("先按 SYMB 查找", 1);
		return;
	}
	n = nb_find_replace_all(u->buf, rep);
	nb_ui_editor_changed(0);
	u->dirty = 1;
	if (n <= 0) {
		nb_ui_message("没有可替换的匹配", 1);
		return;
	}
	{
		char msg[32];

		nb_str_copy(msg, sizeof(msg), "已替换 ", -1);
		{
			char num[12];

			nb_fmt_int(num, sizeof(num), n);
			nb_str_append(msg, sizeof(msg), num);
		}
		nb_str_append(msg, sizeof(msg), " 处");
		nb_ui_message(msg, 0);
	}
}

/* how many matches, and which one the caret is on: 1 when the caret IS on one */
int nb_ui_editor_find_info(int *index, int *total)
{
	nb_ui *u = nb_ui_get();
	int i = 0, on = 0;

	if (!u->buf || !nb_find_active())
		return 0;
	*total = nb_find_count(u->buf);
	on = nb_find_current(u->buf, NB_NULL, NB_NULL, &i);
	if (index)
		*index = on ? i + 1 : 0;
	return on;
}

/* ---------------------------------------------------------------------------
 * the completion popup (nb_complete.c builds the list)
 *
 * Drawn AT THE CARET -- under the line the word is on, or above it when the list
 * would fall off the bottom -- because that is where the eye already is.  Six
 * rows of 16 px, the typed prefix in the muted tone and the rest of each
 * candidate in the text colour, so "what I would add" is visible at a glance.
 * ------------------------------------------------------------------------- */
#define COMP_ROW	16
#define COMP_PAD	3

static void comp_rect(int *x, int *y, int *w, int *h)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int gw = gutter_width();
	int caret_frame = 0, cx, cy, i, longest = 0;

	/* where the caret is on screen: the word's first character, not the
	 * caret itself -- the panel should not jump as the word grows */
	{
		int f = 0;

		nb_text_decode(&u->dec, nb_buf_line(b, b->cur_line),
			       nb_buf_linelen(b, b->cur_line), NB_NULL,
			       b->encoding);
		caret_frame = nb_text_frame_of(&u->dec, b->cur_col);
		f = nb_text_frame_of(&u->dec,
				     b->cur_col - u->comp.prefix_len);
		caret_frame = f;
	}
	cx = gw + (caret_frame - u->left) * 8 - u->xoff;
	cy = BODY_TOP + (b->cur_line - b->top_line + 1) * LINE_H - u->yoff;
	*h = u->comp.n * COMP_ROW + COMP_PAD * 2;
	for (i = 0; i < u->comp.n; i++) {
		int len = nb_strlen(u->comp.item[i]);

		if (len > longest)
			longest = len;
	}
	/* THE BOX HAS TO BE WIDE ENOUGH FOR THE LONGEST CANDIDATE, PLUS ITS OWN
	 * PADDING: the text starts 8 px in and the clip is `w - 12` wide, so a box of
	 * `longest * 8 + 8` cut the LAST CHARACTER of every candidate -- a function
	 * name one letter short of being shown completely. */
	*w = longest * 8 + 12;
	if (*w > NB_SCREEN_W - 8)
		*w = NB_SCREEN_W - 8;
	if (*w < 48)
		*w = 48;
	if (cx + *w > NB_SCREEN_W - 2)
		cx = NB_SCREEN_W - 2 - *w;
	if (cx < gw)
		cx = gw;
	if (cy + *h > BODY_BOT) {
		int above = cy - LINE_H - *h;

		cy = above >= BODY_TOP ? above : BODY_BOT - *h;
	}
	*x = cx;
	*y = cy;
}

/* the popup's rectangle, for a test (the tap target has to be found the same way
 * the drawing finds it, or the test is measuring its own guess) */
void nb_ui_editor_comp_rect(int *x, int *y, int *w, int *h)
{
	if (!nb_complete_active(&nb_ui_get()->comp)) {
		*x = *y = *w = *h = 0;
		return;
	}
	comp_rect(x, y, w, h);
}

static void comp_draw(void)
{
	nb_ui *u = nb_ui_get();
	int x, y, w, h, i;

	if (!nb_complete_active(&u->comp))
		return;
	comp_rect(&x, &y, &w, &h);
	nb_ui_panel(x, y, w, h);
	for (i = 0; i < u->comp.n; i++) {
		int ry = y + COMP_PAD + i * COMP_ROW;
		const char *word = u->comp.item[i];
		char head[2];

		if (i == u->comp.sel) {
			nb_gfx_fill(x + 2, ry, w - 4, COMP_ROW, u->th->sel_bg);
			nb_gfx_fill(x + 2, ry, 2, COMP_ROW, u->th->accent);
		}
		/* the part that is already typed is muted: the eye then sees the
		 * part that would be ADDED */
		head[0] = word[0];
		head[1] = 0;
		(void)head;
		nb_ui_puts_clip(x + 8, ry, u->comp.prefix,
				i == u->comp.sel ? u->th->sel_fg : u->th->muted,
				w - 12);
		nb_ui_puts_clip(x + 8 + u->comp.prefix_len * 8, ry,
				word + u->comp.prefix_len,
				i == u->comp.sel ? u->th->sel_fg : u->th->text,
				w - 12 - u->comp.prefix_len * 8);
	}
}

/* ENTER (or a tap): the word replaces the prefix */
static int comp_accept(void)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int line = b->cur_line;
	int col = b->cur_col;
	int n = u->comp.prefix_len;
	int i;

	if (!nb_complete_active(&u->comp))
		return 0;
	nb_str_copy(u->comp.accepted, sizeof(u->comp.accepted),
		    u->comp.item[u->comp.sel], -1);
	/* delete the prefix, then type the word: the buffer's own primitives, so
	 * undo and the highlighting see one edit */
	for (i = 0; i < n; i++)
		b->cur_col = nb_buf_prev_char(b, line, b->cur_col);
	nb_buf_delete_range(b, line, b->cur_col, line, col);
	{
		const char *word = u->comp.item[u->comp.sel];

		nb_buf_goto(b, line, b->cur_col);
		nb_buf_insert(b, word, nb_strlen(word));
	}
	nb_complete_close(&u->comp);
	nb_ui_editor_changed(b->cur_line);
	u->dirty = 1;
	return 1;
}

/* the popup's own keys, and what closes it.  Returns 1 when the key was eaten. */
static int comp_key(int what)
{
	nb_ui *u = nb_ui_get();

	if (!nb_complete_active(&u->comp))
		return 0;
	switch (what) {
	case NB_KEY_UP:
		nb_complete_move(&u->comp, -1);
		u->dirty = 1;
		return 1;
	case NB_KEY_DOWN:
		nb_complete_move(&u->comp, 1);
		u->dirty = 1;
		return 1;
	case NB_KEY_ENTER:
	case NB_KEY_TAB:
		return comp_accept();
	case NB_KEY_ESC:
		/* esc closes the POPUP, and only the popup: leaving the file is
		 * what esc does when there is nothing to close */
		nb_complete_close(&u->comp);
		u->dirty = 1;
		return 1;
	default:
		/* anything else closes it and then does its own job */
		nb_complete_close(&u->comp);
		u->dirty = 1;
		return 0;
	}
}

/* a tap inside the popup picks the row under the finger */
static int comp_touch(int x, int y)
{
	nb_ui *u = nb_ui_get();
	int px, py, pw, ph;

	if (!nb_complete_active(&u->comp))
		return 0;
	comp_rect(&px, &py, &pw, &ph);
	if (x < px || x >= px + pw || y < py || y >= py + ph)
		return 0;
	{
		int row = (y - py - COMP_PAD) / COMP_ROW;

		if (row >= 0 && row < u->comp.n) {
			u->comp.sel = row;
			comp_accept();
		}
	}
	return 1;
}

/* the language the open buffer is highlighted as (the fold rules and the symbol
 * panel both need to know it) */
static nb_lang nb_lang_of(nb_ui *u)
{
	return u->buf ? nb_hl_lang(&u->hl) : NB_LANG_TXT;
}

/* ---------------------------------------------------------------------------
 * the matching bracket
 *
 * What every editor does, and what the report asked for: the bracket UNDER the
 * caret (or the one just before it, so the pair lights up while the caret sits
 * after a closer) is matched, and the two are drawn so they are told apart from
 * the brackets around them -- a box behind them plus the ACCENT colour for the
 * glyphs, which is the colour this interface already uses for "where you are"
 * (the caret, the scrollbar, the selected row's edge).
 *
 * The scan is bounded: a file that never closes its brace must not cost the frame
 * a pass over itself.
 * ------------------------------------------------------------------------- */
#define MATCH_SCAN	4000

static int bracket_open(int c)
{
	return c == '(' || c == '[' || c == '{';
}

static int bracket_close(int c)
{
	return c == ')' || c == ']' || c == '}';
}

static int bracket_match(int c)
{
	switch (c) {
	case '(':	return ')';
	case '[':	return ']';
	case '{':	return '}';
	case ')':	return '(';
	case ']':	return '[';
	case '}':	return '{';
	default:	return 0;
	}
}

static int line_char_at(nb_buf *b, int line, int col)
{
	int len = nb_buf_linelen(b, line);

	if (line < 0 || line >= b->nlines || col < 0 || col >= len)
		return 0;
	return (unsigned char)nb_buf_line(b, line)[col];
}

/* the next character position in the buffer, or 0 at the end */
static int next_scan(nb_buf *b, int *line, int *col)
{
	int len = nb_buf_linelen(b, *line);

	if (*col < len)
		*col = nb_buf_next_char(b, *line, *col);
	else if (*line + 1 < b->nlines)
		(*line)++, *col = 0;
	else
		return 0;
	return 1;
}

/* the previous character position, or 0 at the start */
static int prev_scan(nb_buf *b, int *line, int *col)
{
	if (*col > 0)
		*col = nb_buf_prev_char(b, *line, *col);
	else if (*line > 0)
		(*line)--, *col = nb_buf_linelen(b, *line);
	else
		return 0;
	return 1;
}

static void find_match(nb_ui *u)
{
	nb_buf *b = u->buf;
	int line, col, start, want, depth = 0, guard = MATCH_SCAN;
	int forward;

	u->match_line = u->match_col = -1;
	u->brace_line = u->brace_col = -1;
	if (!b || u->sel_active)		/* a selection is not a caret */
		return;
	line = b->cur_line;
	col = b->cur_col;
	{
		int here = line_char_at(b, line, col);
		int before = col > 0
			? line_char_at(b, line, nb_buf_prev_char(b, line, col))
			: 0;

		if (bracket_open(here) || bracket_close(here)) {
			start = here;
		} else if (col > 0 &&
			   (bracket_open(before) || bracket_close(before))) {
			col = nb_buf_prev_char(b, line, col);
			start = before;
		} else {
			return;
		}
	}
	forward = bracket_open(start) ? 1 : 0;
	want = bracket_match(start);
	u->brace_line = line;
	u->brace_col = col;
	while (guard-- > 0) {
		int c;

		if (forward ? !next_scan(b, &line, &col)
			    : !prev_scan(b, &line, &col))
			return;			/* unbalanced: nothing lights up */
		c = line_char_at(b, line, col);
		if (c == 0)
			continue;		/* the newline between two lines */
		if (c == start) {
			depth++;		/* the same kind, nested */
			continue;
		}
		if (c != want)
			continue;
		if (depth == 0) {
			u->match_line = line;
			u->match_col = col;
			return;
		}
		depth--;
	}
}

/* THE PIXELS OF ONE CHARACTER of a decoded line: its left frame and how many
 * frames it covers.  Used for the two brackets, which are boxed and then drawn
 * again in the accent colour on top of the line. */
static int char_frames(nb_ui *u, int col, int *frame)
{
	int i;

	for (i = 0; i < u->dec.n; i++) {
		const nb_text_char *c = &u->dec.ch[i];
		int nb = nb_text_char_bytes(&u->dec, i);

		if (col < c->byte_off + nb) {
			*frame = c->frame_off;
			return c->advance / 8 ? c->advance / 8 : 1;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * the editor's popup menu (the MENU key, or the header's hamburger)
 *
 * THE OLD F-KEY ACTIONS LIVE HERE.  The calculator's keyboard has no F1..F6 row
 * (see nb_ui.h), and a list the user can read beats a chord they have to
 * remember; the two that are used constantly -- save, and going back -- are also
 * on their own keys (MENU, esc). */
static const char *const editor_menu_items[] = {
	"保存", "另存为", "撤销", "复制整行", "剪切整行", "剪切选区",
	"剪贴板…", "全部替换…", "文件信息", "跳转…"
};
static const int editor_menu_actions[] = {
	NB_MENU_SAVE, NB_MENU_SAVE_AS, NB_MENU_UNDO, NB_MENU_COPY_LINE,
	NB_MENU_CUT_LINE, NB_MENU_CUT_SEL, NB_MENU_CLIP, NB_MENU_REPLACE_ALL,
	NB_MENU_FILE_INFO, NB_MENU_GOTO
};

void nb_ui_editor_menu(void)
{
	nb_ui *u = nb_ui_get();

	if (!u->buf)
		return;
	nb_ui_menu_open(NB_NULL, editor_menu_items, editor_menu_actions,
			(int)(sizeof(editor_menu_items) /
			      sizeof(editor_menu_items[0])));
}

/* what the menu sends back, routed here by nb_ui.c */
void nb_ui_editor_action(int action)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;

	if (!b)
		return;
	switch (action) {
	case NB_MENU_SAVE:
		nb_ui_editor_save();
		return;
	case NB_MENU_SAVE_AS:
		nb_ui_input_begin("另存为: ", b->name);
		u->input_then = NB_INPUT_SAVE_AS;
		return;
	case NB_MENU_GOTO:
		nb_ui_input_begin("跳转到行: ", "");
		u->input_then = NB_INPUT_GOTO;
		return;
	case NB_MENU_FILE_INFO:
		nb_ui_overlay_open(NB_OVER_INFO);
		return;
	case NB_MENU_CLIP:
		/* the same panel shift+MENU opens: the menu is how it is found */
		nb_ui_clip_open();
		return;
	case NB_MENU_REPLACE_ALL:
		/* REPLACE ALL IS IN THE MENU, not on a key: PLOT replaces one match
		 * at a time (with the next one under the caret afterwards), and a
		 * single press that rewrites the whole file has to be asked for
		 * deliberately.  It uses the search text SYMB left behind. */
		if (!nb_find_active()) {
			nb_ui_message("先按 SYMB 查找", 1);
			return;
		}
		nb_ui_input_begin("全部替换为: ", "");
		u->input_then = NB_INPUT_REPLACE_ALL;
		return;
	case NB_MENU_UNDO:
		if (nb_buf_undo(b)) {
			nb_ui_editor_changed(0);
			nb_ui_message("已撤销", 0);
		} else {
			nb_ui_message("没有可撤销的操作", 1);
		}
		break;
	case NB_MENU_COPY_LINE:
		/* THE MENU'S COPY IS THE CHORD'S COPY (shift+VIEW): whatever the user
		 * copies lands in the GLOBAL list, so the panel offers it later -- in
		 * this file or in the next one.  The buffer's own clipboard is set by
		 * nb_ui_editor_copy() as well, which is what a paste needs. */
		nb_ui_selection_clear();
		nb_ui_editor_copy();
		break;
	case NB_MENU_CUT_LINE:
		nb_ui_selection_clear();
		nb_ui_editor_copy();		/* the line, into the global list */
		if (nb_buf_kill_line(b)) {
			nb_ui_editor_changed(b->cur_line);
			nb_ui_message("已剪切该行", 0);
		}
		break;
	case NB_MENU_CUT_SEL:
		if (nb_ui_selection_active()) {
			int line0 = u->sel_l0;

			/* ... and the cut text goes into the global list FIRST: the
			 * delete is what destroys it, and this action used to delete
			 * the selection without copying it anywhere at all -- a cut
			 * that lost what it cut. */
			nb_ui_editor_copy();
			if (nb_ui_selection_delete()) {
				nb_buf_goto(b, line0, u->sel_c0);
				nb_ui_editor_changed(b->cur_line);
				nb_ui_message("已剪切选区", 0);
			}
		} else {
			nb_ui_message("没有选区", 1);
		}
		break;
	default:
		return;
	}
	u->dirty = 1;
}

/* ---------------------------------------------------------------------------
 * keys
 *
 * The bindings, and why each one is where it is:
 *
 *   arrows/ENTER/BACKSPACE   text editing, the same as the Python PrimeCode
 *   MENU                     the screen's popup menu: save, save as, undo,
 *                            copy/cut, the clipboard, file info, go to, find
 *   shift+VIEW / shift+MENU  copy, and the global clipboard
 *   esc                      back to the file list (asking first if modified)
 *   symb/plot/num/cas/view  ( ) { } -- the punctuation the keypad cannot
 *                            print, on the keys that carry no character of
 *                            their own (the alpha letters live on the keys
 *                            the keyboard prints them on; see nb_keymap.c)
 *   shift+ON                 a real tab character; ON alone quits
 *   F6/help                  the overlay above
 * ------------------------------------------------------------------------- */
static void editor_indent_here(char *out, int cap, int *len)
{
	nb_ui *u = nb_ui_get();
	int n = nb_buf_indent_of(u->buf, u->buf->cur_line);
	int k = 0;

	if (n > cap - 1)
		n = cap - 1;
	while (k < n)
		out[k++] = ' ';
	out[k] = 0;
	*len = k;
	return;
}

void nb_ui_editor_key(int what, int ch)
{
	nb_ui *u = nb_ui_get();
	nb_buf *b = u->buf;
	int width = (u->cfg && u->cfg->tab_width > 0) ? u->cfg->tab_width : 4;
	int changed_line = -1;
	int rc = 0;
	int complete = (u->cfg == NB_NULL) ||
		!nb_cfg_flag(u->cfg, NB_CFG_FLAG_NOCOMPLETE);

	if (!b)
		return;
	if (what == NB_KEY_NONE)
		return;
	/* THE COMPLETION POPUP GETS THE KEY FIRST (it is over the text, and a key
	 * that reached the editor would edit something the user cannot see) */
	if (comp_key(what))
		return;

	switch (what) {
	case NB_KEY_CHAR:
	{
		char bytes[4];
		int n = nb_codec_encode((unsigned int)(unsigned char)ch, bytes, 4);

		if (ch == 0)
			break;
		if (editor_insert(b, bytes, n)) {
			changed_line = b->cur_line;
			/* a word character continues the word the popup is
			 * about; anything else ends it */
			if (complete && n == 1 && nb_word_char((unsigned char)ch))
				nb_complete_update(&u->comp, b,
						   nb_hl_lang(&u->hl));
			else
				nb_complete_close(&u->comp);
		}
		break;
	}
	case NB_KEY_ENTER:
	{
		char indent[64];
		int len = 0;

		editor_indent_here(indent, sizeof(indent), &len);
		/* a line that ends in `{` or `then` or `:` is asking for one more
		 * level; that is the only auto-indent rule nbCode applies, and it
		 * is the setting's "auto_indent" */
		{
			const char *s = nb_buf_line(b, b->cur_line);
			int l = nb_buf_linelen(b, b->cur_line);

			while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t'))
				l--;
			if (l > 0 && (s[l - 1] == '{' || s[l - 1] == ':' ||
				      s[l - 1] == '(')) {
				int k;

				for (k = 0; k < width && len < 60; k++)
					indent[len++] = ' ';
			}
		}
		if (nb_buf_newline(b, indent, len))
			changed_line = b->cur_line;
		break;
	}
	case NB_KEY_BACKSPACE:
		if (nb_ui_selection_active()) {
			if (nb_ui_selection_delete())
				changed_line = b->cur_line;
			break;
		}
		/* DEL INSIDE THE INDENTATION TAKES A WHOLE STEP.  Four spaces in and a
		 * tab width of four: one press, four spaces (the report), because
		 * deleting an indent a character at a time is the kind of small chore
		 * this keyboard should not ask for.  The step is to the PREVIOUS TAB
		 * STOP, which is what makes it predictable when the indentation is not
		 * a multiple of the width (6 spaces in, width 4: two go). */
		{
			int n = indent_backspace(b);

			if (n > 0) {
				if (nb_buf_backspace_n(b, n))
					changed_line = b->cur_line;
				break;
			}
		}
		if (nb_buf_backspace(b))
			changed_line = b->cur_line;
		break;
	case NB_KEY_DELETE:
		if (nb_ui_selection_active()) {
			if (nb_ui_selection_delete())
				changed_line = b->cur_line;
			break;
		}
		if (nb_buf_delete(b, 1))
			changed_line = b->cur_line;
		break;
	case NB_KEY_TAB:
		rc = nb_buf_tab(b, width);
		if (rc)
			changed_line = b->cur_line;
		break;
	case NB_KEY_UP:
	case NB_KEY_DOWN:
	{
		/* A HIDDEN LINE IS NOT SOMEWHERE THE CARET CAN BE: step over them (a
		 * bounded loop, so a buffer that is folded from top to bottom cannot
		 * spin here) */
		int step = (what == NB_KEY_UP) ? -1 : 1;
		int guard = 64;

		if (step < 0)
			nb_buf_up(b);
		else
			nb_buf_down(b);
		while (guard-- > 0 &&
		       nb_fold_hidden(&u->fold, b->cur_line)) {
			int before = b->cur_line;

			if (step < 0)
				nb_buf_up(b);
			else
				nb_buf_down(b);
			if (b->cur_line == before)
				break;		/* the end of the buffer */
		}
		break;
	}
	case NB_KEY_LEFT:
		nb_buf_left(b);
		break;
	case NB_KEY_RIGHT:
		nb_buf_right(b);
		break;
	case NB_KEY_HOME:
		nb_buf_home(b);
		break;
	case NB_KEY_END:
		nb_buf_end(b);
		break;
	case NB_KEY_PGUP:
		nb_buf_page(b, visible_rows(), -1);
		break;
	case NB_KEY_PGDN:
		nb_buf_page(b, visible_rows(), 1);
		break;
	case NB_KEY_WORD_L:
		nb_buf_word_left(b);
		break;
	case NB_KEY_WORD_R:
		nb_buf_word_right(b);
		break;
	case NB_KEY_ESC:
		/* BACK TO THE FILE LIST -- not out of the program.  The question has
		 * to carry that destination: asking "save before quit?" here meant
		 * that answering 'n' quit the editor instead of closing the file. */
		if (b->modified) {
			nb_ui_prompt_save(b->name, NB_THEN_BACK);
			return;
		}
		nb_ui_editor_close();
		nb_ui_enter_browser();
		nb_ui_message("文件列表", 0);
		return;
	case NB_KEY_MENU:
		nb_ui_editor_menu();
		return;
	case NB_KEY_HELP:
		u->show_help = !u->show_help;
		break;
	case NB_KEY_PALETTE:
		/* shift+A: the character table, and it takes the caret with it so the
		 * user can see where the characters will land */
		nb_ui_overlay_open(NB_OVER_PALETTE);
		break;
	case NB_KEY_SYMBOLS:
		/* shift+6: the quick symbol panel (it follows the file's language) */
		nb_ui_symbols_open();
		break;
	case NB_KEY_COPY:
		/* shift+VIEW: copy the selection, or the line the caret is on */
		nb_ui_editor_copy();
		break;
	case NB_KEY_CLIP:
		/* shift+MENU: the global clipboard */
		nb_ui_clip_open();
		break;
	case NB_KEY_FIND:
		/* SYMB: search */
		nb_ui_editor_find_begin();
		return;
	case NB_KEY_REPLACE:
		/* PLOT: replace */
		nb_ui_editor_replace_begin();
		return;
	case NB_KEY_ON:
		if (b->modified)
			nb_ui_prompt_save(b->name, NB_THEN_QUIT);
		else
			u->mode = NB_MODE_QUIT;
		return;
	default:
		return;
	}

	if (changed_line >= 0) {
		nb_ui_editor_changed(changed_line);
		if (complete && what == NB_KEY_BACKSPACE)
			nb_complete_update(&u->comp, b, nb_hl_lang(&u->hl));
	}
	u->dirty = 1;
}
