/*
 * nb_screen.c -- the framebuffer, and the one structure nbCode must read right.
 *
 * THE FIRMWARE'S LCD IS REACHED THROUGH A POINTER, and this file got that wrong
 * once.  The layout was measured for the working gbemu port and is used
 * identically by primetcc (rt/hp_gfx.c) and primeLua (port/plua_hp.c):
 *
 *     p  = get_lcd();               svc 0x1008D -- the LCD structure
 *     q  = *(u32 *)p;               the info structure
 *     w  = *(u16 *)(q + 2);
 *     h  = *(u16 *)(q + 4);
 *     fb = *(u32 *)(q + 16);        the pixels, u32 0x00RRGGBB
 *
 * nbCode used `p[0]` as the framebuffer.  That is the address of the INFO
 * structure, not of the pixels -- and nbCode draws straight into the buffer it is
 * handed, so every frame went into firmware memory: the calculator faulted and
 * rebooted on the first frame, right after the log line saying the editor had
 * started.  Nothing on a host could see it, which is why a range check is not
 * enough here: the WRONG pointer is perfectly in range.
 *
 * SO THIS FILE REFUSES TO GUESS.  If any step of that chain is missing, or the
 * address is not plausibly a framebuffer, nb_screen_init() returns 0 and
 * nb_run() exits with a logged reason instead of drawing.  An editor that does
 * not start is a bug report; an editor that writes 307 KB into the firmware is a
 * reset with no trail.
 *
 * AND IT DRAWS INTO A SHADOW BUFFER, WHICH IS THE FLICKER FIX.
 *
 * The first version drew straight into the firmware's pixels, on the theory that
 * a second 320x240x4 buffer is 307 KB of a heap this image already uses.  That
 * theory is right about the memory and wrong about the screen: every frame
 * CLEARS and redraws the body in place, so the panel shows the erase before it
 * shows the text -- a drag of the code flickers the whole window, and the faster
 * the frame rate the more obvious it is.
 *
 * So the shadow is allocated (firmware `calloc`, once per session, released at
 * exit) and nb_screen_present() copies it into the firmware's buffer in one
 * pass: the panel only ever sees COMPLETE frames.  If the allocation fails the
 * screen still works -- it just flickers like before, and the log says so.
 */
#include "nb_common.h"
#include "nb_port.h"

unsigned hp_svc_getlcd(void);
unsigned hp_svc_calloc(unsigned n, unsigned sz);
void hp_svc_free(unsigned p);

/* Where the firmware's framebuffer can plausibly live: the G1's SDRAM.  A
 * pointer outside this is not one of ours. */
#define NB_RAM_LO	0x30000000u
#define NB_RAM_HI	0x32000000u

static nb_u32 *fb;			/* the firmware's pixels */
static nb_u32 *shadow;			/* where nbCode actually draws */
static int width = NB_SCREEN_W, height = NB_SCREEN_H;
static int screen_ok;

int nb_screen_init(void)
{
	unsigned lcd = hp_svc_getlcd();
	unsigned info = 0, pixels = 0;
	int w = 0, h = 0;

	nb_log_hex("screen: get_lcd=", lcd);
	if (lcd >= NB_RAM_LO && lcd < NB_RAM_HI) {
		info = *(unsigned *)lcd;
		if (info >= NB_RAM_LO && info < NB_RAM_HI) {
			w = (int)*(unsigned short *)(info + 2);
			h = (int)*(unsigned short *)(info + 4);
			pixels = *(unsigned *)(info + 16);
		}
	}
	if (!info || !pixels) {
		nb_log_hex("screen: FAIL: no LCD info structure, info=", info);
		nb_log_hex("screen: FAIL: no framebuffer pointer, pixels=", pixels);
		return 0;
	}
	if (w <= 0 || h <= 0 || w > NB_SCREEN_W || h > NB_SCREEN_H) {
		nb_log_num("screen: FAIL: unusable width=", (unsigned)w);
		nb_log_num("screen: FAIL: unusable height=", (unsigned)h);
		return 0;
	}
	if (pixels < NB_RAM_LO || pixels >= NB_RAM_HI) {
		nb_log_hex("screen: FAIL: framebuffer not in RAM, pixels=", pixels);
		return 0;
	}
	fb = (nb_u32 *)pixels;
	width = w;
	height = h;
	screen_ok = 1;
	nb_log_num("screen: ok width=", (unsigned)w);
	nb_log_num("screen: ok height=", (unsigned)h);
	nb_log_hex("screen: ok framebuffer=", pixels);

	/* THE SHADOW, once per session: the image is reused between runs, so it
	 * is allocated here (not per entry) and freed by nb_screen_free() at
	 * exit.  calloc because the first frame may not paint every pixel of the
	 * buffer, and an uninitialised one would flash whatever the heap held. */
	if (!shadow) {
		unsigned bytes = (unsigned)w * (unsigned)h * 4u;
		unsigned p2 = hp_svc_calloc(bytes, 1);

		if (p2) {
			shadow = (nb_u32 *)p2;
			nb_log_hex("screen: shadow buffer at ", p2);
			nb_log_num("screen: shadow bytes ", bytes);
		} else {
			nb_log_num("screen: no shadow buffer (drawing direct), "
				   "bytes=", bytes);
		}
	}
	return 1;
}

/* give the shadow back (called from the teardown path): the calculator needs
 * this memory while nbCode is not running, and the next session allocates it
 * again */
void nb_screen_free(void)
{
	if (shadow) {
		hp_svc_free((unsigned)(unsigned long)shadow);
		shadow = NB_NULL;
	}
}

/* The drawing layer asks for the surface once per frame.  A null here means
 * nb_screen_init() said no, and nb_run() does not draw in that case -- the check
 * is repeated anyway, because a rasterizer that writes through a null pointer is
 * a reset, not an error message. */
nb_u32 *nb_screen_back(void)
{
	if (!screen_ok)
		return NB_NULL;
	return shadow ? shadow : fb;
}

int nb_screen_width(void)
{
	return width;
}

int nb_screen_height(void)
{
	return height;
}

void nb_screen_present(void)
{
	/* ONE PASS, WORD AT A TIME: this is the whole frame, and a memcpy from
	 * the toolchain is a byte loop on this CPU (307 KB of byte stores is
	 * several times slower than 76,800 word stores).  Nothing here is
	 * clipped or converted -- source and destination are the same format,
	 * which is the point of drawing in the panel's own ARGB8888. */
	nb_u32 *d, *s;
	int n;

	if (!screen_ok || !shadow || shadow == fb)
		return;			/* no shadow: the frame is already there */
	d = fb;
	s = shadow;
	n = width * height;
	while (n-- > 0)
		*d++ = *s++;
}
