/*
 * nb_ui_draw.c -- the shared chrome: header bar, status bar, text helpers.
 *
 * The layout is the one FileManager, the Oxford dictionary app and primeLua's
 * launcher all use, because a user moving between them should not have to learn
 * a second one:
 *
 *   y=0..17    header: what this screen is, and the state on the right
 *   y=18..223  the body
 *   y=224..239 status: hints, position, and the last message
 *
 * The screen is 320x240 at 8 pixels per western character, so a line of the
 * header holds 40 characters: every string in nbCode is written to fit that,
 * and nb_ui_puts() clips rather than wrapping (a wrapped header is how a title
 * loses its state indicator).
 */
#include "nb_ui.h"
#include "nb_gfx.h"
#include "nb_port.h"

#define HEAD_Y		1		/* one row of air above the text */
#define STATUS_Y	(NB_SCREEN_H - 16)

void nb_ui_puts(int x, int y, const char *text, nb_u32 colour)
{
	nb_text_draw(nb_text_scratch(), x, y, text, colour, (const nb_theme *)
		     (nb_ui_get()->th));
}

int nb_ui_puts_w(const char *text)
{
	return nb_text_width(nb_text_scratch(), text);
}

/* the same, clipped to `pixels`: text that would run into the right hand
 * indicator is cut with a '>' so it is obvious the string was longer */
void nb_ui_puts_clip(int x, int y, const char *text, nb_u32 colour, int pixels);

void nb_ui_puts_clip(int x, int y, const char *text, nb_u32 colour, int pixels)
{
	nb_ui *u = nb_ui_get();
	nb_text_decoded *d = nb_text_scratch();
	int frames = pixels / 8;

	nb_text_decode(d, text, nb_strlen(text), NB_NULL, NB_ENC_UTF8);
	if (d->frames > frames && frames > 1) {
		/* drop characters from the end until it fits, then mark it */
		nb_text_decoded cut;
		int bytes = nb_text_byte_at(d, frames - 1);

		nb_text_decode(&cut, text, bytes, NB_NULL, NB_ENC_UTF8);
		nb_text_draw_decoded(&cut, x, y, 0, 0, u->th, u->classes, 0, 0);
		nb_text_draw(d, x + (frames - 1) * 8, y, ">", colour, u->th);
		return;
	}
	nb_text_draw(d, x, y, text, colour, u->th);
}

void nb_ui_header(const char *title, const char *right)
{
	nb_ui *u = nb_ui_get();

	/* A FLAT BAR WITH ONE ACCENT LINE UNDER IT.  The header used to be a dark
	 * blue block (FileManager's) with white text; the modern shape is a quiet
	 * surface with the page's own ink, and a single accent hairline doing the
	 * separating -- which also reads on a 320x240 panel where a heavy frame
	 * just eats pixels.
	 *
	 * THE RULE SITS ON THE LAST ROW OF THE BAR, NOT INSIDE THE TEXT: it used to
	 * be drawn at y=15, which is the bottom row of an 8x16 glyph, and a title
	 * with a descender came out cut off (see NB_HEAD_H). */
	nb_gfx_fill(0, 0, NB_SCREEN_W, NB_HEAD_H, u->th->head_bg);
	nb_gfx_hline(0, NB_HEAD_H - 1, NB_SCREEN_W, u->th->accent);
	nb_ui_puts(4, HEAD_Y, title, u->th->head_fg);
	if (right) {
		int w = nb_ui_puts_w(right);

		/* the right hand end is METADATA (a count, a language): second
		 * colour, not the title's */
		nb_ui_puts(NB_SCREEN_W - 4 - w, HEAD_Y, right, u->th->sub_fg);
	}
}

void nb_ui_status(const char *left, const char *right, nb_u32 bg)
{
	nb_ui *u = nb_ui_get();

	nb_gfx_fill(0, STATUS_Y, NB_SCREEN_W, 16, bg);
	/* the divider between the page and the status bar */
	nb_gfx_hline(0, STATUS_Y, NB_SCREEN_W, u->th->line);
	if (left)
		nb_ui_puts_clip(4, STATUS_Y, left, u->th->sub_fg, 180);
	if (right) {
		int w = nb_ui_puts_w(right);

		nb_ui_puts(NB_SCREEN_W - 4 - w, STATUS_Y, right, u->th->fg);
	}
}

/* a one line message in the status bar, coloured by kind: this is how nbCode
 * tells the user "saved", "opened x.c", "no memory" without a dialog */
void nb_ui_message_line(void)
{
	nb_ui *u = nb_ui_get();
	nb_u32 col = u->th->fg;

	/* NO MESSAGE, NO LINE: this used to fill the status row unconditionally,
	 * which meant the position/file type/layer indicators were covered even
	 * when there was nothing to say.  While the toast is up it owns the row
	 * (the indicators are drawn over it, see nb_ui_editor.c). */
	if (!u->message[0] || u->msg_frames <= 0)
		return;
	if (u->message_kind == 1)
		col = NB_RGB(0x92, 0x40, 0x0E);
	else if (u->message_kind == 2)
		col = NB_RGB(0xB9, 0x1C, 0x1C);
	nb_gfx_fill(0, STATUS_Y, NB_SCREEN_W, 16, u->th->foot_bg);
	nb_ui_puts_clip(4, STATUS_Y, u->message, col, NB_SCREEN_W - 88);
}

/* the right hand end of the status row ON ITS OWN, after the message.
 *
 * nb_ui_message_line() fills the whole row (a message takes the row over), so an
 * indicator drawn before it is erased by every message -- and the editor always
 * has one.  The layer indicator (letters/capitals, see nb_ui_editor.c) is the one
 * thing on that row that must never be hidden, because it is what tells the user
 * that the next key will be a letter instead of a digit.  The message clips
 * itself at NB_SCREEN_W - 88, which is the room this needs. */
void nb_ui_status_right(const char *right)
{
	nb_ui *u = nb_ui_get();

	if (!right || !*right)
		return;
	nb_ui_puts(NB_SCREEN_W - 4 - nb_ui_puts_w(right), STATUS_Y, right,
		   u->th->fg);
}

/* a PANEL: the surface, a hairline border, and a left accent edge.  Every panel
 * in nbCode (the menus, the information, the palette, the help, the question)
 * goes through this so they all look like the same interface. */
void nb_ui_panel(int x, int y, int w, int h)
{
	nb_ui *u = nb_ui_get();

	nb_gfx_fill(x, y, w, h, u->th->foot_bg);
	nb_gfx_frame(x, y, w, h, u->th->line);
	nb_gfx_fill(x, y, 2, h, u->th->accent);
}
