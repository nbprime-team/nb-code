/*
 * nb_fold.c -- code folding, and the bracket matcher the editor shares with it.
 *
 * WHAT CAN BE FOLDED, and it is the same question in three shapes:
 *
 *   BRACKETS    a line with an opener whose partner is on a LATER line folds
 *               everything up to that partner.  This is the C/JS/Java case and
 *               it covers Lua tables and Python literals as well, because the
 *               scan is the bracket matcher and not a language rule.
 *   KEYWORDS    Lua and PPL close their blocks with a word (`end`, `END`),
 *               so `function`, `do`, `if ... then`, `while`, `for`, `repeat`
 *               and the PPL `BEGIN` fold to the matching `end`/`END`/`until`.
 *   INDENTATION Python has no closing token at all: a line ending in `:`
 *               folds the following lines that are indented further than it.
 *
 * A KEYWORD INSIDE A STRING OR A COMMENT WOULD FOOL THE KEYWORD SCAN, and that is
 * accepted: this is a folding aid on a 40 column screen, not a parser, and the
 * cost of being wrong is one tap to unfold.  The bracket scan has no such problem
 * -- it counts nesting -- which is why it is tried first.
 *
 * THE SCAN IS BOUNDED (FOLD_SCAN lines): folding a 10,000 line file must not cost
 * a full pass when the caret's line is folded, and an unclosed brace has to give
 * up rather than run to the end of the buffer.
 */
#include "nb_fold.h"
#include "nb_ui.h"

#define FOLD_SCAN	4000

/* ---------------------------------------------------------------------------
 * the bracket matcher (also used by the editor's pair highlighting)
 * ------------------------------------------------------------------------- */
static int is_open(int c)
{
	return c == '(' || c == '[' || c == '{';
}

static int is_close(int c)
{
	return c == ')' || c == ']' || c == '}';
}

static int partner(int c)
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

int nb_fold_bracket(nb_buf *b, int line, int col)
{
	int len;

	if (!b || line < 0 || line >= b->nlines)
		return 0;
	len = nb_buf_linelen(b, line);
	if (col < 0 || col >= len)
		return 0;
	{
		int c = (unsigned char)nb_buf_line(b, line)[col];

		return (is_open(c) || is_close(c)) ? c : 0;
	}
}

int nb_fold_match(nb_buf *b, int line, int col, int *out_line, int *out_col)
{
	int start = nb_fold_bracket(b, line, col);
	int want, depth = 0, guard = FOLD_SCAN;
	int forward;

	if (!start)
		return 0;
	want = partner(start);
	forward = is_open(start) ? 1 : 0;
	while (guard-- > 0) {
		int len, c;

		/* one character at a time, over the lines */
		if (forward) {
			len = nb_buf_linelen(b, line);
			if (col < len)
				col = nb_buf_next_char(b, line, col);
			else if (line + 1 < b->nlines)
				line++, col = 0;
			else
				return 0;
		} else {
			if (col > 0)
				col = nb_buf_prev_char(b, line, col);
			else if (line > 0)
				line--, col = nb_buf_linelen(b, line);
			else
				return 0;
		}
		len = nb_buf_linelen(b, line);
		if (col >= len)
			continue;		/* the newline between two lines */
		c = (unsigned char)nb_buf_line(b, line)[col];
		if (c == start) {
			depth++;
			continue;
		}
		if (c != want)
			continue;
		if (depth == 0) {
			if (out_line)
				*out_line = line;
			if (out_col)
				*out_col = col;
			return 1;
		}
		depth--;
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * what a line opens
 * ------------------------------------------------------------------------- */
static int word_at(const char *t, int len, int at, const char *w)
{
	int i, n = nb_strlen(w);

	if (at < 0 || at + n > len)
		return 0;
	for (i = 0; i < n; i++) {
		char a = t[at + i], b = w[i];

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (char)(b - 'A' + 'a');
		if (a != b)
			return 0;
	}
	/* a word, not part of a longer one */
	if (at > 0) {
		char p = t[at - 1];

		if ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') ||
		    (p >= '0' && p <= '9') || p == '_')
			return 0;
	}
	if (at + n < len) {
		char p = t[at + n];

		if ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') ||
		    (p >= '0' && p <= '9') || p == '_')
			return 0;
	}
	return 1;
}

/* the last word of a line, and where it starts (-1 when there is none) */
static int last_word(const char *t, int len, int *at, int *n)
{
	int i = len - 1, end;

	while (i >= 0 && (t[i] == ' ' || t[i] == '\t' || t[i] == ';' ||
			  t[i] == ')' || t[i] == '"' || t[i] == '\''))
		i--;
	if (i < 0)
		return 0;
	end = i;
	while (i >= 0 && ((t[i] >= 'a' && t[i] <= 'z') ||
			  (t[i] >= 'A' && t[i] <= 'Z') ||
			  (t[i] >= '0' && t[i] <= '9') || t[i] == '_'))
		i--;
	*at = i + 1;
	*n = end - i;
	return *n > 0;
}

/* Lua and PPL: a line that opens a block with a word, and the word that closes
 * it.  THE OPENER IS LOOKED FOR ANYWHERE ON THE LINE, not as the last word: a Lua
 * block is written `function f(a, b)`, `for i = 1, 10 do`, `if x then` -- the last
 * word of those is `b)`, `do` and `then`.  Word boundaries are respected, so
 * `do_something()` is not a `do`.
 *
 * A KEYWORD INSIDE A STRING OR A COMMENT FOOLS THIS (see the file's header): one
 * tap unfolds it.  Counting nesting is what keeps the common case right. */
static int opens_block(const char *t, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (word_at(t, len, i, "function") ||
		    word_at(t, len, i, "do") ||
		    word_at(t, len, i, "then") ||
		    word_at(t, len, i, "repeat") ||
		    word_at(t, len, i, "BEGIN") ||
		    word_at(t, len, i, "REPEAT"))
			return 1;
	}
	return 0;
}

static int closes_block(const char *t, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (word_at(t, len, i, "end") || word_at(t, len, i, "END") ||
		    word_at(t, len, i, "until") || word_at(t, len, i, "UNTIL"))
			return 1;
	}
	return 0;
}

static int keyword_end(nb_buf *b, int line, int lang)
{
	const char *t = nb_buf_line(b, line);
	int len = nb_buf_linelen(b, line);
	int depth = 1, l;

	if (!opens_block(t, len))
		return -1;
	/* a line that opens AND closes (`end` on the same line) is not a block */
	if (closes_block(t, len))
		return -1;
	for (l = line + 1; l < b->nlines && l - line < FOLD_SCAN; l++) {
		const char *u = nb_buf_line(b, l);
		int ulen = nb_buf_linelen(b, l);

		if (closes_block(u, ulen) && --depth == 0)
			return l;
		if (opens_block(u, ulen))
			depth++;
	}
	(void)lang;
	return -1;
}

/* Python: a line ending in ':' folds the following lines that are indented
 * further than it (blank lines inside the block do not end it) */
static int indent_end(nb_buf *b, int line)
{
	const char *t = nb_buf_line(b, line);
	int len = nb_buf_linelen(b, line);
	int indent = 0, last = -1, l;

	while (indent < len && (t[indent] == ' ' || t[indent] == '\t'))
		indent++;
	if (len == 0 || t[len - 1] != ':')
		return -1;
	for (l = line + 1; l < b->nlines && l - line < FOLD_SCAN; l++) {
		const char *u = nb_buf_line(b, l);
		int ulen = nb_buf_linelen(b, l);
		int ui = 0;

		while (ui < ulen && (u[ui] == ' ' || u[ui] == '\t'))
			ui++;
		if (ui >= ulen) {
			last = l;		/* a blank line: still inside */
			continue;
		}
		if (ui <= indent)
			break;
		last = l;
	}
	return last;
}

int nb_fold_region_end(nb_buf *b, int line, nb_lang lang)
{
	int len, i, mline = 0, mcol = 0;

	if (!b || line < 0 || line >= b->nlines)
		return -1;
	len = nb_buf_linelen(b, line);

	/* 1. brackets: the first opener on the line whose partner is below it */
	for (i = 0; i < len; i++) {
		int c = (unsigned char)nb_buf_line(b, line)[i];

		if (!is_open(c))
			continue;
		if (nb_fold_match(b, line, i, &mline, &mcol) && mline > line)
			return mline;
	}
	/* 2. the keyword blocks (Lua, PPL) */
	if (lang == NB_LANG_LUA || lang == NB_LANG_PPL) {
		int e = keyword_end(b, line, (int)lang);

		if (e > line)
			return e;
	}
	/* 3. Python's indentation blocks */
	if (lang == NB_LANG_PY) {
		int e = indent_end(b, line);

		if (e > line)
			return e;
	}
	return -1;
}

/* ---------------------------------------------------------------------------
 * the fold list
 * ------------------------------------------------------------------------- */
void nb_fold_clear(nb_fold *f)
{
	if (f)
		f->n = 0;
}

/* HOW MANY TIMES the fold list was asked about a line: the row arithmetic in the
 * editor is supposed to be O(folds) per call and not O(lines x folds), and this
 * counter is how a test says so without measuring time (see nb_fold_probes in
 * nb_fold.h). */
unsigned long nb_fold_probes;

int nb_fold_hidden(const nb_fold *f, int line)
{
	int i;

	nb_fold_probes++;
	if (!f)
		return 0;
	for (i = 0; i < f->n; i++) {
		if (line > f->line[i] && line <= f->end[i])
			return 1;
	}
	return 0;
}

int nb_fold_open_at(const nb_fold *f, int line)
{
	int i;

	if (!f)
		return 0;
	for (i = 0; i < f->n; i++) {
		if (f->line[i] == line)
			return 1;
	}
	return 0;
}

/* UNFOLD WHATEVER HIDES `line`.  Folding does not move the caret (a tap on a line
 * number is not a request to go anywhere), so the caret CAN end up inside a fold
 * -- and an edit there has to be visible: this is what makes it. */
int nb_fold_reveal(nb_fold *f, int line)
{
	int i, went = 0;

	if (!f)
		return 0;
	for (i = 0; i < f->n; ) {
		int k;

		if (!(line > f->line[i] && line <= f->end[i])) {
			i++;
			continue;
		}
		for (k = i; k + 1 < f->n; k++) {
			f->line[k] = f->line[k + 1];
			f->end[k] = f->end[k + 1];
		}
		f->n--;
		went = 1;
	}
	return went;
}

int nb_fold_toggle(nb_fold *f, nb_buf *b, int line, nb_lang lang)
{
	int i, end;

	if (!f || !b)
		return 0;
	/* closing it?  BY SHIFTING, NOT BY SWAPPING THE LAST ONE IN: the list has to
	 * stay sorted (see the insert below and line_of_row in nb_ui_editor.c) */
	for (i = 0; i < f->n; i++) {
		if (f->line[i] == line) {
			int k;

			for (k = i; k + 1 < f->n; k++) {
				f->line[k] = f->line[k + 1];
				f->end[k] = f->end[k + 1];
			}
			f->n--;
			return 1;
		}
	}
	end = nb_fold_region_end(b, line, lang);
	if (end <= line)
		return 0;
	/* NO TWO FOLDS MAY OVERLAP.  The editor counts hidden lines by adding up the
	 * ranges (see hidden_before() in nb_ui_editor.c), and a line hidden by two
	 * folds would be counted twice -- the rows and the line numbers would drift
	 * apart by one for every line of the overlap.  Folding a block that contains
	 * an already folded one (or is contained by one) is rare, but it costs one
	 * pass here to make the invariant true by construction: the fold the user
	 * just asked for wins, and the ones it swallows go. */
	for (i = 0; i < f->n; ) {
		int k;

		if (!(f->line[i] <= end && f->end[i] >= line)) {
			i++;
			continue;
		}
		for (k = i; k + 1 < f->n; k++) {
			f->line[k] = f->line[k + 1];
			f->end[k] = f->end[k + 1];
		}
		f->n--;
	}
	if (f->n >= NB_FOLD_MAX)
		return 0;
	/* INSERTED IN ORDER: the editor's row arithmetic walks this list instead of
	 * the file (see line_of_row in nb_ui_editor.c), and that only works if the
	 * ranges are sorted by their first line.  The overlap pass above leaves the
	 * rest of the list in order, so one shift is all it takes. */
	{
		int at = f->n;

		while (at > 0 && f->line[at - 1] > line) {
			f->line[at] = f->line[at - 1];
			f->end[at] = f->end[at - 1];
			at--;
		}
		f->line[at] = line;
		f->end[at] = end;
	}
	f->n++;
	return 1;
}
