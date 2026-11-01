/*
 * nb_complete.c -- completion: the word the caret is in, and what it could be.
 *
 * WHAT IT OFFERS, and why nothing more clever: the CANDIDATES are
 *
 *   * the words of the language profile (keywords, control flow, the library
 *     names nb_highlight.c already carries for the colouring -- so a language
 *     brings its vocabulary along at no extra cost), and
 *   * the identifiers already in the open buffer, which is what makes a longer
 *     name in the same file reachable without typing it again;
 *
 * filtered by what is typed so far, de-duplicated, and sorted shortest first.
 * There is no parser and no scope analysis: a 32 MB calculator with a 40 column
 * screen needs "the name you already typed is three keys away", not an
 * understanding of the code.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: it never opens on a one character prefix
 * (the list would be the whole file), it does not re-open for the word it just
 * inserted, and it only ever runs while the caret is inside a word -- see
 * nb_complete_update().
 */
#include "nb_ui.h"
#include "nb_highlight.h"

/* how much of the buffer is scanned for identifiers, and how far the search for
 * one word reaches: both bounded, because this runs on every keystroke */
#define COMPLETE_LINES		600
#define COMPLETE_WORDS		4000
#define COMPLETE_MIN_PREFIX	1
#define COMPLETE_WORD_MAX	NB_COMPLETE_WORD

int nb_word_char(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

/* the word before the caret: its bytes, and how many there are */
int nb_word_prefix(nb_buf *b, char *out, int cap)
{
	int line, col, start, i, k = 0;
	const char *t;

	if (!b || cap < 1)
		return 0;
	line = b->cur_line;
	col = b->cur_col;
	if (line < 0 || line >= b->nlines)
		return 0;
	t = nb_buf_line(b, line);
	start = col;
	/* walk BACK over the word one CHARACTER at a time: the caret must not end
	 * up inside a multi-byte character, even though every language here has
	 * ASCII identifiers */
	while (start > 0) {
		int prev = nb_buf_prev_char(b, line, start);

		if (!nb_word_char((unsigned char)t[prev]))
			break;
		start = prev;
	}
	/* ... and copy it forwards */
	for (i = start; i < col && k < cap - 1; ) {
		int next = nb_buf_next_char(b, line, i);

		out[k++] = t[i];
		if (next <= i || next > col)
			break;
		i = next;
	}
	out[k] = 0;
	return k;
}

static int have(const nb_complete *c, const char *w)
{
	int i;

	for (i = 0; i < c->n; i++) {
		if (nb_strcmp(c->item[i], w) == 0)
			return 1;
	}
	return 0;
}

static int starts_with(const char *w, const char *prefix)
{
	int i;

	for (i = 0; prefix[i]; i++) {
		if (w[i] != prefix[i])
			return 0;
	}
	return 1;
}

/* insert `w` in the list if it is a longer word that starts with the prefix */
static void offer(nb_complete *c, const char *w, const char *prefix)
{
	int len = nb_strlen(w);
	int i;

	if (len == 0 || len >= COMPLETE_WORD_MAX)
		return;
	if (len <= nb_strlen(prefix))
		return;			/* nothing left to complete */
	if (!starts_with(w, prefix) || have(c, w))
		return;
	if (c->n >= NB_COMPLETE_MAX)
		return;
	nb_str_copy(c->item[c->n], COMPLETE_WORD_MAX, w, -1);
	c->n++;
}

/* the words of a space separated list (nb_highlight.c keeps its keywords that
 * way), offered one at a time */
static void offer_list(nb_complete *c, const char *list, const char *prefix)
{
	char w[COMPLETE_WORD_MAX];
	int n = 0, i;

	if (!list)
		return;
	for (i = 0; ; i++) {
		char ch = list[i];

		if (ch && ch != ' ' && ch != '\t' && ch != '\n') {
			if (n < COMPLETE_WORD_MAX - 1)
				w[n++] = ch;
			continue;
		}
		w[n] = 0;
		if (n)
			offer(c, w, prefix);
		n = 0;
		if (!ch)
			break;
	}
}

/* the identifiers in the buffer: a scan of the lines around the caret first (a
 * name used nearby is the likeliest), then the whole file up to the cap */
static void offer_buffer(nb_complete *c, const nb_buf *b, const char *prefix)
{
	char w[COMPLETE_WORD_MAX];
	int seen = 0, pass;

	for (pass = 0; pass < 2 && c->n < NB_COMPLETE_MAX; pass++) {
		int l;

		for (l = 0; l < b->nlines && l < COMPLETE_LINES; l++) {
			int line = (pass == 0)
				? b->cur_line + ((l & 1) ? -(l + 1) / 2 : l / 2)
				: l;
			const char *t;
			int len, i, n = 0;

			if (line < 0 || line >= b->nlines)
				continue;
			t = nb_buf_line(b, line);
			len = nb_buf_linelen(b, line);
			for (i = 0; i <= len; i++) {
				int ch = i < len ? (unsigned char)t[i] : 0;

				if (nb_word_char(ch)) {
					if (n < COMPLETE_WORD_MAX - 1)
						w[n++] = (char)ch;
					continue;
				}
				w[n] = 0;
				if (n) {
					if (nb_word_char((unsigned char)w[0]) &&
					    !(w[0] >= '0' && w[0] <= '9'))
						offer(c, w, prefix);
					seen++;
				}
				n = 0;
				if (seen > COMPLETE_WORDS)
					return;
			}
		}
	}
}

/* shortest first, then alphabetical: the likeliest word is at the top of a list
 * that is six rows tall */
static void sort(nb_complete *c)
{
	int i, j;

	for (i = 1; i < c->n; i++) {
		char key[COMPLETE_WORD_MAX];

		nb_str_copy(key, sizeof(key), c->item[i], -1);
		for (j = i - 1; j >= 0; j--) {
			int la = nb_strlen(c->item[j]), lb = nb_strlen(key);

			if (la < lb || (la == lb &&
					nb_strcmp(c->item[j], key) <= 0))
				break;
			nb_str_copy(c->item[j + 1], COMPLETE_WORD_MAX,
				    c->item[j], -1);
		}
		nb_str_copy(c->item[j + 1], COMPLETE_WORD_MAX, key, -1);
	}
}

void nb_complete_reset(nb_complete *c)
{
	if (!c)
		return;
	c->n = 0;
	c->sel = 0;
	c->prefix[0] = 0;
	c->prefix_len = 0;
	c->active = 0;
}

void nb_complete_close(nb_complete *c)
{
	if (!c)
		return;
	c->n = 0;
	c->active = 0;
}

/* Build the list for the word under the caret.  Returns how many candidates
 * there are (0 = nothing to offer, and the popup stays closed). */
int nb_complete_update(nb_complete *c, nb_buf *b, nb_lang lang)
{
	const nb_lang_profile *prof;
	int n;

	if (!c || !b)
		return 0;
	c->sel = 0;
	n = nb_word_prefix(b, c->prefix, (int)sizeof(c->prefix));
	c->prefix_len = n;
	if (n < COMPLETE_MIN_PREFIX) {
		c->n = 0;
		c->active = 0;
		return 0;
	}
	/* do not re-open for the word that was just accepted */
	if (c->accepted[0] && nb_strcmp(c->accepted, c->prefix) == 0) {
		c->n = 0;
		c->active = 0;
		return 0;
	}
	c->n = 0;
	prof = nb_lang_get(lang);
	if (prof) {
		offer_list(c, prof->keywords, c->prefix);
		offer_list(c, prof->control, c->prefix);
		offer_list(c, prof->constants, c->prefix);
		offer_list(c, prof->libs, c->prefix);
	}
	offer_buffer(c, b, c->prefix);
	sort(c);
	c->active = c->n > 0;
	return c->n;
}

int nb_complete_active(const nb_complete *c)
{
	return c && c->active && c->n > 0;
}

void nb_complete_move(nb_complete *c, int delta)
{
	if (!nb_complete_active(c))
		return;
	c->sel = (c->sel + delta + c->n) % c->n;
}
