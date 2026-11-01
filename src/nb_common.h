/*
 * nb_common.h -- what every nbCode source file agrees on.
 *
 * nbCode is a C code editor for the HP Prime G1 that runs the way primeLua does:
 * a Python launcher on the calculator loads nbCode.elf with the same shellcode
 * loader DOOM and primetcc use, hands it a config block in r0 and gets out of
 * the way.  This header is the part of that world the editor does not care
 * about -- fixed width types, byte classification, tiny helpers.
 *
 * LAYERING (so the interesting half can be tested on a PC):
 *
 *   src/nb_buffer.c+nb_highlight.c+nb_codec.c+nb_text.c   pure logic + blits
 *   src/nb_gfx.c                                          drawing primitives
 *   src/nb_ui.c                                           screens and keys
 *   src/nb_app.c                                          the main loop
 *   port/nb_port_fw.c + nb_mem.c + nb_input_fw.c          firmware & loading
 *
 * The first group never includes a firmware header and never calls malloc
 * directly -- it goes through nb_alloc below -- so the host build links it
 * against plain libc and the device build against the firmware heap.
 */
#ifndef NB_COMMON_H
#define NB_COMMON_H

/* ---------------------------------------------------------------------------
 * fixed width types (there is no <stdint.h>: the device build is freestanding
 * and the host tests must agree with it bit for bit)
 * ------------------------------------------------------------------------- */
typedef unsigned char nb_u8;
typedef signed char nb_s8;
typedef unsigned short nb_u16;
typedef signed short nb_s16;
typedef unsigned int nb_u32;
typedef signed int nb_s32;

#define NB_NULL		((void *)0)

/* ---------------------------------------------------------------------------
 * memory: the firmware heap on the device, the C heap in the host tests.
 * port/nb_mem.c implements these; nothing above this line allocates any other
 * way, which is what makes "no leak on repeated runs" a property the emulator
 * tests can actually measure.
 * ------------------------------------------------------------------------- */
void *nb_alloc(unsigned int bytes);
void *nb_realloc(void *p, unsigned int bytes);
void nb_free(void *p);
void *nb_memcpy(void *dst, const void *src, unsigned int n);
void *nb_memset(void *dst, int c, unsigned int n);
int nb_memcmp(const void *a, const void *b, unsigned int n);
unsigned int nb_strlen(const char *s);
int nb_strcmp(const char *a, const char *b);

/* ---------------------------------------------------------------------------
 * byte classification.  A .c file is BYTES (UTF-8 or GB2312 or plain ASCII),
 * and a decoder keeps multibyte sequences whole, so the highlighter can be a
 * byte machine: every byte >= 0x80 is opaque text that never starts a token.
 * ------------------------------------------------------------------------- */
#define NB_ISSPACE(c)	((c) == ' ' || (c) == '\t')
#define NB_ISDIGIT(c)	((c) >= '0' && (c) <= '9')
#define NB_ISALPHA(c)	(((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
#define NB_ISALNUM(c)	(NB_ISALPHA(c) || NB_ISDIGIT(c))
#define NB_ISHEX(c)	(NB_ISDIGIT(c) || ((c) >= 'a' && (c) <= 'f') || \
			 ((c) >= 'A' && (c) <= 'F'))
#define NB_ISWORD(c)	(NB_ISALNUM(c) || (c) == '_' || (c) >= 0x80)
#define NB_ISUPPER(c)	((c) >= 'A' && (c) <= 'Z')

/* ---------------------------------------------------------------------------
 * limits.  Every one of these is a bound the calculator's memory made us pick,
 * not a limitation of the editor model.
 * ------------------------------------------------------------------------- */
#define NB_MAX_LINE	2048	/* bytes in one line, hard cap on paste/insert */
#define NB_MAX_LINES	8000	/* lines in one file (8000 * 4 B of pointers) */
#define NB_FILE_MAX	(1024 * 1024)	/* refuse to open anything bigger */
#define NB_PATH_MAX	256
#define NB_SCREEN_W	320
#define NB_SCREEN_H	240

#define NB_YES		1
#define NB_NO		0

/* ---------------------------------------------------------------------------
 * how a file is stored on the calculator.  The value is what the status bar
 * shows and what the saver writes back: a file read as GB2312 is written back
 * as GB2312, because silently re-encoding a Chinese source tree to UTF-8 is
 * data damage that looks like nothing at all inside the editor.
 * ------------------------------------------------------------------------- */
typedef enum {
	NB_ENC_UTF8 = 0,
	NB_ENC_GB2312,
	NB_ENC_LATIN1
} nb_encoding;

typedef enum {
	NB_EOL_LF = 0,
	NB_EOL_CRLF
} nb_eol;

/* the languages the highlighter knows; NB_LANG_AUTO is resolved from the file
 * name when a file is opened */
typedef enum {
	NB_LANG_AUTO = 0,
	NB_LANG_PPL,
	NB_LANG_LUA,
	NB_LANG_PY,
	NB_LANG_C,
	NB_LANG_TXT
} nb_lang;

/* ---------------------------------------------------------------------------
 * colours: the firmware framebuffer is ARGB8888, so a palette is arithmetic.
 * The values are the ones the Python FileManager/PrimeCode use, which keeps
 * nbCode looking like the rest of this workspace's apps.
 * ------------------------------------------------------------------------- */
#define NB_RGB(r, g, b)	(0xFF000000u | ((nb_u32)(r) << 16) | \
			 ((nb_u32)(g) << 8) | (nb_u32)(b))

#define NB_BLACK	NB_RGB(0x00, 0x00, 0x00)
#define NB_WHITE	NB_RGB(0xFF, 0xFF, 0xFF)

/* ---------------------------------------------------------------------------
 * THE PALETTE: A FLAT MODERN SET, MEASURED
 *
 * The interface is chrome + code, and both are the same handful of surfaces:
 *
 *   bg        the page (the code, the lists)
 *   foot_bg   the CHROME surface: the header bar, the status bar, every panel
 *   line      the hairlines between them (header underline, gutter edge, the
 *             border of a panel) -- deliberately low contrast (1.1-1.4:1), which
 *             is what a divider is
 *   accent    one colour for everything interactive: the caret, the scrollbar,
 *             the header underline, the edge of the selected row
 *   sel_bg/fg a selected row / a selection tint, and the popup's cursor row
 *   sub_fg    secondary text (hints, sizes, dates)
 *
 * The light theme is a paper-and-ink scheme with a GitHub-like syntax palette;
 * the dark one is the same set inverted on 0x0D1117.  Both are checked
 * numerically by tests/test_host.c, not by eye alone (the numbers are in the
 * comments).
 * ------------------------------------------------------------------------- */
#define NB_L_BG		NB_RGB(0xFF, 0xFF, 0xFF)
#define NB_L_SURFACE	NB_RGB(0xF6, 0xF8, 0xFA)	/* header, status, panels */
#define NB_L_TEXT	NB_RGB(0x1F, 0x23, 0x28)	/* 15.8:1 */
#define NB_L_SUB	NB_RGB(0x59, 0x63, 0x6E)	/*  6.1:1 on the page */
#define NB_L_LINE	NB_RGB(0xD8, 0xDE, 0xE4)	/*  1.36:1: a hairline */
#define NB_L_ACCENT	NB_RGB(0x09, 0x69, 0xDA)	/*  5.2:1: caret, bar, edge */
#define NB_L_SEL_BG	NB_RGB(0xE3, 0xEE, 0xFF)	/* a tint, not a bar */
#define NB_L_SEL_FG	NB_RGB(0x0A, 0x30, 0x69)	/* 10.9:1 on it */
#define NB_L_CURLINE	NB_RGB(0xF1, 0xF6, 0xFD)	/* the current line */
#define NB_L_GUTTER_BG	NB_RGB(0xF6, 0xF8, 0xFA)
#define NB_L_MATCH	NB_RGB(0xF0, 0xF6, 0xFF)	/* the matching bracket's box */
/* the search highlight: a warm tint that says "this is a hit", not "this is
 * selected" (nb_find.c; the theme test checks the code colours on it too) */
#define NB_L_FIND	NB_RGB(0xFF, 0xF6, 0xD8)
#define NB_L_MUTED	NB_RGB(0x9A, 0xA3, 0xAD)	/* a panel's title (decorative) */

/* THE CODE COLOURS, and the two rules they are chosen by (both measured):
 *
 *   CONTRAST: every class is at least 4.5:1 on its background -- on a 320x240 LCD
 *   with a 1-pixel-stroke font a mid-tone is unreadable ("the highlighting is
 *   there but I cannot see it").  Comments are allowed to be the quietest thing
 *   on the screen, and they are the only class that is not saturated.
 *
 *   COLOUR: contrast alone drives everything toward black, and then the colouring
 *   is invisible for the opposite reason ("it all looks like black text"), so the
 *   classes also stay saturated (HSV >= 0.45) and their hues are far apart: red
 *   keywords, violet control flow, blue numbers, green strings, orange operators,
 *   teal library calls, magenta directives, grey comments.
 *
 * The same colours have to work on three surfaces (the page, the current line,
 * the selection tint); the selection is the tightest of the three, which is why
 * the test floor there is 4.2 rather than 4.5 -- a selection that cannot be seen
 * is worse than one whose syntax colours are 4.2:1. */
#define NB_C_TEXT	NB_RGB(0x1F, 0x23, 0x28)	/* 15.8:1 */
#define NB_C_KEY	NB_RGB(0xCF, 0x22, 0x2E)	/*  5.4:1, sat 0.84 */
#define NB_C_CTRL	NB_RGB(0x82, 0x50, 0xDF)	/*  5.1:1, sat 0.64 */
#define NB_C_CONST	NB_RGB(0x05, 0x50, 0xAE)	/*  7.6:1, sat 0.97 */
#define NB_C_STRING	NB_RGB(0x1A, 0x7F, 0x37)	/*  5.1:1, sat 0.80 */
#define NB_C_OP		NB_RGB(0xBC, 0x4C, 0x00)	/*  5.0:1, sat 1.00 */
#define NB_C_LIB	NB_RGB(0x1B, 0x7C, 0x83)	/*  4.9:1, sat 0.79 */
#define NB_C_COMMENT	NB_RGB(0x63, 0x6C, 0x76)	/*  5.3:1, quiet on purpose */
#define NB_C_PREPROC	NB_RGB(0xBF, 0x39, 0x89)	/*  5.1:1, sat 0.70 */
#define NB_C_CURSOR	NB_L_ACCENT
#define NB_C_CURLINE	NB_L_CURLINE
#define NB_C_GUTTER_BG	NB_L_GUTTER_BG
#define NB_C_GUTTER_FG	NB_L_SUB		/*  5.7:1 on the gutter */
#define NB_C_MATCH	NB_L_MATCH
#define NB_C_FIND	NB_L_FIND

/* the dark theme: the same scheme on 0x0D1117 */
#define NB_D_BG		NB_RGB(0x0D, 0x11, 0x17)
#define NB_D_SURFACE	NB_RGB(0x16, 0x1B, 0x22)
#define NB_D_TEXT	NB_RGB(0xE6, 0xED, 0xF3)	/* 16.0:1 */
#define NB_D_SUB	NB_RGB(0x8B, 0x94, 0x9E)	/*  6.2:1 on the page */
#define NB_D_LINE	NB_RGB(0x21, 0x26, 0x2D)	/*  1.24:1 */
#define NB_D_ACCENT	NB_RGB(0x2F, 0x81, 0xF7)	/*  5.1:1 */
#define NB_D_SEL_BG	NB_RGB(0x16, 0x26, 0x3D)
#define NB_D_SEL_FG	NB_RGB(0xE6, 0xED, 0xF3)	/* 12.9:1 on it */
#define NB_D_CURLINE	NB_RGB(0x14, 0x1A, 0x22)
#define NB_D_GUTTER_BG	NB_RGB(0x10, 0x15, 0x1C)
#define NB_D_MATCH	NB_RGB(0x1A, 0x24, 0x30)
/* THE SEARCH HIGHLIGHT: warm, quiet, and every code class stays readable on it
 * (the theme test checks it like the other surfaces) */
#define NB_D_FIND	NB_RGB(0x2A, 0x24, 0x10)
#define NB_D_MUTED	NB_RGB(0x6E, 0x76, 0x81)	/* a panel's title (decorative) */

#define NB_D_KEY	NB_RGB(0xFF, 0x7B, 0x72)	/*  7.5:1, sat 0.55 */
#define NB_D_CTRL	NB_RGB(0xA3, 0x71, 0xF7)	/*  5.6:1, sat 0.54 */
#define NB_D_CONST	NB_RGB(0x79, 0xC0, 0xFF)	/*  9.7:1, sat 0.53 */
#define NB_D_STRING	NB_RGB(0x56, 0xD3, 0x64)	/*  9.8:1, sat 0.59 */
#define NB_D_OP		NB_RGB(0xF0, 0x88, 0x3E)	/*  7.5:1, sat 0.74 */
#define NB_D_LIB	NB_RGB(0x39, 0xD2, 0xC0)	/* 10.1:1, sat 0.73 */
#define NB_D_COMMENT	NB_RGB(0x8B, 0x94, 0x9E)	/*  6.2:1 */
#define NB_D_PREPROC	NB_RGB(0xDB, 0x61, 0xA2)	/*  5.6:1, sat 0.56 */
#define NB_D_CURSOR	NB_D_ACCENT
#define NB_D_GUTTER_FG	NB_D_SUB		/*  6.0:1 on the gutter */

/* the palette the renderer actually reads; nb_theme_set fills it in */
typedef struct nb_theme {
	nb_u32 text, key, ctrl, constant, string, op, lib, comment, preproc;
	nb_u32 cursor, curline, gutter_bg, gutter_fg, match;
	/* the shading under every SEARCH result (nb_find.c): a warm tint that is
	 * not the selection and not the bracket box, so the three are told apart */
	nb_u32 find;
	nb_u32 bg, fg, head_bg, head_fg, sel_bg, sel_fg, line, foot_bg, sub_fg;
	/* ONE COLOUR FOR EVERYTHING INTERACTIVE: the caret, the scrollbar's thumb,
	 * the header's underline, the edge of the selected row */
	nb_u32 accent;
	/* a DECORATIVE tone: a panel's title, which names the panel the user has
	 * already opened and is therefore not information they have to read.  It is
	 * deliberately lighter than sub_fg (which is the colour of things that ARE
	 * read: hints, sizes) and it is the one text tone that does not have to
	 * pass the 4.5:1 rule. */
	nb_u32 muted;
	int dark;
} nb_theme;

void nb_theme_set(nb_theme *t, int dark);
const nb_theme *nb_theme_get(void);

/* ---------------------------------------------------------------------------
 * small string helpers used all over the UI (sprintf-free: the device has no
 * printf, and every one of these is a fixed-size buffer by construction)
 * ------------------------------------------------------------------------- */
int nb_fmt_int(char *out, int cap, int value);			/* decimal */
int nb_fmt_uint(char *out, int cap, unsigned int value);
int nb_fmt_hex(char *out, int cap, unsigned int value, int digits);
int nb_fmt_size(char *out, int cap, unsigned int bytes);	/* "1.4K", "812B" */
int nb_str_copy(char *dst, int cap, const char *src, int n);
int nb_str_append(char *dst, int cap, const char *src);

#endif /* NB_COMMON_H */
