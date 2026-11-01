/*
 * nb_hex.c -- the hex view (nb_hex.h has the reasons and the limits).
 *
 * THE BUFFER IS THE HEAP'S, not .bss: 32 KB of static array would grow the image
 * that every session pays for, and this is opened for one file at a time.  One
 * read, one allocation, freed on the way out.
 *
 * THE ROW IS EXACTLY 40 COLUMNS, which is the whole width of the screen:
 *
 *     000000  41 42 43 44 45 46 47 48 ABCDEFGH
 *     ^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^
 *     6       23 (eight bytes, spaces) 8
 *
 * so a byte and the character it would print sit on the same line, and no column
 * has to be guessed at.
 */
#include "nb_hex.h"
#include "nb_ui.h"
#include "nb_gfx.h"
#include "nb_port.h"

#define OFF_X		4	/* where the offset starts */
#define OFF_COLS	6	/* hex digits of the offset (16 MB of file) */
#define TEXT_X		(OFF_X + (OFF_COLS + 1) * 8)
#define HEAD_Y		1	/* the header's text row (see nb_ui_draw.c) */

static char *buf;			/* the bytes held (<= NB_HEX_MAX) */
static char hex_name[64];
static char hex_path[NB_PATH_MAX];
static int len;				/* bytes held */
static int full_len;			/* the file's size, even when truncated */
static int yoff;			/* pixel scroll, like every other view */
static int drag_last_y;

void nb_hex_init(void)
{
	if (buf)
		nb_free(buf);
	buf = NB_NULL;
	hex_name[0] = 0;
	hex_path[0] = 0;
	len = 0;
	full_len = 0;
	yoff = 0;
	drag_last_y = 0;
}

int nb_hex_active(void)
{
	return buf != NB_NULL;
}

const char *nb_hex_name(void)
{
	return hex_name;
}

const char *nb_hex_path(void)
{
	return hex_path;
}

int nb_hex_len(void)
{
	return len;
}

int nb_hex_full_len(void)
{
	return full_len;
}

void nb_hex_close(void)
{
	if (buf)
		nb_free(buf);
	buf = NB_NULL;
	len = 0;
	full_len = 0;
	hex_name[0] = 0;
	hex_path[0] = 0;
	yoff = 0;
}

int nb_hex_open(const char *p, const char *n)
{
	void *h;
	int got = 0;

	nb_hex_close();
	if (!p || !*p)
		return 0;
	nb_str_copy(hex_path, sizeof(hex_path), p, -1);
	nb_str_copy(hex_name, sizeof(hex_name), (n && *n) ? n : p, -1);
	full_len = nb_file_size(p);
	if (full_len < 0)
		full_len = 0;
	h = nb_file_open(p, "rb");
	if (!h)
		return 0;
	buf = (char *)nb_alloc(NB_HEX_MAX);
	if (!buf) {
		nb_file_close(h);
		return 0;
	}
	/* ONE READ, CAPPED: this is a view, and a file bigger than the cap is shown
	 * from its start with the status line saying so */
	while (got < NB_HEX_MAX) {
		int n2 = nb_file_read(h, buf + got, NB_HEX_MAX - got);

		if (n2 <= 0)
			break;
		got += n2;
	}
	nb_file_close(h);
	if (got <= 0) {
		nb_hex_close();
		return 0;
	}
	len = got;
	yoff = 0;
	return 1;
}

/* ---------------------------------------------------------------------------
 * the view, in pixels (the same arrangement as the editor and the file list)
 * ------------------------------------------------------------------------- */
static int total_rows(void)
{
	return (len + NB_HEX_COLS - 1) / NB_HEX_COLS;
}

int nb_hex_max_scroll_px(void)
{
	int total = total_rows() * NB_HEX_ROW_H;
	int body = NB_BODY_BOT - NB_BODY_TOP;

	return total > body ? total - body : 0;
}

int nb_hex_scroll_px(void)
{
	return yoff;
}

void nb_hex_set_scroll_px(int px)
{
	int max = nb_hex_max_scroll_px();

	if (px < 0)
		px = 0;
	else if (px > max)
		px = max;
	yoff = px;
}

static int visible_rows(void)
{
	return (NB_BODY_BOT - NB_BODY_TOP) / NB_HEX_ROW_H;
}

static void scroll_rows(int n)
{
	nb_hex_set_scroll_px(yoff + n * NB_HEX_ROW_H);
}

/* ---------------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------------- */
static void hex_digits(char *out, unsigned int v, int digits)
{
	nb_fmt_hex(out, digits + 1, v, digits);
}

void nb_hex_draw(void)
{
	nb_ui *u = nb_ui_get();
	char line[48];
	char head[80];
	int i, row0, row1, y, first, last;

	if (!buf) {
		nb_ui_header("HEX", NB_NULL);
		nb_ui_status("没有打开文件", NB_NULL, u->th->foot_bg);
		return;
	}
	row0 = yoff / NB_HEX_ROW_H;
	row1 = row0 + visible_rows() + 1;

	nb_gfx_fill(0, NB_BODY_TOP, NB_SCREEN_W, NB_BODY_BOT - NB_BODY_TOP,
		    u->th->bg);
	nb_gfx_fill(0, NB_BODY_TOP, TEXT_X - OFF_X, NB_BODY_BOT - NB_BODY_TOP,
		    u->th->gutter_bg);
	nb_gfx_hline(TEXT_X - 5, NB_BODY_TOP, 1,
		     /* the gutter's edge, the whole body tall */
		     u->th->line);

	for (i = row0; i <= row1; i++) {
		int off = i * NB_HEX_COLS;
		int k, n = 0;

		y = NB_BODY_TOP + i * NB_HEX_ROW_H - yoff;
		if (y <= NB_BODY_TOP - NB_HEX_ROW_H || y >= NB_BODY_BOT)
			continue;
		if (off >= len)
			break;
		/* the gutter: the offset, in the gutter's own colour */
		hex_digits(head, (unsigned int)off, OFF_COLS);
		nb_ui_puts(OFF_X, y, head, u->th->gutter_fg);
		/* the row: eight bytes, a space, eight characters */
		for (k = 0; k < NB_HEX_COLS; k++) {
			if (k)
				line[n++] = ' ';
			if (off + k < len)
				hex_digits(line + n, (unsigned char)buf[off + k],
					   2);
			else
				line[n] = line[n + 1] = '.';
			n += 2;
		}
		line[n++] = ' ';
		for (k = 0; k < NB_HEX_COLS; k++) {
			int c = (off + k < len) ? (unsigned char)buf[off + k]
						: ' ';

			line[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
		}
		line[n] = 0;
		nb_ui_puts(TEXT_X, y, line, u->th->text);
	}
	nb_ui_scrollbar_draw();
	/* the header: the file's name and HEX, where the type would be */
	{
		char right[24];

		nb_str_copy(head, sizeof(head), hex_name, -1);
		nb_fmt_size(right, sizeof(right), (unsigned int)full_len);
		nb_str_append(head, sizeof(head), "  HEX  ");
		nb_str_append(head, sizeof(head), right);
	}
	nb_ui_header(head, NB_NULL);
	/* the status: which bytes are on the glass, and the truncation if there is
	 * one (a view that silently showed the first 32 KB of a 300 KB file would
	 * be the kind of lie this editor does not tell) */
	{
		char pos[48];
		char sz[16];

		first = row0 * NB_HEX_COLS;
		last = (row0 + visible_rows()) * NB_HEX_COLS;
		if (last > len)
			last = len;
		nb_str_copy(pos, sizeof(pos), "0x", -1);
		hex_digits(sz, (unsigned int)first, 4);
		nb_str_append(pos, sizeof(pos), sz);
		nb_str_append(pos, sizeof(pos), "-0x");
		hex_digits(sz, (unsigned int)last, 4);
		nb_str_append(pos, sizeof(pos), sz);
		nb_str_append(pos, sizeof(pos), "  ");
		nb_fmt_size(sz, sizeof(sz), (unsigned int)len);
		nb_str_append(pos, sizeof(pos), sz);
		if (full_len > len)
			nb_str_append(pos, sizeof(pos), "（只读前 32K）");
		else
			nb_str_append(pos, sizeof(pos), "  只读");
		nb_ui_status(NB_NULL, NB_NULL, u->th->foot_bg);
		nb_ui_puts_clip(4, NB_SCREEN_H - 16, pos, u->th->sub_fg,
				NB_SCREEN_W - 8);
	}
	(void)HEAD_Y;
}

/* ---------------------------------------------------------------------------
 * the menu: hand the file to the editor, or close the view
 * ------------------------------------------------------------------------- */
static const char *const hex_menu_items[] = { "以文本打开", "关闭" };
static const int hex_menu_actions[] = { NB_MENU_HEX_TEXT, NB_MENU_HEX_CLOSE };

void nb_ui_hex_menu(void)
{
	if (!buf)
		return;
	nb_ui_menu_open(NB_NULL, hex_menu_items, hex_menu_actions, 2);
}

/* THE MENU'S ANSWERS ARE CARRIED OUT HERE because the hex view owns its state:
 * nb_ui.c routes the action id to whoever is up (see nb_ui_menu_action). */
void nb_hex_menu_action(int action)
{
	char dir[NB_PATH_MAX];
	char nm[64];
	int i, cut = -1;

	if (!buf)
		return;
	/* THE MENU GOES FIRST: the overlay is what the user just answered, and both
	 * answers here change the whole screen */
	nb_ui_overlay_close();
	nb_str_copy(dir, sizeof(dir), hex_path, -1);
	nb_str_copy(nm, sizeof(nm), hex_name, -1);
	if (action == NB_MENU_HEX_CLOSE) {
		nb_hex_close();
		nb_ui_enter_browser();
		nb_ui_message("文件列表", 0);
		return;
	}
	if (action != NB_MENU_HEX_TEXT)
		return;
	for (i = 0; dir[i]; i++) {
		if (dir[i] == '\\' || dir[i] == '/')
			cut = i;
	}
	if (cut < 0) {
		dir[0] = 0;
	} else {
		dir[cut] = 0;
	}
	nb_hex_close();
	/* the editor, unconditionally: the user has just said that this IS text
	 * (a Latin-1 source file is a real thing, and the guess that sent it to the
	 * hex view was a guess) */
	if (!nb_ui_open_file_text(dir, nm))
		nb_ui_enter_browser();
}

/* ---------------------------------------------------------------------------
 * keys and touches
 * ------------------------------------------------------------------------- */
void nb_hex_key(int what, int ch)
{
	nb_ui *u = nb_ui_get();

	if (!buf)
		return;
	switch (what) {
	case NB_KEY_UP:
		scroll_rows(-1);
		break;
	case NB_KEY_DOWN:
		scroll_rows(1);
		break;
	case NB_KEY_PGUP:
		scroll_rows(-visible_rows());
		break;
	case NB_KEY_PGDN:
		scroll_rows(visible_rows());
		break;
	case NB_KEY_HOME:
		nb_hex_set_scroll_px(0);
		break;
	case NB_KEY_END:
		nb_hex_set_scroll_px(nb_hex_max_scroll_px());
		break;
	case NB_KEY_ESC:
		nb_hex_close();
		nb_ui_enter_browser();
		nb_ui_message("文件列表", 0);
		return;
	case NB_KEY_MENU:
		nb_ui_hex_menu();
		return;
	default:
		(void)ch;
		break;
	}
	u->dirty = 1;
}

void nb_hex_touch(int action, int x, int y)
{
	if (!buf)
		return;
	if (action == 1) {
		drag_last_y = y;
		return;
	}
	if (action == 2) {
		/* DRAGGING MOVES THE BYTES WITH THE FINGER, like the other two views
		 * (a pixel at a time, clamped to the file by set_scroll_px) */
		nb_hex_set_scroll_px(yoff - (y - drag_last_y));
		drag_last_y = y;
		nb_ui_get()->dirty = 1;
		return;
	}
	drag_last_y = 0;
	(void)x;
}
