/*
 * nb_ui.h -- nbCode's screens and its state.
 *
 * THREE SCREENS, one state struct: the workspace picker (which folder), the file
 * browser (which file), and the editor.  They share the header/status-bar frame
 * and the same key discipline (down-edge detection, hold-to-repeat for the
 * arrows) so moving between them feels like one program rather than three.
 *
 * Everything the UI owns lives in nb_ui_state below and in ONE static instance
 * (ui_get).  Static rather than passed around because the calculator gives a
 * loaded image one thread and no reason to want two editors; the field names are
 * the documentation.
 *
 * KEY CODES ARE DEVICE SCAN CODES (port/hp_input.h), never PPL GETKEY numbers.
 * In this workspace the two tables overlap in the worst way -- GETKEY's ESC is
 * the device's RIGHT arrow -- and a right arrow that quits an app is exactly the
 * bug that convention prevents.  nb_keymap.c is the only place that knows what a
 * key means.
 */
#ifndef NB_UI_H
#define NB_UI_H

#include "nb_common.h"
#include "nb_buffer.h"
#include "nb_highlight.h"
#include "nb_complete.h"
#include "nb_fold.h"
#include "nb_text.h"
#include "nb_port.h"			/* nb_config, nb_event: this layer talks
					 * to both */

#define NB_ROWS_HEAD	1		/* header bar, NB_HEAD_H px */
#define NB_ROWS_FOOT	1		/* status bar, 16 px */

/* THE THREE BANDS EVERY SCREEN IS MADE OF (nb_ui_draw.c draws them): a header,
 * the body, and a status line.  The editor and the overlays both need the body's
 * edges, so they are named once here.
 *
 * THE HEADER IS TALLER THAN ONE GLYPH ON PURPOSE: it carries a line of 8x16 text
 * and the accent rule that separates it from the body, and at 16 px those were
 * the same pixel row -- every title with a descender (g, j, p, q, y) lost its
 * bottom row to the rule.  18 px leaves a row of air above and below the text. */
#define NB_HEAD_H	18
#define NB_STATUS_H	16
#define NB_BODY_TOP	NB_HEAD_H
#define NB_BODY_BOT	(NB_SCREEN_H - NB_STATUS_H)
#define NB_UI_MAX_LIST	64		/* files shown in one browser page */
#define NB_INPUT_MAX	48		/* bytes in the one line text input */

typedef enum {
	NB_MODE_WORKSPACE = 0,		/* choose a folder */
	NB_MODE_BROWSER,		/* choose a file in it */
	NB_MODE_EDITOR,
	NB_MODE_HEX,			/* a file that is not text (nb_hex.c) */
	NB_MODE_QUIT
} nb_mode;

typedef struct nb_ui {
	const nb_config *cfg;
	const nb_theme *th;
	nb_mode mode;

	/* ---- drawing ---- */
	nb_gcache chr;			/* per-line glyph cache */
	nb_text_decoded dec;		/* the line being drawn / hit-tested */
	nb_u32 classes[NB_HS_KIND];
	int dirty;			/* redraw needed */
	int frames;			/* frames drawn, for the log */

	/* ---- the file list (browser) ---- */
	int ws_sel;			/* selected workspace */
	int sel;			/* selected row in the browser */
	int top;			/* first visible row */
	int count;			/* rows in the list */
	int col_lines;			/* rows of the list that fit */
	char dir[NB_PATH_MAX];

	/* ---- the open file ---- */
	nb_buf *buf;
	nb_hl hl;
	unsigned char **styles;		/* one style array per line */
	int styles_cap;
	int left;			/* horizontal scroll, in frames (8 px) */
	int left_max;			/* frames of the longest VISIBLE line, from
					 * the last draw: the drag's right hand
					 * limit, so the text cannot be dragged off
					 * into blank space */
	int xoff;			/* ... plus the rest of the drag, 0..7 px:
					 * the pair is the horizontal scroll in PIXELS
					 * (left * 8 + xoff), the same way top_line
					 * and yoff are the vertical one */
	int match_line, match_col;	/* the bracket that matches the cursor's,
					 * and the one AT the cursor: both are
					 * boxed and drawn in the accent, which is
					 * how a pair is told apart from the
					 * brackets around it */
	int brace_line, brace_col;
	int show_help;			/* the key help overlay */

	/* ---- message line / prompts ---- */
	char message[64];
	int message_kind;		/* 0 info, 1 warn, 2 error */
	int msg_frames;			/* frames the message has left: it is a
					 * TOAST that gives the status row back
					 * (nb_ui_message / nb_ui_tick) */
	int *style_cap;			/* bytes styles[i] can hold (nb_hl_lines
					 * keeps this true; 0 = no buffer) */
	/* ---- touch, in the editor ----
	 *
	 * A DRAG SCROLLS BY PIXELS (yoff is the sub-line offset: the text follows the
	 * finger rather than jumping a line at a time), and a LONG PRESS starts a
	 * selection which a further drag extends.  A tap moves the caret. */
	int touch_down;			/* a finger is on the screen */
	int touch_moved;		/* it moved far enough to be a drag */
	/* THE GESTURE WAS CONSUMED by a panel, the gutter or the header: the finger
	 * must then be ignored until it comes up.  touch_moved alone is not enough
	 * -- a tap on a line number sets it to keep the RELEASE from becoming a tap,
	 * and the drag path only looked at this flag, so a finger that jittered
	 * after the tap dragged the whole view with it ("I tapped a line number to
	 * fold and the code ran around, left and right most visibly"). */
	int touch_eaten;
	/* how many code rows the last editor draw painted: the body is not a whole
	 * number of rows tall, so this is the invariant that says the row at the
	 * bottom edge is always drawn (explained in nb_ui_editor.c) */
	int rows_drawn;
	int touch_frames;		/* frames since the press (long press) */
	int touch_x, touch_y;		/* where it is now */
	int touch_idle;			/* frames since the last touch event: a
					 * finger that leaves the glass can take
					 * its release event with it, and this is
					 * what stops the edge scroll when it
					 * does (see nb_ui_editor_touch_tick) */
	int touch_last_x, touch_last_y;	/* where it was last frame */
	int yoff;			/* vertical scroll inside the first
					 * visible line, 0..LINE_H-1 */
	int scrub;			/* a finger is dragging the scrollbar */
	nb_complete comp;		/* the completion popup (nb_complete.c) */
	nb_fold fold;			/* the folded blocks (nb_fold.c) */
	int list_yoff;			/* the LISTS scroll by pixels too (the
					 * browser and the workspace picker): the
					 * remainder inside `top`, like yoff in the
					 * editor */
	int list_drag;			/* a finger is dragging the list */
	int menu_cursor;		/* the popup menu's highlighted row */
	int pal_cursor;			/* the character table's cursor (an index
					 * into nb_ui_palette_cp; a tap or an
					 * arrow moves it, ENTER inserts it) */
	int over_repaint;		/* frames of forced redraw after a panel
					 * opened: a panel that does not appear
					 * until the next key press is exactly what
					 * was reported, and this makes the frame
					 * it opens on unconditional */
	int view_line, view_col;	/* the caret the window was last fitted to,
					 * and the line count it was fitted against:
					 * the follow only runs when one of them
					 * changed, or a drag would be undone by the
					 * next frame (see scroll_into_view) */
	int view_nlines;
	int sel_active;			/* a selection exists */
	int sel_l0, sel_c0;		/* its anchor (line, byte column) */
	int sel_l1, sel_c1;		/* its end, sorted on use */

	int hl_nlines;			/* lines when the highlighter was last
					 * told about this buffer: a change means a
					 * newline went in or came out, and then every
					 * per-line state below the edit is stale */
	int prompt;			/* NB_PROMPT_*: a modal question is up */
	int prompt_then;		/* NB_THEN_*: where the editor goes once
					 * the save question is answered */
	char prompt_text[64];
	char prompt_name[NB_PATH_MAX];	/* the file a delete would remove */

	/* ---- key state ---- */
	nb_u8 held[128];		/* device code -> currently down (edge detect) */
	int repeat_code;
	int repeat_count;

	int exit_status;

	/* the one line text input: 0 when nothing is being typed */
	int input_active;
	int input_then;			/* NB_INPUT_*: what ENTER does with it */
	int prompt_sel;			/* which answer button of the question is
					 * selected (the question is answered with
					 * LEFT/RIGHT + ENTER, or a tap) */
	char input_title[32];
	char input_text[NB_INPUT_MAX];
	int input_len;
	int input_cursor;
} nb_ui;

/* the one instance */
nb_ui *nb_ui_get(void);

/* ---- lifetime ---- */
void nb_ui_init(const nb_config *cfg);
void nb_ui_free(void);

/* the entry points the main loop drives */
void nb_ui_draw(void);
void nb_ui_key(const nb_event *ev);	/* called for every event */
void nb_ui_touch(const nb_event *ev);
void nb_ui_tick(void);			/* once per frame: key repeat */
int nb_ui_wants_redraw(void);		/* 0 = nothing changed, skip the frame */
void nb_ui_invalidate(void);
int nb_ui_finished(void);

/* ---- the browser (implemented in nb_ui_browser.c) ---- */
void nb_ui_enter_workspace(int index);
void nb_ui_enter_browser(void);
void nb_ui_browser_key(int code, int ch);
void nb_ui_browser_draw(void);

/* ---- the editor (nb_ui_editor.c) ---- */
int nb_ui_open_file(const char *dir, const char *name);
/* ... and the same file as TEXT, whatever it looks like (the hex view's menu) */
int nb_ui_open_file_text(const char *dir, const char *name);
void nb_ui_editor_key(int code, int ch);
void nb_ui_editor_draw(void);
int nb_ui_editor_save(void);
void nb_ui_editor_close(void);
void nb_ui_editor_changed(int line);	/* re-highlight from `line` */
/* touch: a drag scrolls by pixels, a long press starts a selection, a tap moves
 * the caret (see nb_ui_touch) */
void nb_ui_editor_touch(int action, int x, int y);
void nb_ui_editor_long_press(int x, int y);
void nb_ui_touch_tick(void);
/* is this byte of this line inside the selection? (for the renderer) */
int nb_ui_selection_on(int line, int byte_off);
int nb_ui_selection_active(void);
void nb_ui_selection_clear(void);
int nb_ui_selection_delete(void);	/* remove it; the cursor stays */
int nb_ui_style_at(int line, int byte_off);	/* the cached style byte, or -1 */
const unsigned char *nb_ui_style_buf(int line, int *cap);	/* its room */
void nb_ui_editor_help_draw(void);

/* ---- questions (nb_ui_prompt.c) ---- */
#define NB_PROMPT_NONE		0
#define NB_PROMPT_SAVE		1	/* "save before leaving?" */
#define NB_PROMPT_DISCARD	2
#define NB_PROMPT_DELETE	3
#define NB_PROMPT_HELP		4		/* not a question: the overlay */

/* where the editor goes once the SAVE question is answered.  ESC in the editor
 * means "back to the file list" and ON means "quit", and BOTH ask about unsaved
 * changes -- with the same question but a different destination.  Getting this
 * wrong is how "esc ... n" quit the whole editor: it asked "save before quit?"
 * and answered itself. */
#define NB_THEN_NOWHERE		0		/* stay (the question was cancelled) */
#define NB_THEN_QUIT		1
#define NB_THEN_BACK		2		/* close the file, back to the list */

/* ask about unsaved changes; `then` is what a yes OR a no leads to */
void nb_ui_prompt_save(const char *name, int then);
void nb_ui_prompt(int kind, const char *text);
void nb_ui_prompt_key(int code);
void nb_ui_prompt_draw(void);
/* THE QUESTION'S BUTTONS.  The answers are buttons, not letters (nb_ui_prompt.c):
 * these are what the drawing, the touch handler and the tests share. */
int nb_ui_prompt_button_count(void);
const char *nb_ui_prompt_button(int i);
void nb_ui_prompt_rect(int i, int *x, int *y, int *w, int *h);
int nb_ui_prompt_hit(int px, int py);		/* the button at a point, or -1 */
void nb_ui_prompt_choose(int i);		/* press button i (a tap) */

/* ---- shared chrome ---- */
void nb_ui_header(const char *title, const char *right);
void nb_ui_status(const char *left, const char *right, nb_u32 bg);
void nb_ui_puts(int x, int y, const char *text, nb_u32 colour);
void nb_ui_puts_clip(int x, int y, const char *text, nb_u32 colour, int px);
int nb_ui_puts_w(const char *text);
void nb_ui_message(const char *text, int kind);
void nb_ui_message_line(void);
void nb_ui_panel(int x, int y, int w, int h);
/* the right hand end of the status row alone, drawn AFTER the message (which
 * fills the row): the layer indicator must not be erasable by a message */
void nb_ui_status_right(const char *right);

/* ---- the one line text input (a new file name, a rename) ---- */
void nb_ui_input_begin(const char *title, const char *initial);
void nb_ui_input_key(int what, int ch);
int nb_ui_input_active(void);
void nb_ui_input_draw(void);
const char *nb_ui_input_text(void);
void nb_ui_input_end(void);

/* ---- key mapping (nb_keymap.c) ---- */
/* what a key press means; NB_KEY_NONE when it is a modifier or unknown */
#define NB_KEY_NONE	0
#define NB_KEY_CHAR	1	/* `ch` holds the byte(s) to insert */
#define NB_KEY_UP	2
#define NB_KEY_DOWN	3
#define NB_KEY_LEFT	4
#define NB_KEY_RIGHT	5
#define NB_KEY_HOME	6
#define NB_KEY_END	7
#define NB_KEY_PGUP	8
#define NB_KEY_PGDN	9
#define NB_KEY_ENTER	10
#define NB_KEY_BACKSPACE 11
#define NB_KEY_DELETE	12
#define NB_KEY_TAB	13
#define NB_KEY_ESC	14
/* THE CALCULATOR HAS NO F1..F6.  It has Apps, Home, Symb, Plot, Num, Help,
 * View, Menu, Esc and CAS, and nbCode uses two of them: MENU opens the current
 * screen's popup menu (which is where the old F-key actions live now) and HELP
 * opens the key help.  The rest do nothing -- a key that does something
 * undiscoverable on a calculator with no function-key row is worse than a key
 * that does nothing. */
#define NB_KEY_MENU	15
#define NB_KEY_HELP	16
#define NB_KEY_WORD_L	21
#define NB_KEY_WORD_R	22
#define NB_KEY_ON	23
#define NB_KEY_PALETTE	24	/* shift+A: the character table */
#define NB_KEY_SYMBOLS	25	/* shift+6: the quick symbol panel */
#define NB_KEY_COPY	26	/* shift+VIEW: copy the selection, or the line */
#define NB_KEY_CLIP	27	/* shift+MENU: the global clipboard panel */
#define NB_KEY_FIND	28	/* SYMB: search */
#define NB_KEY_REPLACE	29	/* PLOT: replace */

/* the hex view (nb_hex.c): a file whose bytes are not text opens there instead of
 * in the editor, and its menu hands the file to the editor when the user says so */
#include "nb_hex.h"

typedef struct nb_key_meaning {
	int what;
	char ch[4];			/* a single character (the alpha layer, the
					 * punctuation), for NB_KEY_CHAR */
	const char *ins;		/* a SNIPPET to insert instead: primeCode's
					 * key layers give "sin()" and "()", which
					 * is more than one character and needs no
					 * copy -- it points into the keymap's own
					 * static table */
} nb_key_meaning;

/* THE ALPHA LAYER'S STATE (nb_keymap.c).  The calculator's ALPHA key cycles
 * OFF -> the next key -> locked -> OFF, and SHIFT+ALPHA picks the capitals, so
 * "what a key types" depends on two bits of state that the UI has to be able to
 * show: an unmarked locked alpha is how a user types 'q' where a '7' belongs. */
#define NB_ALPHA_OFF	0
#define NB_ALPHA_ONCE	1
#define NB_ALPHA_LOCK	2

/* translate a device scan code, honouring the shift/alpha state the mapper is
 * holding.  nb_keymap_modifier() is called for every press/release first. */
int nb_keymap_modifier(int code, int down);
nb_key_meaning nb_keymap(int code);
int nb_keymap_held_shift(void);
/* 1 when an insertion should leave the caret INSIDE it: primeCode's rule for a
 * snippet that ends in ) ] } " or ' (see nb_keymap.c) */
int nb_key_wants_caret_inside(const char *text, int len);
/* insert a snippet (a key's layer, already UTF-8) at the caret */
void nb_ui_editor_ins(const char *text);
int nb_keymap_alpha_state(void);
int nb_keymap_alpha_upper(void);
void nb_keymap_reset(void);

#endif /* NB_UI_H */

/* the quick symbol panel (shift+6): a grid of the common comparison operators of
 * the CURRENT language, inserted by tapping a cell or pressing its digit.  The
 * digits follow the KEYPAD (7 8 9 on the top row), so the shortcut sits under the
 * key that fires it -- nb_ui_symbol_key() is the digit of a cell. */
#define NB_SYM_CELLS	9
const char *nb_ui_symbol_token(nb_lang lang, int index);
int nb_ui_symbol_key(int index);		/* the digit key of a cell */
int nb_ui_symbol_cell_of_key(int ch);		/* ... and back, -1 if none */
void nb_ui_symbols_open(void);

/* ---- the overlays (nb_ui_overlay.c) ----
 *
 * Panels drawn OVER the editor.  They are modal: nb_ui.c offers every touch and
 * every key to the one that is up first, and if it answers 1 the editor below
 * never sees the event (a tap that both picked a character and moved the caret
 * would be a bug that is hard to see). */
#define NB_OVER_NONE	0
#define NB_OVER_PALETTE	1	/* the character table (shift+A) */
#define NB_OVER_MENU	2	/* the header's hamburger */
#define NB_OVER_INFO	3	/* the file information panel */
#define NB_OVER_SYMBOLS	4	/* the quick symbol panel (shift+6) */
#define NB_OVER_CLIP	5	/* the global clipboard (shift+MENU) */

int nb_ui_overlay(void);
void nb_ui_overlay_open(int which);
void nb_ui_overlay_close(void);
void nb_ui_overlay_draw(void);
int nb_ui_overlay_touch(int action, int x, int y);
int nb_ui_overlay_key(int what, int ch);

/* the character table: what is in it (the test walks it), where the cursor is,
 * and what the arrow keys and ENTER do with it */
int nb_ui_palette_count(void);
unsigned int nb_ui_palette_cp(int index);
int nb_ui_palette_cursor(void);
void nb_ui_palette_move(int dx, int dy);
void nb_ui_palette_choose(void);

/* the hamburger: the icon is drawn with the header, the hit box and the panel
 * are in the overlay */
int nb_ui_menu_icon_hit(int x, int y);
void nb_ui_menu_icon_draw(void);

/* THE SCROLL POSITION OF THE SCREEN THAT IS UP: the editor's is the code, the
 * browser's is the file list.  One interface, so the scrollbar (and a finger
 * dragging it) works on both without knowing which screen it is over. */
int nb_ui_scroll_px(void);
void nb_ui_scroll_set_px(int px);
int nb_ui_scroll_max_px(void);

/* the scrollbar: see-through, dragged for pixel-level scrolling */
int nb_ui_scrollbar_visible(void);
int nb_ui_scrollbar_hit(int x, int y);
void nb_ui_scrollbar_draw(void);
void nb_ui_scrollbar_touch(int action, int x, int y);

/* ---- what the editor exposes to the overlays ---- */
int nb_ui_editor_scroll_px(void);
void nb_ui_editor_set_scroll_px(int px);
/* the HORIZONTAL scroll, for the same reasons (the selection tests drive it, and
 * the edge auto-scroll moves it) */
int nb_ui_editor_scroll_left_px(void);
void nb_ui_editor_set_scroll_left_px(int px);
int nb_ui_editor_max_scroll_px(void);
int nb_ui_editor_body_px(void);
int nb_ui_editor_insert_cp(unsigned int cp);
void nb_ui_editor_goto(int line);		/* 1 based */
void nb_ui_editor_touch_tick(void);	/* per frame, while a finger is down */
void nb_ui_editor_comp_rect(int *x, int *y, int *w, int *h);
/* how many VISIBLE lines are between b->top_line and `line` (folding hides
 * lines, so a screen row is not a line number any more) */
int nb_ui_editor_row_of(int line);
/* the line number gutter's width: it grows with the number of digits, so the
 * fold marker (x 3..10) and the number never overlap */
int nb_ui_editor_gutter_w(void);
/* how many full code lines fit between the header and the status bar */
int nb_ui_editor_rows(void);
/* how many of them the last draw actually painted (always nb_ui_editor_rows():
 * the last row is drawn to the SCREEN's edge and the status bar covers its tail) */
int nb_ui_editor_rows_drawn(void);
/* the right hand end of the status row: the file type (ABBREVIATED) and the
 * keyboard layer (shift/alpha/alock/Alock).  Returns 1 when it wrote something. */
int nb_ui_editor_status_label(char *out, int n);
/* COPY: the selection when there is one, otherwise the caret's line.  The text
 * goes into the GLOBAL clipboard (nb_clip.h), which is what the panel opened by
 * shift+MENU shows; the return value says what was copied (0 nothing, 1 the
 * selection, 2 the line) so the caller can say so and the tests can ask. */
int nb_ui_editor_copy(void);
/* insert a piece of text (a clipboard entry) at the caret: multi-line text splits
 * into lines, and the caret ends after it */
int nb_ui_editor_insert_text(const char *text, int len);
/* the clipboard panel (shift+MENU): draw/keys/touch live in nb_ui_overlay.c */
void nb_ui_clip_open(void);

/* ---- FIND AND REPLACE (src/nb_find.c matches, nb_ui_editor.c draws) ----
 *
 * SYMB asks for the text to search for, PLOT for what to put in its place; the
 * header shows `k/n` and two buttons that step through the matches. */
void nb_ui_editor_find_begin(void);
void nb_ui_editor_find_commit(const char *text);
void nb_ui_editor_replace_begin(void);
void nb_ui_editor_replace_commit(const char *rep);
void nb_ui_editor_replace_all_commit(const char *rep);
int nb_ui_editor_find_step(int dir);		/* 1 = next, -1 = previous */
/* the floating find window's rectangle, for the touch handler and the tests
 * (0,0,0,0 when there is no search) */
void nb_ui_editor_find_window(int *x, int *y, int *w, int *h);
/* how many matches and which one the caret is on (index is 1 based, 0 = none) */
int nb_ui_editor_find_info(int *index, int *total);
/* the LIST's scroll (the browser and the workspace picker), for the scrollbar */
int nb_ui_list_scroll_px(void);
void nb_ui_list_set_scroll_px(int px);
int nb_ui_list_max_px(void);
int nb_ui_editor_save_as(const char *name);

/* ---- the popup menu's items (nb_ui_overlay.c opens it, nb_ui.c dispatches) ----
 *
 * One list per screen, chosen by the screen itself; the action ids are what the
 * overlay sends back through nb_ui_menu_action(). */
enum {
	NB_MENU_NONE = 0,
	/* the editor */
	NB_MENU_SAVE, NB_MENU_SAVE_AS, NB_MENU_UNDO, NB_MENU_COPY_LINE,
	NB_MENU_CUT_LINE, NB_MENU_CUT_SEL, NB_MENU_FILE_INFO, NB_MENU_GOTO,
	NB_MENU_CLIP, NB_MENU_REPLACE_ALL,
	/* the hex view */
	NB_MENU_HEX_TEXT, NB_MENU_HEX_CLOSE,
	/* the file browser */
	NB_MENU_OPEN, NB_MENU_NEW_FILE, NB_MENU_RENAME, NB_MENU_DELETE,
	NB_MENU_REFRESH,
	/* the workspace picker */
	NB_MENU_ENTER
};

/* open the popup with `items` (action ids in `actions`) and select the first */
void nb_ui_menu_open(const char *title, const char *const *items,
		     const int *actions, int n);
int nb_ui_menu_active(void);
/* run an action: nb_ui.c routes it to the screen that is up */
void nb_ui_menu_action(int action);
void nb_ui_browser_menu(void);
void nb_ui_browser_touch(int action, int x, int y);
void nb_ui_help_draw(void);		/* the key reference, shared by the screens */
void nb_ui_editor_menu(void);
void nb_ui_hex_menu(void);		/* the hex view's own menu (nb_hex.c) */
/* the screens' actions, called by the router */
void nb_ui_browser_action(int action);
void nb_ui_editor_action(int action);

/* what the one line input DOES with what it was given (nb_ui_input_key in
 * nb_ui_browser.c): the browser names a new file, the editor's menu saves as or
 * jumps to a line */
#define NB_INPUT_NEW_FILE	0
#define NB_INPUT_SAVE_AS	1
#define NB_INPUT_GOTO		2
#define NB_INPUT_RENAME	3
#define NB_INPUT_FIND	4	/* the search text (SYMB) */
#define NB_INPUT_REPLACE 5	/* the replacement (PLOT); empty = delete */
#define NB_INPUT_REPLACE_ALL 6	/* the replacement, for every match at once */

/* the config's booleans live in one flags word (nb_cfg.h) */
int nb_cfg_flag(const nb_config *c, unsigned int flag);
