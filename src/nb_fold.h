/*
 * nb_fold.h -- code folding: which line starts a block, and what a folded line hides.
 *
 * The state is a small list of FOLDED lines (their end lines included, so the
 * renderer does not have to re-scan on every frame).  A fold is dropped when the
 * line count changes: an edit that adds or removes a line moves every fold below
 * it, and re-folding is one tap -- hiding the wrong lines is a bug the user cannot
 * even see.
 */
#ifndef NB_FOLD_H
#define NB_FOLD_H

#include "nb_common.h"
#include "nb_buffer.h"

#define NB_FOLD_MAX	24

typedef struct nb_fold {
	int line[NB_FOLD_MAX];		/* the first line of the folded block */
	int end[NB_FOLD_MAX];		/* its last line (inclusive, and hidden) */
	int n;
} nb_fold;

/* THE BRACKET MATCHER, shared with the editor's pair highlighting: from the
 * bracket at (line, col), find its partner (nesting of the same kind included).
 * Returns 1 and fills out_line/out_col, or 0 when there is no partner. */
int nb_fold_match(nb_buf *b, int line, int col, int *out_line, int *out_col);
/* which bracket character is at (line, col), or 0 */
int nb_fold_bracket(nb_buf *b, int line, int col);

/* the last line a fold starting at `line` would hide, or -1 when the line cannot
 * be folded.  Language aware: brackets for every language, `function/do/... end`
 * for Lua and PPL, indentation for Python. */
int nb_fold_region_end(nb_buf *b, int line, nb_lang lang);

void nb_fold_clear(nb_fold *f);
/* is this line hidden by a fold that is closed? (the fold's own first line is
 * always visible: it is what carries the marker) */
/* every nb_fold_hidden() call, counted: a test can then check the COMPLEXITY of a
 * code path (the editor's row arithmetic must not walk the file line by line)
 * without measuring time, which is not reproducible on a host. */
extern unsigned long nb_fold_probes;

int nb_fold_hidden(const nb_fold *f, int line);
/* is this line the first line of a folded block? */
int nb_fold_open_at(const nb_fold *f, int line);
/* fold or unfold the block at `line`; returns 1 when the state changed */
int nb_fold_toggle(nb_fold *f, nb_buf *b, int line, nb_lang lang);
/* unfold whatever hides `line` (the caret must never be inside a fold while the
 * user is typing); returns 1 when a fold went */
int nb_fold_reveal(nb_fold *f, int line);

#endif /* NB_FOLD_H */
