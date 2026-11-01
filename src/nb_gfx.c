/*
 * nb_gfx.c -- primitives on the back buffer.
 *
 * Small, boring and clipped (see nb_gfx.h for why the clipping is not
 * optional).  nb_gfx_bitmap_stride is the font path: the generated glyph table
 * stores one 16-bit word per row, so a glyph is 16 rows of `stride` bytes with
 * bit 15 at the glyph's left edge.
 */
#include "nb_gfx.h"
#include "nb_port.h"

static nb_u32 *fb;
static int fb_w, fb_h;

int nb_gfx_init(void)
{
	fb = nb_screen_back();
	fb_w = nb_screen_width();
	fb_h = nb_screen_height();
	if (!fb || fb_w <= 0 || fb_h <= 0)
		return NB_GFX_FAIL;
	return NB_GFX_OK;
}

nb_u32 *nb_gfx_target(void)
{
	return fb;
}

int nb_gfx_width(void)
{
	return fb_w;
}

int nb_gfx_height(void)
{
	return fb_h;
}

void nb_gfx_clear(nb_u32 colour)
{
	int i, n;

	if (!fb)
		return;
	n = fb_w * fb_h;
	for (i = 0; i < n; i++)
		fb[i] = colour;
}

void nb_gfx_pixel(int x, int y, nb_u32 colour)
{
	if (!fb || x < 0 || y < 0 || x >= fb_w || y >= fb_h)
		return;
	fb[y * fb_w + x] = colour;
}

void nb_gfx_hline(int x, int y, int w, nb_u32 colour)
{
	int i;

	if (!fb || y < 0 || y >= fb_h || w <= 0)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (x + w > fb_w)
		w = fb_w - x;
	if (w <= 0)
		return;
	{
		nb_u32 *p = fb + y * fb_w + x;

		for (i = 0; i < w; i++)
			p[i] = colour;
	}
}

void nb_gfx_vline(int x, int y, int h, nb_u32 colour)
{
	int i;

	if (!fb || x < 0 || x >= fb_w || h <= 0)
		return;
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (y + h > fb_h)
		h = fb_h - y;
	for (i = 0; i < h; i++)
		fb[(y + i) * fb_w + x] = colour;
}

/* one channel of one pixel, mixed */
static nb_u32 mix(nb_u32 under, nb_u32 over, int a)
{
	unsigned int r, g, b;

	if (a <= 0)
		return under;
	if (a >= 255)
		return over;
	r = (((under >> 16) & 0xFF) * (unsigned int)(255 - a) +
	     ((over >> 16) & 0xFF) * (unsigned int)a) / 255;
	g = (((under >> 8) & 0xFF) * (unsigned int)(255 - a) +
	     ((over >> 8) & 0xFF) * (unsigned int)a) / 255;
	b = ((under & 0xFF) * (unsigned int)(255 - a) +
	     (over & 0xFF) * (unsigned int)a) / 255;
	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void nb_gfx_blend(int x, int y, int w, int h, nb_u32 colour, int alpha)
{
	int j;

	if (!fb || w <= 0 || h <= 0 || alpha <= 0)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > fb_w)
		w = fb_w - x;
	if (y + h > fb_h)
		h = fb_h - y;
	if (w <= 0 || h <= 0)
		return;
	for (j = 0; j < h; j++) {
		nb_u32 *row = fb + (y + j) * fb_w + x;
		int i;

		for (i = 0; i < w; i++)
			row[i] = mix(row[i], colour, alpha);
	}
}

void nb_gfx_fill(int x, int y, int w, int h, nb_u32 colour)
{
	int j;

	if (!fb || w <= 0 || h <= 0)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > fb_w)
		w = fb_w - x;
	if (y + h > fb_h)
		h = fb_h - y;
	if (w <= 0 || h <= 0)
		return;
	for (j = 0; j < h; j++)
		nb_gfx_hline(x, y + j, w, colour);
}

void nb_gfx_frame(int x, int y, int w, int h, nb_u32 colour)
{
	if (w <= 0 || h <= 0)
		return;
	nb_gfx_hline(x, y, w, colour);
	nb_gfx_hline(x, y + h - 1, w, colour);
	nb_gfx_vline(x, y, h, colour);
	nb_gfx_vline(x + w - 1, y, h, colour);
}

void nb_gfx_bitmap(int x, int y, int w, int h, const nb_u8 *bits, nb_u32 colour)
{
	int row, col, stride = (w + 7) / 8;

	if (!fb || !bits || w <= 0 || h <= 0)
		return;
	for (row = 0; row < h; row++) {
		int py = y + row;

		if (py < 0 || py >= fb_h)
			continue;
		for (col = 0; col < w; col++) {
			int px = x + col;

			if (px < 0 || px >= fb_w)
				continue;
			if (bits[row * stride + (col >> 3)] &
			    (0x80 >> (col & 7)))
				fb[py * fb_w + px] = colour;
		}
	}
}

void nb_gfx_bitmap_stride(int x, int y, int w, int h, const nb_u32 *rows,
			  int stride_bytes, nb_u32 colour)
{
	int row, col;
	const nb_u8 *base = (const nb_u8 *)rows;

	if (!fb || !rows || w <= 0 || h <= 0)
		return;
	for (row = 0; row < h; row++) {
		int py = y + row;
		const nb_u8 *rp = base + (unsigned int)row * (unsigned int)stride_bytes;

		if (py < 0 || py >= fb_h)
			continue;
		for (col = 0; col < w; col++) {
			int px = x + col;

			if (px < 0 || px >= fb_w)
				continue;
			if (rp[col >> 3] & (0x80 >> (col & 7)))
				fb[py * fb_w + px] = colour;
		}
	}
}
