/*
 * nb_complete.h -- completion: the word the caret is in, and what it could be.
 *
 * The state is a small fixed array (six candidates of 24 bytes), because the
 * popup is a list on a 40 column screen, not a menu: a long list would cover the
 * code it is completing.  See nb_complete.c for what is offered and what is
 * deliberately not.
 */
#ifndef NB_COMPLETE_H
#define NB_COMPLETE_H

#include "nb_common.h"
#include "nb_buffer.h"
#include "nb_highlight.h"

#define NB_COMPLETE_MAX		6
/* THE FULL NAME HAS TO FIT: a candidate longer than this is STORED truncated, and
 * the popup then shows a function name one letter short of the real one -- which
 * is what a user reported ("some functions are cut off although one more letter
 * would show them completely").  40 bytes costs nothing (six candidates) and
 * covers every real identifier on a 320 pixel screen. */
#define NB_COMPLETE_WORD	40

typedef struct nb_complete {
	char item[NB_COMPLETE_MAX][NB_COMPLETE_WORD];
	int n;
	int sel;
	char prefix[NB_COMPLETE_WORD];	/* what is already typed */
	int prefix_len;
	int active;
	char accepted[NB_COMPLETE_WORD];	/* the word just accepted: it must
						 * not re-open the popup */
} nb_complete;

/* is this byte part of a word? (the completion's idea of an identifier) */
int nb_word_char(int c);
/* the word before the caret: its bytes copied into `out`, returns the length */
int nb_word_prefix(nb_buf *b, char *out, int cap);

void nb_complete_reset(nb_complete *c);
void nb_complete_close(nb_complete *c);
/* rebuild for the caret's word; returns the number of candidates (0 = closed) */
int nb_complete_update(nb_complete *c, nb_buf *b, nb_lang lang);
int nb_complete_active(const nb_complete *c);
void nb_complete_move(nb_complete *c, int delta);

#endif /* NB_COMPLETE_H */
