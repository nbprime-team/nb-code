/*
 * nb_ui_browser.c -- the start screen: choose a workspace, then a file.
 *
 * This is the FileManager idiom (one GROB, a header bar, a list that inverts the
 * selected row, arrows to move, ENTER to open, no footer beyond one status line)
 * applied to the two questions nbCode has to ask before it can edit anything:
 *
 *   WORKSPACE   which folder the user means -- C:\DATA\nbCode.hpappdir itself,
 *               C:\DATA, or any folder the launcher found to be interesting
 *   FILE        which file in it.  Names come from the launcher (the firmware's
 *               directory enumeration lives in the MicroPython application, not
 *               in a loaded image -- see nb_cfg.h), sizes come with them, and a
 *               folder entry is marked with a trailing '/'.
 *
 * The list is scrolled so the selection is always visible, which is the one bit
 * of behaviour a file manager must get right: on a 12 row screen a folder with 60
 * files is unusable otherwise.
 *
 * Keys are dispatched by nb_ui.c; this file only implements the actions, and
 * every action has a letter shortcut as well as a function key, so a key table
 * that turns out to be wrong on hardware (which has happened twice in this
 * workspace) still leaves the program usable.
 */
#include "nb_ui.h"
#include "nb_gfx.h"
#include "nb_port.h"

#define ROW_H		16
/* THE LIST STARTS WHERE THE HEADER ENDS.  It used to be a hard 16, which was the
 * header's own height: when the header grew (see NB_HEAD_H) the first row of the
 * list sat UNDER it, two pixels of every first row clipped by the bar drawn over
 * it.  The body's top edge is named once, in nb_ui.h. */
#define LIST_TOP	NB_BODY_TOP
#define LIST_BOT	(NB_SCREEN_H - 16)

static int rows_fit(void)
{
	return (LIST_BOT - LIST_TOP) / ROW_H;
}

/* ---------------------------------------------------------------------------
 * the lists scroll by PIXELS, like the editor (a finger that drags the list moves
 * it with the finger, not a row at a time).  `top` is the first row on screen and
 * `list_yoff` is how much of it is scrolled off above, so the two together are
 * one pixel number -- the same arrangement, and for the same reason, as the
 * editor's top_line/yoff.
 * ------------------------------------------------------------------------- */
static int list_scroll_px(void)
{
	nb_ui *u = nb_ui_get();

	return u->top * ROW_H + u->list_yoff;
}

static int list_max_px(void)
{
	nb_ui *u = nb_ui_get();
	int total = u->count * ROW_H;
	int body = LIST_BOT - LIST_TOP;

	return total > body ? total - body : 0;
}

static void set_list_scroll_px(int px)
{
	nb_ui *u = nb_ui_get();
	int max = list_max_px();

	if (px < 0)
		px = 0;
	else if (px > max)
		px = max;
	u->top = px / ROW_H;
	u->list_yoff = px % ROW_H;
}

/* the same numbers, for the overlays (the scrollbar) */
int nb_ui_list_scroll_px(void)
{
	return list_scroll_px();
}

void nb_ui_list_set_scroll_px(int px)
{
	set_list_scroll_px(px);
}

int nb_ui_list_max_px(void)
{
	return list_max_px();
}

/* keep the selection inside the visible window (in pixels, so a half scrolled
 * row does not hide it) */
static void scroll_to_selection(void)
{
	nb_ui *u = nb_ui_get();
	int top = u->sel * ROW_H;
	int px = list_scroll_px();
	int body = LIST_BOT - LIST_TOP;

	if (top < px)
		px = top;
	else if (top + ROW_H > px + body)
		px = top + ROW_H - body;
	set_list_scroll_px(px);
}

/* how many rows the screen that is up has: the workspace picker shows the
 * WORKSPACES, the file browser shows the current folder's entries */
static int list_rows(void)
{
	nb_ui *u = nb_ui_get();

	return u->mode == NB_MODE_WORKSPACE ? nb_ws_count() : u->count;
}

/* which row is under screen y, or -1 */
static int row_at_y(int y)
{
	nb_ui *u = nb_ui_get();
	int rows = list_rows();
	int row;

	if (y < LIST_TOP || y >= LIST_BOT)
		return -1;
	/* the workspace list always starts at the top: it has at most
	 * NB_CFG_MAX_WS rows, so it never scrolls */
	row = (u->mode == NB_MODE_WORKSPACE ? 0 : list_scroll_px()) + y -
		LIST_TOP;
	row /= ROW_H;
	return (row >= 0 && row < rows) ? row : -1;
}

/* A TAP OPENS, A DRAG SCROLLS.  Touch in a file browser is a pointing device:
 * tapping a row opens the file (or steps into the folder) rather than only moving
 * a selection that then needs ENTER -- which is what a finger expects, and what
 * the report asked for. */
void nb_ui_browser_touch(int action, int x, int y)
{
	nb_ui *u = nb_ui_get();
	int dy;

	u->dirty = 1;
	/* the scrollbar has the finger first (it is drawn over the rows) */
	if (u->scrub || (action == 1 && nb_ui_scrollbar_hit(x, y))) {
		nb_ui_scrollbar_touch(action, x, y);
		return;
	}
	if (action == 1) {
		u->touch_down = 1;
		u->touch_moved = 0;
		u->touch_frames = 0;
		u->touch_x = u->touch_last_x = x;
		u->touch_y = u->touch_last_y = y;
		return;
	}
	if (action == 2) {
		dy = y - u->touch_last_y;
		u->touch_x = x;
		u->touch_y = y;
		if (!u->touch_moved &&
		    ((x - u->touch_last_x) * (x - u->touch_last_x) +
		     dy * dy) > (5 * 5))
			u->touch_moved = 1;
		u->touch_last_x = x;
		u->touch_last_y = y;
		if (!u->touch_moved)
			return;
		u->list_drag = 1;
		if (dy)
			set_list_scroll_px(list_scroll_px() - dy);
		return;
	}
	if (action == 8) {
		int was_tap = u->touch_down && !u->touch_moved &&
			u->touch_frames < 12;
		int row;

		u->touch_down = 0;
		u->touch_moved = 0;
		u->list_drag = 0;
		if (!was_tap)
			return;
		row = row_at_y(y);
		if (row < 0)
			return;
		if (u->mode == NB_MODE_WORKSPACE) {
			u->ws_sel = row;
			nb_ui_enter_workspace(row);
		} else {
			u->sel = row;
			/* the same as ENTER: a folder steps in, a file opens */
			nb_ui_browser_key(NB_KEY_ENTER, 0);
		}
		return;
	}
}

void nb_ui_enter_workspace(int index)
{
	nb_ui *u = nb_ui_get();

	if (!u->cfg || index < 0 || index >= u->cfg->nworkspaces)
		return;
	u->ws_sel = index;
	if (!nb_file_select(index)) {
		/* NO LISTING FOR THIS WORKSPACE (the launcher had no directory for
		 * it): say so instead of showing the previous folder's files */
		nb_str_copy(u->dir, sizeof(u->dir), nb_ws_dir(index), -1);
		u->mode = NB_MODE_BROWSER;
		u->sel = 0;
		u->top = u->list_yoff = 0;
		u->count = 0;
		nb_ui_message("这个工作区没有列表", 1);
		u->dirty = 1;
		return;
	}
	nb_str_copy(u->dir, sizeof(u->dir), nb_file_current_dir(), -1);
	u->mode = NB_MODE_BROWSER;
	u->sel = 0;
	u->top = 0;
	u->list_yoff = 0;
	u->count = nb_file_count();
	u->dirty = 1;
}

void nb_ui_enter_browser(void)
{
	nb_ui *u = nb_ui_get();

	/* the list belongs to a DIRECTORY now: coming back from the editor means
	 * showing the folder the file was in */
	nb_str_copy(u->dir, sizeof(u->dir), nb_file_current_dir(), -1);
	u->count = nb_file_count();
	u->mode = NB_MODE_BROWSER;
	u->count = nb_file_count();
	u->dirty = 1;
	scroll_to_selection();
}

/* ---------------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------------- */
static void draw_workspaces(void)
{
	nb_ui *u = nb_ui_get();
	nb_u32 classes[NB_HS_KIND];
	char right[24];
	int i, fit = rows_fit();

	nb_text_classes(u->th, classes);
	nb_gfx_fill(0, LIST_TOP, NB_SCREEN_W, LIST_BOT - LIST_TOP, u->th->bg);

	for (i = 0; i < nb_ws_count() && i < fit; i++) {
		int y = LIST_TOP + i * ROW_H;
		const char *label = nb_ws_label(i);
		int selected = (i == u->ws_sel);

		if (selected) {
			nb_gfx_fill(0, y, NB_SCREEN_W, ROW_H, u->th->sel_bg);
			nb_gfx_fill(0, y, 2, ROW_H, u->th->accent);
			nb_ui_puts_clip(4, y, label,
					u->th->sel_fg, NB_SCREEN_W - 8);
		} else {
			nb_ui_puts_clip(4, y, label, u->th->text,
					NB_SCREEN_W - 8);
		}
	}
	/* the selected workspace's path, so the label alone is never a guess */
	{
		int y = LIST_TOP + (nb_ws_count() > fit ? fit : nb_ws_count()) *
			ROW_H;

		if (y < LIST_BOT) {
			nb_gfx_fill(0, y, NB_SCREEN_W, LIST_BOT - y,
				    u->th->foot_bg);
			nb_ui_puts_clip(4, y, nb_ws_dir(u->ws_sel), u->th->sub_fg,
					NB_SCREEN_W - 8);
		}
	}
	nb_fmt_int(right, sizeof(right), nb_ws_count());
	nb_ui_header("nbCode：工作区", nb_ws_count() ? "文件" : "");
	/* ... and this screen's legend goes too: MENU says what MENU does, and the
	 * help page lists the rest (the user's report: no key legends in the bars) */
	nb_ui_status(NB_NULL, right, u->th->foot_bg);
}

static void draw_files(void)
{
	nb_ui *u = nb_ui_get();
	nb_u32 classes[NB_HS_KIND];
	char right[24];
	char line[64];
	char title[40];
	int i, fit = rows_fit();

	nb_text_classes(u->th, classes);
	{
		nb_str_copy(title, sizeof(title), "nbCode : ", -1);
		/* the folder's last component is the useful part */
		{
			const char *dir = u->dir;
			int last = 0, k;

			for (k = 0; dir[k]; k++) {
				if (dir[k] == '\\' || dir[k] == '/')
					last = k + 1;
			}
			nb_str_append(title, sizeof(title), dir + last);
		}
		nb_fmt_int(right, sizeof(right), u->count);
	}
	nb_gfx_fill(0, LIST_TOP, NB_SCREEN_W, LIST_BOT - LIST_TOP, u->th->bg);

	if (u->count == 0) {
		nb_ui_puts(4, LIST_TOP + 4, "(空文件夹)", u->th->sub_fg);
	}

	for (i = u->top; i < u->count && i < u->top + fit + 1; i++) {
		int y = LIST_TOP + (i - u->top) * ROW_H - u->list_yoff;
		int selected = (i == u->sel);
		int isdir = nb_file_is_dir(i);
		int n;

		if (y <= LIST_TOP - ROW_H || y >= LIST_BOT)
			continue;

		if (selected) {
			nb_gfx_fill(0, y, NB_SCREEN_W, ROW_H, u->th->sel_bg);
			/* the accent edge: which row is current, visible at a glance
			 * on a display where a 1 px tint is easy to miss */
			nb_gfx_fill(0, y, 2, ROW_H, u->th->accent);
		}
		n = nb_str_copy(line, sizeof(line), nb_file_name(i), -1);
		if (isdir && n < (int)sizeof(line) - 1) {
			line[n++] = '/';
			line[n] = 0;
		}
		if (selected) {
			nb_ui_puts_clip(4, y, line, u->th->sel_fg,
					NB_SCREEN_W - 70);
			if (!isdir) {
				char sz[16];

				nb_fmt_size(sz, sizeof(sz), nb_file_bytes(i));
				/* ONE PIXEL CLEAR OF THE SCROLLBAR: the bar is
				 * flush with the screen's edge now, and the size
				 * column used to touch it */
				nb_ui_puts(NB_SCREEN_W - 5 - nb_ui_puts_w(sz), y,
					   sz, u->th->sel_fg);
			}
		} else {
			nb_ui_puts_clip(4, y, line, u->th->text,
					NB_SCREEN_W - 70);
			if (!isdir) {
				char sz[16];

				nb_fmt_size(sz, sizeof(sz), nb_file_bytes(i));
				/* one pixel clear of the scrollbar (see above) */
				nb_ui_puts(NB_SCREEN_W - 5 - nb_ui_puts_w(sz), y,
					   sz, u->th->sub_fg);
			}
		}
	}
	/* THE SCROLLBAR, when the folder is taller than the list -- the same
	 * see-through bar the editor has, dragged for pixel-level scrolling */
	nb_ui_scrollbar_draw();
	/* AND THE HEADER LAST: a half scrolled row is drawn above LIST_TOP on
	 * purpose (that sliver is what makes the pixel scroll look right), so the
	 * header has to be painted over it -- otherwise the file names run into
	 * the title bar. */
	nb_ui_header(title, right);
	/* THE STATUS ROW CARRIES THE POSITION AND NOTHING ELSE.  It used to spell
	 * out "↵打开 退格 删除 esc 上级", which is a key legend on a keyboard that
	 * has no room for one: it was clipped in the middle of a word ("esc>"), it
	 * repeated what MENU and the help page say better, and the user asked for it
	 * to go. */
	if (u->count > fit) {
		char pos[24];

		nb_fmt_int(pos, sizeof(pos), u->sel + 1);
		nb_str_append(pos, sizeof(pos), "/");
		{
			char tot[12];

			nb_fmt_int(tot, sizeof(tot), u->count);
			nb_str_append(pos, sizeof(pos), tot);
		}
		nb_ui_status(NB_NULL, pos, u->th->foot_bg);
	} else {
		nb_ui_status(NB_NULL, NB_NULL, u->th->foot_bg);
	}
}

void nb_ui_browser_draw(void)
{
	nb_ui *u = nb_ui_get();

	if (u->mode == NB_MODE_WORKSPACE)
		draw_workspaces();
	else
		draw_files();
	if (u->input_active)
		nb_ui_input_draw();
	nb_ui_message_line();
	if (u->show_help)
		nb_ui_help_draw();
}

/* ---------------------------------------------------------------------------
 * actions
 * ------------------------------------------------------------------------- */
static void open_selected(void)
{
	nb_ui *u = nb_ui_get();
	const char *name;

	if (u->count <= 0)
		return;
	name = nb_file_name(u->sel);
	if (nb_file_is_dir(u->sel)) {
		/* A FOLDER ROW: the launcher listed the folders inside each workspace
		 * (src/nb_cfg.h: the directory table), so stepping in is a table lookup
		 * -- no round trip to Python, which is what made going back a key
		 *    press in the first place
		 * message before.  It is how the .hpappdir folders under C:\DATA are
		 * opened. */
		if (nb_file_enter(u->sel)) {
			nb_str_copy(u->dir, sizeof(u->dir),
				    nb_file_current_dir(), -1);
			u->count = nb_file_count();
			u->sel = 0;
			u->top = 0;
			u->dirty = 1;
		} else {
			nb_ui_message("无法打开该文件夹", 2);
		}
		return;
	}
	nb_ui_open_file(u->dir, name);
}

static void new_file_prompt(void)
{
	nb_ui *u = nb_ui_get();

	nb_ui_input_begin("new file name", ".c");
	(void)u;
}

static void new_file_commit(const char *name)
{
	nb_ui *u = nb_ui_get();
	char path[NB_PATH_MAX];
	int len;
	char *empty = (char *)nb_alloc(1);

	if (!empty)
		return;
	empty[0] = 0;
	nb_str_copy(path, sizeof(path), u->dir, -1);
	len = nb_strlen(path);
	if (len < (int)sizeof(path) - 2) {
		path[len] = '\\';
		path[len + 1] = 0;
	}
	nb_str_append(path, sizeof(path), name);
	{
		const char *err = "";

		if (!nb_file_spit(path, empty, 0, &err)) {
			nb_ui_message(err, 2);
			nb_free(empty);
			return;
		}
	}
	nb_free(empty);
	nb_ui_message("已新建", 0);
	nb_ui_open_file(u->dir, name);
}

static void delete_selected(void)
{
	nb_ui *u = nb_ui_get();

	if (u->count <= 0)
		return;
	nb_ui_prompt(NB_PROMPT_DELETE, nb_file_name(u->sel));
}

void nb_ui_browser_delete_commit(void)
{
	/* implemented by the prompt callback (nb_ui_prompt.c) */
}

void nb_ui_browser_after_delete(const char *name)
{
	nb_ui *u = nb_ui_get();
	int i;

	(void)name;
	for (i = 0; i < u->count; i++) {
		if (i == u->sel)
			break;
	}
	/* the launcher owns the real list, so ask it to rebuild: on the device this
	 * is a no-op until the next refresh (F6/esc back), which is honest */
	u->count = nb_file_count();
	if (u->sel >= u->count)
		u->sel = u->count > 0 ? u->count - 1 : 0;
	scroll_to_selection();
	u->dirty = 1;
}

/* ---------------------------------------------------------------------------
 * the browser's popup menu (the MENU key)
 *
 * The actions the F-keys used to hold, in a list the user can read.  New file and
 * refresh were already here; rename is new, and it is a real rename (the bytes go
 * to the new name and the old file is removed) because the launcher's listing is
 * a snapshot: the browser cannot rename a file it has not read.
 * ------------------------------------------------------------------------- */
static const char *const browser_menu_items[] = {
	"打开", "新建文件", "重命名", "删除", "刷新"
};
static const int browser_menu_actions[] = {
	NB_MENU_OPEN, NB_MENU_NEW_FILE, NB_MENU_RENAME, NB_MENU_DELETE,
	NB_MENU_REFRESH
};

void nb_ui_browser_menu(void)
{
	nb_ui *u = nb_ui_get();

	if (u->mode == NB_MODE_WORKSPACE) {
		static const char *const items[] = { "进入", "刷新" };
		static const int actions[] = { NB_MENU_ENTER, NB_MENU_REFRESH };

		nb_ui_menu_open("工作区", items, actions, 2);
		return;
	}
	nb_ui_menu_open("文件", browser_menu_items, browser_menu_actions,
			(int)(sizeof(browser_menu_items) /
			      sizeof(browser_menu_items[0])));
}

/* the file the delete/rename acts on, as a device path */
static void selected_path(char *out, int cap)
{
	nb_ui *u = nb_ui_get();
	const char *dir = nb_file_current_dir();

	nb_str_copy(out, cap, dir && dir[0] ? dir : u->dir, -1);
	if (out[0] && out[nb_strlen(out) - 1] != '\\' &&
	    nb_strlen(out) < cap - 2) {
		nb_str_append(out, cap, "\\");
	}
	nb_str_append(out, cap, nb_file_name(u->sel));
}

/* RENAME: read the file, write it under the new name, remove the old one.  The
 * calculator has no rename call in the API nbCode uses, and this is also what
 * makes it work across the two file systems (the firmware's and uio's). */
static void rename_selected(const char *name)
{
	nb_ui *u = nb_ui_get();
	char from[NB_PATH_MAX], to[NB_PATH_MAX];
	char *raw;
	const char *err = "";
	int len = 0;

	if (!name || !*name || u->count <= 0)
		return;
	if (nb_file_is_dir(u->sel)) {
		nb_ui_message("文件夹不能重命名", 1);
		return;
	}
	selected_path(from, sizeof(from));
	nb_str_copy(to, sizeof(to), from, -1);
	{
		int i, cut = -1;

		for (i = 0; to[i]; i++) {
			if (to[i] == '\\' || to[i] == '/')
				cut = i;
		}
		if (cut < 0)
			to[0] = 0;
		else
			to[cut + 1] = 0;
	}
	nb_str_append(to, sizeof(to), name);
	raw = nb_file_slurp(from, &len, &err);
	if (!raw) {
		nb_ui_message(err && *err ? err : "无法读取", 2);
		return;
	}
	if (!nb_file_spit(to, raw, len, &err)) {
		nb_free(raw);
		nb_ui_message(err && *err ? err : "写入失败", 2);
		return;
	}
	nb_free(raw);
	nb_file_remove(from);
	u->count = nb_file_count();
	nb_ui_message("已重命名", 0);
	u->dirty = 1;
}

void nb_ui_browser_action(int action)
{
	nb_ui *u = nb_ui_get();

	switch (action) {
	case NB_MENU_OPEN:
		nb_ui_browser_key(NB_KEY_ENTER, 0);
		break;
	case NB_MENU_ENTER:
		nb_ui_browser_key(NB_KEY_ENTER, 0);
		break;
	case NB_MENU_NEW_FILE:
		new_file_prompt();
		break;
	case NB_MENU_RENAME:
		if (u->count > 0) {
			nb_ui_input_begin("重命名为: ", nb_file_name(u->sel));
			u->input_then = NB_INPUT_RENAME;
		}
		break;
	case NB_MENU_DELETE:
		delete_selected();
		break;
	case NB_MENU_REFRESH:
		u->count = nb_file_count();
		nb_ui_message("列表已刷新", 0);
		break;
	default:
		break;
	}
	u->dirty = 1;
}

void nb_ui_browser_key(int what, int ch)
{
	nb_ui *u = nb_ui_get();

	/* the one line input is routed in nb_ui.c (the editor's menu opens it too),
	 * so by the time a key gets here there is no input to feed */

	/* THE TWO FUNCTION KEYS, ON EVERY SCREEN: MENU (this screen's popup) and
	 * HELP, before the per-screen dispatch so neither screen has to remember
	 * them (the calculator has no F1..F6, see nb_ui.h) */
	if (what == NB_KEY_MENU) {
		nb_ui_browser_menu();
		return;
	}
	if (what == NB_KEY_HELP) {
		u->show_help = !u->show_help;
		u->dirty = 1;
		return;
	}

	if (u->mode == NB_MODE_WORKSPACE) {
		switch (what) {
		case NB_KEY_UP:
			if (u->ws_sel > 0)
				u->ws_sel--;
			break;
		case NB_KEY_DOWN:
			if (u->ws_sel + 1 < nb_ws_count())
				u->ws_sel++;
			break;
		case NB_KEY_ENTER:
		case NB_KEY_RIGHT:
			nb_ui_enter_workspace(u->ws_sel);
			return;
		case NB_KEY_ESC:
			u->mode = NB_MODE_QUIT;
			break;
		case NB_KEY_CHAR:
			/* a letter jumps to a workspace whose label starts with it */
			if (ch) {
				int i;

				for (i = 0; i < nb_ws_count(); i++) {
					const char *l = nb_ws_label(i);

					if (l[0] == ch || l[0] == ch - 32) {
						u->ws_sel = i;
						break;
					}
				}
			}
			break;
		default:
			break;
		}
		u->dirty = 1;
		return;
	}

	/* the file list */
	switch (what) {
	case NB_KEY_UP:
		if (u->sel > 0)
			u->sel--;
		break;
	case NB_KEY_DOWN:
		if (u->sel + 1 < u->count)
			u->sel++;
		break;
	case NB_KEY_PGUP:
		u->sel -= rows_fit();
		if (u->sel < 0)
			u->sel = 0;
		break;
	case NB_KEY_PGDN:
		u->sel += rows_fit();
		if (u->sel >= u->count)
			u->sel = u->count > 0 ? u->count - 1 : 0;
		break;
	case NB_KEY_HOME:
		u->sel = 0;
		break;
	case NB_KEY_END:
		u->sel = u->count > 0 ? u->count - 1 : 0;
		break;
	case NB_KEY_ENTER:
	case NB_KEY_RIGHT:
		open_selected();
		return;
	case NB_KEY_ESC:
		/* ESC GOES UP ONE FOLDER FIRST, and only then back to the workspace
		 * list: that is what a file browser does, and it is how a folder
		 * entered by mistake is left. */
		if (nb_file_up()) {
			nb_str_copy(u->dir, sizeof(u->dir),
				    nb_file_current_dir(), -1);
			u->count = nb_file_count();
			u->sel = 0;
			u->top = 0;
			u->list_yoff = 0;
			u->dirty = 1;
			break;
		}
		u->mode = NB_MODE_WORKSPACE;
		break;
	/* THE DEL KEY (PPL 19, the key the keyboard labels with a backspace arrow) IS
	 * WHAT DELETES A FILE -- it is the obvious key for it, and the report asked
	 * for it by that name instead of a function-key chord. */
	case NB_KEY_BACKSPACE:
	case NB_KEY_DELETE:
		if (!u->input_active)
			delete_selected();
		break;
	case NB_KEY_CHAR:
		/* A LETTER jumps to the next file whose name starts with it, which
		 * is how a 60 file folder stays navigable; every other character
		 * is ignored here (digits are part of the calculator's alpha
		 * layer, and jumping on one would be nonsense). */
		if (ch && NB_ISALPHA(ch)) {
			int i;

			for (i = u->sel + 1; i < u->count; i++) {
				const char *n = nb_file_name(i);

				if (n[0] == ch || n[0] == ch - 32) {
					u->sel = i;
					break;
				}
			}
		}
		break;
	default:
		break;
	}
	scroll_to_selection();
	u->dirty = 1;
}

/* the text input lives here because the browser is the only user */
void nb_ui_input_begin(const char *title, const char *initial)
{
	nb_ui *u = nb_ui_get();

	u->input_then = NB_INPUT_NEW_FILE;

	nb_str_copy(u->input_title, sizeof(u->input_title), title, -1);
	nb_str_copy(u->input_text, sizeof(u->input_text),
		    initial ? initial : "", -1);
	u->input_len = nb_strlen(u->input_text);
	u->input_cursor = u->input_len;
	u->input_active = 1;
	u->dirty = 1;
}

void nb_ui_input_end(void)
{
	nb_ui *u = nb_ui_get();

	u->input_active = 0;
	u->input_len = 0;
	u->input_cursor = 0;
	u->input_text[0] = 0;
	u->dirty = 1;
}

int nb_ui_input_active(void)
{
	return nb_ui_get()->input_active;
}

const char *nb_ui_input_text(void)
{
	return nb_ui_get()->input_text;
}

/* the digits of a typed line number, or 0 when there are none */
static int line_number(const char *s)
{
	int v = 0, digits = 0;

	while (*s == ' ')
		s++;
	while (*s >= '0' && *s <= '9' && digits < 7) {
		v = v * 10 + (*s - '0');
		s++;
		digits++;
	}
	return digits ? v : 0;
}

void nb_ui_input_key(int what, int ch)
{
	nb_ui *u = nb_ui_get();

	switch (what) {
	case NB_KEY_CHAR:
		if (ch > 0 && u->input_len < NB_INPUT_MAX - 1) {
			u->input_text[u->input_len++] = (char)ch;
			u->input_text[u->input_len] = 0;
			u->input_cursor = u->input_len;
		}
		break;
	case NB_KEY_BACKSPACE:
		if (u->input_len > 0) {
			u->input_len--;
			u->input_text[u->input_len] = 0;
			u->input_cursor = u->input_len;
		}
		break;
	case NB_KEY_ENTER:
		{
			char name[NB_INPUT_MAX];
			int then = u->input_then;

			nb_str_copy(name, sizeof(name), u->input_text, -1);
			nb_ui_input_end();
			/* AN EMPTY ANSWER IS AN ANSWER for the replacement (it deletes
			 * the match) and for the search text (it clears it); every other
			 * question wants something typed */
			if (then == NB_INPUT_FIND) {
				nb_ui_editor_find_commit(name);
				break;
			}
			if (then == NB_INPUT_REPLACE) {
				nb_ui_editor_replace_commit(name);
				break;
			}
			if (then == NB_INPUT_REPLACE_ALL) {
				nb_ui_editor_replace_all_commit(name);
				break;
			}
			if (!name[0]) {
				nb_ui_message("没有输入", 1);
				break;
			}
			/* WHO ASKED decides what the answer means: the browser is
			 * naming a new file, the editor's menu is saving as or
			 * jumping to a line */
			if (then == NB_INPUT_SAVE_AS)
				nb_ui_editor_save_as(name);
			else if (then == NB_INPUT_GOTO)
				nb_ui_editor_goto(line_number(name));
			else if (then == NB_INPUT_RENAME)
				rename_selected(name);
			else
				new_file_commit(name);
		}
		break;
	case NB_KEY_ESC:
		nb_ui_input_end();
		nb_ui_message("已取消", 0);
		break;
	default:
		break;
	}
	u->dirty = 1;
}

void nb_ui_input_draw(void)
{
	nb_ui *u = nb_ui_get();
	int y = NB_SCREEN_H - 32;

	nb_gfx_fill(0, y, NB_SCREEN_W, 16, u->th->sel_bg);
	nb_ui_puts(4, y, u->input_title, u->th->sel_fg);
	nb_ui_puts(4 + 8 * (nb_strlen(u->input_title) + 1), y, u->input_text,
		   u->th->sel_fg);
	/* the caret: a block after the last character */
	{
		int x = 4 + 8 * (nb_strlen(u->input_title) + 1) +
			u->input_cursor * 8;

		nb_gfx_fill(x, y, 8, 16, u->th->sel_fg);
		nb_ui_puts(x, y, " ", u->th->sel_bg);
	}
}
