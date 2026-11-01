/*
 * nb_ui.c -- the UI's shared state, the frame fan-out and the key router.
 *
 * This is the file that decides WHAT a key press means once nb_keymap.c has
 * said which key it is: it owns the down-edge detection (so holding an arrow
 * repeats and holding a letter types one character), the modal prompt routing
 * (a question is answered before any screen sees the key) and the redraw flag.
 *
 * One static instance, one thread, no globals that outlive a run: the image
 * stays resident between runs on this machine (primeLua's launcher reuses it),
 * so nb_ui_init() must leave nothing behind from the previous session.
 */
#include <stdio.h>
#include "nb_ui.h"
#include "nb_port.h"

static nb_ui state;

nb_ui *nb_ui_get(void)
{
	return &state;
}

/* A TOAST, NOT A LABEL: the message takes the status row for TOAST_FRAMES
 * (about a second) and then gives it back, so "已打开 x.c" does not sit there
 * until the next message replaces it.  A failure gets twice as long, because it
 * is the one message the user may need to READ. */
#define TOAST_FRAMES		30
#define TOAST_FRAMES_ERROR	60

void nb_ui_message(const char *text, int kind)
{
	nb_str_copy(state.message, sizeof(state.message), text, -1);
	state.message_kind = kind;
	state.msg_frames = (kind == 2) ? TOAST_FRAMES_ERROR : TOAST_FRAMES;
	state.dirty = 1;
	/* every message the editor shows also goes to the log ring: on the
	 * calculator the ring is the only console nbCode has, and a message the
	 * user never got to see (because the screen never came up) is exactly
	 * what a device log is read for */
	nb_log("msg: ");
	nb_log(text);
	nb_log("\n");
}

void nb_ui_init(const nb_config *cfg)
{
	nb_ui *u = &state;

	nb_memset(u, 0, sizeof(*u));
	u->cfg = cfg;
	u->mode = NB_MODE_WORKSPACE;
	u->ws_sel = cfg ? cfg->default_workspace : 0;
	u->dirty = 1;
	u->prompt = NB_PROMPT_NONE;
	nb_theme_set((nb_theme *)nb_theme_get(), cfg ? cfg->theme : 0);
	u->th = nb_theme_get();
	nb_text_classes(u->th, u->classes);
	nb_gcache_reset(&u->chr);
	if (cfg && cfg->nworkspaces > 0 && cfg->nworkspaces <= NB_CFG_MAX_WS) {
		/* nothing else to do: nb_ui_enter_workspace fills the list */
	}
	nb_ui_message("nbCode " NB_VERSION, 0);

	/* the key tables are per run: a session that ended with shift held or
	 * ALPHA locked must not start that way (a file typed in the wrong
	 * layer, with no way to see why) */
	nb_keymap_reset();
	nb_hex_init();
}

void nb_ui_free(void)
{
	nb_ui *u = &state;

	nb_ui_editor_close();
	if (u->styles) {
		int i;

		for (i = 0; i < u->styles_cap; i++) {
			if (u->styles[i])
				nb_free(u->styles[i]);
		}
		nb_free(u->styles);
		u->styles = NB_NULL;
		u->styles_cap = 0;
	}
	nb_hl_free(&u->hl);
	nb_gcache_reset(&u->chr);
}

int nb_ui_finished(void)
{
	return state.mode == NB_MODE_QUIT;
}

void nb_ui_draw(void)
{
	nb_ui *u = &state;

	switch (u->mode) {
	case NB_MODE_WORKSPACE:
	case NB_MODE_BROWSER:
		nb_ui_browser_draw();
		break;
	case NB_MODE_EDITOR:
		nb_ui_editor_draw();
		break;
	case NB_MODE_HEX:
		nb_hex_draw();
		break;
	default:
		break;
	}
	/* the panels go over the screen they belong to; the question goes over
	 * everything (it is the one thing that cannot be answered anywhere else) */
	nb_ui_overlay_draw();
	if (u->prompt != NB_PROMPT_NONE)
		nb_ui_prompt_draw();
	nb_screen_present();
	u->dirty = 0;
	u->frames++;
}

/* the scrollbar's view of the world: whichever screen is up owns the position */
int nb_ui_scroll_px(void)
{
	if (state.mode == NB_MODE_EDITOR)
		return nb_ui_editor_scroll_px();
	if (state.mode == NB_MODE_HEX)
		return nb_hex_scroll_px();
	return nb_ui_list_scroll_px();
}

void nb_ui_scroll_set_px(int px)
{
	if (state.mode == NB_MODE_EDITOR)
		nb_ui_editor_set_scroll_px(px);
	else if (state.mode == NB_MODE_HEX)
		nb_hex_set_scroll_px(px);
	else
		nb_ui_list_set_scroll_px(px);
}

int nb_ui_scroll_max_px(void)
{
	if (state.mode == NB_MODE_EDITOR)
		return nb_ui_editor_max_scroll_px();
	if (state.mode == NB_MODE_HEX)
		return nb_hex_max_scroll_px();
	return nb_ui_list_max_px();
}

/* THE POPUP MENU'S ANSWER, routed to the screen that opened it.  The menu is one
 * widget (nb_ui_overlay.c) with a per-screen item list, so this is the only place
 * that has to know which screen is up. */
void nb_ui_menu_action(int action)
{
	/* THE HEX VIEW OWNS ITS MENU'S ACTIONS (nb_hex.c): "as text" has to close this
	 * view and open the editor with the same file */
	if (state.mode == NB_MODE_HEX &&
	    (action == NB_MENU_HEX_TEXT || action == NB_MENU_HEX_CLOSE)) {
		nb_hex_menu_action(action);
		state.dirty = 1;
		return;
	}
	nb_ui *u = &state;

	switch (u->mode) {
	case NB_MODE_EDITOR:
		nb_ui_editor_action(action);
		break;
	case NB_MODE_BROWSER:
	case NB_MODE_WORKSPACE:
		nb_ui_browser_action(action);
		break;
	default:
		break;
	}
	u->dirty = 1;
}

/* how long a held key waits before it repeats, and how fast it repeats: the
 * frame loop runs at about 30 Hz, so 12 frames is a third of a second */
#define REPEAT_DELAY	12
#define REPEAT_RATE	3

/* does this key repeat when held?  (arrows and backspace: the keys a user
 * holds; every other key is edge triggered) */
static int repeats(int what)
{
	return what == NB_KEY_UP || what == NB_KEY_DOWN ||
	       what == NB_KEY_LEFT || what == NB_KEY_RIGHT ||
	       what == NB_KEY_BACKSPACE || what == NB_KEY_PGUP ||
	       what == NB_KEY_PGDN;
}

void nb_ui_key(const nb_event *ev)
{
	nb_ui *u = &state;
	nb_key_meaning m;
	int code = ev->code & 0x7F;

	if (ev->type != NB_EV_KEY)
		return;
	/* ON (0x83) is handled by code, not through the table: it is 0x83, which
	 * does not fit the held[] bitmap, and it means "quit" on every screen. */
	if (ev->code == 0x83) {
		if (!ev->down)
			return;
		if (u->prompt != NB_PROMPT_NONE)
			nb_ui_prompt_key(0x83);
		else if (u->show_help) {
			/* the help page is the only thing on the screen: ON closes
			 * it, it does not quit the editor */
			u->show_help = 0;
			u->dirty = 1;
			return;
		} else if (nb_ui_overlay_key(NB_KEY_ON, 0)) {
			/* ON closes a panel instead of asking to quit: the editor
			 * underneath is not what the user is looking at */
		} else if (u->mode == NB_MODE_EDITOR)
			nb_ui_editor_key(NB_KEY_ON, 0);
		else if (u->mode == NB_MODE_HEX)
			nb_hex_key(NB_KEY_ESC, 0);
		else
			u->mode = NB_MODE_QUIT;
		u->dirty = 1;
		return;
	}
	/* THE DEVICE SCAN CODES ARE NOT ASCII, and most of the function key row is
	 * above 0x7F (0x91 symb, 0xB2 plot, 0xB3 num/F3, 0xB4 view, 0xB5 CAS,
	 * 0x93 menu, 0x95 help, 0xB6 alpha).  Only the held[] bitmap below needs
	 * the low seven bits, so nothing here may reject a code for its value --
	 * an "out of range -> ignore" test at this point is how that entire row
	 * silently stopped reaching the editor. */
	if (ev->code == 0)
		return;
	if (nb_keymap_modifier(ev->code, ev->down)) {
		/* BOTH edges repaint: the status line names the layer the next key
		 * will type (shift/alpha), so a release has to redraw it too --
		 * otherwise "shift" stays on screen after shift comes up */
		u->dirty = 1;
		return;
	}

	if (!ev->down) {
		u->held[code] = 0;
		if (u->repeat_code == (int)ev->code)
			u->repeat_code = 0;
		return;
	}

	/* A PANEL KEY IS NEVER FILTERED BY THE HELD BITMAP, AND THAT IS THE "I
	 * OPENED THE KEY HELP AND COULD NOT CLOSE IT" BUG.  The bitmap below is the
	 * auto-repeat guard (`if (u->held[code]) return;`) and it only clears when
	 * the key's RELEASE arrives -- so one swallowed release kills the key until
	 * some other release comes along.  HELP and MENU open or toggle panels and
	 * have nothing to repeat, so their press is taken as a fresh one whatever
	 * the bitmap says. */
	if (ev->code == 0x95 || ev->code == 0x93)
		u->held[code] = 0;
	if (u->held[code])
		return;			/* held down: nb_ui_tick repeats it */
	u->held[code] = 1;

	/* a modal question eats the key */
	if (u->prompt != NB_PROMPT_NONE) {
		nb_ui_prompt_key(ev->code);
		u->dirty = 1;
		return;
	}

	m = nb_keymap(ev->code);
	if (m.what == NB_KEY_NONE)
		return;
	/* THE KEY HELP IS CLOSED BY ESC (and by ON and HELP): it covers the whole
	 * screen, so a key that reached the editor would edit something the user
	 * cannot see -- and ESC's own meaning (leave the file) is the last thing a
	 * user wants to do to get rid of a help page. */
	if (u->show_help &&
	    (m.what == NB_KEY_ESC || m.what == NB_KEY_ON || m.what == NB_KEY_HELP)) {
		if (nb_ui_overlay_key(m.what, 0) == 0) {
			u->show_help = 0;
			u->dirty = 1;
			return;
		}
	}
	/* a panel over the editor eats it too (it draws over the text, so a key
	 * that reached the editor would edit something the user cannot see) */
	if (nb_ui_overlay_key(m.what,
			       m.what == NB_KEY_CHAR ? m.ch[0] : 0)) {
		u->dirty = 1;
		return;
	}
	/* and the one line input, which the editor's menu opens as well */
	if (u->input_active) {
		nb_ui_input_key(m.what, m.what == NB_KEY_CHAR ? m.ch[0] : 0);
		u->dirty = 1;
		return;
	}
	if (repeats(m.what)) {
		u->repeat_code = ev->code;
		u->repeat_count = 0;
	}
	u->dirty = 1;
	switch (u->mode) {
	case NB_MODE_WORKSPACE:
	case NB_MODE_BROWSER:
		nb_ui_browser_key(m.what, m.what == NB_KEY_CHAR ? m.ch[0] : 0);
		break;
	case NB_MODE_EDITOR:
		/* a key that inserts a SNIPPET (primeCode's "sin()", "()") goes
		 * through its own entry point: the one-character path cannot carry
		 * it, and the caret-inside rule belongs with the insertion */
		if (m.what == NB_KEY_CHAR && m.ins)
			nb_ui_editor_ins(m.ins);
		else
			nb_ui_editor_key(m.what,
					 m.what == NB_KEY_CHAR ? m.ch[0] : 0);
		break;
	case NB_MODE_HEX:
		nb_hex_key(m.what, m.what == NB_KEY_CHAR ? m.ch[0] : 0);
		break;
	default:
		break;
	}
}

/* called once per frame: give a held key its repeat */
void nb_ui_tick(void);

void nb_ui_tick(void)
{
	nb_ui *u = &state;

	/* THE MESSAGE IS A TOAST: it takes the status row for about a second and
	 * then gives it back.  A message that stays until the next one is a line
	 * the user cannot get rid of ("已打开 x.c" sat there for the whole
	 * session). */
	if (u->msg_frames > 0 && --u->msg_frames == 0) {
		u->message[0] = 0;
		u->dirty = 1;
	}
	/* a panel is guaranteed a few frames of redraw when it opens: a panel
	 * that does not appear until the next key press is what was reported */
	if (u->over_repaint > 0) {
		u->over_repaint--;
		u->dirty = 1;
	}
	nb_ui_touch_tick();

	if (!u->repeat_code || u->prompt != NB_PROMPT_NONE)
		return;
	if (++u->repeat_count < REPEAT_DELAY)
		return;
	u->repeat_count = REPEAT_DELAY - REPEAT_RATE;
	{
		nb_key_meaning m = nb_keymap((int)(u->repeat_code & 0x7F));

		u->dirty = 1;
		if (u->mode == NB_MODE_EDITOR)
			nb_ui_editor_key(m.what, m.what == NB_KEY_CHAR ? m.ch[0] : 0);
		else
			nb_ui_browser_key(m.what, 0);
	}
}

/* an "input is idle" callback the frame loop uses to drop the redraw flag:
 * without it every frame would repaint and the calculator's battery would pay
 * for a screen that did not change */
int nb_ui_wants_redraw(void)
{
	return state.dirty;
}

void nb_ui_invalidate(void)
{
	state.dirty = 1;
}

/* the longest press that is still a tap, in frames: the frame loop runs at about
 * 30 Hz, so 12 frames is a bit under half a second -- long enough not to fire while
 * scrolling, short enough not to feel slow */
#define LONG_PRESS_FRAMES	12

void nb_ui_touch(const nb_event *ev)
{
	nb_ui *u = &state;

	u->dirty = 1;
	u->touch_idle = 0;		/* the finger is still reporting */
	/* A QUESTION OWNS THE SCREEN, so it owns the finger too: a tap goes to its
	 * buttons and nowhere else (the answers are buttons now, nb_ui_prompt.c --
	 * the panel below is what the user is being asked about, not what they are
	 * touching) */
	if (u->prompt != NB_PROMPT_NONE) {
		if (ev->action == 1) {
			int b = nb_ui_prompt_hit(ev->x, ev->y);

			if (b >= 0)
				nb_ui_prompt_choose(b);
		}
		u->touch_down = 0;
		u->touch_moved = 1;
		u->touch_eaten = 1;
		return;
	}
	if (nb_ui_overlay_touch(ev->action, ev->x, ev->y))
		return;			/* a panel had the finger */
	if (u->mode == NB_MODE_EDITOR)
		nb_ui_editor_touch(ev->action, ev->x, ev->y);
	else if (u->mode == NB_MODE_HEX)
		nb_hex_touch(ev->action, ev->x, ev->y);
	else if (u->mode == NB_MODE_BROWSER || u->mode == NB_MODE_WORKSPACE)
		nb_ui_browser_touch(ev->action, ev->x, ev->y);
}

/* called once per frame: this is where a long press becomes a selection */
void nb_ui_touch_tick(void)
{
	nb_ui *u = &state;

	if (u->touch_down)
		u->touch_idle++;
	/* the edge scroll of a selection drag runs every frame the finger is
	 * down, whether or not it has moved */
	if (u->mode == NB_MODE_EDITOR && nb_ui_overlay() == NB_OVER_NONE)
		nb_ui_editor_touch_tick();
	if (!u->touch_down || u->touch_moved || u->scrub ||
	    nb_ui_overlay() != NB_OVER_NONE || u->mode != NB_MODE_EDITOR)
		return;
	if (++u->touch_frames == LONG_PRESS_FRAMES)
		nb_ui_editor_long_press(u->touch_x, u->touch_y);
}
