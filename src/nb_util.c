/*
 * nb_util.c -- the small shared helpers: themes and number formatting.
 *
 * There is no printf on the device (the primetcc runtime has one, but it is a
 * 25 KB engine for a 6 KB job and its format support is the historical source of
 * two bugs in this workspace), so nbCode formats numbers with the handful of
 * functions below and every caller passes a fixed buffer.
 */
#include "nb_common.h"
#include "nb_cfg.h"

/* ---------------------------------------------------------------------------
 * themes
 * ------------------------------------------------------------------------- */
static nb_theme theme;

void nb_theme_set(nb_theme *t, int dark)
{
	if (dark) {
		t->text = NB_D_TEXT;
		t->key = NB_D_KEY;
		t->ctrl = NB_D_CTRL;
		t->constant = NB_D_CONST;
		t->string = NB_D_STRING;
		t->op = NB_D_OP;
		t->lib = NB_D_LIB;
		t->comment = NB_D_COMMENT;
		t->preproc = NB_D_PREPROC;
		t->cursor = NB_D_CURSOR;
		t->curline = NB_D_CURLINE;
		t->gutter_bg = NB_D_GUTTER_BG;
		t->gutter_fg = NB_D_GUTTER_FG;
		t->match = NB_D_MATCH;
		t->find = NB_D_FIND;
		t->bg = NB_D_BG;
		t->fg = NB_D_TEXT;
		t->head_bg = NB_D_SURFACE;
		t->head_fg = NB_D_TEXT;
		t->sel_bg = NB_D_SEL_BG;
		t->sel_fg = NB_D_SEL_FG;
		t->line = NB_D_LINE;
		t->foot_bg = NB_D_SURFACE;
		t->sub_fg = NB_D_SUB;
		t->accent = NB_D_ACCENT;
		t->muted = NB_D_MUTED;
		t->dark = 1;
	} else {
		t->text = NB_C_TEXT;
		t->key = NB_C_KEY;
		t->ctrl = NB_C_CTRL;
		t->constant = NB_C_CONST;
		t->string = NB_C_STRING;
		t->op = NB_C_OP;
		t->lib = NB_C_LIB;
		t->comment = NB_C_COMMENT;
		t->preproc = NB_C_PREPROC;
		t->cursor = NB_C_CURSOR;
		t->curline = NB_C_CURLINE;
		t->gutter_bg = NB_C_GUTTER_BG;
		t->gutter_fg = NB_C_GUTTER_FG;
		t->match = NB_C_MATCH;
		t->find = NB_C_FIND;
		t->bg = NB_L_BG;
		t->fg = NB_L_TEXT;
		t->head_bg = NB_L_SURFACE;
		t->head_fg = NB_L_TEXT;
		t->sel_bg = NB_L_SEL_BG;
		t->sel_fg = NB_L_SEL_FG;
		t->line = NB_L_LINE;
		t->foot_bg = NB_L_SURFACE;
		t->sub_fg = NB_L_SUB;
		t->accent = NB_L_ACCENT;
		t->muted = NB_L_MUTED;
		t->dark = 0;
	}
}

const nb_theme *nb_theme_get(void)
{
	return &theme;
}

/* ---------------------------------------------------------------------------
 * numbers into fixed buffers
 * ------------------------------------------------------------------------- */
int nb_fmt_uint(char *out, int cap, unsigned int v)
{
	char tmp[12];
	int n = 0, i;

	if (cap <= 0)
		return 0;
	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && n < 11);
	for (i = 0; i < n && i < cap - 1; i++)
		out[i] = tmp[n - 1 - i];
	out[i] = 0;
	return i;
}

int nb_fmt_int(char *out, int cap, int v)
{
	if (v < 0) {
		if (cap <= 1) {
			out[0] = 0;
			return 0;
		}
		out[0] = '-';
		return 1 + nb_fmt_uint(out + 1, cap - 1, (unsigned int)(-v));
	}
	return nb_fmt_uint(out, cap, (unsigned int)v);
}

int nb_fmt_hex(char *out, int cap, unsigned int v, int digits)
{
	static const char *hex = "0123456789ABCDEF";
	int i;

	if (cap <= 0)
		return 0;
	if (digits <= 0)
		digits = 8;
	if (digits > cap - 1)
		digits = cap - 1;
	for (i = 0; i < digits; i++)
		out[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xF];
	out[digits] = 0;
	return digits;
}

/* "812B", "1.4K", "2.3M": the sizes a calculator file list shows */
int nb_fmt_size(char *out, int cap, unsigned int bytes)
{
	int n;

	if (bytes < 1024) {
		n = nb_fmt_uint(out, cap, bytes);
		if (n < cap - 1) {
			out[n++] = 'B';
			out[n] = 0;
		}
		return n;
	}
	if (bytes < 1024u * 1024u) {
		unsigned int whole = bytes >> 10;
		unsigned int tenth = ((bytes & 1023u) * 10u) >> 10;

		n = nb_fmt_uint(out, cap, whole);
		if (n < cap - 2) {
			out[n++] = '.';
			out[n++] = (char)('0' + tenth);
			out[n++] = 'K';
			out[n] = 0;
		}
		return n;
	}
	{
		unsigned int whole = bytes >> 20;
		unsigned int tenth = ((bytes & 0xFFFFFu) * 10u) >> 20;

		n = nb_fmt_uint(out, cap, whole);
		if (n < cap - 2) {
			out[n++] = '.';
			out[n++] = (char)('0' + tenth);
			out[n++] = 'M';
			out[n] = 0;
		}
		return n;
	}
}

int nb_str_copy(char *dst, int cap, const char *src, int n)
{
	int i = 0;

	if (cap <= 0)
		return 0;
	if (n < 0) {
		while (src && src[i] && i < cap - 1) {
			dst[i] = src[i];
			i++;
		}
	} else {
		while (src && i < n && i < cap - 1) {
			dst[i] = src[i];
			i++;
		}
	}
	dst[i] = 0;
	return i;
}

int nb_str_append(char *dst, int cap, const char *src)
{
	int n = nb_strlen(dst);

	return n + nb_str_copy(dst + n, cap - n, src, -1);
}

/* the config's boolean flags (nb_cfg.h) as a predicate */
int nb_cfg_flag(const nb_config *c, unsigned int flag)
{
	return (c && (c->flags & flag)) ? 1 : 0;
}
