/*
 * nb_find.h -- SEARCH AND REPLACE.
 *
 * What the editor offers: SYMB asks for a search text, PLOT asks for the
 * replacement, and the header shows `k/n` -- which match of how many -- together
 * with two buttons that step to the previous and the next one.  This file is the
 * matching itself: no drawing, no keys, no UI state, so the whole of it is
 * testable on its own (the panel that draws the count lives in nb_ui_editor.c).
 *
 * THE RULES, once:
 *
 *   * plain text, CASE INSENSITIVE for ASCII (`if` finds `IF`; a Chinese search
 *     text is compared byte for byte, and the bytes never fold);
 *   * a match is a run of bytes inside ONE line: the query never spans a line
 *     break, because a query with a newline in it cannot be typed into the
 *     editor's one line input anyway;
 *   * matches do not overlap, and they are counted in file order;
 *   * searching wraps around the end of the file, which is what "next" means when
 *     the caret is already on the last match.
 *
 * THE COUNT IS CAPPED: a search for a single space in a megabyte of code would be
 * tens of thousands of matches, and nobody reads `48213/99876`.  The cap is
 * reported as "999+" and the jumping still works.
 */
#ifndef NB_FIND_H
#define NB_FIND_H

#include "nb_common.h"
#include "nb_buffer.h"

#define NB_FIND_MAX	64		/* bytes of the search text */
#define NB_FIND_COUNT_MAX 999		/* the count stops here ("999+") */

void nb_find_clear(void);
int nb_find_set(const char *text);		/* 1 when there is something to find */
const char *nb_find_text(void);
int nb_find_active(void);

/* where the match at `n` (0 based, in file order) starts, or 0 when there is none */
int nb_find_at(nb_buf *b, int n, int *line, int *col);

/* THE MATCHES THAT START ON ONE LINE, as byte-offset/length pairs.  This is what
 * the editor's highlight is drawn from: it needs the spans of the lines ON THE
 * SCREEN only, and asking line by line is what keeps that cheap (no list of every
 * match in the file is ever built -- see nb_find.c).  Returns how many were
 * written, up to `max`. */
int nb_find_line(nb_buf *b, int line, int *offs, int *lens, int max);

/* how many matches the file has (capped at NB_FIND_COUNT_MAX) */
int nb_find_count(nb_buf *b);

/* the match that contains the caret, or the first one after it; 0 when the file
 * has none.  *index receives its position (0 based). */
int nb_find_current(nb_buf *b, int *line, int *col, int *index);

/* the next (dir > 0) or previous match from the caret, wrapping; 1 when found */
int nb_find_step(nb_buf *b, int dir, int *line, int *col, int *index);

/* REPLACE.  `rep` is the text to put there (empty deletes the match).  Returns 1
 * and sets *out_line/*out_col to the start of the NEXT match when one was
 * replaced, 0 when the caret is not on a match. */
int nb_find_replace(nb_buf *b, const char *rep, int *out_line, int *out_col);

/* every match, in one undo record each; returns how many were replaced */
int nb_find_replace_all(nb_buf *b, const char *rep);

#endif /* NB_FIND_H */
