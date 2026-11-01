/*
 * nb_ui_prompt.c -- the modal questions.
 *
 * Four keys answer everything: y / n / ENTER / ESC, plus the letter shortcuts
 * the rest of this workspace's apps use (y = yes, n = no).  A question owns the
 * keyboard while it is up -- nb_ui.c routes every key here first -- which is what
 * makes "quit without saving?" impossible to answer by accident with an arrow
 * key.
 *
 * The delete question is the only destructive one, and it names the file it will
 * remove, in the box, so a mis-pressed F3 cannot delete something the user did
 * not read.
 */
#include "nb_ui.h"
#include "nb_gfx.h"
#include "nb_port.h"

/* THE ANSWERS ARE BUTTONS, NOT LETTERS.
 *
 * The question used to be answered with y / n / esc, which is a keyboard legend
 * on a calculator whose keyboard prints no "y" and no "n" (they are the alpha
 * layer of two keypad keys).  Every question is now a row of BUTTONS: the LEFT and
 * RIGHT arrows move the highlight, ENTER -- or a tap -- presses the highlighted
 * one, and ESC cancels.  The labels and the answers are here, together, so the
 * drawing and the key handling cannot disagree about the order. */
static const char *const save_labels[] = { "保存", "不保存", "取消" };
static const int save_answers[] = { 1, -1, 0 };
static const char *const yesno_labels[] = { "是", "否" };
static const int yesno_answers[] = { 1, -1 };

static int prompt_buttons(const char *const **labels, const int **answers)
{
	switch (nb_ui_get()->prompt) {
	case NB_PROMPT_SAVE:
		*labels = save_labels;
		*answers = save_answers;
		return 3;
	case NB_PROMPT_DISCARD:
	case NB_PROMPT_DELETE:
		*labels = yesno_labels;
		*answers = yesno_answers;
		return 2;
	default:
		*labels = NB_NULL;
		*answers = NB_NULL;
		return 0;
	}
}

int nb_ui_prompt_button_count(void)
{
	const char *const *l;
	const int *a;

	return prompt_buttons(&l, &a);
}

const char *nb_ui_prompt_button(int i)
{
	const char *const *l;
	const int *a;
	int n = prompt_buttons(&l, &a);

	return (l && i >= 0 && i < n) ? l[i] : NB_NULL;
}

void nb_ui_prompt(int kind, const char *text)
{
	nb_ui *u = nb_ui_get();

	u->prompt = kind;
	nb_str_copy(u->prompt_text, sizeof(u->prompt_text),
		    text ? text : "", -1);
	/* THE FIRST BUTTON IS THE DEFAULT, and for the save question that is
	 * "save": the safe answer is the one a user reaches by pressing ENTER. */
	u->prompt_sel = 0;
	u->dirty = 1;
}

static void leave_editor(int then)
{
	nb_ui *u = nb_ui_get();

	if (then == NB_THEN_QUIT) {
		u->mode = NB_MODE_QUIT;
	} else if (then == NB_THEN_BACK) {
		nb_ui_editor_close();
		nb_ui_enter_browser();
		nb_ui_message("文件列表", 0);
	}
}

/* THE ANSWER IS THREE WAYS, NOT TWO: yes, no, and "never mind".
 *
 * ENTER/ON and 'y' mean yes; 'n' means no; ESC means CANCEL.  It used to fold ESC
 * into "no", which is fine for a delete question and wrong for a save one: ESC
 * asked "退出前保存吗？" and answered no, so the editor quit -- the question
 * the user was answering with 'n' was never really a choice. */
static void finish(int answer)
{
	nb_ui *u = nb_ui_get();
	int kind = u->prompt;
	int then = u->prompt_then;

	u->prompt = NB_PROMPT_NONE;
	u->prompt_then = NB_THEN_NOWHERE;
	u->dirty = 1;
	switch (kind) {
	case NB_PROMPT_SAVE:
		if (answer == 0)
			return;			/* cancel: stay in the editor */
		if (answer > 0 && !nb_ui_editor_save()) {
			nb_ui_message("保存失败：继续编辑", 2);
			return;			/* still open, nothing lost */
		}
		leave_editor(then);		/* yes or no: the key said leave */
		break;
	case NB_PROMPT_DISCARD:
		if (answer > 0)
			leave_editor(NB_THEN_BACK);
		break;
	case NB_PROMPT_DELETE:
		if (answer > 0) {
			char path[NB_PATH_MAX];

			nb_str_copy(path, sizeof(path), u->dir, -1);
			{
				int n = nb_strlen(path);

				if (n > 0 && path[n - 1] != '\\' &&
				    path[n - 1] != '/') {
					path[n] = '\\';
					path[n + 1] = 0;
				}
			}
			nb_str_append(path, sizeof(path), u->prompt_text);
			if (nb_file_remove(path))
				nb_ui_message("已删除", 0);
			else
				nb_ui_message("删除失败", 2);
			u->count = nb_file_count();
			if (u->sel >= u->count)
				u->sel = u->count > 0 ? u->count - 1 : 0;
		}
		break;
	default:
		break;
	}
}

void nb_ui_prompt_save(const char *name, int then)
{
	nb_ui *u = nb_ui_get();

	nb_ui_prompt(NB_PROMPT_SAVE, name);
	u->prompt_then = then;
}

/* THE BUTTONS AND THE KEYS, IN ONE PLACE.  A scan code comes in, the row of
 * buttons says which answer it means.
 *
 * The y / n shortcuts still WORK (they are what a user of the other apps in this
 * workspace has in their fingers, and the delete question has had them for
 * months), but nothing on the screen mentions them any more: the buttons are the
 * interface.  'y' is the key at 0x59 and 'n' the one at 0x4E (nb_keymap.c has the
 * verified table), and the ASCII values are accepted as well -- which is what a
 * host test types. */
void nb_ui_prompt_key(int code)
{
	nb_ui *u = nb_ui_get();
	const char *const *labels;
	const int *answers;
	int n = prompt_buttons(&labels, &answers);
	int sel = u->prompt_sel;

	if (n <= 0)
		return;
	if (code == 0x02 || code == 0x03) {		/* LEFT / UP */
		sel--;
		if (sel < 0)
			sel = n - 1;
	} else if (code == 0x04 || code == 0x05) {	/* RIGHT / DOWN */
		sel++;
		if (sel >= n)
			sel = 0;
	} else if (code == 0x0D) {			/* ENTER */
		finish(answers[sel]);
		return;
	} else if (code == 0x01 || code == 0x83) {
		finish(0);			/* ESC and ON cancel */
		return;
	} else if (code == 0x59 || code == 0x79) {
		finish(1);			/* the old 'y' alias */
		return;
	} else if (code == 0x4E || code == 0x6E) {
		finish(n > 1 ? answers[1] : 0);	/* the old 'n' alias */
		return;
	} else {
		return;
	}
	u->prompt_sel = sel;
	u->dirty = 1;
}

/* PRESS BUTTON `i` (the touch handler and the tests use this) */
void nb_ui_prompt_choose(int i)
{
	const char *const *labels;
	const int *answers;
	int n = prompt_buttons(&labels, &answers);

	if (n <= 0 || i < 0 || i >= n)
		return;
	nb_ui_get()->prompt_sel = i;
	finish(answers[i]);
}

/* the button row's geometry, shared by the drawing and the touch handler */
#define PROMPT_X	16
#define PROMPT_W	(NB_SCREEN_W - 32)
#define PROMPT_Y	92
#define PROMPT_H	84
#define BTN_H		22
#define BTN_GAP		8
#define BTN_Y		(PROMPT_Y + PROMPT_H - BTN_H - 8)

void nb_ui_prompt_rect(int i, int *x, int *y, int *w, int *h)
{
	const char *const *labels;
	const int *answers;
	int n = prompt_buttons(&labels, &answers);
	int bw;

	if (x)
		*x = 0;
	if (y)
		*y = 0;
	if (w)
		*w = 0;
	if (h)
		*h = 0;
	if (n <= 0 || i < 0 || i >= n)
		return;
	bw = (PROMPT_W - 16 - (n - 1) * BTN_GAP) / n;
	if (x)
		*x = PROMPT_X + 8 + i * (bw + BTN_GAP);
	if (y)
		*y = BTN_Y;
	if (w)
		*w = bw;
	if (h)
		*h = BTN_H;
}

/* which button a point is on, or -1 */
int nb_ui_prompt_hit(int px, int py)
{
	int i, n = nb_ui_prompt_button_count();

	for (i = 0; i < n; i++) {
		int x, y, w, h;

		nb_ui_prompt_rect(i, &x, &y, &w, &h);
		if (px >= x && px < x + w && py >= y && py < y + h)
			return i;
	}
	return -1;
}

void nb_ui_prompt_draw(void)
{
	nb_ui *u = nb_ui_get();
	const char *const *labels;
	const int *answers;
	int n = prompt_buttons(&labels, &answers);
	const char *q = "？";
	int i;

	switch (u->prompt) {
	case NB_PROMPT_SAVE:
		q = "关闭前保存吗？";
		break;
	case NB_PROMPT_DISCARD:
		q = "放弃修改？";
		break;
	case NB_PROMPT_DELETE:
		q = "删除此文件？";
		break;
	default:
		return;
	}
	if (u->prompt_sel < 0 || u->prompt_sel >= n)
		u->prompt_sel = 0;
	/* the same look as every other panel (surface, hairline, accent edge) */
	nb_ui_panel(PROMPT_X, PROMPT_Y, PROMPT_W, PROMPT_H);
	nb_ui_puts(PROMPT_X + 8, PROMPT_Y + 8, q, u->th->text);
	nb_ui_puts_clip(PROMPT_X + 8, PROMPT_Y + 26, u->prompt_text,
			u->th->sub_fg, PROMPT_W - 16);
	for (i = 0; i < n; i++) {
		int x, y, w, h, tw;

		nb_ui_prompt_rect(i, &x, &y, &w, &h);
		if (i == u->prompt_sel) {
			nb_gfx_fill(x, y, w, h, u->th->sel_bg);
			nb_gfx_frame(x, y, w, h, u->th->accent);
		} else {
			nb_gfx_fill(x, y, w, h, u->th->bg);
			nb_gfx_frame(x, y, w, h, u->th->line);
		}
		tw = nb_ui_puts_w(labels[i]);
		nb_ui_puts(x + (w - tw) / 2, y + (h - 16) / 2, labels[i],
			   i == u->prompt_sel ? u->th->sel_fg : u->th->text);
	}
}
