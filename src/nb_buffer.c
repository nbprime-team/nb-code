/*
 * nb_buffer.c -- lines, cursor, undo, clipboard.
 *
 * The model is one heap line per line of text (see nb_buffer.h).  Everything an
 * edit does goes through line_insert/line_delete, so the undo records and the
 * dirty flag cannot be forgotten in one corner of the editor: a mutation that
 * bypasses them does not exist.
 *
 * NO realloc IS TRUSTED FOR ALIGNMENT.  The firmware heap only promises 4-byte
 * alignment, and the calculator is an ARM926EJ-S: an unaligned LDRD/STRD is a
 * Data Abort, which on this machine is a reset, not a crash report.  nb_line is
 * all ints and chars, so the compiler never emits them -- but nb_realloc is
 * also written (port/nb_mem.c) to keep the 4-byte promise explicit.
 */
#include "nb_buffer.h"
#include "nb_codec.h"

/* ---------------------------------------------------------------------------
 * primitives
 * ------------------------------------------------------------------------- */
static nb_line *line_new(int cap)
{
	nb_line *l;

	if (cap < 1)
		cap = 1;
	l = (nb_line *)nb_alloc(sizeof(nb_line) + (unsigned int)cap);
	if (!l)
		return NB_NULL;
	l->len = 0;
	l->cap = cap;
	l->flags = 0;
	l->text[0] = 0;
	return l;
}

static void line_free(nb_line *l)
{
	if (l)
		nb_free(l);
}

/* GROW THE LINE SO IT CAN HOLD `need` BYTES (the text and its NUL).
 *
 * THE POINTER BELONGS TO THE BUFFER, NOT TO THE CALLER.  nb_realloc may MOVE the
 * block, and every caller here holds a COPY of the old pointer (`l = b->line[i]`)
 * -- so a helper that kept its own copy and forgot to write the new one back left
 * the buffer pointing at FREED memory.  Nothing noticed while the text stayed
 * short: line_new() over-allocates by 64 bytes, and a line only grows past that
 * when something long arrives -- a paste, or a long line typed from the character
 * table.  Then the next allocation reuses the block and the editor reboots: a
 * pasted line longer than the line it lands in is exactly that.  This is why the
 * helper takes the BUFFER and the line's INDEX: there is only one place that
 * knows where the pointer lives.
 */
static int line_reserve(nb_buf *b, int at, int need)
{
	nb_line *l, *n;
	int cap;

	if (!b || at < 0 || at >= b->nlines)
		return NB_NO;
	l = b->line[at];
	if (need <= l->cap)
		return NB_YES;
	cap = l->cap;
	while (cap < need)
		cap = cap < 64 ? 64 : cap * 2;
	n = (nb_line *)nb_realloc(l, sizeof(nb_line) + (unsigned int)cap);
	if (!n)
		return NB_NO;
	n->cap = cap;
	b->line[at] = n;		/* THE pointer the buffer uses */
	return NB_YES;
}

/* put `len` bytes at `col` of line `at`, keeping the line NUL-terminated.  The
 * line may MOVE (see line_reserve), so it is re-read from the buffer afterwards
 * and the caller must re-read its own copy too. */
static int line_insert(nb_buf *b, int at, int col, const char *text, int len)
{
	nb_line *l;

	if (!b || at < 0 || at >= b->nlines)
		return len <= 0 ? NB_YES : NB_NO;
	if (len <= 0)
		return NB_YES;
	l = b->line[at];
	if (col < 0)
		col = 0;
	if (col > l->len)
		col = l->len;
	if (l->len + len > NB_MAX_LINE)
		len = NB_MAX_LINE - l->len;
	if (len <= 0)
		return NB_NO;
	if (!line_reserve(b, at, l->len + len + 1))
		return NB_NO;
	l = b->line[at];		/* it may have moved */
	/* memmove by hand: the region can overlap when the caller is inserting
	 * its own text back (undo) */
	{
		int i;

		for (i = l->len - 1; i >= col; i--)
			l->text[i + len] = l->text[i];
	}
	nb_memcpy(l->text + col, text, (unsigned int)len);
	l->len += len;
	l->text[l->len] = 0;
	return NB_YES;
}

static void line_delete(nb_line *l, int col, int len)
{
	int i;

	if (len <= 0)
		return;
	if (col < 0)
		col = 0;
	if (col + len > l->len)
		len = l->len - col;
	if (len <= 0)
		return;
	for (i = col; i + len < l->len; i++)
		l->text[i] = l->text[i + len];
	l->len -= len;
	l->text[l->len] = 0;
}

/* ---------------------------------------------------------------------------
 * the buffer
 * ------------------------------------------------------------------------- */
nb_buf *nb_buf_new(void)
{
	nb_buf *b = (nb_buf *)nb_alloc(sizeof(nb_buf));

	if (!b)
		return NB_NULL;
	nb_memset(b, 0, sizeof(nb_buf));
	b->cap = 64;
	b->line = (nb_line **)nb_alloc((unsigned int)b->cap * sizeof(nb_line *));
	if (!b->line) {
		nb_free(b);
		return NB_NULL;
	}
	nb_buf_clear(b);
	b->name[0] = 0;
	b->path[0] = 0;
	return b;
}

void nb_buf_clear(nb_buf *b)
{
	int i;

	for (i = 0; i < b->nlines; i++)
		line_free(b->line[i]);
	b->nlines = 0;
	b->nbytes = 0;
	b->cur_line = b->cur_col = 0;
	b->want_col = 0;
	b->top_line = b->left_col = 0;
	b->modified = NB_NO;
	b->encoding = NB_ENC_UTF8;
	b->eol = NB_EOL_LF;
	b->undo_top = b->undo_depth = 0;
	b->version++;
	b->line[0] = line_new(64);
	if (b->line[0])
		b->nlines = 1;
}

void nb_buf_free(nb_buf *b)
{
	int i;

	if (!b)
		return;
	for (i = 0; i < b->nlines; i++)
		line_free(b->line[i]);
	nb_free(b->line);
	if (b->clip)
		nb_free(b->clip);
	nb_free(b);
}

const char *nb_buf_line(const nb_buf *b, int i)
{
	if (i < 0 || i >= b->nlines)
		return "";
	return b->line[i]->text;
}

int nb_buf_linelen(const nb_buf *b, int i)
{
	if (i < 0 || i >= b->nlines)
		return 0;
	return b->line[i]->len;
}

void nb_buf_set_name(nb_buf *b, const char *name, const char *path)
{
	int i, last = 0;

	if (name) {
		for (i = 0; name[i]; i++) {
			if (name[i] == '\\' || name[i] == '/')
				last = i + 1;
		}
		nb_str_copy(b->name, sizeof(b->name), name + last, -1);
	}
	nb_str_copy(b->path, sizeof(b->path), path ? path : "", -1);
}

/* ---------------------------------------------------------------------------
 * growing the line array
 * ------------------------------------------------------------------------- */
static int lines_reserve(nb_buf *b, int need)
{
	int cap;
	nb_line **n;

	if (need <= b->cap)
		return NB_YES;
	cap = b->cap;
	while (cap < need)
		cap *= 2;
	n = (nb_line **)nb_realloc(b->line,
				   (unsigned int)cap * sizeof(nb_line *));
	if (!n)
		return NB_NO;
	b->line = n;
	b->cap = cap;
	return NB_YES;
}

/* insert an empty line at `at` (an existing line is not moved out of the way:
 * the caller has already handled its text) */
static nb_line *line_hole(nb_buf *b, int at, int cap)
{
	int i;
	nb_line *l;

	if (b->nlines >= NB_MAX_LINES)
		return NB_NULL;
	if (!lines_reserve(b, b->nlines + 1))
		return NB_NULL;
	l = line_new(cap);
	if (!l)
		return NB_NULL;
	for (i = b->nlines; i > at; i--)
		b->line[i] = b->line[i - 1];
	b->line[at] = l;
	b->nlines++;
	return l;
}

static void line_take_out(nb_buf *b, int at)
{
	int i;

	line_free(b->line[at]);
	for (i = at; i + 1 < b->nlines; i++)
		b->line[i] = b->line[i + 1];
	b->nlines--;
}

/* ---------------------------------------------------------------------------
 * undo
 * ------------------------------------------------------------------------- */
static void undo_push(nb_buf *b, int line, int col, int removed,
		      const char *replaced, int replaced_len,
		      int cur_line, int cur_col)
{
	nb_undo *u;

	if (b->undo_depth >= NB_UNDO_MAX) {
		/* full: drop the oldest.  An editor that silently forgets the
		 * first undo is worse than one that refuses the 129th. */
		int i;

		for (i = 1; i < NB_UNDO_MAX; i++)
			nb_memcpy(&b->undo[i - 1], &b->undo[i], sizeof(nb_undo));
		b->undo_depth--;
		b->undo_top = b->undo_depth;
	}
	u = &b->undo[b->undo_depth];
	u->line = line;
	u->col = col;
	u->removed = removed;
	u->replaced_len = replaced_len;
	if (replaced_len > NB_UNDO_TEXT)
		replaced_len = NB_UNDO_TEXT;
	if (replaced_len > 0 && replaced)
		nb_memcpy(u->replaced, replaced, (unsigned int)replaced_len);
	u->cursor_line = cur_line;
	u->cursor_col = cur_col;
	b->undo_depth++;
	b->undo_top = b->undo_depth;
}

int nb_buf_can_undo(const nb_buf *b)
{
	return b->undo_depth > 0;
}

int nb_buf_undo(nb_buf *b)
{
	nb_undo *u;
	nb_line *l;
	int i;

	if (b->undo_depth <= 0)
		return NB_NO;
	b->undo_depth--;
	u = &b->undo[b->undo_depth];

	/* remove what the edit inserted... */
	if (u->removed > 0) {
		int line = u->line;

		for (i = 0; i < u->removed && line < b->nlines; i++) {
			l = b->line[line];
			if (u->col < l->len) {
				line_delete(l, u->col, 1);
				b->nbytes--;
			} else if (line + 1 < b->nlines) {
				/* the tail of a multi-line edit: join */
				nb_line *nx = b->line[line + 1];
				int k;

				for (k = 0; k < nx->len; k++) {
					if (!line_insert(b, line,
							 b->line[line]->len,
							 nx->text + k, 1))
						break;
				}
				b->nbytes -= 1;
				line_take_out(b, line + 1);
			} else {
				break;
			}
		}
	}
	/* ...and put back what it displaced */
	if (u->replaced_len > 0 && u->line < b->nlines) {
		line_insert(b, u->line, u->col, u->replaced, u->replaced_len);
		b->nbytes += (unsigned int)u->replaced_len;
	}
	nb_buf_goto(b, u->cursor_line, u->cursor_col);
	b->modified = NB_YES;
	b->version++;
	return NB_YES;
}

/* ---------------------------------------------------------------------------
 * editing
 * ------------------------------------------------------------------------- */
static void touch(nb_buf *b)
{
	b->modified = NB_YES;
	b->version++;
}

int nb_buf_insert(nb_buf *b, const char *text, int len)
{
	nb_line *l;
	int i;

	if (len <= 0 || !text || b->readonly)
		return NB_NO;
	if (b->cur_line >= b->nlines)
		return NB_NO;
	l = b->line[b->cur_line];
	undo_push(b, b->cur_line, b->cur_col, len, NB_NULL, 0,
		  b->cur_line, b->cur_col);
	for (i = 0; i < len; i++) {
		/* RE-READ THE LINE EVERY TIME ROUND: an insert on the previous
		 * byte may have MOVED it (line_reserve), and the copy this loop
		 * used to keep was then a pointer into freed memory -- which is
		 * what a multi-line paste did, because a paste arrives as one
		 * nb_buf_insert call with newlines in it */
		l = b->line[b->cur_line];
		if (text[i] == '\n' || text[i] == '\r') {
			/* a pasted multi-line block: split, one line per \n */
			int rest = l->len - b->cur_col;
			nb_line *nl;

			if (!line_reserve(b, b->cur_line, b->cur_col + 1))
				break;
			l = b->line[b->cur_line];
			nl = line_hole(b, b->cur_line + 1, rest + 64);
			if (!nl)
				break;
			nb_memcpy(nl->text, l->text + b->cur_col,
				  (unsigned int)rest);
			nl->len = rest;
			nl->text[rest] = 0;
			l->len = b->cur_col;
			l->text[l->len] = 0;
			b->cur_line++;
			b->cur_col = 0;
			b->nbytes++;		/* one more line break */
			l = nl;
			if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')
				i++;
			continue;
		}
		if (line_insert(b, b->cur_line, b->cur_col, text + i, 1)) {
			b->cur_col++;
			b->nbytes++;
		}
	}
	b->want_col = b->cur_col;
	touch(b);
	return NB_YES;
}

int nb_buf_delete(nb_buf *b, int len)
{
	nb_line *l;
	int done = 0;

	if (len <= 0 || b->readonly || b->cur_line >= b->nlines)
		return NB_NO;
	l = b->line[b->cur_line];
	if (b->cur_col >= l->len) {
		/* delete at end of line joins with the next one */
		nb_line *nx;
		int k;

		if (b->cur_line + 1 >= b->nlines)
			return NB_NO;
		nx = b->line[b->cur_line + 1];
		undo_push(b, b->cur_line, b->cur_col, 1, NB_NULL, 0,
			  b->cur_line, b->cur_col);
		/* the whole tail in one call: the LINE goes away, so the byte count
		 * drops by exactly (its length + its newline) */
		for (k = 0; k < nx->len; k++) {
			if (!line_insert(b, b->cur_line,
					 b->line[b->cur_line]->len,
					 nx->text + k, 1))
				break;
		}
		line_take_out(b, b->cur_line + 1);
		b->nbytes -= 1;		/* the joined line's break */
		touch(b);
		return NB_YES;
	}
	{
		int n = len;

		if (n > l->len - b->cur_col)
			n = l->len - b->cur_col;
		undo_push(b, b->cur_line, b->cur_col, 0, l->text + b->cur_col, n,
			  b->cur_line, b->cur_col);
		line_delete(l, b->cur_col, n);
		b->nbytes -= (unsigned int)n;
		done = 1;
	}
	if (done)
		touch(b);
	return done;
}

/* ---------------------------------------------------------------------------
 * deleting a RANGE (a and b are (line, byte column) pairs, either order)
 *
 * THE SELECTION'S DELETE.  nb_buf_delete() removes one character (it is the Del
 * key), which is why cutting a three-line selection used to remove one byte and
 * leave the lines -- the range belongs here, where the undo record can be built
 * once for the whole edit instead of once per character.
 *
 * Returns: 0 nothing to do, 1 deleted (undoable), 2 deleted but NOT undoable
 * (the removed text is bigger than an undo record holds: NB_UNDO_TEXT bytes).
 * The caller should say so rather than pretending the cut can be undone.
 * ------------------------------------------------------------------------- */
int nb_buf_delete_range(nb_buf *b, int l0, int c0, int l1, int c1)
{
	int l, total = 0, fits;
	char text[NB_UNDO_TEXT];

	if (!b || b->readonly || b->nlines <= 0)
		return 0;
	/* sort: the anchor may be either end of the selection */
	if (l0 > l1 || (l0 == l1 && c0 > c1)) {
		int t = l0; l0 = l1; l1 = t;
		t = c0; c0 = c1; c1 = t;
	}
	if (l0 < 0)
		l0 = 0;
	if (l1 >= b->nlines)
		l1 = b->nlines - 1;
	if (l0 > l1)
		return 0;
	if (c0 > nb_buf_linelen(b, l0))
		c0 = nb_buf_linelen(b, l0);
	if (c1 > nb_buf_linelen(b, l1))
		c1 = nb_buf_linelen(b, l1);

	/* how much it is (the newlines between the lines included) */
	for (l = l0; l <= l1; l++) {
		int len = nb_buf_linelen(b, l);

		if (l == l0 && l == l1)
			total += c1 - c0;
		else if (l == l0)
			total += len - c0;
		else if (l == l1)
			total += c1;
		else
			total += len;
		if (l < l1)
			total += 1;
	}
	if (total <= 0)
		return 0;

	/* THE REMOVED TEXT, up to what one undo record holds */
	fits = (total <= NB_UNDO_TEXT);
	if (fits) {
		int n = 0;

		for (l = l0; l <= l1 && n < NB_UNDO_TEXT; l++) {
			const char *t = nb_buf_line(b, l);
			int len = nb_buf_linelen(b, l);
			int from = (l == l0) ? c0 : 0;
			int to = (l == l1) ? c1 : len;

			while (from < to && n < NB_UNDO_TEXT)
				text[n++] = t[from++];
			if (l < l1 && n < NB_UNDO_TEXT)
				text[n++] = '\n';	/* the joined line's break */
		}
	}

	/* delete it: from the end of the first line, one line at a time, so the
	 * newline handling is the same code the rest of the buffer uses */
	b->cur_line = l0;
	b->cur_col = c0;
	if (l0 == l1) {
		line_delete(b->line[l0], c0, c1 - c0);
		b->nbytes -= (unsigned int)(c1 - c0);
	} else {
		/* THE THREE PIECES, IN THIS ORDER, AND THE COUNT HAS TO MATCH.
		 *
		 * What goes is: the first line's TAIL, the lines strictly BETWEEN
		 * (newline and all), and the LAST line's head plus the newline that
		 * separated it from the one before.  That is exactly the `total`
		 * computed above, which is what the undo record holds and what the
		 * status bar's byte count is made of.
		 *
		 * This used to run its loop `l1 - l0` times -- one line TOO MANY --
		 * and then join `b->line[l0 + 1]`, which by then was the line AFTER
		 * the range (or past the end of the array when the range ended the
		 * file).  Deleting the first two lines of a two line buffer read a
		 * freed line and corrupted the heap: a test that cut a selection
		 * across lines brought it down as "corrupted double-linked list". */
		nb_line *first = b->line[l0];
		int between = l1 - l0 - 1;
		int keep = l0 + 1;
		int cut = first->len - c0;	/* the first line's tail */

		line_delete(first, c0, cut);
		b->nbytes -= (unsigned int)cut;
		/* the lines strictly between go, with their newlines */
		while (between-- > 0) {
			b->nbytes -= (unsigned int)b->line[keep]->len + 1u;
			line_take_out(b, keep);
		}
		/* the last line's head joins what is left of the first */
		{
			nb_line *nx = b->line[keep];
			int k;

			for (k = c1; k < nx->len; k++)
				line_insert(b, l0, b->line[l0]->len,
					    nx->text + k, 1);
			b->nbytes -= (unsigned int)c1 + 1u;
			line_take_out(b, keep);
		}
	}
	b->want_col = b->cur_col;
	touch(b);
	if (fits) {
		undo_push(b, l0, c0, 0, text, total, l0, c0);
		return 1;
	}
	/* TOO BIG TO UNDO, and saying so is better than a record that would
	 * restore the first 96 bytes and call it undone */
	b->undo_depth = 0;
	b->undo_top = 0;
	return 2;
}

/* DELETE UP TO `n` CHARACTERS BEFORE THE CURSOR, AS ONE UNDO STEP.
 *
 * The Del key uses this to take out a whole indent step at once (nb_ui_editor.c):
 * calling nb_buf_backspace() in a loop would leave one undo record per space, so
 * undoing the press would put the indentation back one character at a time. */
int nb_buf_backspace_n(nb_buf *b, int n)
{
	int start, end, steps = 0;

	if (!b || b->readonly || n <= 0 || b->cur_col <= 0)
		return NB_NO;
	start = b->cur_col;
	end = start;
	while (steps < n && end > 0) {
		end = nb_buf_prev_char(b, b->cur_line, end);
		steps++;
	}
	if (steps == 0 || end == start)
		return NB_NO;
	b->cur_col = end;			/* where the undo record points */
	b->want_col = end;
	return nb_buf_delete(b, start - end) ? NB_YES : NB_NO;
}

int nb_buf_backspace(nb_buf *b)
{
	if (b->readonly)
		return NB_NO;
	if (b->cur_col > 0) {
		int was = b->cur_col;
		int back = nb_buf_prev_char(b, b->cur_line, b->cur_col);

		b->cur_col = back;
		/* one character, which nb_buf_delete measures in BYTES: the
		 * difference between the new column and the old one */
		return nb_buf_delete(b, was - back) ? NB_YES : NB_NO;
	}
	if (b->cur_line > 0) {
		/* (probe) which branch and where the cursor is */
		/* Delete the line BREAK: the next line's text moves up to the end
		 * of the previous line.  nb_buf_delete does exactly that when the
		 * cursor sits at the end of a line with a next line to join, so
		 * the cursor is placed there first -- doing it by hand is how the
		 * first version of this managed to move the WRONG line (the text
		 * of the line the cursor was already on). */
		b->cur_line--;
		b->cur_col = nb_buf_linelen(b, b->cur_line);
		b->want_col = b->cur_col;
		return nb_buf_delete(b, 1) ? NB_YES : NB_NO;
	}
	return NB_NO;
}

/* Does a GB2312 character start at `i`?  Both bytes in the GB2312 rows and the
 * pair a real character -- the same test nb_buf_next_char's fallback uses, so
 * the two directions cannot disagree about where a character begins. */
static int gb_pair_at(const char *s, int len, int i)
{
	if (i < 0 || i + 1 >= len)
		return 0;
	if ((unsigned char)s[i] < 0xA1 || (unsigned char)s[i] > 0xFE)
		return 0;
	if ((unsigned char)s[i + 1] < 0xA1 || (unsigned char)s[i + 1] > 0xFE)
		return 0;
	return 1;
}

/* the byte index of the character before `col` on `line` */
int nb_buf_prev_char(const nb_buf *b, int line, int col)
{
	const char *s = nb_buf_line(b, line);
	int len = nb_buf_linelen(b, line);
	int i = col - 1;

	if (col <= 0)
		return 0;
	if (i >= len)
		i = len - 1;
	/* FROM THE END OF A CHARACTER: UTF-8 first.  Step over the continuation
	 * bytes to the lead byte -- one step of this loop consumes one
	 * continuation. */
	while (i > 0 && nb_buf_byte_is_cont((unsigned char)s[i]))
		i--;
	if (i > 0) {
		int n = nb_codec_utf8_seq((const unsigned char *)s + i - 1,
					  len - i + 1);

		if (n > 1 && i - 1 + n == i + 1)
			return i - 1;		/* s[i-1] starts a UTF-8 char
						 * that ENDS at i */
	}
	/* FROM THE MIDDLE OF A CHARACTER (or from a byte that no sequence
	 * explains): a GB2312 pair is two bytes and neither is a UTF-8
	 * continuation byte.  Preferring UTF-8 here swallowed the second byte of
	 * U+4E2D, whose first two bytes E4 B8 are a legal GB2312 pair. */
	if (i > 0 && gb_pair_at(s, len, i - 1))
		return i - 1;
	return i;
}

int nb_buf_next_char(const nb_buf *b, int line, int col)
{
	const char *s = nb_buf_line(b, line);
	int len = nb_buf_linelen(b, line);
	int n;

	if (col >= len)
		return len;
	/* UTF-8 first, and this order matters: in a UTF-8 file the bytes of
	 * U+4E2D are E4 B8 AD, and E4 B8 is a perfectly legal GB2312 pair -- a
	 * GB2312-first test would move the cursor one byte into a character.
	 * GB2312 is the fallback, for the pair a UTF-8 sequence cannot explain. */
	n = nb_codec_utf8_seq((const unsigned char *)s + col, len - col);
	if (n > 1)
		return col + n;
	if ((unsigned char)s[col] >= 0xA1 && (unsigned char)s[col] <= 0xFE &&
	    col + 1 < len && (unsigned char)s[col + 1] >= 0xA1 &&
	    (unsigned char)s[col + 1] <= 0xFE)
		return col + 2;
	return col + 1;
}

int nb_buf_byte_is_cont(unsigned char c)
{
	return (c & 0xC0) == 0x80;
}

int nb_buf_newline(nb_buf *b, const char *indent, int indent_len)
{
	if (b->readonly || b->cur_line >= b->nlines)
		return NB_NO;
	if (b->nlines >= NB_MAX_LINES)
		return NB_NO;
	{
		nb_line *l = b->line[b->cur_line];
		int rest = l->len - b->cur_col;
		nb_line *nl;

		undo_push(b, b->cur_line, b->cur_col, 1 + indent_len, NB_NULL, 0,
			  b->cur_line, b->cur_col);
		nl = line_hole(b, b->cur_line + 1, rest + indent_len + 64);
		if (!nl)
			return NB_NO;
		nb_memcpy(nl->text, l->text + b->cur_col, (unsigned int)rest);
		nl->len = rest;
		nl->text[rest] = 0;
		/* the new line carries the indentation the user's style asks
		 * for: auto-indent is a setting, so this is the only place that
		 * decides it */
		if (indent_len > 0)
			line_insert(b, b->cur_line + 1, 0, indent, indent_len);
		nl = b->line[b->cur_line + 1];	/* it may have moved */
		nl->len = rest + indent_len;
		nl->text[nl->len] = 0;
		l->len = b->cur_col;
		l->text[b->cur_col] = 0;
		b->cur_line++;
		b->cur_col = indent_len;
		b->want_col = indent_len;
		b->nbytes++;
		touch(b);
	}
	return NB_YES;
}

/* indent or outdent ONE LINE, by index: the line can move (line_reserve), so the
 * caller's copy of the pointer would go stale */
static int shift_line(nb_buf *b, int line, int dir, int width)
{
	nb_line *l = b->line[line];

	if (dir > 0) {
		int i;

		if (width <= 0)
			width = 1;
		for (i = 0; i < width; i++) {
			if (!line_insert(b, line, 0, " ", 1))
				return NB_NO;
		}
		return NB_YES;
	}
	/* outdent: remove up to `width` leading spaces, then one tab */
	{
		int n = 0;

		l = b->line[line];
		while (n < width && n < l->len && l->text[n] == ' ')
			n++;
		if (n == 0 && l->len > 0 && l->text[0] == '\t')
			n = 1;
		if (n > 0)
			line_delete(l, 0, n);
		return n > 0;
	}
}

int nb_buf_indent(nb_buf *b, int dir, int width)
{
	nb_line *l;

	if (b->readonly || b->cur_line >= b->nlines)
		return NB_NO;
	l = b->line[b->cur_line];
	undo_push(b, b->cur_line, b->cur_col, dir > 0 ? width : 0,
		  l->text, l->len, b->cur_line, b->cur_col);
	if (!shift_line(b, b->cur_line, dir, width))
		return NB_NO;
	l = b->line[b->cur_line];	/* the line may have moved */
	b->nbytes = (unsigned int)((int)b->nbytes +
				   (dir > 0 ? width : -(int)b->cur_col));
	if (b->cur_col > 0 || dir > 0)
		b->cur_col += dir > 0 ? width : 0;
	if (b->cur_col > l->len)
		b->cur_col = l->len;
	nb_buf_goto(b, b->cur_line, b->cur_col);
	touch(b);
	return NB_YES;
}

int nb_buf_tab(nb_buf *b, int width)
{
	int col, spaces;

	if (b->readonly || b->cur_line >= b->nlines)
		return NB_NO;
	if (width <= 0)
		width = 4;
	col = b->cur_col;
	spaces = width - (col % width);
	if (spaces <= 0)
		spaces = width;
	{
		char pad[8];
		int i;

		for (i = 0; i < spaces && i < 8; i++)
			pad[i] = ' ';
		return nb_buf_insert(b, pad, spaces);
	}
}

int nb_buf_kill_line(nb_buf *b)
{
	nb_line *l;

	if (b->readonly || b->cur_line >= b->nlines)
		return NB_NO;
	l = b->line[b->cur_line];
	if (l->len == 0) {
		/* an empty line: remove it, unless it is the only one */
		if (b->nlines <= 1)
			return NB_NO;
		undo_push(b, b->cur_line, 0, 1, NB_NULL, 0, b->cur_line, 0);
		line_take_out(b, b->cur_line);
		if (b->cur_line >= b->nlines)
			b->cur_line = b->nlines - 1;
		b->cur_col = 0;
		touch(b);
		return NB_YES;
	}
	undo_push(b, b->cur_line, 0, 0, l->text, l->len, b->cur_line, 0);
	b->nbytes -= (unsigned int)l->len;
	line_delete(l, 0, l->len);
	b->cur_col = 0;
	touch(b);
	return NB_YES;
}

int nb_buf_dup_line(nb_buf *b)
{
	nb_line *l, *nl;

	if (b->readonly || b->cur_line >= b->nlines)
		return NB_NO;
	l = b->line[b->cur_line];
	nl = line_hole(b, b->cur_line + 1, l->len + 64);
	if (!nl)
		return NB_NO;
	nb_memcpy(nl->text, l->text, (unsigned int)l->len);
	nl->len = l->len;
	nl->text[nl->len] = 0;
	b->cur_line++;
	b->nbytes += (unsigned int)l->len + 1;
	touch(b);
	return NB_YES;
}

int nb_buf_insert_line_text(nb_buf *b, int at, const char *text, int len)
{
	nb_line *l;

	if (at < 0 || at > b->nlines || len < 0)
		return NB_NO;
	if (len > NB_MAX_LINE)
		len = NB_MAX_LINE;
	l = line_new(len + 64);
	if (!l)
		return NB_NO;
	if (len > 0)
		nb_memcpy(l->text, text, (unsigned int)len);
	l->len = len;
	l->text[len] = 0;
	{
		int i;

		if (!lines_reserve(b, b->nlines + 1)) {
			line_free(l);
			return NB_NO;
		}
		for (i = b->nlines; i > at; i--)
			b->line[i] = b->line[i - 1];
		b->line[at] = l;
		b->nlines++;
	}
	b->nbytes += (unsigned int)len + 1;
	return NB_YES;
}

/* ---------------------------------------------------------------------------
 * clipboard
 * ------------------------------------------------------------------------- */
int nb_buf_clip_set(nb_buf *b, const char *text, int len)
{
	if (len < 0)
		return NB_NO;
	if (b->clip_cap < len + 1) {
		char *n = (char *)nb_realloc(b->clip, (unsigned int)len + 64);

		if (!n)
			return NB_NO;
		b->clip = n;
		b->clip_cap = len + 64;
	}
	if (len > 0 && text)
		nb_memcpy(b->clip, text, (unsigned int)len);
	b->clip[len] = 0;
	b->clip_len = len;
	return NB_YES;
}

int nb_buf_clip_get(const nb_buf *b, const char **text)
{
	if (b->clip_len <= 0 || !b->clip)
		return 0;
	*text = b->clip;
	return b->clip_len;
}

int nb_buf_copy_line(nb_buf *b)
{
	const char *s = nb_buf_line(b, b->cur_line);
	int len = nb_buf_linelen(b, b->cur_line);

	if (!nb_buf_clip_set(b, s, len))
		return NB_NO;
	{
		if (b->clip_cap >= b->clip_len + 2) {
			b->clip[b->clip_len] = '\n';
			b->clip[b->clip_len + 1] = 0;
			b->clip_len++;
		}
	}
	return NB_YES;
}

int nb_buf_cut_line(nb_buf *b)
{
	if (!nb_buf_copy_line(b))
		return NB_NO;
	return nb_buf_kill_line(b);
}

/* ---------------------------------------------------------------------------
 * movement
 * ------------------------------------------------------------------------- */
void nb_buf_goto(nb_buf *b, int line, int col)
{
	if (line < 0)
		line = 0;
	if (line >= b->nlines)
		line = b->nlines - 1;
	if (col < 0)
		col = 0;
	if (col > nb_buf_linelen(b, line))
		col = nb_buf_linelen(b, line);
	b->cur_line = line;
	b->cur_col = col;
	b->want_col = col;
}

void nb_buf_home(nb_buf *b)
{
	/* home goes to the first non-blank character, and to column 0 when it is
	 * already there -- the behaviour every editor converged on */
	const char *s = nb_buf_line(b, b->cur_line);
	int i = 0;

	while (s[i] == ' ' || s[i] == '\t')
		i++;
	b->cur_col = (b->cur_col == i) ? 0 : i;
	b->want_col = b->cur_col;
}

void nb_buf_end(nb_buf *b)
{
	b->cur_col = nb_buf_linelen(b, b->cur_line);
	b->want_col = b->cur_col;
}

void nb_buf_left(nb_buf *b)
{
	if (b->cur_col > 0) {
		b->cur_col = nb_buf_prev_char(b, b->cur_line, b->cur_col);
	} else if (b->cur_line > 0) {
		b->cur_line--;
		b->cur_col = nb_buf_linelen(b, b->cur_line);
	}
	b->want_col = b->cur_col;
}

void nb_buf_right(nb_buf *b)
{
	if (b->cur_col < nb_buf_linelen(b, b->cur_line)) {
		b->cur_col = nb_buf_next_char(b, b->cur_line, b->cur_col);
	} else if (b->cur_line + 1 < b->nlines) {
		b->cur_line++;
		b->cur_col = 0;
	}
	b->want_col = b->cur_col;
}

void nb_buf_up(nb_buf *b)
{
	if (b->cur_line > 0) {
		b->cur_line--;
		if (b->want_col > nb_buf_linelen(b, b->cur_line))
			b->cur_col = nb_buf_linelen(b, b->cur_line);
		else
			b->cur_col = b->want_col;
	}
}

void nb_buf_down(nb_buf *b)
{
	if (b->cur_line + 1 < b->nlines) {
		b->cur_line++;
		if (b->want_col > nb_buf_linelen(b, b->cur_line))
			b->cur_col = nb_buf_linelen(b, b->cur_line);
		else
			b->cur_col = b->want_col;
	}
}

void nb_buf_page(nb_buf *b, int rows, int dir)
{
	int i;

	if (rows < 1)
		rows = 1;
	for (i = 0; i < rows; i++) {
		if (dir > 0)
			nb_buf_down(b);
		else
			nb_buf_up(b);
	}
	/* a page keeps the cursor's SCREEN row: the viewport follows in
	 * nb_ui.c, which owns top_line */
}

static int word_at(const char *s, int len, int i)
{
	if (i >= len)
		return 0;
	return NB_ISWORD((unsigned char)s[i]);
}

void nb_buf_word_left(nb_buf *b)
{
	const char *s = nb_buf_line(b, b->cur_line);
	int len = nb_buf_linelen(b, b->cur_line);
	int i = b->cur_col;

	if (i == 0 && b->cur_line > 0) {
		b->cur_line--;
		s = nb_buf_line(b, b->cur_line);
		len = nb_buf_linelen(b, b->cur_line);
		i = len;
	}
	while (i > 0 && !word_at(s, len, i - 1))
		i--;
	while (i > 0 && word_at(s, len, i - 1))
		i = nb_buf_prev_char(b, b->cur_line, i);
	b->cur_col = i;
	b->want_col = i;
}

void nb_buf_word_right(nb_buf *b)
{
	const char *s = nb_buf_line(b, b->cur_line);
	int len = nb_buf_linelen(b, b->cur_line);
	int i = b->cur_col;

	if (i >= len && b->cur_line + 1 < b->nlines) {
		b->cur_line++;
		b->cur_col = 0;
		b->want_col = 0;
		return;
	}
	while (i < len && word_at(s, len, i))
		i = nb_buf_next_char(b, b->cur_line, i);
	while (i < len && !word_at(s, len, i))
		i = nb_buf_next_char(b, b->cur_line, i);
	b->cur_col = i;
	b->want_col = i;
}

/* ---------------------------------------------------------------------------
 * questions
 * ------------------------------------------------------------------------- */
int nb_buf_indent_of(const nb_buf *b, int line)
{
	const char *s = nb_buf_line(b, line);
	int i = 0, col = 0;

	while (s[i] == ' ' || s[i] == '\t') {
		col += (s[i] == '\t') ? 4 : 1;
		i++;
	}
	return col;
}

int nb_buf_is_blank(const nb_buf *b, int line)
{
	const char *s = nb_buf_line(b, line);
	int i;

	for (i = 0; s[i]; i++) {
		if (s[i] != ' ' && s[i] != '\t')
			return NB_NO;
	}
	return NB_YES;
}

/* ---------------------------------------------------------------------------
 * loading and saving
 * ------------------------------------------------------------------------- */
struct sink_ctx {
	nb_buf *b;
	int failed;
};

static int sink_line(void *ctx, const char *text, int len, int index)
{
	struct sink_ctx *s = (struct sink_ctx *)ctx;

	if (index == 0) {
		/* the first line replaces the empty line nb_buf_clear made, and
		 * that line has to be freed or every open would leak it (the
		 * emulator's leak test is what finds this kind of thing) */
		int k;

		for (k = 0; k < s->b->nlines; k++)
			nb_free(s->b->line[k]);
		s->b->nlines = 0;
		s->b->nbytes = 0;
	}
	if (!nb_buf_insert_line_text(s->b, s->b->nlines, text, len)) {
		s->failed = 1;
		return 0;
	}
	return 1;
}

int nb_buf_from_raw(nb_buf *b, const char *raw, int len, nb_encoding enc,
		    nb_eol eol)
{
	struct sink_ctx ctx;
	int bom = 0, lines = 0;
	int i;

	nb_buf_clear(b);
	b->encoding = enc;
	b->eol = eol;
	b->ends_with_newline = (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r'))
		? 1 : 0;
	b->had_wide = 0;
	for (i = 0; i < len; i++) {
		if ((unsigned char)raw[i] >= 0x80) {
			b->had_wide = 1;
			break;
		}
	}
	ctx.b = b;
	ctx.failed = 0;
	nb_codec_split(raw + bom, len - bom, sink_line, &ctx, enc, &lines);
	if (ctx.failed)
		return NB_NO;
	if (b->nlines == 0)
		nb_buf_clear(b);
	/* GB2312 text arrives as GB2312 bytes and is NORMALISED to UTF-8 in the
	 * buffer, so the cursor, the encoder and the renderer have one model
	 * (the save path re-encodes; see nb_buf_to_raw). */
	if (enc == NB_ENC_GB2312) {
		for (i = 0; i < b->nlines; i++) {
			nb_line *l = b->line[i];
			char *out = (char *)nb_alloc((unsigned int)NB_MAX_LINE * 3 + 2);
			int o = 0, k = 0;

			if (!out)
				return NB_NO;
			while (k < l->len) {
				if ((unsigned char)l->text[k] >= 0x81 &&
				    k + 1 < l->len) {
					unsigned int cp = nb_font_gb_unicode(
						(unsigned char)l->text[k],
						(unsigned char)l->text[k + 1]);
					char tmp[4];
					int n;

					if (cp != 0xFFFD) {
						n = nb_codec_encode(cp, tmp, 4);
						nb_memcpy(out + o, tmp, (unsigned int)n);
						o += n;
						k += 2;
						continue;
					}
				}
				out[o++] = l->text[k++];
				if (o >= NB_MAX_LINE * 3)
					break;
			}
			out[o] = 0;
			if (o <= l->cap) {
				nb_memcpy(l->text, out, (unsigned int)o + 1);
				l->len = o;
			} else {
				/* grew (rare): re-allocate the line */
				nb_line *n = (nb_line *)nb_realloc(
					l, sizeof(nb_line) + (unsigned int)o + 64);

				if (!n) {
					nb_free(out);
					return NB_NO;
				}
				b->line[i] = n;
				n->cap = o + 64;
				nb_memcpy(n->text, out, (unsigned int)o + 1);
				n->len = o;
			}
			nb_free(out);
		}
	}
	b->nbytes = 0;
	for (i = 0; i < b->nlines; i++)
		b->nbytes += (unsigned int)b->line[i]->len + 1;
	b->modified = NB_NO;
	b->undo_depth = b->undo_top = 0;
	b->version++;
	nb_buf_goto(b, 0, 0);
	return NB_YES;
}

int nb_buf_to_raw(const nb_buf *b, char **out, int *out_len, int *had_wide)
{
	int i, cap, used = 0, lossy_total = 0;
	char *buf;
	char *tmp;
	nb_encoding enc = b->encoding;

	if (had_wide)
		*had_wide = 0;
	/* WHAT ENCODING TO WRITE, and the rule is "the file's own, unless the
	 * edit produced text it cannot hold":
	 *
	 *   a UTF-8 file stays UTF-8 even if every character in it happens to be
	 *   representable in GB2312 (rewriting it would be silent damage to a
	 *   file the user never touched the encoding of);
	 *   a GB2312 file stays GB2312;
	 *   a file that was plain ASCII (or an empty one) and has just gained
	 *   Chinese is written as GB2312, because that is what every other tool
	 *   on this calculator can read.
	 *
	 * nb_codec_detect_text answers "is there Chinese here"; the file's own
	 * encoding decides the rest. */
	{
		int total = 0;
		char *probe;
		int has_wide = 0;

		for (i = 0; i < b->nlines; i++) {
			int k;

			total += b->line[i]->len + 2;
			for (k = 0; k < b->line[i]->len; k++) {
				if ((unsigned char)b->line[i]->text[k] >= 0x80)
					has_wide = 1;
			}
		}
		if (!has_wide) {
			enc = b->encoding;	/* pure ASCII: write it as it was */
		} else {
			probe = (char *)nb_alloc((unsigned int)(total > 0 ? total : 1));
			if (!probe)
				return NB_NO;
			{
				int o = 0;

				for (i = 0; i < b->nlines; i++) {
					if (b->line[i]->len > 0) {
						nb_memcpy(probe + o, b->line[i]->text,
							  (unsigned int)b->line[i]->len);
						o += b->line[i]->len;
					}
					probe[o++] = '\n';
				}
				enc = nb_codec_detect_text(probe, o);
			}
			nb_free(probe);
			if (b->encoding == NB_ENC_UTF8 && b->had_wide)
				enc = NB_ENC_UTF8;	/* never re-encode a UTF-8
							 * file behind the user */
			else if (b->encoding == NB_ENC_GB2312 &&
				 enc == NB_ENC_UTF8)
				enc = NB_ENC_GB2312;	/* ... nor a GB2312 one */
		}
	}

	cap = (int)b->nbytes * 3 + 16;
	buf = (char *)nb_alloc((unsigned int)cap);
	tmp = (char *)nb_alloc(NB_MAX_LINE * 3 + 8);
	if (!buf || !tmp) {
		if (buf)
			nb_free(buf);
		if (tmp)
			nb_free(tmp);
		return NB_NO;
	}

	for (i = 0; i < b->nlines; i++) {
		const nb_line *l = b->line[i];
		int n;

		n = nb_codec_line_to_bytes(l->text, l->len, enc, tmp,
					   NB_MAX_LINE * 3 + 4, &lossy_total);
		if (used + n + 2 > cap) {
			/* the file outgrew the estimate (a GB2312 write of UTF-8
			 * text is 2/3 the size, a UTF-8 write of pasted CJK can
			 * be 1.5x): grow */
			int ncap = cap * 2 + n + 16;
			char *nb = (char *)nb_realloc(buf, (unsigned int)ncap);

			if (!nb)
				break;
			buf = nb;
			cap = ncap;
		}
		nb_memcpy(buf + used, tmp, (unsigned int)n);
		used += n;
		if (i + 1 < b->nlines) {
			if (b->eol == NB_EOL_CRLF)
				buf[used++] = '\r';
			buf[used++] = '\n';
		}
	}
	/* the file's own final newline (see nb_buffer.h): restored here, so
	 * opening and saving an untouched file is byte for byte a no-op */
	if (b->ends_with_newline && b->nlines > 0) {
		if (used + 2 > cap) {
			char *nb = (char *)nb_realloc(buf, (unsigned int)(used + 8));

			if (!nb) {
				nb_free(tmp);
				return NB_NO;
			}
			buf = nb;
			cap = used + 8;
		}
		if (b->eol == NB_EOL_CRLF)
			buf[used++] = '\r';
		buf[used++] = '\n';
	}
	buf[used] = 0;
	nb_free(tmp);
	*out = buf;
	if (out_len)
		*out_len = used;
	if (had_wide)
		*had_wide = lossy_total;
	return NB_YES;
}

/* ---------------------------------------------------------------------------
 * column arithmetic for the UI: where a tab lands, and how wide a byte is
 * ------------------------------------------------------------------------- */
int nb_buf_col_advance(const nb_buf *b, int col, int tab_width)
{
	(void)b;
	if (tab_width < 1)
		tab_width = 4;
	return ((col / tab_width) + 1) * tab_width;
}
