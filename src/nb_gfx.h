/*
 * nb_gfx.h -- the drawing primitives, on one ARGB8888 target.
 *
 * WHY EVERYTHING IS CLIPPED, ALWAYS
 * The firmware's framebuffer is 320x240 at a fixed address, and the code that
 * draws into it runs with no memory protection: one pixel outside the buffer is
 * a write into whatever the firmware keeps next to the LCD structure, which on
 * this machine is a reset.  g1fm lost a session to exactly that class of bug, so
 * every primitive here takes the full rectangle and clips it (nb_gfx_clip)
 * before touching memory.  The cost is a few compares per call.
 *
 * One target only: nbCode draws in the back buffer and pushes the frame with one
 * blit (nb_screen_present).  Drawing straight to the visible buffer flickers,
 * which is why FileManager, primeLua and every other app in this workspace
 * double-buffer.
 */
#ifndef NB_GFX_H
#define NB_GFX_H

#include "nb_common.h"

#define NB_GFX_OK	1
#define NB_GFX_FAIL	0

int nb_gfx_init(void);			/* bind the framebuffer, clear it */
nb_u32 *nb_gfx_target(void);
int nb_gfx_width(void);
int nb_gfx_height(void);

void nb_gfx_clear(nb_u32 colour);
void nb_gfx_pixel(int x, int y, nb_u32 colour);
void nb_gfx_hline(int x, int y, int w, nb_u32 colour);
void nb_gfx_vline(int x, int y, int h, nb_u32 colour);
void nb_gfx_fill(int x, int y, int w, int h, nb_u32 colour);
void nb_gfx_frame(int x, int y, int w, int h, nb_u32 colour);

/* Mix `colour` into what is already there, `alpha` out of 255.  THE PANEL HAS NO
 * HARDWARE ALPHA (the framebuffer is ARGB8888 but the LCD ignores the top byte),
 * so anything that has to be see-through -- the editor's scrollbar, which is drawn
 * OVER the text -- is mixed here, in the back buffer.  Clipped like every other
 * primitive. */
void nb_gfx_blend(int x, int y, int w, int h, nb_u32 colour, int alpha);

/* A 1bpp bitmap: one bit per pixel, MSB first, rows padded to whole bytes.
 * This is how every glyph and every icon in nbCode is stored. */
void nb_gfx_bitmap(int x, int y, int w, int h, const nb_u8 *bits,
		   nb_u32 colour);
/* the same, but each row is `stride` bytes apart (the font's row words) */
void nb_gfx_bitmap_stride(int x, int y, int w, int h, const nb_u32 *rows,
			  int stride_bytes, nb_u32 colour);

#endif /* NB_GFX_H */
