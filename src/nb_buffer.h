/*
 * nb_buffer.h -- the editable text: lines, cursor, undo.
 *
 * A file is a dynamic array of separately allocated lines.  That is the shape
 * an editor wants (insert a line, split a line, join two, no memmove through a
 * megabyte) and it is the shape the calculator's heap can afford: 8000 lines
 * cost 64 KB of pointers and structures, and each line costs its own length
 * plus the record header.
 *
 * COLUMNS ARE BYTE OFFSETS.  A line is whatever bytes the file had -- UTF-8,
 * GB2312 or plain ASCII -- and the editor never rewrites them, so a cursor
 * column is an index into that byte string.  Moving the cursor BY CHARACTER is
 * a separate step (nb_buf_left/right) which skips continuation bytes, so a
 * Chinese character is one cursor stop and not two.  This is the same design
 * the Python PrimeCode reached with its per-character lists, with the bytes
 * kept as they were instead of being decoded and re-encoded on save.
 *
 * UNDO is a stack of inverse operations (nb_undo_t): inserting N bytes is
 * undone by deleting them, deleting is undone by re-inserting.  Every mutation
 * that a user can start pushes exactly one record, which is why "one undo =
 * one keystroke" holds for typing and for a block delete alike.
 */
#ifndef NB_BUFFER_H
#define NB_BUFFER_H

#include "nb_common.h"

#define NB_UNDO_MAX	128
#define NB_UNDO_TEXT	96	/* bytes of replaced text kept per record */

typedef struct nb_line {
	int len;
	int cap;
	int flags;			/* NB_LF_* below */
	char text[1];			/* flexible: cap bytes, always NUL'd */
} nb_line;

/* A saved undo step.  It records the edit and what it displaced, which is all
 * an undo needs: at (line,col) remove `removed` bytes and put back the
 * `replaced` bytes that used to be there. */
typedef struct nb_undo {
	int line, col;
	int removed;			/* bytes the edit inserted */
	int replaced_len;		/* bytes it overwrote (in replaced[]) */
	int cursor_line, cursor_col;	/* where the cursor was before */
	char replaced[NB_UNDO_TEXT];
} nb_undo;

typedef struct nb_buf {
	nb_line **line;
	int nlines;
	int cap;
	unsigned int nbytes;		/* total payload bytes, for the status bar */

	int cur_line, cur_col;
	int want_col;			/* sticky column for up/down */
	int top_line, left_col;		/* the viewport (owned by the UI, kept
					 * here so the cursor arithmetic and the
					 * scroll agree on one place) */

	nb_encoding encoding;
	nb_eol eol;
	int had_wide;			/* the file as OPENED contained a byte >=
					 * 0x80.  A pure ASCII file is a subset
					 * of both encodings, so "the file was
					 * UTF-8" is only meaningful when it
					 * actually had non-ASCII text -- see
					 * the save policy in nb_buf_to_raw. */
	int ends_with_newline;		/* the file ended in a line break.  The
					 * buffer never holds a phantom empty last
					 * line (that is a UI choice most
					 * editors make), so the fact has to be
					 * remembered here or a save would
					 * silently drop the final newline --
					 * which every diff and every patch
					 * tool then reports as a change. */
	int modified;
	int readonly;
	unsigned int version;		/* bumped on every mutation: the
					 * highlighter's cache keys off it */

	char name[NB_PATH_MAX];		/* "src/main.c" */
	char path[NB_PATH_MAX];		/* the absolute path to save to */

	nb_undo undo[NB_UNDO_MAX];
	int undo_top, undo_depth;

	/* the clipboard is a buffer of its own so a block delete can survive
	 * the buffer it came from (see nb_buf_copy_lines) */
	char *clip;
	int clip_len, clip_cap;
} nb_buf;

/* ---- lifetime ---- */
nb_buf *nb_buf_new(void);
void nb_buf_free(nb_buf *b);
void nb_buf_clear(nb_buf *b);		/* one empty line, cursor home */

/* ---- lines ---- */
const char *nb_buf_line(const nb_buf *b, int i);
int nb_buf_linelen(const nb_buf *b, int i);
void nb_buf_set_name(nb_buf *b, const char *name, const char *path);

/* ---- text loading: the codec (nb_codec.c) decodes the raw bytes, these
 * helpers install the result and remember the encoding for the save ---- */
int nb_buf_from_raw(nb_buf *b, const char *raw, int len, nb_encoding enc,
		    nb_eol eol);
int nb_buf_to_raw(const nb_buf *b, char **out, int *out_len, int *had_wide);

/* ---- movement ---- */
void nb_buf_goto(nb_buf *b, int line, int col);
void nb_buf_home(nb_buf *b);
void nb_buf_end(nb_buf *b);			/* end of the line's text */
void nb_buf_left(nb_buf *b);
void nb_buf_right(nb_buf *b);
void nb_buf_up(nb_buf *b);
void nb_buf_down(nb_buf *b);
void nb_buf_page(nb_buf *b, int rows, int dir);
void nb_buf_word_left(nb_buf *b);
void nb_buf_word_right(nb_buf *b);

/* ---- editing.  Each returns NB_YES when the buffer changed. ---- */
int nb_buf_insert(nb_buf *b, const char *text, int len);
int nb_buf_delete(nb_buf *b, int len);		/* forward (Del) */
int nb_buf_backspace(nb_buf *b);
/* ... and the same for `n` characters at once (one undo record) */
int nb_buf_backspace_n(nb_buf *b, int n);		/* one character back */
/* remove a range (a selection): (line, byte column) pairs, either order.
 * Returns 0 nothing, 1 deleted, 2 deleted but too big to undo. */
int nb_buf_delete_range(nb_buf *b, int l0, int c0, int l1, int c1);
int nb_buf_newline(nb_buf *b, const char *indent, int indent_len);
int nb_buf_kill_line(nb_buf *b);
int nb_buf_indent(nb_buf *b, int dir, int width);
int nb_buf_tab(nb_buf *b, int width);
int nb_buf_dup_line(nb_buf *b);
int nb_buf_insert_line_text(nb_buf *b, int at, const char *text, int len);

/* ---- clipboard ---- */
int nb_buf_clip_set(nb_buf *b, const char *text, int len);
int nb_buf_clip_get(const nb_buf *b, const char **text);
int nb_buf_copy_line(nb_buf *b);
int nb_buf_cut_line(nb_buf *b);

/* ---- undo ---- */
int nb_buf_undo(nb_buf *b);
int nb_buf_can_undo(const nb_buf *b);

/* ---- questions ---- */
int nb_buf_indent_of(const nb_buf *b, int line);
int nb_buf_is_blank(const nb_buf *b, int line);
int nb_buf_byte_is_cont(unsigned char c);	/* 0x80..0xBF (UTF-8) */

/* character stepping: the byte index of the character before/after `col`.
 * A GB2312 pair counts as one character even in a file that is otherwise
 * UTF-8, which is what makes a mixed tree navigable. */
int nb_buf_prev_char(const nb_buf *b, int line, int col);
int nb_buf_next_char(const nb_buf *b, int line, int col);

/* the column a tab-indent should land on (tab_width aware), for the UI */
int nb_buf_col_advance(const nb_buf *b, int col, int tab_width);

#endif /* NB_BUFFER_H */
