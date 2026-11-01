/*
 * nb_hex.h -- THE HEX VIEW: what a file that is not text opens in.
 *
 * WHY IT EXISTS.  A file whose bytes are neither valid UTF-8 nor valid GB2312 is
 * decoded as Latin-1 (nb_codec.c) so that every byte has SOMETHING to draw -- and
 * that is fine for reading a file that really is Latin-1, and useless for
 * anything else: a binary, a compressed blob, an image, or a file whose encoding
 * the editor guessed wrong comes up as a screen full of mojibake, and no key the
 * editor has can tell the user anything about it.  A user asked for exactly this:
 * "a Latin-1 file should open in a hex editor, not in the normal one".
 *
 * WHAT IT IS: a READ-ONLY view of the first NB_HEX_MAX bytes -- offset, eight
 * bytes in hex, and the printable bytes beside them -- with the keys the file
 * list uses (arrows, page, home/end, esc) and the same pixel scrolling and
 * scrollbar.  Editing bytes is deliberately NOT here: it is a different feature
 * with a different set of ways to damage a file, and the useful half of "hex
 * editor" is being able to LOOK.
 *
 * The MENU key offers "以文本打开", which hands the file to the normal editor --
 * a Latin-1 text file is a real thing, and the guess that sent it here is a
 * guess.
 */
#ifndef NB_HEX_H
#define NB_HEX_H

#include "nb_common.h"

#define NB_HEX_MAX	(32 * 1024)	/* bytes shown; the rest is not read */
#define NB_HEX_COLS	8		/* bytes per row: 40 columns fits exactly */
#define NB_HEX_ROW_H	16

int nb_hex_open(const char *path, const char *name);	/* 1 = it opened */
void nb_hex_close(void);
int nb_hex_active(void);
const char *nb_hex_name(void);
const char *nb_hex_path(void);
int nb_hex_len(void);
int nb_hex_full_len(void);		/* the file's size, even when truncated */

/* the view, in pixels, exactly like the editor's and the list's */
int nb_hex_scroll_px(void);
void nb_hex_set_scroll_px(int px);
int nb_hex_max_scroll_px(void);

void nb_hex_draw(void);
void nb_hex_menu_action(int action);	/* the hex view's menu, carried out here */
void nb_hex_key(int what, int ch);
void nb_hex_touch(int action, int x, int y);
void nb_hex_init(void);			/* forget the state (a new session) */

#endif /* NB_HEX_H */
