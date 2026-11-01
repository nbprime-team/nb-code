/*
 * nb_find.c -- the matching (nb_find.h has the rules and the reasons).
 *
 * NO INDEX IS BUILT AND NOTHING IS CACHED: the file is at most 1 MB (NB_FILE_MAX)
 * and a search is a request from the user, not a per-frame cost, so the matcher
 * walks the lines whenever it is asked.  What it must NOT do is build a list of
 * matches: that would be an allocation whose size is the file's, on a machine
 * where the editor is already holding the text, the styles and the clipboard.
 */
#include "nb_find.h"

static char query[NB_FIND_MAX];
static int query_len;

void nb_find_clear(void)
{
	query[0] = 0;
	query_len = 0;
}

int nb_find_set(const char *text)
{
	int n = 0;

	if (text) {
		while (text[n] && n < NB_FIND_MAX - 1)
			n++;
	}
	if (n <= 0) {
		nb_find_clear();
		return 0;
	}
	nb_memcpy(query, text, (unsigned int)n);
	query[n] = 0;
	query_len = n;
	return 1;
}

const char *nb_find_text(void)
{
	return query;
}

int nb_find_active(void)
{
	return query_len > 0;
}

/* ASCII case folding, and only ASCII: a UTF-8 continuation byte is above 0x7F and
 * is left alone, so Chinese matches itself */
static int fold(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';
	return c;
}

/* does `line` contain the query at byte `at`? */
static int hit_at(const char *line, int len, int at)
{
	int i;

	if (at + query_len > len)
		return 0;
	for (i = 0; i < query_len; i++) {
		if (fold((unsigned char)line[at + i]) !=
		    fold((unsigned char)query[i]))
			return 0;
	}
	return 1;
}

/* the next match at or after (line, col), searching FORWARD; returns 0 at the end
 * of the file.  *index is the 0 based position of that match among all of them. */
static int scan_forward(nb_buf *b, int line, int col, int *out_line, int *out_col,
			int *index, int start_index)
{
	int l, at, n = start_index;

	if (!b || query_len <= 0)
		return 0;
	if (line < 0)
		line = 0;
	for (l = line; l < b->nlines; l++) {
		const char *t = nb_buf_line(b, l);
		int len = nb_buf_linelen(b, l);
		int from = (l == line) ? col : 0;

		if (from < 0)
			from = 0;
		for (at = from; at + query_len <= len; at++) {
			if (!hit_at(t, len, at))
				continue;
			if (out_line)
				*out_line = l;
			if (out_col)
				*out_col = at;
			if (index)
				*index = n;
			return 1;
			/* (one match per position: overlapping matches are not
			 * counted twice, so the scan does not skip `at += 1` by
			 * hand -- the loop's own step does it) */
		}
	}
	return 0;
}

/* how many matches there are before (line, col): that is the index of the match
 * found there, and it is what makes `k/n` possible without keeping a list */
static int count_before(nb_buf *b, int line, int col)
{
	int l, at, n = 0;

	for (l = 0; l <= line && l < b->nlines; l++) {
		const char *t = nb_buf_line(b, l);
		int len = nb_buf_linelen(b, l);
		int lim = (l == line) ? col : len;

		for (at = 0; at + query_len <= len && at < lim; at++) {
			if (!hit_at(t, len, at))
				continue;
			if (++n >= NB_FIND_COUNT_MAX)
				return n;
			at += query_len - 1;	/* no overlapping matches */
		}
	}
	return n;
}

int nb_find_line(nb_buf *b, int line, int *offs, int *lens, int max)
{
	const char *t;
	int len, at, n = 0;

	if (!b || query_len <= 0 || line < 0 || line >= b->nlines || max <= 0)
		return 0;
	t = nb_buf_line(b, line);
	len = nb_buf_linelen(b, line);
	for (at = 0; at + query_len <= len && n < max; at++) {
		if (!hit_at(t, len, at))
			continue;
		offs[n] = at;
		lens[n] = query_len;
		n++;
		at += query_len - 1;	/* no overlapping matches */
	}
	return n;
}

int nb_find_count(nb_buf *b)
{
	int l, at, n = 0;

	if (!b || query_len <= 0)
		return 0;
	for (l = 0; l < b->nlines; l++) {
		const char *t = nb_buf_line(b, l);
		int len = nb_buf_linelen(b, l);

		for (at = 0; at + query_len <= len; at++) {
			if (!hit_at(t, len, at))
				continue;
			if (++n >= NB_FIND_COUNT_MAX)
				return n;
			at += query_len - 1;
		}
	}
	return n;
}

int nb_find_at(nb_buf *b, int n, int *line, int *col)
{
	int l, at, k = 0;

	if (!b || query_len <= 0 || n < 0)
		return 0;
	for (l = 0; l < b->nlines; l++) {
		const char *t = nb_buf_line(b, l);
		int len = nb_buf_linelen(b, l);

		for (at = 0; at + query_len <= len; at++) {
			if (!hit_at(t, len, at))
				continue;
			if (k == n) {
				if (line)
					*line = l;
				if (col)
					*col = at;
				return 1;
			}
			k++;
			at += query_len - 1;
		}
	}
	return 0;
}

int nb_find_current(nb_buf *b, int *line, int *col, int *index)
{
	int l = 0, c = 0, idx = 0;

	if (!b || query_len <= 0)
		return 0;
	/* A MATCH UNDER THE CARET IS THE CURRENT ONE: the caret sits at the start of
	 * the match the last jump made, so "replace" and "next" agree about where
	 * the user is. */
	{
		const char *t = nb_buf_line(b, b->cur_line);
		int len = nb_buf_linelen(b, b->cur_line);

		if (hit_at(t, len, b->cur_col)) {
			l = b->cur_line;
			c = b->cur_col;
			idx = count_before(b, l, c);
		} else if (!scan_forward(b, b->cur_line, b->cur_col + 1, &l, &c,
					 &idx, 0)) {
			return 0;
		} else {
			idx = count_before(b, l, c);
		}
	}
	if (line)
		*line = l;
	if (col)
		*col = c;
	if (index)
		*index = idx;
	return 1;
}

int nb_find_step(nb_buf *b, int dir, int *line, int *col, int *index)
{
	int l = 0, c = 0, idx = 0;
	int total;

	if (!b || query_len <= 0)
		return 0;
	total = nb_find_count(b);
	if (total <= 0)
		return 0;
	if (dir >= 0) {
		/* from the match under the caret, or the first one after it */
		int cur_l = 0, cur_c = 0, cur_i = 0;

		if (nb_find_current(b, &cur_l, &cur_c, &cur_i)) {
			/* the NEXT one: one byte past the start of the current */
			if (!scan_forward(b, cur_l, cur_c + 1, &l, &c, NB_NULL,
					  0)) {
				/* wrap: the first match in the file */
				if (!nb_find_at(b, 0, &l, &c))
					return 0;
			}
		} else if (!nb_find_at(b, 0, &l, &c)) {
			return 0;
		}
	} else {
		/* THE PREVIOUS ONE, and the only cheap way is to walk the file: the
		 * count is capped, so the index cannot be used to seek */
		int prev_l = -1, prev_c = -1;

		if (nb_find_current(b, &l, &c, NB_NULL)) {
			/* is the caret ON a match?  then "previous" means the one
			 * before it, otherwise the last one before the caret */
			const char *t = nb_buf_line(b, b->cur_line);
			int len = nb_buf_linelen(b, b->cur_line);
			int stop_l = b->cur_line;
			int stop_c = hit_at(t, len, b->cur_col) ? b->cur_col
							       : b->cur_col + 1;
			int ll, at;

			for (ll = 0; ll <= stop_l && ll < b->nlines; ll++) {
				const char *lt = nb_buf_line(b, ll);
				int llen = nb_buf_linelen(b, ll);

				for (at = 0; at + query_len <= llen; at++) {
					if (!hit_at(lt, llen, at))
						continue;
					if (ll == stop_l && at >= stop_c)
						break;
					prev_l = ll;
					prev_c = at;
					at += query_len - 1;
				}
			}
			if (prev_l < 0) {
				/* wrap: the LAST match in the file */
				int n = nb_find_count(b);

				if (n <= 0 || !nb_find_at(b, n - 1, &prev_l,
							  &prev_c))
					return 0;
			}
			l = prev_l;
			c = prev_c;
		} else if (!nb_find_at(b, total - 1, &l, &c)) {
			return 0;
		}
	}
	idx = count_before(b, l, c);
	if (line)
		*line = l;
	if (col)
		*col = c;
	if (index)
		*index = idx;
	return 1;
}

int nb_find_replace(nb_buf *b, const char *rep, int *out_line, int *out_col)
{
	const char *t;
	int len, rlen = 0, next_l = 0, next_c = 0;

	if (!b || query_len <= 0 || b->readonly)
		return 0;
	/* THE MATCH HAS TO BE UNDER THE CARET: replacing "the next match" would
	 * silently edit something the user has not looked at */
	t = nb_buf_line(b, b->cur_line);
	len = nb_buf_linelen(b, b->cur_line);
	if (!hit_at(t, len, b->cur_col))
		return 0;
	if (rep)
		rlen = (int)nb_strlen(rep);
	/* one undo record: delete the match and insert the replacement in its place */
	if (!nb_buf_delete_range(b, b->cur_line, b->cur_col, b->cur_line,
				 b->cur_col + query_len))
		return 0;
	nb_buf_goto(b, b->cur_line, b->cur_col);
	if (rlen > 0 && !nb_buf_insert(b, rep, rlen))
		return 0;
	/* ... and where the NEXT match is now: the same line, after what went in */
	if (scan_forward(b, b->cur_line, b->cur_col + rlen, &next_l, &next_c,
			 NB_NULL, 0)) {
		if (out_line)
			*out_line = next_l;
		if (out_col)
			*out_col = next_c;
	} else if (nb_find_at(b, 0, &next_l, &next_c)) {
		if (out_line)
			*out_line = next_l;
		if (out_col)
			*out_col = next_c;
	} else {
		if (out_line)
			*out_line = b->cur_line;
		if (out_col)
			*out_col = b->cur_col;
	}
	return 1;
}

int nb_find_replace_all(nb_buf *b, const char *rep)
{
	int done = 0, l = 0;
	int rlen = rep ? (int)nb_strlen(rep) : 0;

	if (!b || query_len <= 0 || b->readonly)
		return 0;
	/* FROM THE END BACKWARDS, so the line/column of the matches still to come
	 * are not moved by the replacement.  Every match is its own undo record: a
	 * "replace all" that cannot be taken back one step at a time is a trap. */
	while (l < b->nlines) {
		const char *t = nb_buf_line(b, l);
		int len = nb_buf_linelen(b, l);
		int at = 0;

		while (at + query_len <= len) {
			if (!hit_at(t, len, at)) {
				at++;
				continue;
			}
			if (nb_buf_delete_range(b, l, at, l, at + query_len)) {
				nb_buf_goto(b, l, at);
				if (rlen > 0)
					nb_buf_insert(b, rep, rlen);
				done++;
			}
			t = nb_buf_line(b, l);
			len = nb_buf_linelen(b, l);
			at += rlen > 0 ? rlen : 0;
			if (rlen == 0 && at > len)
				break;
			if (rlen > 0 && at + query_len <= len &&
			    hit_at(t, len, at) == 0)
				at++;
		}
		l++;
	}
	return done;
}
