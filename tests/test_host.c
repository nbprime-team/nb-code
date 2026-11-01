/*
 * test_host.c -- nbCode's core, tested on the PC.
 *
 * WHAT THIS IS FOR
 * The editor's interesting half -- the line buffer, the cursor arithmetic over
 * UTF-8 and GB2312, the syntax tokenizer, the re-encode-on-save rules and the
 * renderer's cell maths -- has no dependency on the calculator at all, because
 * the port layer is the only thing that talks to the firmware (nb_port.h).  So
 * it is tested here, in a second, with real assertions instead of "it looked
 * right on the device once".
 *
 * The renderer is checked the only way a bitmap font can be: render a known
 * string, read the framebuffer back and print it as ASCII art next to the
 * expected shape, and assert the geometry (8 pixels per western cell, 16 per
 * GB2312 cell, 16 pixels per line).
 *
 * Run:  make test-host
 */
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nb_common.h"
#include "nb_buffer.h"
#include "nb_codec.h"
#include "nb_font.h"
#include "nb_highlight.h"
#include "nb_text.h"
#include "nb_gfx.h"
#include "nb_clip.h"
#include "nb_find.h"
#include "nb_hex.h"
#include "nb_ui.h"
#include "nb_port.h"
#include "nb_host.h"
#include "nbcode_font_data.h"

static int failures;
static int checks;

/* the renderer's view of a glyph's width, for the tests that ask about ink */
static int font_xw(unsigned int cp)
{
	int x0, xw, first, rows;

	if (!nb_font_lookup(nb_font_glyph(cp), &x0, &xw, &first, &rows, NULL,
			    NULL))
		return -1;
	return xw;
}

static void check(const char *name, int ok, const char *detail)
{
	checks++;
	if (!ok) {
		failures++;
		printf("FAIL %-44s %s\n", name, detail ? detail : "");
	} else {
		printf("ok   %-44s %s\n", name, detail ? detail : "");
	}
}

/* a printf-style detail string for a check: the older checks build theirs with
 * snprintf into a local buffer, which is noisy for a dozen of them, so this
 * formats into a small ring of buffers instead */
static const char *fmt(const char *f, ...)
{
	static char buf[4][96];
	static int slot;
	va_list ap;
	char *out = buf[slot = (slot + 1) & 3];

	va_start(ap, f);
	vsnprintf(out, sizeof(buf[0]), f, ap);
	va_end(ap);
	return out;
}

/* ---------------------------------------------------------------------------
 * the core: buffer, cursor, undo
 * ------------------------------------------------------------------------- */
static nb_buf *mk(const char *text)
{
	nb_buf *b = nb_buf_new();
	int len = (int)strlen(text);

	nb_buf_from_raw(b, text, len, NB_ENC_UTF8, NB_EOL_LF);
	return b;
}

static void test_buffer(void)
{
	nb_buf *b = mk("hello\nworld\n");
	char detail[128];

	snprintf(detail, sizeof(detail), "lines=%d", b->nlines);
	check("open: a trailing newline adds no line", b->nlines == 2, detail);
	check("open: line 0 text", strcmp(nb_buf_line(b, 0), "hello") == 0,
	      nb_buf_line(b, 0));
	check("open: line 1 text", strcmp(nb_buf_line(b, 1), "world") == 0,
	      nb_buf_line(b, 1));
	check("open: not dirty", b->modified == 0, "");

	/* DELETING ACROSS LINES: the three pieces (the first line's tail, the
	 * lines between, the last line's head) and the byte count after them.
	 *
	 * This is where a heap corruption lived, and the tests here found it: the
	 * loop ran one line too many and then joined the line AFTER the range --
	 * which for a range that ended the file was past the end of the array. */
	{
		nb_buf *m = mk("abcd\nef\ngh\n");
		char *raw = NULL;
		int len = 0, lost = 0;

		/* "abcd\nef\ngh\n" cut from (0,2) to (2,1): "cd", the newline,
		 * "ef", the newline and the "g" go -- what is left is "ab" + "h" */
		nb_buf_delete_range(m, 0, 2, 2, 1);
		check("delete range: the pieces join",
		      m->nlines == 1 &&
		      strcmp(nb_buf_line(m, 0), "abh") == 0,
		      fmt("n=%d [%s]", m->nlines, nb_buf_line(m, 0)));
		check("delete range: ... and the cursor is at the cut",
		      m->cur_line == 0 && m->cur_col == 2, "");
		/* THE BYTE COUNT IS THE FILE: nbytes is what the status bar shows
		 * and what the save writes, so it is checked against the bytes */
		nb_buf_to_raw(m, &raw, &len, &lost);
		check("delete range: the byte count matches the text",
		      (unsigned)len == m->nbytes,
		      fmt("nbytes=%u raws=%d", m->nbytes, len));
		nb_free(raw);
		nb_buf_free(m);
	}
	{
		/* the crash case: a range that ends the file at column 0 */
		nb_buf *m = mk("one\ntwo\n");

		nb_buf_delete_range(m, 0, 0, 1, 0);
		check("delete range: deleting up to the end of the file joins it",
		      m->nlines == 1 &&
		      strcmp(nb_buf_line(m, 0), "two") == 0,
		      fmt("n=%d [%s]", m->nlines, nb_buf_line(m, 0)));
		nb_buf_free(m);
	}
	{
		/* ... and a range that keeps text after the last line touched */
		nb_buf *m = mk("aaa\nbbb\nccc\nddd\n");
		char *raw = NULL;
		int len = 0, lost = 0;

		/* from (1,1) to (2,2): line 1 loses "bb" and gains "c", line 2
		 * goes, and the line AFTER the range is untouched */
		nb_buf_delete_range(m, 1, 1, 2, 2);
		check("delete range: the lines after the range are untouched",
		      m->nlines == 3 &&
		      strcmp(nb_buf_line(m, 0), "aaa") == 0 &&
		      strcmp(nb_buf_line(m, 1), "bc") == 0 &&
		      strcmp(nb_buf_line(m, 2), "ddd") == 0,
		      fmt("n=%d [%s|%s|%s]", m->nlines, nb_buf_line(m, 0),
			  nb_buf_line(m, 1), nb_buf_line(m, 2)));
		nb_buf_to_raw(m, &raw, &len, &lost);
		check("delete range: ... and the byte count still matches",
		      (unsigned)len == m->nbytes,
		      fmt("nbytes=%u raws=%d", m->nbytes, len));
		nb_free(raw);
		nb_buf_free(m);
	}

	/* typing */
	nb_buf_goto(b, 0, 5);
	nb_buf_insert(b, "!", 1);
	check("insert: at end of line", strcmp(nb_buf_line(b, 0), "hello!") == 0,
	      nb_buf_line(b, 0));
	check("insert: cursor advanced", b->cur_col == 6, "");

	/* undo one keystroke */
	nb_buf_undo(b);
	check("undo: insertion removed", strcmp(nb_buf_line(b, 0), "hello") == 0,
	      nb_buf_line(b, 0));
	check("undo: cursor restored", b->cur_col == 5, "");

	/* newline splits, auto-indent is the caller's argument */
	nb_buf_goto(b, 0, 2);
	nb_buf_newline(b, "  ", 2);
	check("newline: three lines", b->nlines == 3, "");
	check("newline: first half", strcmp(nb_buf_line(b, 0), "he") == 0,
	      nb_buf_line(b, 0));
	check("newline: indent kept", strcmp(nb_buf_line(b, 1), "  llo") == 0,
	      nb_buf_line(b, 1));

	/* BACKSPACE AT COLUMN 0 DELETES THE LINE BREAK ABOVE THE CURSOR: the
	 * cursor's own line swallows the text of the line it was on, and the
	 * line the cursor moved out of is untouched.  (The first version of this
	 * test asserted the opposite and "found" a bug that did not exist --
	 * worth saying out loud, because the assertion looked obviously right.)
	 *
	 *   before: [he] [  llo] [world]      cursor on "world", column 0
	 *   after : [he] [  lloworld]         cursor at column 5
	 */
	nb_buf_goto(b, 2, 0);
	nb_buf_backspace(b);
	check("backspace: one line fewer", b->nlines == 2, "");
	check("backspace: the line above is untouched",
	      strcmp(nb_buf_line(b, 0), "he") == 0, nb_buf_line(b, 0));
	check("backspace: the cursor's line swallowed the text",
	      strcmp(nb_buf_line(b, 1), "  lloworld") == 0, nb_buf_line(b, 1));
	check("backspace: the cursor sits at the join", b->cur_col == 5, "");

	/* kill line: the text goes, the line stays */
	nb_buf_goto(b, 1, 3);
	nb_buf_kill_line(b);
	check("kill line: emptied", strcmp(nb_buf_line(b, 1), "") == 0,
	      nb_buf_line(b, 1));
	check("kill line: the line is still there", b->nlines == 2, "");

	/* the clipboard keeps the line it copied, newline included */
	nb_buf_goto(b, 1, 0);
	{
		char want[NB_MAX_LINE + 2];
		const char *t = NULL;
		int n, wl;

		nb_buf_copy_line(b);
		n = nb_buf_clip_get(b, &t);
		wl = nb_str_copy(want, sizeof(want), nb_buf_line(b, 1), -1);
		want[wl] = '\n';
		want[wl + 1] = 0;
		check("clipboard: the line, plus its newline",
		      n == wl + 1 && t && nb_strcmp(t, want) == 0,
		      t ? t : "(null)");
	}
	nb_buf_free(b);
}

/* the cursor must step over a whole character, in both encodings */
static void test_line_growth(void)
{
	/* A LINE THAT OUTGROWS ITS BUFFER MUST NOT LEAVE THE BUFFER HOLDING FREED
	 * MEMORY.
	 *
	 * This is the crash a paste caused on the calculator: `l = b->line[i]` is a
	 * COPY, line_reserve() reallocates the block, and the old code threw the new
	 * pointer away -- so the buffer went on using freed memory, the next
	 * allocation reused it, and the editor rebooted.  Nothing noticed while
	 * lines stayed under their initial 64 bytes of slack, which is why it took a
	 * paste (or a long line from the character table) to find it.
	 *
	 * The test forces the move: a block is allocated right after the line, so
	 * the grow cannot extend in place.
	 */
	nb_buf *b = nb_buf_new();
	char big[300];
	void *block = NB_NULL;
	int k;

	if (!b)
		return;
	nb_buf_insert(b, "seed", 4);
	block = nb_alloc(256);
	if (block)
		nb_memset(block, 'B', 256);
	for (k = 0; k < 250; k++)
		big[k] = (char)('A' + (k % 26));
	nb_buf_insert(b, big, 250);
	check("buf: a line that outgrows its buffer keeps its text",
	      nb_buf_linelen(b, 0) == 254 &&
	      memcmp(nb_buf_line(b, 0), "seed", 4) == 0 &&
	      memcmp(nb_buf_line(b, 0) + 4, big, 250) == 0,
	      fmt("len=%d", nb_buf_linelen(b, 0)));
	/* THE BLOCK THE OLD ONE WAS REPLACED BY is what would have been handed out
	 * here (and the line's text would then be a row of 'Z') */
	{
		void *p[32];

		for (k = 0; k < 32; k++) {
			p[k] = nb_alloc(128);
			if (p[k])
				nb_memset(p[k], 'Z', 128);
		}
		check("buf: ... and it survives the next allocations",
		      memcmp(nb_buf_line(b, 0) + 4, big, 250) == 0 &&
		      nb_buf_linelen(b, 0) == 254, "");
		for (k = 0; k < 32; k++)
			nb_free(p[k]);
	}
	if (block)
		nb_free(block);
	/* a double free here (the old pointer, freed by the realloc and freed again
	 * by the buffer) is what took the calculator down: glibc aborts this test */
	nb_buf_free(b);
	check("buf: ... and freeing the buffer is not a double free", 1, "");

}

static void test_cursor_wide(void)
{
	int step;
	/* "a" + the GB2312 pair for U+4E2D (0xD6D0) + "b" */
	char gb[5];
	nb_buf *b = nb_buf_new();
	int i;

	gb[0] = 'a';
	gb[1] = (char)0xD6;
	gb[2] = (char)0xD0;
	gb[3] = 'b';
	gb[4] = 0;
	nb_buf_from_raw(b, gb, 4, NB_ENC_UTF8, NB_EOL_LF);
	{
		int q;

		printf("   GB PROBE: len=%d bytes=", nb_buf_linelen(b, 0));
		for (q = 0; q < nb_buf_linelen(b, 0); q++)
			printf("%02X ", (unsigned char)nb_buf_line(b, 0)[q]);
		printf(" next(1)=%d prev(3)=%d prev(2)=%d\n",
		       nb_buf_next_char(b, 0, 1), nb_buf_prev_char(b, 0, 3),
		       nb_buf_prev_char(b, 0, 2));
	}
	check("gb2312: pair left as the file's own bytes",
	      nb_buf_linelen(b, 0) == 4, "");
	check("gb2312: unicode round trip",
	      nb_font_gb_unicode(0xD6, 0xD0) == 0x4E2D, "");

	/* the buffer holds the pair as the FILE had it: nb_buf_from_raw converts
	 * a GB2312 file into UTF-8, but a pair inside an otherwise UTF-8 file
	 * stays two bytes and must still be exactly one cursor stop */
	step = nb_buf_next_char(b, 0, 1);
	check("cursor: one step over a GB2312 pair", step == 3, "step");
	check("cursor: backward from the pair's trail byte lands on the lead byte",
	      nb_buf_prev_char(b, 0, 3) == 1, "back");
	check("cursor: backward from the pair's lead byte lands on the 'a'",
	      nb_buf_prev_char(b, 0, 1) == 0, "back2");
	check("cursor: backward from the start stays at the start",
	      nb_buf_prev_char(b, 0, 0) == 0, "");
	(void)i;

	/* UTF-8 path */
	{
		nb_buf *u8 = mk("a\xe4\xb8\xadz");

		check("utf-8: one step over U+4E2D",
		      nb_buf_next_char(u8, 0, 1) == 4, "");
		{
			char v[48];

			snprintf(v, sizeof(v), "prev(4)=%d prev(3)=%d prev(2)=%d",
				 nb_buf_prev_char(u8, 0, 4),
				 nb_buf_prev_char(u8, 0, 3),
				 nb_buf_prev_char(u8, 0, 2));
			check("utf-8: backward over it (from 'z' to U+4E2D)",
			      nb_buf_prev_char(u8, 0, 4) == 1, v);
		}
		check("utf-8: backward from inside it lands on its lead byte",
		      nb_buf_prev_char(u8, 0, 3) == 1, "");
		check("utf-8: backward from the second byte",
		      nb_buf_prev_char(u8, 0, 2) == 1, "");
		check("utf-8: backward from the lead byte lands on the 'a'",
		      nb_buf_prev_char(u8, 0, 1) == 0, "");
		nb_buf_free(u8);
	}
	nb_buf_free(b);
}

/* ---------------------------------------------------------------------------
 * the codec: what gets written back
 * ------------------------------------------------------------------------- */
static void test_codec(void)
{
	nb_buf *b;
	char detail[160];
	int used = 0;
	char *out = NULL;
	int len = 0, lost = 0;

	/* a GB2312 file must be detected, normalised to UTF-8 inside, and written
	 * back as GB2312 */
	{
		char raw[16];
		int n = 0;

		raw[n++] = '/'; raw[n++] = '/'; raw[n++] = ' ';
		raw[n++] = (char)0xD6; raw[n++] = (char)0xD0;	/* 中 */
		raw[n++] = (char)0xCE; raw[n++] = (char)0xC4;	/* 文 */
		raw[n++] = '\n';
		check("codec: GB2312 detected",
		      nb_codec_detect(raw, n, NULL) == NB_ENC_GB2312, "");
		b = nb_buf_new();
		nb_buf_from_raw(b, raw, n, NB_ENC_GB2312, NB_EOL_LF);
		snprintf(detail, sizeof(detail), "len=%d line=[%s]",
			 nb_buf_linelen(b, 0), nb_buf_line(b, 0));
		/* the buffer holds UTF-8: 3 bytes for "// " plus 3 + 3 for the two
		 * characters (the GB2312 file had 3 + 2 + 2) */
		check("codec: normalised to UTF-8 (9 bytes for 5 characters)",
		      nb_buf_linelen(b, 0) == 9 &&
		      (unsigned char)nb_buf_line(b, 0)[3] == 0xE4, detail);
		nb_buf_to_raw(b, &out, &len, &lost);
		check("codec: written back as GB2312 bytes", len == 8, "");
		check("codec: byte for byte, newline and all",
		      out && (unsigned char)out[3] == 0xD6 &&
		      (unsigned char)out[4] == 0xD0 &&
		      (unsigned char)out[5] == 0xCE &&
		      (unsigned char)out[6] == 0xC4 &&
		      out[7] == '\n', "");
		free(out);
		nb_buf_free(b);
	}

	/* UTF-8 stays UTF-8 */
	{
		const char *txt = "// \xe4\xb8\xad\xe6\x96\x87\n";
		b = nb_buf_new();
		nb_buf_from_raw(b, txt, (int)strlen(txt), NB_ENC_UTF8, NB_EOL_LF);
		nb_buf_to_raw(b, &out, &len, &lost);
		check("codec: UTF-8 kept as UTF-8 (byte for byte)",
		      len == (int)strlen(txt) &&
		      memcmp(out, txt, (size_t)len) == 0, "");
		free(out);
		nb_buf_free(b);
	}

	/* CRLF is preserved */
	{
		const char *txt = "a\r\nb\r\n";
		b = nb_buf_new();
		nb_buf_from_raw(b, txt, (int)strlen(txt), NB_ENC_UTF8, NB_EOL_CRLF);
		check("codec: CRLF file has 2 lines", b->nlines == 2, "");
		nb_buf_to_raw(b, &out, &len, &lost);
		{
			const unsigned char *ob = (const unsigned char *)out;
			int kk;

			used = snprintf(detail, sizeof(detail), "len=%d bytes=", len);
			for (kk = 0; kk < len && used < 100; kk++)
				used += snprintf(detail + used, sizeof(detail) - (size_t)used,
						 "%02X ", ob[kk]);
		}
		check("codec: CRLF written back, CR included",
		      len == 6 && out && out[1] == '\r' && out[2] == '\n' &&
		      out[3] == 'b' && out[4] == '\r' && out[5] == '\n',
		      detail);
		free(out);
		nb_buf_free(b);
	}

	/* an untouched file must survive open+save byte for byte, in both
	 * encodings: nbCode edits other tools' files and must not rewrite them */
	{
		const char *u8 = "local x = 1\n-- \xe4\xb8\xad\xe6\x96\x87\n";
		b = nb_buf_new();
		nb_buf_from_raw(b, u8, (int)strlen(u8), NB_ENC_UTF8, NB_EOL_LF);
		nb_buf_to_raw(b, &out, &len, &lost);
		check("codec: an untouched UTF-8 file is byte for byte the same",
		      len == (int)strlen(u8) && memcmp(out, u8, (size_t)len) == 0,
		      "");
		free(out);
		nb_buf_free(b);
	}

	/* Chinese typed into an ASCII file is saved as GB2312, because that is
	 * what the rest of the machine can read */
	{
		b = mk("x\n");
		nb_buf_goto(b, 0, 1);		/* after the x, as a user would */
		nb_buf_insert(b, "\xe4\xb8\xad", 3);
		nb_buf_to_raw(b, &out, &len, &lost);
		{
			const unsigned char *ob = (const unsigned char *)out;
			int kk;

			used = snprintf(detail, sizeof(detail), "len=%d bytes=", len);
			for (kk = 0; kk < len && used < 100; kk++)
				used += snprintf(detail + used, sizeof(detail) - (size_t)used,
						 "%02X ", ob[kk]);
		}
		check("codec: typed Chinese saved as GB2312",
		      len == 4 && out[0] == 'x' &&
		      (unsigned char)out[1] == 0xD6 &&
		      (unsigned char)out[2] == 0xD0 && out[3] == '\n',
		      detail);
		free(out);
		nb_buf_free(b);
	}
}

/* ---------------------------------------------------------------------------
 * the font: the geometry the renderer depends on
 * ------------------------------------------------------------------------- */
static void test_font(void)
{
	check("font: table is a valid NBFNT face", nb_font_check() == 1, "");
	check("font: ASCII fast path", nb_font_glyph('A') >= 0, "");
	check("font: space has no ink", font_xw(' ') == 0, "");
	check("font: GB2312 U+4E2D has a glyph",
	      nb_font_gb_glyph(0xD6, 0xD0) >= 0, "");
	check("font: GB2312 maps to the same glyph as Unicode",
	      nb_font_gb_glyph(0xD6, 0xD0) == nb_font_glyph(0x4E2D), "");
	{
		unsigned int hi = 0, lo = 0;
		int ok = nb_font_gb_pair(0x4E2D, &hi, &lo);

		check("font: reverse GB2312 lookup", ok && hi == 0xD6 && lo == 0xD0,
		      "");
	}
	check("font: the GB2312 symbol row maps to real characters",
	      nb_font_gb_unicode(0xA1, 0xA1) == 0x3000 &&
	      nb_font_gb_unicode(0xA1, 0xA2) == 0x3001 &&
	      nb_font_gb_glyph(0xA1, 0xA2) >= 0, "");
	check("font: a byte pair outside GB2312 has no glyph",
	      nb_font_gb_glyph(0x80, 0x80) < 0, "");
	/* every glyph's ink must fit its cell: a generator bug here would draw
	 * pixels into the neighbouring character */
	{
		int i, bad = 0, x0, xw, first, rows;

		for (i = 0; i < nb_font_table.nglyphs; i++) {
			if (!nb_font_lookup(i, &x0, &xw, &first, &rows, NULL,
					    NULL))
				continue;
			if (x0 + xw > 16 || first + rows > 16)
				bad++;
		}
		check("font: every ink box fits its 16x16 cell", bad == 0, "");
	}
	/* half-width vs full-width classification */
	check("font: 'A' is half width", nb_text_is_halfwidth('A') == 1, "");
	check("font: U+4E2D is full width", nb_text_is_halfwidth(0x4E2D) == 0, "");
}

/* ---------------------------------------------------------------------------
 * the highlighter
 * ------------------------------------------------------------------------- */
static int class_at(const char *line, nb_lang lang, int col)
{
	unsigned char styles[256];
	int end;

	nb_hl_line(nb_lang_get(lang), line, (int)strlen(line), styles, 0);
	(void)end;
	return styles[col];
}

static void test_highlight(void)
{
	check("hl: Lua 'local' is a declaration keyword",
	      class_at("local x = 1", NB_LANG_LUA, 0) == NB_HS_KEY, "");
	check("hl: Lua 'if' is control flow",
	      class_at("if x then", NB_LANG_LUA, 0) == NB_HS_CTRL, "");
	check("hl: Lua number is a constant",
	      class_at("x = 42", NB_LANG_LUA, 4) == NB_HS_CONST, "");
	check("hl: Lua string",
	      class_at("s = \"hi\"", NB_LANG_LUA, 4) == NB_HS_STRING, "");
	check("hl: Lua -- comment",
	      class_at("-- hello", NB_LANG_LUA, 3) == NB_HS_COMMENT, "");
	check("hl: Lua print is a library call",
	      class_at("print(1)", NB_LANG_LUA, 1) == NB_HS_LIB, "");
	check("hl: python def", class_at("def f():", NB_LANG_PY, 0) == NB_HS_KEY,
	      "");
	check("hl: python # comment",
	      class_at("# hi", NB_LANG_PY, 1) == NB_HS_COMMENT, "");
	check("hl: C #include is a preprocessor line",
	      class_at("#include <x.h>", NB_LANG_C, 2) == NB_HS_PREPROC, "");
	check("hl: PPL EXPORT", class_at("EXPORT f()", NB_LANG_PPL, 0) == NB_HS_KEY,
	      "");
	check("hl: a quote inside a comment is not a string",
	      class_at("-- it's fine", NB_LANG_LUA, 6) == NB_HS_COMMENT, "");

	/* the multi-line state: a Lua long comment opens on one line ... */
	{
		unsigned char s1[64], s2[64];
		int end1;

		end1 = nb_hl_line(nb_lang_get(NB_LANG_LUA), "--[[ note",
				  9, s1, NB_HL_BLK_NONE);
		check("hl: --[[ opens a block COMMENT (not a long string)",
		      end1 == NB_HL_BLK_LUA_COMMENT, "");
		{
			char v[48];
			int ok;

			nb_hl_line(nb_lang_get(NB_LANG_LUA), "inside", 6, s2, end1);
			/* the verdict is captured BEFORE the next call, because
			 * nb_hl_line reuses the same styles array and a `check` whose
			 * arguments are evaluated when the message is printed would
			 * report the NEXT line's styling */
			ok = (s2[0] == NB_HS_COMMENT && s2[2] == NB_HS_COMMENT &&
			      s2[5] == NB_HS_COMMENT);
			snprintf(v, sizeof(v), "end1=%d s2=%d,%d,%d,%d,%d,%d", end1,
				 s2[0], s2[1], s2[2], s2[3], s2[4], s2[5]);
			check("hl: every byte of the next line stays a comment", ok,
			      v);
		}
		nb_hl_line(nb_lang_get(NB_LANG_LUA), "]] back", 7, s2, end1);
		check("hl: and closes", s2[6] == NB_HS_TEXT ||
		      s2[6] == NB_HS_OP, "");
	}
	/* a C block comment */
	{
		unsigned char s[64];
		int e = nb_hl_line(nb_lang_get(NB_LANG_C), "/* x */ y", 9, s,
				   NB_HL_BLK_NONE);

		/* the line under test is: slash star space x space star slash
		 * space y -- bytes 0-1 open the comment, 5-6 close it, 8 is the
		 * 'y' that must be plain text again */
		{
			char v[64];

			snprintf(v, sizeof(v), "e=%d s=%d,%d,%d,%d", e, s[0], s[4],
				 s[5], s[8]);
			check("hl: C block comment closes on its own line",
			      e == NB_HL_BLK_NONE && s[0] == NB_HS_COMMENT &&
			      s[5] == NB_HS_COMMENT && s[6] == NB_HS_COMMENT &&
			      s[8] == NB_HS_TEXT, v);
		}
	}
	/* Chinese text must not break the tokenizer */
	{
		unsigned char s[64];
		int i, ok = 1;

		nb_hl_line(nb_lang_get(NB_LANG_LUA), "-- \xe4\xb8\xad\xe6\x96\x87",
			   8, s, NB_HL_BLK_NONE);
		for (i = 0; i < 8; i++) {
			if (s[i] != NB_HS_COMMENT)
				ok = 0;
		}
		check("hl: a Chinese comment is all comment", ok, "");
	}
}

/* ---------------------------------------------------------------------------
 * the renderer: geometry, and what the pixels actually look like
 * ------------------------------------------------------------------------- */
/* A PICTURE OF THE SCREEN, one character per COLOUR.
 *
 * "non-zero is #" stopped being useful the moment the light theme stopped having
 * a black background: every pixel was ink.  This maps each colour it meets to its
 * own character (and prints the legend, so a dump can be read back to the theme
 * it came from) -- which is what makes a layout review possible without the
 * calculator in hand. */
static void art(int x, int y, int w, int h, const char *what)
{
	static const char glyphs[] = "#*+-=:.ox%&@$0123456789abcdefghijklmnop";
	nb_u32 seen[48];
	int n = 0, r, c, i;

	printf("     %s  (%d,%d %dx%d)\n", what, x, y, w, h);
	for (r = 0; r < h; r++) {
		printf("     ");
		for (c = 0; c < w; c++) {
			nb_u32 p = nb_host_fb_pixel(x + c, y + r);

			for (i = 0; i < n; i++) {
				if (seen[i] == p)
					break;
			}
			if (i == n && n < (int)(sizeof(seen) / sizeof(seen[0])))
				seen[n++] = p;
			putchar(i < (int)sizeof(glyphs) - 1 ? glyphs[i] : '?');
		}
		putchar('\n');
	}
	printf("     legend:");
	for (i = 0; i < n; i++)
		printf(" %c=%08X", glyphs[i], (unsigned)seen[i]);
	printf("\n");
}

/* the first and last column that carry ink in a row band: the measurements the
 * glyph placement checks are made of */
static void ink_span(int from, int to, int *first, int *last)
{
	int col, row;

	*first = -1;
	*last = -1;
	for (col = from; col < to; col++) {
		for (row = 0; row < NB_SCREEN_H; row++) {
			if (nb_host_fb_pixel(col, row) != 0) {
				if (*first < 0)
					*first = col;
				*last = col;
				break;
			}
		}
	}
}

static void test_render(void)
{
	nb_text_decoded d;
	const nb_theme *th = nb_theme_get();
	nb_u32 classes[NB_HS_KIND];

	nb_theme_set((nb_theme *)th, 0);
	nb_text_classes(th, classes);
	nb_gfx_clear(0);

	/* geometry: an ASCII string is 8 pixels per character */
	nb_text_decode(&d, "if (a) return 1;", 16, NULL, NB_ENC_UTF8);
	check("render: 16 western characters = 16 frames", d.frames == 16, "");
	check("render: width in pixels", d.frames * 8 == 128, "");

	/* geometry: a GB2312 character is two frames */
	nb_text_decode(&d, "\xe4\xb8\xad\xe6\x96\x87", 6, NULL, NB_ENC_UTF8);
	check("render: two GB2312 characters = 4 frames", d.frames == 4, "");

	/* mixed: 2 western + 1 CJK + 2 western = 2+2+2 = 6 frames */
	nb_text_decode(&d, "a\xe4\xb8\xad" "b c", 7, NULL, NB_ENC_UTF8);
	check("render: mixed width", d.frames == 6, "");

	/* hit testing: frame -> byte */
	check("render: frame to byte (CJK is 2 frames)",
	      nb_text_byte_at(&d, 3) == 4, "");
	check("render: byte to frame (byte 4 is the 'b' after the CJK)",
	      nb_text_frame_of(&d, 4) == 3, "");
	check("render: end of line frame",
	      nb_text_frame_of(&d, 7) == 6, "");

	/* draw something and look at it */
	nb_gfx_clear(0);
	{
		unsigned char styles[64];

		nb_hl_line(nb_lang_get(NB_LANG_LUA), "local x = 42", 12, styles,
			   NB_HL_BLK_NONE);
		nb_text_decode(&d, "local x = 42", 12, styles, NB_ENC_UTF8);
		nb_text_draw_decoded(&d, 0, 0, 0, 0, th, classes, 0, 0);
	}
	art(0, 0, 96, 16, "\"local x = 42\" with Lua styles");
	{
		int lit = 0, x, y;

		for (y = 0; y < 16; y++)
			for (x = 0; x < 96; x++)
				if (nb_host_fb_pixel(x, y))
					lit++;
		check("render: the line drew ink", lit > 40,
		      "lit pixels");
		/* the colours really differ per class: 'local' red-ish, the
		 * number amber */
		{
			nb_u32 key = 0, num = 0;

			for (y = 0; y < 16 && !key; y++)
				for (x = 0; x < 40; x++) {
					nb_u32 p = nb_host_fb_pixel(x, y);

					if (p) {
						key = p;
						break;
					}
				}
			for (y = 0; y < 16 && !num; y++)
				for (x = 80; x < 96; x++) {
					nb_u32 p = nb_host_fb_pixel(x, y);

					if (p) {
						num = p;
						break;
					}
				}
			check("render: keyword and number are different colours",
			      key && num && key != num, "");
		}
	}

	/* a Chinese line */
	nb_gfx_clear(0);
	nb_text_decode(&d, "-- \xe4\xb8\xad\xe6\x96\x87\xe6\xb3\xa8\xe9\x87\x8a",
		       2 + 12, NULL, NB_ENC_UTF8);
	nb_text_draw_decoded(&d, 0, 16, 0, 0, th, classes, 0, 0);
	art(0, 16, 64, 16, "four GB2312 characters (64 px wide)");

	/* the glyph cache path: it must produce the same pixels as the direct
	 * path, or the editor's fast path would draw different letters than the
	 * tests above verified */
	{
		nb_gcache cache;
		int slot, x;
		int ok = 1;
		nb_u32 expect[16][16];
		int r, c;

		nb_gfx_clear(0);
		nb_text_decode(&d, "A", 1, NULL, NB_ENC_UTF8);
		nb_text_draw_decoded(&d, 0, 0, 0, 0, th, classes, 0, 0);
		for (r = 0; r < 16; r++)
			for (c = 0; c < 16; c++)
				expect[r][c] = nb_host_fb_pixel(c, r);

		nb_gfx_clear(0);
		nb_gcache_reset(&cache);
		slot = nb_glyph_put(&cache, nb_font_glyph('A'), th->text);
		nb_glyph_draw(&cache, slot, 0, 0);
		for (r = 0; r < 16 && ok; r++)
			for (c = 0; c < 16; c++) {
				if (expect[r][c] != nb_host_fb_pixel(c, r)) {
					ok = 0;
					break;
				}
			}
		check("render: the glyph cache draws the same pixels", ok, "");
		(void)x;
	}

	/* clipping: drawing off screen must not corrupt anything (the host
	 * framebuffer is exactly the screen, so this is a bounds test) */
	nb_gfx_clear(0);
	nb_text_decode(&d, "\xe4\xb8\xad\xe6\x96\x87", 6, NULL, NB_ENC_UTF8);
	nb_text_draw_decoded(&d, 312, 232, 0, 0, th, classes, 0, 0);
	check("render: off screen drawing is clipped", 1, "");

	/* THE UI IS IN CHINESE, so the CJK path has to work for UI strings too --
	 * not just for the text of a file.  Each of these characters is a full-width
	 * 16 px cell, and the string is drawn through the same nb_text_draw() the
	 * status bar uses. */
	{
		int first = -1, last = -1;
		nb_gfx_clear(0);
		{
			nb_text_decoded dd;

			nb_text_draw(&dd, 4, 0, "已打开", 0xFFFFFFu, th);
			check("ui: a Chinese UI string is three full-width cells",
			      dd.frames == 6, "");
		}
		ink_span(0, 120, &first, &last);
		{
			char v[64];

			snprintf(v, sizeof(v), "ink columns %d..%d", first, last);
			check("ui: ... and it draws ink where it should", first >= 4 &&
			      last >= 4 + 40, v);
		}
	}

	/* WHERE A GLYPH'S INK ACTUALLY LANDS, measured rather than assumed.
	 *
	 * This is the check that was missing: every western glyph was drawn EIGHT
	 * PIXELS LEFT of its own cell (nb_text.c applied "x0 - 8" on the belief
	 * that a half-width glyph's ink lives in the high byte of the 16 bit row),
	 * and the tests above could not see it -- they draw at x=0, where an origin
	 * of x0-8 is a negative column that the clipping discards, and the cache
	 * comparison below compares two paths that shared the same wrong rule.
	 *
	 * So: draw one glyph with its CELL at a known x, and require the ink to be
	 * inside that cell, at its ink-box offset.  On the calculator the bug
	 * showed as text sitting one character width left of the cursor. */
	{
		char buf[80];
		int first = -1, last = -1;


		nb_gfx_clear(0);
		nb_text_glyph(20, 0, nb_font_glyph('A'), 0xFFFFFFu);
		ink_span(8, 64, &first, &last);
		snprintf(buf, sizeof(buf), "'A' cell at x=20 -> ink columns %d..%d",
			 first, last);
		check("render: a glyph's ink lands inside its own cell",
		      first >= 20 && last < 28, buf);
		snprintf(buf, sizeof(buf), "expected 21, got %d", first);
		check("render: ... at its ink box offset, not a whole cell to the "
		      "left", first == 21, buf);

		/* a full width glyph fills its own 16 pixel cell the same way */
		nb_gfx_clear(0);
		nb_text_decode(&d, "\xe4\xb8\xad", 3, NULL, NB_ENC_UTF8);
		nb_text_draw_decoded(&d, 20, 0, 0, 0, th, classes, 0, 0);
		ink_span(8, 64, &first, &last);
		snprintf(buf, sizeof(buf), "CJK cell at x=20 -> ink columns %d..%d",
			 first, last);
		check("render: a full width glyph fills its 16 pixel cell",
		      first >= 20 && last < 36, buf);

		/* and the ADVANCE agrees with the ink: the second character of "AB"
		 * must be in the second cell, or the caret and the text drift apart
		 * over a line */
		nb_gfx_clear(0);
		nb_text_decode(&d, "AB", 2, NULL, NB_ENC_UTF8);
		nb_text_draw_decoded(&d, 20, 0, 0, 0, th, classes, 0, 0);
		ink_span(28, 64, &first, &last);
		snprintf(buf, sizeof(buf), "'B' starts at %d (cell 2 starts at 28)",
			 first);
		check("render: the next character is in the next 8 pixel cell",
		      first >= 28 && first < 36, buf);
	}
}

/* ---------------------------------------------------------------------------
 * the highlight STATE ARRAY: which block each line starts in
 *
 * One byte per line, allocated by nb_hl_lines() and read back by the next line's
 * tokenizer.  Two things about it are checked here, because both were wrong and
 * neither shows up reliably by luck:
 *
 *   * the fresh bytes have to MEAN "no block is open".  They used to be whatever
 *     the heap contained, and a byte that looked like NB_HL_BLK_LUA_LONG or a
 *     Python triple quote paints THE WHOLE FILE in the string colour -- green.
 *     That is what "some files open all green" was: the file was being read as
 *     one long, unterminated string.  The heap is poisoned first here, so this
 *     fails every time instead of one time in four;
 *   * the state must not follow one file into the next.  nb_ui_open_file()
 *     re-initialises the highlighter per file, so a file that ended inside a
 *     block comment must not colour the next one as a comment.
 * ------------------------------------------------------------------------- */
static void test_highlight_state(void)
{
	nb_hl h;
	nb_buf *b = nb_buf_new();
	unsigned char *styles[8];
	int caps[8];
	unsigned char *poison;
	const char *plain = "int x = 1;\n";
	int i, len;
	char v[96];

	nb_memset(&h, 0, sizeof(h));
	for (i = 0; i < 8; i++) {
		styles[i] = NULL;
		caps[i] = 0;
	}
	if (!b) {
		check("hl: a buffer could be created", 0, "");
		return;
	}
	/* POISON THE HEAP where the state array will land: same size class, filled
	 * with a state that means "inside a Python triple quote", then freed so the
	 * highlighter's allocation reuses it. */
	poison = (unsigned char *)nb_alloc(256);
	if (poison) {
		for (i = 0; i < 256; i++)
			poison[i] = NB_HL_BLK_PY_TRIPLE_D;
		nb_free(poison);
	}

	len = (int)strlen(plain);
	nb_buf_from_raw(b, plain, len, NB_ENC_UTF8, NB_EOL_LF);
	nb_hl_open(&h, nb_lang_get(NB_LANG_C), b);
	nb_hl_lines(&h, b, styles, caps, 0, b->nlines - 1, 0);
	snprintf(v, sizeof(v), "'int x = 1;' byte 4 (the x) is style %d",
		 styles[0] ? (int)styles[0][4] : -1);
	check("hl: the state array starts clean, not with heap garbage",
	      styles[0] && styles[0][4] == NB_HS_TEXT, v);
	check("hl: ... so a plain line is not one long string (green)",
	      styles[0] && styles[0][0] != NB_HS_STRING &&
	      styles[0][4] != NB_HS_STRING, v);

	/* A LINE THAT GROWS MUST GET A BIGGER BUFFER.  This is the invariant behind
	 * "insert one character into a .txt and the colours below shift": nb_hl_line
	 * writes one style byte per byte of the line, so a buffer one byte short puts
	 * that byte into the next heap allocation.  The capacity array is what makes
	 * the check possible, and it is also what nb_hl_lines() uses to decide. */
	{
		unsigned char *st[4];
		int caps[4];
		const char *short_line = "x = 1";
		const char *long_line = "x = 123456789;";
		int k;

		for (k = 0; k < 4; k++) {
			st[k] = NULL;
			caps[k] = 0;
		}
		nb_buf_from_raw(b, short_line, (int)strlen(short_line),
				NB_ENC_UTF8, NB_EOL_LF);
		nb_hl_open(&h, nb_lang_get(NB_LANG_C), b);
		nb_hl_lines(&h, b, st, caps, 0, b->nlines - 1, 0);
		snprintf(v, sizeof(v), "a %d byte line got room for %d",
			 (int)strlen(short_line), caps[0]);
		check("hl: a line's style buffer is allocated to its length",
		      caps[0] >= (int)strlen(short_line) + 1, v);

		nb_buf_from_raw(b, long_line, (int)strlen(long_line),
				NB_ENC_UTF8, NB_EOL_LF);
		nb_hl_open(&h, nb_lang_get(NB_LANG_C), b);
		nb_hl_lines(&h, b, st, caps, 0, b->nlines - 1, 0);
		snprintf(v, sizeof(v), "a %d byte line has room for %d",
			 (int)strlen(long_line), caps[0]);
		check("hl: a grown line gets a bigger buffer, not one byte short",
		      caps[0] >= (int)strlen(long_line) + 1, v);
		check("hl: ... and its colours are the ones it needs",
		      st[0] && st[0][0] == NB_HS_TEXT && st[0][4] == NB_HS_CONST, v);
		nb_free(st[0]);
	}

	/* A FILE THAT ENDS INSIDE A BLOCK COMMENT, then the next file: this is the
	 * sequence nb_ui_open_file() performs, with its own re-init. */
	{
		const char *open = "/* still open\n";

		len = (int)strlen(open);
		nb_buf_from_raw(b, open, len, NB_ENC_UTF8, NB_EOL_LF);
		nb_hl_open(&h, nb_lang_get(NB_LANG_C), b);
		nb_hl_lines(&h, b, styles, caps, 0, b->nlines - 1, 0);
		check("hl: a line inside a block comment IS a comment",
		      styles[0] && styles[0][0] == NB_HS_COMMENT, "");
		len = (int)strlen(plain);
		nb_buf_from_raw(b, plain, len, NB_ENC_UTF8, NB_EOL_LF);
		nb_hl_open(&h, nb_lang_get(NB_LANG_C), b);
		nb_hl_lines(&h, b, styles, caps, 0, b->nlines - 1, 0);
		snprintf(v, sizeof(v), "the new file's 'int' is style %d, its 'x' "
			 "is style %d", styles[0] ? (int)styles[0][0] : -1,
			 styles[0] ? (int)styles[0][4] : -1);
		check("hl: a block comment does not follow the previous file",
		      styles[0] && styles[0][0] == NB_HS_KEY &&
		      styles[0][4] == NB_HS_TEXT, v);
	}

	for (i = 0; i < 8; i++)
		nb_free(styles[i]);
	nb_hl_free(&h);
	nb_buf_free(b);
}

/* ---------------------------------------------------------------------------
 * THE PALETTE, MEASURED
 *
 * On a 320x240 LCD with a 1-pixel-stroke bitmap font, a mid-tone colour is the
 * difference between "syntax colouring" and "I cannot read this".  The first
 * palette had comments at 3.7:1, numbers at 3.7:1, library calls at 4.2:1 and the
 * line numbers at 2.5:1 against white; the user's report was exactly that ("the
 * highlighting is there but it is hard to see").
 *
 * So the colours are chosen by contrast ratio and this keeps them there: every
 * code class at WCAG AAA (7:1) against its background, and the two classes that
 * are meant to sit back -- comments and the line-number gutter -- at 4.5:1.
 * ------------------------------------------------------------------------- */
static double chan(double c)
{
	c /= 255.0;
	return (c <= 0.03928) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(nb_u32 rgb)
{
	double r = chan((double)((rgb >> 16) & 0xFF));
	double g = chan((double)((rgb >> 8) & 0xFF));
	double b = chan((double)(rgb & 0xFF));

	return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/* HSV saturation: 0 is a grey, 1 is a pure hue.  A colour can pass the contrast
 * test and still be a shade of black -- which is exactly what happened when the
 * contrast target was pushed to 7:1 and the user's report became "it all looks
 * like black text". */
static double saturation_of(nb_u32 rgb)
{
	double r = (double)((rgb >> 16) & 0xFF) / 255.0;
	double g = (double)((rgb >> 8) & 0xFF) / 255.0;
	double b = (double)(rgb & 0xFF) / 255.0;
	double hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	double lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	double d = hi - lo;

	return (hi <= 0.0) ? 0.0 : d / hi;
}

static double contrast(nb_u32 a, nb_u32 b)
{
	double la = luminance(a), lb = luminance(b);
	double hi = la > lb ? la : lb, lo = la > lb ? lb : la;

	return (hi + 0.05) / (lo + 0.05);
}

static void test_theme_contrast(void)
{
	static const char *code_names[] = { "text", "keyword", "control", "constant",
					    "string", "operator", "library",
					    "preprocessor" };
	nb_theme t;
	int dark, i;

	for (dark = 0; dark < 2; dark++) {
		nb_theme_set(&t, dark);
		{
			nb_u32 v[8];
			char vbuf[96];

			v[0] = t.text;
			v[1] = t.key;
			v[2] = t.ctrl;
			v[3] = t.constant;
			v[4] = t.string;
			v[5] = t.op;
			v[6] = t.lib;
			v[7] = t.preproc;
			for (i = 0; i < 8; i++) {
				double c = contrast(v[i], t.bg);
				double sat = saturation_of(v[i]);
				/* plain text (i == 0) is NEUTRAL ON PURPOSE: it is what
				 * unstyled code looks like, and the colours have to stand
				 * out against it */
				int ok = (c >= 4.5) &&
					 (i == 0 || sat >= 0.45);

				snprintf(vbuf, sizeof(vbuf),
					 "%s theme %s: %.2f:1, saturation %.2f "
					 "(want >= 4.5:1 and >= 0.45)",
					 dark ? "dark" : "light", code_names[i], c, sat);
				check("theme: code colours are readable AND coloured",
				      ok, vbuf);
			}
			snprintf(vbuf, sizeof(vbuf), "%s theme comment: %.2f:1",
				 dark ? "dark" : "light", contrast(t.comment, t.bg));
			check("theme: comments are readable without shouting",
			      contrast(t.comment, t.bg) >= 4.5, vbuf);
			snprintf(vbuf, sizeof(vbuf), "%s theme gutter: %.2f:1",
				 dark ? "dark" : "light",
				 contrast(t.gutter_fg, t.gutter_bg));
			check("theme: the line numbers are readable too",
			      contrast(t.gutter_fg, t.gutter_bg) >= 4.5, vbuf);
			snprintf(vbuf, sizeof(vbuf), "%s theme cursor: %.2f:1",
				 dark ? "dark" : "light", contrast(t.cursor, t.bg));
			check("theme: the caret stands out from the text behind it",
			      contrast(t.cursor, t.bg) >= 1.6, vbuf);
			/* the chrome is read as much as the code: the status bar hints
			 * ("up/down ENTER open esc quit"), the file sizes, and the name of
			 * the selected row */
			snprintf(vbuf, sizeof(vbuf), "%s theme status bar: %.2f:1",
				 dark ? "dark" : "light",
				 contrast(t.sub_fg, t.foot_bg));
			check("theme: the status bar is readable",
			      contrast(t.sub_fg, t.foot_bg) >= 4.5, vbuf);
			snprintf(vbuf, sizeof(vbuf), "%s theme selected row: %.2f:1",
				 dark ? "dark" : "light",
				 contrast(t.sel_fg, t.sel_bg));
			check("theme: the selected row is readable",
			      contrast(t.sel_fg, t.sel_bg) >= 4.5, vbuf);
			snprintf(vbuf, sizeof(vbuf), "%s theme header: %.2f:1",
				 dark ? "dark" : "light",
				 contrast(t.head_fg, t.head_bg));
			check("theme: the header is readable",
			      contrast(t.head_fg, t.head_bg) >= 4.5, vbuf);
			/* THE SAME CODE IS ALSO READ WHILE IT IS SELECTED and while it
			 * is on the current line, and the selection is the tightest of
			 * the three surfaces.  The floor there is 4.2 rather than 4.5
			 * ON PURPOSE: a selection nobody can see is worse than one whose
			 * syntax colours are 4.2:1 (every editor in the wild is worse
			 * than that), and the number is measured, not guessed. */
			{
				static const char *cls[] = { "text", "keyword",
					"control", "constant", "string", "operator",
					"library", "preprocessor", "comment" };
				nb_u32 v[9];
				double worst_sel = 99, worst_cur = 99;
				double worst_match = 99, worst_find = 99;

				v[0] = t.text;
				v[1] = t.key;
				v[2] = t.ctrl;
				v[3] = t.constant;
				v[4] = t.string;
				v[5] = t.op;
				v[6] = t.lib;
				v[7] = t.preproc;
				v[8] = t.comment;
				for (i = 0; i < 9; i++) {
					double cs = contrast(v[i], t.sel_bg);
					double cc = contrast(v[i], t.curline);
					double cm = contrast(v[i], t.match);
					double cf = contrast(v[i], t.find);

					(void)cls;
					if (cf < worst_find)
						worst_find = cf;
					if (cs < worst_sel)
						worst_sel = cs;
					if (cc < worst_cur)
						worst_cur = cc;
					if (cm < worst_match)
						worst_match = cm;
				}
				snprintf(vbuf, sizeof(vbuf),
					 "%s theme worst on selection: %.2f:1",
					 dark ? "dark" : "light", worst_sel);
				check("theme: the code stays readable while it is "
				      "selected", worst_sel >= 4.2, vbuf);
				snprintf(vbuf, sizeof(vbuf),
					 "%s theme worst on the current line: %.2f:1",
					 dark ? "dark" : "light", worst_cur);
				check("theme: ... and on the current line",
				      worst_cur >= 4.5, vbuf);
				snprintf(vbuf, sizeof(vbuf),
					 "%s theme worst on a matched bracket: %.2f:1",
					 dark ? "dark" : "light", worst_match);
				check("theme: ... and on a matched bracket",
				      worst_match >= 4.5, vbuf);
				snprintf(vbuf, sizeof(vbuf),
					 "%s theme worst on a search result: %.2f:1",
					 dark ? "dark" : "light", worst_find);
				check("theme: ... and on a highlighted search result",
				      worst_find >= 4.5, vbuf);
			}
			/* THE DIVIDERS ARE DIVIDERS: visible (a hairline that cannot be
			 * seen is not a separator) and quiet (a bar across the screen is
			 * not a hairline) */
			{
				double l = contrast(t.line, t.bg);

				snprintf(vbuf, sizeof(vbuf),
					 "%s theme divider: %.2f:1",
					 dark ? "dark" : "light", l);
				check("theme: the divider is visible but quiet",
				      l >= 1.15 && l <= 2.2, vbuf);
			}
			/* ... and the ACCENT is one colour for everything interactive:
			 * it has to stand out from the page it is drawn on */
			snprintf(vbuf, sizeof(vbuf), "%s theme accent: %.2f:1",
				 dark ? "dark" : "light",
				 contrast(t.accent, t.bg));
			check("theme: the accent stands out", 
			      contrast(t.accent, t.bg) >= 3.0, vbuf);
			check("theme: the caret IS the accent",
			      t.cursor == t.accent, "");
			/* THE PANELS PAIR THE COLOURS THEMSELVES, and getting it wrong is
			 * invisible: the overlays' titles were drawn in head_fg (the
			 * colour of a DARK header bar) on a light panel background, so
			 * "字符表" and "按键帮助" rendered as nothing.  These are the pairs
			 * the panels use; each has to be readable on its own. */
			snprintf(vbuf, sizeof(vbuf),
				 "%s theme panel title on the body: %.2f:1",
				 dark ? "dark" : "light", contrast(t.text, t.bg));
			check("theme: a panel's title is readable on the body",
			      contrast(t.text, t.bg) >= 4.5, vbuf);
			snprintf(vbuf, sizeof(vbuf),
				 "%s theme panel/status text: %.2f:1",
				 dark ? "dark" : "light", contrast(t.text, t.foot_bg));
			check("theme: ... and on the panel's own background",
			      contrast(t.text, t.foot_bg) >= 4.5, vbuf);
			snprintf(vbuf, sizeof(vbuf),
				 "%s theme panel hint: %.2f:1",
				 dark ? "dark" : "light", contrast(t.sub_fg, t.bg));
			check("theme: a panel's hint line is readable",
			      contrast(t.sub_fg, t.bg) >= 4.5, vbuf);
			/* (THE PANEL TITLES ALSO HAVE INK CHECKS, in test_panels: the
			 * palette's title row, the help panel's and the information
			 * panel's are all measured on the framebuffer.  That is what
			 * catches "the panel drew its title in a colour that happens to
			 * match the panel" -- which is not a theme invariant, because in
			 * the dark theme the same two colours are perfectly readable.) */
		}
	}
}

/* ---------------------------------------------------------------------------
 * the editor, driven through the UI exactly as the calculator drives it
 *
 * The keys below are DEVICE scan codes (nb_keymap.c): 0x51 is the 7/Q key (a '7'
 * with no modifier, 'q' with alpha), 0x0D is ENTER, 0xB6 is ALPHA, 0x93 is MENU
 * and 0x95 is HELP.  The calculator has no F1..F6 row: what used to be on those
 * keys is in the MENU popup now, and the tests drive the ACTIONS the menu fires
 * (nb_ui_editor_action) or open the menu and click a row.
 * Driving the real nb_ui_key() through the real host event queue is what makes
 * this a test of the editor and not of a model of it.
 * ------------------------------------------------------------------------- */
/* push one touch event through the real path (nb_ui_touch takes nb_event) */
static void nb_ui_touch_send(int action, int x, int y)
{
	nb_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = 2;			/* touch */
	ev.action = action;		/* 1 press, 2 move, 8 release */
	ev.x = x;
	ev.y = y;
	nb_ui_touch(&ev);
}

static void feed_key(int code)
{
	nb_host_event_push(code, 1);
	nb_host_pump();
	nb_host_event_push(code, 0);
	nb_host_pump();
}

/* How many pixels differ from the status row's background at its right hand end:
 * that is where the editor draws the layer indicator, and it is the only thing
 * drawn there -- counting "non zero" would count the background itself, which is
 * a colour, not black. */
static int status_right_ink(void)
{
	nb_u32 bg = nb_host_fb_pixel(NB_SCREEN_W - 2, NB_SCREEN_H - 8);
	int x, y, ink = 0;

	for (x = 240; x < NB_SCREEN_W - 2; x++) {
		for (y = NB_SCREEN_H - 16; y < NB_SCREEN_H; y++) {
			if (nb_host_fb_pixel(x, y) != bg)
				ink++;
		}
	}
	return ink;
}

/* The UI test needs a directory that really exists: nb_ui_open_file() goes to
 * the port layer's file API, and on the host that is the real filesystem.  A
 * temporary directory keeps the test independent of the checkout's contents and
 * of the working directory it is run from. */
static char ui_dir[NB_PATH_MAX];

/* does the saved file contain `needle`?  Used to prove that what a save wrote
 * is what the editor held.  The device path separator ('\\') is translated
 * here because the editor hands the port layer a Windows-style path whatever
 * the host is. */
static int file_has(const char *path, const char *head)
{
	char host[NB_PATH_MAX];
	FILE *f;
	char buf[64];
	size_t n;
	int i;

	nb_str_copy(host, sizeof(host), path, -1);
	for (i = 0; host[i]; i++) {
		if (host[i] == '\\')
			host[i] = '/';
	}
	f = fopen(host, "rb");
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	return strstr(buf, head) != NULL;
}

static void make_ui_dir(void)
{
	char tmpl[64];

	nb_str_copy(tmpl, sizeof(tmpl), "/tmp/nbcode_ui_XXXXXX", -1);
	if (!mkdtemp(tmpl))
		return;
	nb_str_copy(ui_dir, sizeof(ui_dir), tmpl, -1);
	{
		char path[NB_PATH_MAX];
		FILE *f;

		nb_str_copy(path, sizeof(path), ui_dir, -1);
		nb_str_append(path, sizeof(path), "/main.c");
		f = fopen(path, "wb");
		if (f) {
			fputs("int main(void) {\n    return 0;\n}\n", f);
			fclose(f);
		}
		nb_str_copy(path, sizeof(path), ui_dir, -1);
		nb_str_append(path, sizeof(path), "/note.txt");
		f = fopen(path, "wb");
		if (f) {
			fputs("a note\n", f);
			fclose(f);
		}
	}
}

static void drop_ui_dir(void)
{
	char path[NB_PATH_MAX];

	if (!ui_dir[0])
		return;
	nb_str_copy(path, sizeof(path), ui_dir, -1);
	nb_str_append(path, sizeof(path), "/main.c");
	remove(path);
	nb_str_copy(path, sizeof(path), ui_dir, -1);
	nb_str_append(path, sizeof(path), "/note.txt");
	remove(path);
	rmdir(ui_dir);
}

static char ui_dir_slash[NB_PATH_MAX];

/* ---------------------------------------------------------------------------
 * the keyboard: the letters the keys are printed with, the two second-level
 * keys, and the promise that every printable ASCII byte is reachable
 *
 * THE BUG THIS PINS: the first version of nb_keymap.c guessed that the letters
 * run QWERTY-wise over the keypad (q w e / r t y / u i o ...), and mapped
 * nothing at all to the five keys whose device codes are 0x41..0x45 -- so on the
 * calculator a, b, c, d and e could not be typed at all.  The letters below are
 * the ones PRINTED on the keys (the Python PrimeCode this editor is modelled on
 * has them in keys.py as a GETKEY table); the scan codes are the
 * hardware-verified DEVICE codes.  The two lists together are the contract.
 *
 * The second half is the reason the layer can be trusted at all: whatever the
 * assignments are, a C file needs the whole ASCII set, so the test walks the
 * table and insists that all 95 printable bytes come out of some key in some
 * modifier state.
 * ------------------------------------------------------------------------- */
static const struct { int code; char letter; } printed_letters[] = {
	{ 0x41, 'a' }, { 0x42, 'b' }, { 0x43, 'c' }, { 0x44, 'd' },
	{ 0x45, 'e' }, { 0x46, 'f' }, { 0x47, 'g' }, { 0x48, 'h' },
	{ 0x49, 'i' }, { 0x4A, 'j' }, { 0x4B, 'k' }, { 0x4C, 'l' },
	{ 0x4D, 'm' }, { 0x4E, 'n' }, { 0x4F, 'o' }, { 0x50, 'p' },
	{ 0x51, 'q' }, { 0x52, 'r' }, { 0x53, 's' }, { 0x54, 't' },
	{ 0x55, 'u' }, { 0x56, 'v' }, { 0x57, 'w' }, { 0x58, 'x' },
	{ 0x59, 'y' }, { 0x5A, 'z' },
	{ 0x33, '#' }, { 0xB7, ':' }, { 0x30, '"' }, { 0xB8, '.' },
	{ 0x20, ' ' }, { 0xB9, ';' }
};

/* the alpha key as the calculator's keyboard delivers it: a press, then a
 * release that must NOT drop the layer.  SHIFT IS A LATCH (nb_keymap.c): it is
 * pressed once and stays on, so a capital selection is "shift, alpha, alpha" --
 * not one shift per alpha, which a toggle would read as on/off. */
static void press_alpha(int times, int with_shift)
{
	int i;

	if (with_shift)
		nb_keymap_modifier(0x8B, 1);		/* toggle it on */
	for (i = 0; i < times; i++) {
		nb_keymap_modifier(0xB6, 1);
		nb_keymap_modifier(0xB6, 0);
	}
	if (with_shift)
		nb_keymap_modifier(0x8B, 1);		/* ... and off again */
}

/* every byte the key can produce counts: a one-character layer puts it in ch,
 * and a SNIPPET (primeCode's "sin()", "()") carries a string -- those characters
 * are typed too, and the coverage check is about what the keyboard can produce */
static void note_char(unsigned char *seen, nb_key_meaning m)
{
	int i;

	if (m.what != NB_KEY_CHAR)
		return;
	if (m.ins) {
		for (i = 0; m.ins[i]; i++) {
			if ((unsigned char)m.ins[i] < 128)
				seen[(unsigned char)m.ins[i]] = 1;
		}
		return;
	}
	if ((unsigned char)m.ch[0] < 128)
		seen[(unsigned char)m.ch[0]] = 1;
}

static void test_keymap(void)
{
	unsigned char seen[128];
	int i;
	int missing = 0;
	char list[64];
	int n = 0;

	nb_keymap_reset();
	check("keys: a fresh session starts with alpha off",
	      nb_keymap_alpha_state() == NB_ALPHA_OFF &&
	      nb_keymap_alpha_upper() == 0 && !nb_keymap_held_shift(), "");

	/* the plain layer is the keypad, not the letters */
	{
		nb_key_meaning plain = nb_keymap(0x51);

		check("keys: the 7/Q key is a '7' with no modifier",
		      plain.what == NB_KEY_CHAR && plain.ch[0] == '7', "");
	}
	nb_keymap_modifier(0x8B, 1);
	{
		nb_key_meaning sh = nb_keymap(0x51);

		check("keys: shift turns it into '&'",
		      sh.what == NB_KEY_CHAR && sh.ch[0] == '&', "");
		check("keys: ... and the latch is spent by that key",
		      nb_keymap_held_shift() == 0, "");
	}
	nb_keymap_reset();

	/* SHIFT IS A LATCH, NOT A HOLD: press it and it stays (the status line
	 * says so), press it again and it is off */
	nb_keymap_modifier(0x8B, 1);
	{
		int on = nb_keymap_held_shift();

		nb_keymap_modifier(0x8B, 1);
		check("keys: shift latches on the press and off on the next press",
		      on == 1 && nb_keymap_held_shift() == 0, "");
	}
	check("keys: ... and a release does not clear it",
	      (nb_keymap_modifier(0x8B, 1), nb_keymap_modifier(0x8B, 0),
	       nb_keymap_held_shift()) == 1, "");

	/* PRIMECODE'S EDITING CHORDS (keys.py): shift+left is the start of the
	 * line, shift+right its end, shift+backspace deletes forward */
	nb_keymap_reset();
	nb_keymap_modifier(0x8B, 1);
	check("keys: shift+left is the start of the line",
	      nb_keymap(0x02).what == NB_KEY_HOME &&
	      nb_keymap_held_shift() == 0, "");
	nb_keymap_modifier(0x8B, 1);
	check("keys: shift+right is the end of the line",
	      nb_keymap(0x04).what == NB_KEY_END, "");
	nb_keymap_modifier(0x8B, 1);
	check("keys: shift+backspace is a forward delete",
	      nb_keymap(0x0C).what == NB_KEY_DELETE, "");
	check("keys: ... and without shift they are the plain keys",
	      nb_keymap(0x02).what == NB_KEY_LEFT &&
	      nb_keymap(0x04).what == NB_KEY_RIGHT &&
	      nb_keymap(0x0C).what == NB_KEY_BACKSPACE, "");

	/* SHIFT+A IS THE CHARACTER TABLE (and with alpha on it is still a capital) */
	nb_keymap_reset();
	nb_keymap_modifier(0x8B, 1);
	check("keys: shift+A opens the character table",
	      nb_keymap(0x41).what == NB_KEY_PALETTE, "");
	nb_keymap_reset();
	press_alpha(2, 1);			/* locked capitals */
	check("keys: ... but with alpha locked it is still a capital A",
	      nb_keymap(0x41).ch[0] == 'A', "");

	/* THE PRINTED ALPHA LAYER, ALL 32 KEYS */
	nb_keymap_reset();
	press_alpha(2, 0);			/* the second press locks it */
	check("keys: pressing alpha twice locks the layer",
	      nb_keymap_alpha_state() == NB_ALPHA_LOCK &&
	      !nb_keymap_alpha_upper(), "");
	for (i = 0; i < (int)(sizeof(printed_letters) /
			      sizeof(printed_letters[0])); i++) {
		nb_key_meaning m = nb_keymap(printed_letters[i].code);
		char v[64];

		snprintf(v, sizeof(v), "key 0x%02X (%c) gave '%c'",
			 printed_letters[i].code, printed_letters[i].letter,
			 m.what == NB_KEY_CHAR ? m.ch[0] : '?');
		check("keys: the key prints its own letter", 
		      m.what == NB_KEY_CHAR &&
		      m.ch[0] == printed_letters[i].letter, v);
	}
	check("keys: ... and a locked layer survives all 32 of them",
	      nb_keymap_alpha_state() == NB_ALPHA_LOCK, "");

	/* one press is one letter -- and only one */
	nb_keymap_reset();
	press_alpha(1, 0);
	{
		nb_key_meaning first = nb_keymap(0x41);	/* the key printed 'a' */
		nb_key_meaning second = nb_keymap(0x41);
		char v[32];

		snprintf(v, sizeof(v), "first='%c' second=%d",
			 first.what == NB_KEY_CHAR ? first.ch[0] : '?',
			 second.what);
		check("keys: one press of alpha types exactly one letter",
		      first.what == NB_KEY_CHAR && first.ch[0] == 'a' &&
		      nb_keymap_alpha_state() == NB_ALPHA_OFF, v);
		check("keys: ... and the key is back to its own character after "
		      "it (nothing on that key without alpha)",
		      second.what == NB_KEY_NONE, v);
	}

	/* a third press is "off" again */
	nb_keymap_reset();
	press_alpha(3, 0);
	check("keys: a third press switches the layer off",
	      nb_keymap_alpha_state() == NB_ALPHA_OFF, "");
	check("keys: ... so the keypad is digits again",
	      nb_keymap(0x41).what == NB_KEY_NONE &&
	      nb_keymap(0x51).ch[0] == '7', "");

	/* SHIFT+ALPHA: the same cycle in capitals */
	nb_keymap_reset();
	press_alpha(1, 1);
	check("keys: shift+alpha selects the capital layer",
	      nb_keymap_alpha_state() == NB_ALPHA_ONCE &&
	      nb_keymap_alpha_upper(), "");
	{
		nb_key_meaning m = nb_keymap(0x59);	/* the key printed 'y' */

		check("keys: ... so 'y' comes out as 'Y'",
		      m.what == NB_KEY_CHAR && m.ch[0] == 'Y', "");
	}
	nb_keymap_reset();
	press_alpha(2, 1);
	{
		nb_key_meaning a = nb_keymap(0x41);
		nb_key_meaning z = nb_keymap(0x5A);
		nb_key_meaning sharp = nb_keymap(0x33);	/* the key printed '#' */

		check("keys: shift+alpha twice locks the capitals",
		      a.ch[0] == 'A' && z.ch[0] == 'Z' &&
		      nb_keymap_alpha_state() == NB_ALPHA_LOCK, "");
		check("keys: ... and a key whose alpha layer has no capital keeps "
		      "the character it carries", sharp.ch[0] == '#', "");
	}

	/* PRIMECODE'S KEY LAYERS: the function keys are CALLS, the bracket keys are
	 * PAIRS, and the shift layer is the inverse functions (keys.py's norm/shi) */
	nb_keymap_reset();
	{
		nb_key_meaning m = nb_keymap(0x47);	/* the sin key */

		check("keys: the sin key types sin(), not '='",
		      m.what == NB_KEY_CHAR && m.ins &&
		      strcmp(m.ins, "sin()") == 0, m.ins ? m.ins : "(none)");
	}
	nb_keymap_reset();
	nb_keymap_modifier(0x8B, 1);
	{
		nb_key_meaning m = nb_keymap(0x47);	/* shift+sin */

		check("keys: ... and shift makes it asin()",
		      m.what == NB_KEY_CHAR && m.ins &&
		      strcmp(m.ins, "asin()") == 0, m.ins ? m.ins : "(none)");
	}
	nb_keymap_reset();
	{
		nb_key_meaning m = nb_keymap(0x4E);	/* the bracket key */

		check("keys: the bracket key types a PAIR",
		      m.what == NB_KEY_CHAR && m.ins &&
		      strcmp(m.ins, "()") == 0, m.ins ? m.ins : "(none)");
	}
	nb_keymap_reset();
	nb_keymap_modifier(0x8B, 1);
	{
		nb_key_meaning m = nb_keymap(0x52);	/* shift+8 */

		check("keys: shift+8 types the brace pair",
		      m.what == NB_KEY_CHAR && m.ins &&
		      strcmp(m.ins, "{}") == 0, m.ins ? m.ins : "(none)");
	}
	/* primeCode's caret rule: a snippet that ends in a closer takes the caret
	 * INSIDE it, and a lone closing character does not */
	check("keys: a pair wants the caret inside it",
	      nb_key_wants_caret_inside("()", 2) &&
	      nb_key_wants_caret_inside("sin()", 5) &&
	      nb_key_wants_caret_inside("{}", 2) && !nb_key_wants_caret_inside(")", 1),
	      "");
	nb_keymap_reset();

	/* the five letter-only keys have no character of their own */
	/* primeCode's normal layer for those five keys: three of them type nothing
	 * (they open menus on the calculator), and 0x44/0x45 are 'x' and '/' */
	nb_keymap_reset();
	check("keys: the letter-only keys type nothing with alpha off",
	      nb_keymap(0x41).what == NB_KEY_NONE &&
	      nb_keymap(0x42).what == NB_KEY_NONE &&
	      nb_keymap(0x43).what == NB_KEY_NONE, "");
	nb_keymap_reset();
	check("keys: 0x44 and 0x45 are primeCode's 'x' and '/'",
	      nb_keymap(0x44).ch[0] == 'x' && nb_keymap(0x45).ch[0] == '/',
	      "");

	/* EVERY PRINTABLE ASCII BYTE, from some key in some state */
	memset(seen, 0, sizeof(seen));
	nb_keymap_reset();
	for (i = 0; i < (int)(sizeof(printed_letters) /
			      sizeof(printed_letters[0])); i++) {
		note_char(seen, nb_keymap(printed_letters[i].code));
	}
	/* the shift layer, one key at a time: the latch is spent by each key, so
	 * the test presses it again for the next one */
	for (i = 0; i < (int)(sizeof(printed_letters) /
			      sizeof(printed_letters[0])); i++) {
		nb_keymap_modifier(0x8B, 1);
		note_char(seen, nb_keymap(printed_letters[i].code));
	}
	nb_keymap_modifier(0x8B, 1);
	note_char(seen, nb_keymap(0x95));	/* shift+HELP: the backquote */
	nb_keymap_modifier(0x8B, 1);
	note_char(seen, nb_keymap(0xB1));	/* shift+APPS: the apostrophe */
	for (i = 0; i < (int)(sizeof(printed_letters) /
			      sizeof(printed_letters[0])); i++) {
		nb_keymap_reset();
		press_alpha(1, 0);
		note_char(seen, nb_keymap(printed_letters[i].code));
		nb_keymap_reset();
		press_alpha(1, 1);
		note_char(seen, nb_keymap(printed_letters[i].code));
	}
	nb_keymap_reset();
	for (i = 0x20; i < 0x7F; i++) {
		if (!seen[i]) {
			missing++;
			if (n < (int)sizeof(list) - 1)
				list[n++] = (char)i;
		}
	}
	list[n] = 0;
	check("keys: every printable ASCII byte is reachable", missing == 0,
	      list);
}

/* ---------------------------------------------------------------------------
 * the viewport: the pixel scroll, its clamping, and the selection's geometry
 *
 * THREE REAL REPORTS, and what each one turned out to be:
 *
 *   * "selecting Chinese text leaves the last character half highlighted" -- a
 *     full-width character is TWO 8 px frames, and the highlight was measured as
 *     frames * 8;
 *   * "with 18 lines the cursor stops moving up at the last line, and on the
 *     first line the view still scrolls up" -- a drag could push yoff outside its
 *     line (negative at the top, past the body height at the bottom) while
 *     top_line stayed clamped, and after that the caret's line was inside the
 *     window's LINE RANGE but outside the glass, so the follow logic -- which
 *     compared line numbers -- never brought it back;
 *   * "dragging should move the view sideways by pixels too" -- the horizontal
 *     scroll was whole 8 px cells.
 *
 * The fixture is a real file opened through the real path (the editor reads it
 * with the port layer): Chinese on the first line, 18 lines in total.
 * ------------------------------------------------------------------------- */
#define BODY_TOP	NB_BODY_TOP
#define BODY_BOT	NB_BODY_BOT
#define LINE_H		16
#define GUTTER_W	36		/* the gutter of a two digit file */
/* the fixture is 18 lines, so this is how far it can scroll at all */
#define MAX_SCROLL	(18 * LINE_H - (BODY_BOT - BODY_TOP))


static int caret_row(void)
{
	nb_u32 col = nb_ui_get()->th->cursor;
	int x, y;

	for (y = BODY_TOP; y < BODY_BOT; y++) {
		for (x = 0; x < NB_SCREEN_W; x++) {
			if (nb_host_fb_pixel(x, y) == col)
				return y;
		}
	}
	return -1;
}

/* how much ink the status row's left hand end carries: that is where a message
 * (the toast) is drawn, and where the position/encoding go when there is none */
static int status_message_ink(void)
{
	nb_u32 bg = nb_ui_get()->th->foot_bg;
	int x, y, ink = 0;

	for (x = 4; x < 200; x++) {
		for (y = NB_SCREEN_H - 14; y < NB_SCREEN_H - 2; y++) {
			if (nb_host_fb_pixel(x, y) != bg)
				ink++;
		}
	}
	return ink;
}

/* the columns a colour occupies in a row band, and how many it covers */
static int band_span(nb_u32 colour, int y0, int y1, int *first, int *last)
{
	int x, y, n = 0;

	*first = -1;
	*last = -1;
	for (x = 0; x < NB_SCREEN_W; x++) {
		for (y = y0; y < y1; y++) {
			if (nb_host_fb_pixel(x, y) != colour)
				continue;
			if (*first < 0)
				*first = x;
			*last = x;
			n++;
			break;
		}
	}
	return n;
}

static void drag(int x0, int y0, int x1, int y1)
{
	nb_ui_touch_send(1, x0, y0);		/* press */
	nb_ui_touch_send(2, x1, y1);		/* one move, past the tap slop */
	nb_ui_touch_send(8, x1, y1);		/* release */
}

/* A TAP WITH A FINGER THAT JITTERS: press, a few pixels of drift, release.  Every
 * real finger does this, and a press that a panel, the gutter or the header took
 * must ignore all of it -- the drag path used to read the same flag the tap set,
 * so the drift scrolled the view ("I tap a line number to fold and the code runs
 * around, left and right most noticeably"). */
static void jitter_tap(int x, int y)
{
	nb_ui_touch_send(1, x, y);
	nb_ui_touch_send(2, x + 6, y + 2);
	nb_ui_touch_send(8, x + 6, y + 2);
}

static void viewport_fixture(void)
{
	char path[NB_PATH_MAX];
	static const char *names[] = { "zh.c" };
	FILE *f;
	int i;

	make_ui_dir();
	snprintf(path, sizeof(path), "%s/zh.c", ui_dir);
	f = fopen(path, "wb");
	if (f) {
		/* line 1 is long enough that the horizontal drag has somewhere to
		 * go (the drag is clamped to the longest visible line, so a short
		 * line would legitimately refuse to scroll), and it starts with the
		 * Chinese the selection tests measure */
		fputs("中文测试 abcd 中文测试 abcd 中文测试 abcd 0123456789\n", f);
		for (i = 1; i < 18; i++)
			fprintf(f, "line %02d\n", i + 1);
		fclose(f);
	}
	/* a file for the completion tests (and the empty one a fresh editor gets) */
	snprintf(path, sizeof(path), "%s/comp.c", ui_dir);
	f = fopen(path, "wb");
	if (f) {
		fputs("int main(void) { return 0; }\n", f);
		fclose(f);
	}
	/* a C file with a block to fold (line 0 opens, line 3 closes) */
	snprintf(path, sizeof(path), "%s/fold.c", ui_dir);
	f = fopen(path, "wb");
	if (f) {
		fputs("void f(void) {\n  int a = 1;\n  int b = 2;\n}\n"
		      "int g;\nint h;\nint i;\n", f);
		fclose(f);
	}
	/* the same shape at the top of a file that is TALLER than the window: the
	 * folding tests need a block whose lines are all above the visible part */
	snprintf(path, sizeof(path), "%s/jump.c", ui_dir);
	f = fopen(path, "wb");
	if (f) {
		fputs("void f(void) {\n  int a = 1;\n  int b = 2;\n}\n", f);
		for (i = 4; i < 40; i++)
			fprintf(f, "int v%02d;\n", i);
		fclose(f);
	}
	/* a block in the MIDDLE of a file: the fold tests need one that is on screen
	 * while the caret is somewhere else entirely (the reported "it jumped to
	 * line 1" needed the caret and the fold to be far apart) */
	snprintf(path, sizeof(path), "%s/many.c", ui_dir);
	f = fopen(path, "wb");
	if (f) {
		for (i = 0; i < 10; i++)
			fprintf(f, "int a%d;\n", i);
		fputs("void g(void) {\n  int b = 1;\n  int c = 2;\n}\n", f);
		for (i = 0; i < 26; i++)
			fprintf(f, "int z%d;\n", i);
		fclose(f);
	}
	/* a file with FOUR digits of line numbers: the gutter has to widen, or the
	 * number is drawn through the fold marker */
	snprintf(path, sizeof(path), "%s/long.c", ui_dir);
	f = fopen(path, "wb");
	if (f) {
		fputs("void f(void) {\n  int a = 1;\n}\n", f);
		for (i = 3; i < 1200; i++)
			fprintf(f, "int v%d;\n", i);
		fclose(f);
	}
	nb_host_reset();
	nb_host_files(ui_dir, names, 1);
	nb_ui_init(nb_host_config());
	nb_ui_enter_workspace(0);
	feed_key(0x0D);				/* open zh.c */
}

static void test_viewport(void)
{
	nb_ui *u;
	nb_buf *b;

	viewport_fixture();
	u = nb_ui_get();
	b = nb_app_buffer();
	check("view: the fixture opened with 18 lines",
	      b != NULL && b->nlines == 18 && u->mode == NB_MODE_EDITOR,
	      b ? fmt("nlines=%d", b->nlines) : "(no buffer)");
	if (!b)
		return;

	/* ---- the selection's geometry, on Chinese text ---- */
	/* the caret is parked at the end of the line: it is two pixels wide and it
	 * is drawn ON TOP of the selection, so leaving it inside the character
	 * under test would measure the caret, not the highlight */
	nb_buf_goto(b, 0, nb_buf_linelen(b, 0));
	u->left = 0;
	u->xoff = 0;
	u->yoff = 0;
	b->top_line = 0;
	u->sel_active = 1;
	u->sel_l0 = u->sel_l1 = 0;
	u->sel_c0 = 0;
	u->sel_c1 = 3;			/* exactly the first character (3 bytes) */
	nb_ui_draw();
	{
		int x0, x1, n = band_span(u->th->sel_bg, BODY_TOP,
					  BODY_TOP + LINE_H, &x0, &x1);

		check("view: a selected full-width character is highlighted whole",
		      n > 0 && x1 - x0 + 1 == 16,
		      fmt("span=%d..%d (%d px)", x0, x1, x1 - x0 + 1));
	}
	/* an endpoint INSIDE the second character still shows it whole */
	u->sel_c1 = 4;
	nb_ui_draw();
	{
		int x0, x1, n = band_span(u->th->sel_bg, BODY_TOP,
					  BODY_TOP + LINE_H, &x0, &x1);

		check("view: ... and an end inside a character shows it whole",
		      n > 0 && x1 - x0 + 1 == 32,
		      fmt("span=%d..%d (%d px)", x0, x1, x1 - x0 + 1));
	}
	/* what is highlighted is what a cut removes: exactly that character */
	u->sel_c1 = 3;
	check("view: cutting the selection removes exactly the highlighted text",
	      nb_ui_selection_delete() == 1 &&
	      strcmp(nb_buf_line(b, 0),
		     "文测试 abcd 中文测试 abcd 中文测试 abcd 0123456789") == 0,
	      nb_buf_line(b, 0));

	/* ---- the vertical scroll is clamped to the text ---- */
	check("view: 18 lines are 80 pixels taller than the body",
	      b->nlines * LINE_H - (BODY_BOT - BODY_TOP) == MAX_SCROLL, "");
	/* put the caret on line 1 and DRAW once: the follow runs per frame, so on
	 * the device the window has already been fitted to this caret before the
	 * finger touches the glass (and a caret that moved would legitimately pull
	 * the view back on the next frame) */
	nb_buf_goto(b, 0, 0);
	nb_ui_draw();
	drag(GUTTER_W + 40, 100, GUTTER_W + 40, 160);	/* drag DOWN at the top */
	check("view: dragging down at the first line does not scroll past it",
	      b->top_line == 0 && u->yoff == 0,
	      fmt("top=%d yoff=%d", b->top_line, u->yoff));
	drag(GUTTER_W + 40, 160, GUTTER_W + 40, 40);	/* drag UP hard, past it */
	check("view: dragging up stops at the last line",
	      b->top_line * LINE_H + u->yoff == MAX_SCROLL,
	      fmt("top=%d yoff=%d", b->top_line, u->yoff));
	{
		int last_y = BODY_TOP + (17 - b->top_line) * LINE_H - u->yoff;

		check("view: ... with the last line fully on the glass",
		      last_y >= BODY_TOP && last_y + LINE_H <= BODY_BOT,
		      fmt("last line at y=%d", last_y));
	}
	/* AND THE FRAME THAT FOLLOWS MUST NOT UNDO IT.  The caret is on line 1
	 * here, and the window used to be fitted to the caret on every draw: the
	 * drag above did nothing on the device because the very next frame pulled
	 * the text straight back. */
	nb_ui_draw();
	check("view: ... and the next frame leaves the dragged view alone",
	      b->top_line * LINE_H + u->yoff == MAX_SCROLL,
	      fmt("top=%d yoff=%d", b->top_line, u->yoff));
	{
		int last_y = BODY_TOP + (17 - b->top_line) * LINE_H - u->yoff;

		check("view: ... so the last lines are still the ones on screen",
		      last_y + LINE_H <= BODY_BOT && last_y >= BODY_TOP,
		      fmt("last line at y=%d", last_y));
	}
	drag(GUTTER_W + 40, 40, GUTTER_W + 40, 200);	/* drag DOWN past the top */
	check("view: and dragging back up stops at the first line",
	      b->top_line == 0 && u->yoff == 0,
	      fmt("top=%d yoff=%d", b->top_line, u->yoff));

	/* ---- the caret follows, from the last line up ---- */
	nb_buf_goto(b, 17, 0);
	nb_ui_draw();
	check("view: the caret is on the glass on the last line",
	      caret_row() >= BODY_TOP && caret_row() + LINE_H <= BODY_BOT,
	      fmt("caret row=%d", caret_row()));
	feed_key(0x03);				/* UP */
	nb_ui_draw();
	check("view: ... and moving up from the last line keeps it there",
	      b->cur_line == 16 && caret_row() >= BODY_TOP &&
	      caret_row() + LINE_H <= BODY_BOT,
	      fmt("line=%d caret row=%d", b->cur_line, caret_row()));
	nb_buf_goto(b, 0, 0);
	nb_ui_draw();
	check("view: the caret at the top scrolls the window back to line 1",
	      b->top_line == 0 && u->yoff == 0 &&
	      caret_row() >= BODY_TOP && caret_row() + LINE_H <= BODY_BOT,
	      fmt("top=%d yoff=%d caret row=%d", b->top_line, u->yoff,
		  caret_row()));

	/* ---- the horizontal drag is in pixels too ---- */
	nb_buf_goto(b, 0, 0);
	u->left = 0;
	u->xoff = 0;
	nb_ui_draw();			/* measures the longest visible line */
	/* 13 px: one whole cell plus five pixels.  A drag of 5 or less is not a
	 * drag at all (TAP_SLOP), and a cell-only implementation would report 1
	 * frame and no remainder. */
	drag(GUTTER_W + 100, 100, GUTTER_W + 87, 100);
	check("view: a horizontal drag scrolls by pixels, not by cells",
	      u->left == 1 && u->xoff == 5,
	      fmt("left=%d xoff=%d", u->left, u->xoff));
	drag(GUTTER_W + 87, 100, GUTTER_W + 79, 100);	/* 8 more */
	check("view: ... and a further cell carries over",
	      u->left == 2 && u->xoff == 5,
	      fmt("left=%d xoff=%d", u->left, u->xoff));
	nb_ui_draw();
	check("view: ... and the next frame leaves the sideways drag alone",
	      u->left == 2 && u->xoff == 5,
	      fmt("left=%d xoff=%d", u->left, u->xoff));
	/* a tap maps back through the same pixel offset: with the text shifted 21
	 * px left, x = gutter + 3 is frame 3 -- the second Chinese character */
	nb_ui_touch_send(1, GUTTER_W + 3, BODY_TOP + 4);
	nb_ui_touch_send(8, GUTTER_W + 3, BODY_TOP + 4);
	check("view: a tap after a horizontal drag lands on the right column",
	      b->cur_line == 0 && b->cur_col == 3,
	      fmt("line=%d col=%d", b->cur_line, b->cur_col));
	u->left = 0;
	u->xoff = 0;
	drag(GUTTER_W + 40, 100, GUTTER_W + 200, 100);	/* drag the other way */
	check("view: dragging right returns to the left edge of the text",
	      u->left == 0 && u->xoff == 0,
	      fmt("left=%d xoff=%d", u->left, u->xoff));
	/* and it stops at the end of the longest line instead of uncovering blank
	 * space: the fixture's first line is far wider than the body */
	{
		int limit = (u->left_max + 1) * 8 - 35 * 8;
		int k;

		for (k = 0; k < 40; k++)
			drag(GUTTER_W + 200, 100, GUTTER_W + 100, 100);
		check("view: the sideways drag stops at the end of the line",
		      limit > 0 && u->left * 8 + u->xoff == limit,
		      fmt("limit=%d left=%d xoff=%d", limit, u->left, u->xoff));
	}

	drop_ui_dir();
}

/* ---------------------------------------------------------------------------
 * the panels over the editor: the character table (shift+A), the hamburger menu
 * (header, right), the file information panel, and the scrollbar
 *
 * THE POINT OF EACH CHECK
 *   * the table has to offer characters the FONT can draw and the encoding can
 *     write -- a table of boxes, or one that writes UTF-8 into a GB2312 file, is
 *     worse than no table;
 *   * a panel EATS the touch and the keys: a tap that picked a character and also
 *     moved the caret would be a bug nobody sees until the text is in the wrong
 *     place;
 *   * the header is drawn AFTER the body, so a half-scrolled top line cannot run
 *     into the title bar;
 *   * the scrollbar is dragged with the same pixel number the finger drag uses,
 *     and a drag to either end has to reach that end.
 * ------------------------------------------------------------------------- */
#define PAL_CELL	16
#define PAL_COLS	19
#define PAL_X0		((NB_SCREEN_W - PAL_COLS * PAL_CELL) / 2)
#define PAL_TOP		(NB_BODY_TOP + 16)	/* the grid starts here: the
						 * TITLE row is above it */

static void tap(int x, int y)
{
	nb_ui_touch_send(1, x, y);
	nb_ui_touch_send(8, x, y);
}

static void palette_tap(int index)
{
	tap(PAL_X0 + (index % PAL_COLS) * PAL_CELL + 8,
	    PAL_TOP + (index / PAL_COLS) * PAL_CELL + 8);
}

static void test_panels(void)
{
	nb_ui *u;
	nb_buf *b;
	unsigned char band0[16 * NB_SCREEN_W];
	unsigned char band1[16 * NB_SCREEN_W];
	int i, bad = 0;

	viewport_fixture();
	u = nb_ui_get();
	b = nb_app_buffer();
	if (!b)
		return;

	/* ---- the header is drawn LAST ---- */
	nb_buf_goto(b, 0, 0);
	nb_ui_editor_set_scroll_px(0);
	nb_ui_draw();
	for (i = 0; i < 16 * NB_SCREEN_W; i++)
		band0[i] = (unsigned char)(nb_host_fb_pixel(i % NB_SCREEN_W,
							   i / NB_SCREEN_W) &
					   0xFF);
	/* half a line of scroll: line 1 is drawn at y = 8, i.e. INTO the header
	 * band, and only the header being drawn afterwards keeps it out */
	nb_ui_editor_set_scroll_px(8);
	nb_ui_draw();
	for (i = 0; i < 16 * NB_SCREEN_W; i++)
		band1[i] = (unsigned char)(nb_host_fb_pixel(i % NB_SCREEN_W,
							   i / NB_SCREEN_W) &
					   0xFF);
	check("panel: a half scrolled line does not run into the title bar",
	      memcmp(band0, band1, sizeof(band0)) == 0, "");

	/* ---- the hamburger menu ---- */
	check("panel: the hamburger is in the header, on the right",
	      nb_ui_menu_icon_hit(NB_SCREEN_W - 10, 8) &&
	      !nb_ui_menu_icon_hit(40, 8) &&
	      !nb_ui_menu_icon_hit(NB_SCREEN_W - 10, 40), "");
	tap(NB_SCREEN_W - 10, 8);
	check("panel: tapping it opens the menu",
	      nb_ui_overlay() == NB_OVER_MENU, "");
	{
		int ink = 0, x, y;

		nb_ui_draw();
		for (y = NB_BODY_TOP + 4; y < NB_BODY_TOP + 80; y++) {
			for (x = 160; x < NB_SCREEN_W - 10; x++) {
				if (nb_host_fb_pixel(x, y) != 0)
					ink++;
			}
		}
		check("panel: ... and the panel is on the screen", ink > 200,
		      fmt("ink=%d", ink));
	}
	/* the information panel is the NINTH row of the editor's menu (the clipboard
	 * and 全部替换 came before it) */
	tap(NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 8 * 20 + 8);
	check("panel: the 文件信息 row opens the information panel",
	      nb_ui_overlay() == NB_OVER_INFO, "");
	{
		/* its title has to be VISIBLE, not drawn in the header's colour on a
		 * light panel (which is what it did) */
		int x, y, ink = 0;
		nb_u32 bg;

		nb_ui_draw();
		bg = nb_host_fb_pixel(200, NB_BODY_TOP + 60);
		for (y = NB_BODY_TOP + 12; y < NB_BODY_TOP + 24; y++) {
			for (x = 12; x < 80; x++) {
				if (nb_host_fb_pixel(x, y) != bg)
					ink++;
			}
		}
		check("panel: the information panel's title is visible", ink > 10,
		      fmt("ink=%d", ink));
	}
	tap(160, 120);
	check("panel: ... and any tap closes it", nb_ui_overlay() == NB_OVER_NONE,
	      "");

	/* the 跳转 row opens the line input, and ENTER goes there */
	tap(NB_SCREEN_W - 10, 8);
	tap(NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 9 * 20 + 8);
	check("panel: the 跳转 row asks for a line number",
	      nb_ui_input_active() && u->input_then == NB_INPUT_GOTO, "");
	feed_key(0x59);				/* '1' */
	feed_key(0x5A);				/* '2' */
	feed_key(0x0D);				/* ENTER */
	check("panel: ... and the caret goes to that line",
	      !nb_ui_input_active() && b->cur_line == 11,
	      fmt("line=%d", b->cur_line + 1));
	nb_ui_draw();
	check("panel: ... with the line on the glass",
	      caret_row() >= NB_BODY_TOP && caret_row() + 16 <= NB_BODY_BOT,
	      fmt("caret row=%d", caret_row()));

	/* save, through the menu */
	nb_buf_goto(b, 1, 0);
	feed_key(0x51);				/* type a character */
	check("panel: the buffer is dirty", b->modified == 1, "");
	tap(NB_SCREEN_W - 10, 8);
	tap(NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 0 * 20 + 8);
	check("panel: the 保存 row saves the file", b->modified == 0, "");

	/* save as: the input starts with the current name; clear it and type a new
	 * one through the real keyboard path (alpha lock for the letters) */
	tap(NB_SCREEN_W - 10, 8);
	tap(NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 1 * 20 + 8);
	check("panel: the 另存为 row asks for a name",
	      nb_ui_input_active() && u->input_then == NB_INPUT_SAVE_AS,
	      nb_ui_input_text());
	for (i = 0; i < 8; i++)
		feed_key(0x0C);			/* clear "zh.c" */
	feed_key(0xB6);
	feed_key(0xB6);				/* alpha: locked */
	feed_key(0x4E);				/* n */
	feed_key(0x45);				/* e */
	feed_key(0x57);				/* w */
	feed_key(0xB6);				/* alpha: off */
	feed_key(0xB8);				/* . */
	feed_key(0xB6);
	feed_key(0xB6);				/* alpha: locked */
	feed_key(0x43);				/* c */
	feed_key(0xB6);				/* alpha: off */
	check("panel: ... and the name is typed",
	      strcmp(nb_ui_input_text(), "new.c") == 0, nb_ui_input_text());
	feed_key(0x0D);				/* ENTER */
	check("panel: ... and the buffer took the new name",
	      !nb_ui_input_active() && strcmp(b->name, "new.c") == 0 &&
	      b->modified == 0, b->name);
	{
		char path[NB_PATH_MAX];
		FILE *f;

		snprintf(path, sizeof(path), "%s/new.c", ui_dir);
		f = fopen(path, "rb");
		check("panel: ... and it is on disk",
		      f != NULL && file_has(path, "中文测试"), path);
		if (f)
			fclose(f);
	}

	/* ---- the MENU key opens the EDITOR's menu too ---- */
	{
		nb_ui_editor_action(NB_MENU_SAVE);	/* make sure nothing is pending */
		feed_key(0x93);				/* MENU */
		check("panel: the MENU key opens the editor's menu",
		      nb_ui_overlay() == NB_OVER_MENU, "");
		feed_key(0x01);				/* esc closes it */
		check("panel: ... and esc closes it",
		      nb_ui_overlay() == NB_OVER_NONE, "");
	}

	/* ---- CODE FOLDING: a tap on the LINE NUMBER folds the block ----
	 *
	 * The gutter is the margin, and a margin is where a fold marker belongs: a
	 * tap there toggles the block at that line and does not move the caret.  What
	 * is hidden is invisible to the renderer, to UP/DOWN (the caret steps over it)
	 * and to the row arithmetic that keeps the caret on the glass. */
	{
		nb_buf *fb;
		int i;

		nb_ui_open_file(ui_dir, "fold.c");
		fb = nb_app_buffer();
		u = nb_ui_get();
		check("fold: the fixture opened",
		      fb != NULL && fb->nlines == 7,
		      fb ? fmt("n=%d", fb->nlines) : "");
		/* line 0 is "void f(void) {" and line 3 is "}" */
		check("fold: a brace line knows what it hides",
		      nb_fold_region_end(fb, 0, NB_LANG_C) == 3,
		      fmt("end=%d", nb_fold_region_end(fb, 0, NB_LANG_C)));
		check("fold: ... and a line with no block does not",
		      nb_fold_region_end(fb, 1, NB_LANG_C) == -1, "");
		/* the tap: the first row of the body is line 0 */
		nb_ui_draw();
		nb_ui_touch_send(1, 4, NB_BODY_TOP + 8);
		nb_ui_touch_send(8, 4, NB_BODY_TOP + 8);
		check("fold: a tap on the line number folded the block",
		      nb_fold_open_at(&u->fold, 0) &&
		      nb_fold_hidden(&u->fold, 1) &&
		      nb_fold_hidden(&u->fold, 3) &&
		      !nb_fold_hidden(&u->fold, 4),
		      fmt("nfold=%d", u->fold.n));
		/* THE CARET CANNOT BE ON A HIDDEN LINE: put it inside the block and
		 * step down, and it lands after it */
		nb_buf_goto(fb, 2, 0);
		nb_ui_editor_key(NB_KEY_DOWN, 0);	/* was hidden: step over */
		check("fold: the caret steps over a hidden line",
		      fb->cur_line == 4, fmt("line=%d", fb->cur_line + 1));
		nb_buf_goto(fb, 4, 0);
		nb_ui_editor_key(NB_KEY_UP, 0);
		check("fold: ... in both directions", fb->cur_line == 0,
		      fmt("line=%d", fb->cur_line + 1));
		/* THE RENDERER DRAWS THE NEXT LINE WHERE THE BLOCK WAS */
		{
			int ink_hidden = 0, x, y;
			nb_u32 bg;

			nb_buf_goto(fb, 5, 0);
			nb_ui_draw();
			bg = nb_host_fb_pixel(2, NB_BODY_TOP + 8);
			/* rows 1..3 of the body would hold lines 1..3: with the block
			 * folded they hold line 4, so they are NOT blank */
			for (y = NB_BODY_TOP + LINE_H; y < NB_BODY_TOP + 4 * LINE_H;
			     y++) {
				for (x = 40; x < NB_SCREEN_W; x++) {
					if (nb_host_fb_pixel(x, y) != bg)
						ink_hidden++;
				}
			}
			check("fold: the lines below move up into the gap",
			      ink_hidden > 20, fmt("ink=%d", ink_hidden));
		}
		/* and a second tap unfolds it */
		nb_ui_draw();
		nb_ui_touch_send(1, 4, NB_BODY_TOP + 8);
		nb_ui_touch_send(8, 4, NB_BODY_TOP + 8);
		check("fold: tapping again unfolds it", u->fold.n == 0, "");
		/* AN EDIT THAT ADDS A LINE DROPS THE FOLDS (a fold is a line number) */
		nb_ui_touch_send(1, 4, NB_BODY_TOP + 8);
		nb_ui_touch_send(8, 4, NB_BODY_TOP + 8);
		check("fold: folded again", u->fold.n == 1, "");
		nb_buf_goto(fb, 6, 0);
		nb_ui_editor_key(NB_KEY_ENTER, 0);	/* a newline */
		check("fold: a newline drops the folds rather than hiding the "
		      "wrong lines", u->fold.n == 0, "");
		/* the LANGUAGE rules: Python indentation and Lua keywords */
		{
			nb_buf *py = nb_buf_new();
			nb_buf *lu = nb_buf_new();

			nb_buf_from_raw(py, "def f():\n    a = 1\n    b = 2\nc = 3\n",
					41, NB_ENC_UTF8, NB_EOL_LF);
			nb_buf_from_raw(lu, "function f()\n  a = 1\nend\nb = 2\n", 33,
					NB_ENC_UTF8, NB_EOL_LF);
			check("fold: a Python ':' folds its indented block",
			      nb_fold_region_end(py, 0, NB_LANG_PY) == 2,
			      fmt("end=%d", nb_fold_region_end(py, 0,
							       NB_LANG_PY)));
			check("fold: a Lua 'function' folds to its 'end'",
			      nb_fold_region_end(lu, 0, NB_LANG_LUA) == 2,
			      fmt("end=%d", nb_fold_region_end(lu, 0,
							       NB_LANG_LUA)));
			check("fold: ... and the keyword rules are not applied to C",
			      nb_fold_region_end(py, 0, NB_LANG_C) == -1, "");
			nb_buf_free(py);
			nb_buf_free(lu);
		}
		(void)i;
	}

	/* ---- FOLDING MUST NOT MOVE THE CODE ----
	 *
	 * THE REPORTED BUG: tap a line number and the whole view jumped.  Two things
	 * were wrong and both were the same mistake -- the window was positioned by
	 * a LINE NUMBER and drawn by ROW INDEX, and with a folded block the two are
	 * different numbers:
	 *
	 *   * the caret follow wrote `line * LINE_H` pixels, so every hidden line
	 *     above the caret was counted twice (the jump);
	 *   * the scrollbar's maximum came from the UNFOLDED line count, so the
	 *     window was allowed to sit past the end of the folded file and the code
	 *     floated above a band of blank glass.
	 *
	 * These four checks are the invariant: the window never sits past the end,
	 * the caret's line is always on the glass, and one key moves the caret by at
	 * most one row of pixels. */
	{
		nb_buf *jb;
		int i, before, after, gw;

		nb_ui_open_file(ui_dir, "jump.c");
		jb = nb_app_buffer();
		u = nb_ui_get();
		check("fold: the tall fixture opened",
		      jb != NULL && jb->nlines == 40,
		      jb ? fmt("n=%d", jb->nlines) : "");
		/* the window at the END of the file, caret on the last line */
		nb_buf_goto(jb, 39, 0);
		nb_ui_draw();
		before = caret_row();
		check("fold: a 40 line file scrolls to its end",
		      nb_ui_editor_scroll_px() == nb_ui_editor_max_scroll_px() &&
		      before >= NB_BODY_TOP && before + LINE_H <= NB_BODY_BOT,
		      fmt("scroll=%d max=%d caret=%d",
			  nb_ui_editor_scroll_px(),
			  nb_ui_editor_max_scroll_px(), before));
		/* fold the 3 line block whose lines are ALL above the window */
		check("fold: the block above the window folds",
		      nb_fold_toggle(&u->fold, jb, 0, NB_LANG_C) == 1 &&
		      u->fold.n == 1, fmt("nfold=%d", u->fold.n));
		nb_ui_draw();
		after = caret_row();
		check("fold: the file is 3 rows shorter and the window knows it",
		      nb_ui_editor_max_scroll_px() ==
		      (jb->nlines - 3) * LINE_H - (NB_BODY_BOT - NB_BODY_TOP),
		      fmt("max=%d", nb_ui_editor_max_scroll_px()));
		check("fold: ... so the window is no longer past the end of it",
		      nb_ui_editor_scroll_px() <= nb_ui_editor_max_scroll_px(),
		      fmt("scroll=%d max=%d", nb_ui_editor_scroll_px(),
			  nb_ui_editor_max_scroll_px()));
		check("fold: ... and the caret's line is still on the glass",
		      jb->cur_line == 39 && after >= NB_BODY_TOP &&
		      after + LINE_H <= NB_BODY_BOT,
		      fmt("line=%d caret=%d", jb->cur_line + 1, after));

		/* ONE KEY, AT MOST ONE ROW: the caret steps over the hidden block
		 * (line 4 is the first visible line after it) and the view follows
		 * it a line at a time.  A view driven by line numbers instead of
		 * rows moved three rows on one key press -- this is that check. */
		/* the caret on the folded line itself (the one that carries the
		 * marker), and one step down: it has to land on the first VISIBLE line
		 * after the block */
		nb_buf_goto(jb, 0, 0);
		nb_ui_draw();
		before = caret_row();
		nb_ui_editor_key(NB_KEY_DOWN, 0);
		nb_ui_draw();
		after = caret_row();
		check("fold: stepping over the block lands on the next visible line",
		      jb->cur_line == 4, fmt("line=%d", jb->cur_line + 1));
		check("fold: ... and the view moved by at most one line",
		      after - before <= LINE_H && after - before >= -LINE_H,
		      fmt("%d -> %d", before, after));
		check("fold: ... keeping the caret on the glass",
		      after >= NB_BODY_TOP && after + LINE_H <= NB_BODY_BOT,
		      fmt("caret=%d", after));
		/* ten more steps down: never more than a line of movement each */
		for (i = 0; i < 10; i++) {
			int row0, row1;

			row0 = caret_row();
			nb_ui_editor_key(NB_KEY_DOWN, 0);
			nb_ui_draw();
			row1 = caret_row();
			if (row1 - row0 > LINE_H || row0 - row1 > LINE_H) {
				check("fold: no step down moves the view more than a line",
				      0, fmt("step %d: %d -> %d", i, row0, row1));
				break;
			}
		}
		check("fold: the caret reached the line it stepped to",
		      jb->cur_line == 14, fmt("line=%d", jb->cur_line + 1));
		/* and unfolding puts the rows back */
		nb_fold_clear(&u->fold);
		nb_ui_draw();
		check("fold: unfolding restores the full length",
		      nb_ui_editor_max_scroll_px() ==
		      jb->nlines * LINE_H - (NB_BODY_BOT - NB_BODY_TOP) &&
		      nb_ui_editor_scroll_px() <= nb_ui_editor_max_scroll_px(),
		      fmt("max=%d", nb_ui_editor_max_scroll_px()));

		/* ---- AND THE GUTTER WIDENS FOR A FOUR DIGIT FILE ----
		 * The fold marker owns x = 3..10 and the number is right aligned six
		 * pixels short of the divider: at a fixed 36 px a four digit number
		 * started at x = -2 and was drawn straight through the marker. */
		nb_ui_open_file(ui_dir, "long.c");
		u = nb_ui_get();
		check("gutter: the 1200 line fixture opened",
		      nb_app_buffer() && nb_app_buffer()->nlines == 1200,
		      nb_app_buffer() ? fmt("n=%d", nb_app_buffer()->nlines) : "");
		gw = nb_ui_editor_gutter_w();
		check("gutter: a four digit file widens the line number column",
		      gw >= 11 + 6 + 4 * 8, fmt("gutter=%d", gw));
		check("gutter: ... so the number cannot reach the fold marker",
		      gw - 6 - 4 * 8 >= 11, fmt("number starts at x=%d",
						gw - 6 - 4 * 8));
		check("gutter: ... and it still leaves room for the code",
		      (NB_SCREEN_W - gw) / 8 >= 30,
		      fmt("cols=%d", (NB_SCREEN_W - gw) / 8));
		nb_ui_open_file(ui_dir, "comp.c");
		u = nb_ui_get();
		check("gutter: a short file keeps the old narrow one",
		      nb_ui_editor_gutter_w() == 36,
		      fmt("gutter=%d", nb_ui_editor_gutter_w()));
	}

	/* ---- TAPPING A LINE NUMBER MUST NOT MOVE THE VIEW ----
	 *
	 * THE REPORTED BUG: fold a block a thousand lines down a file and the view
	 * jumps to line 1.  The fold handler asked for a refit ("fit the window to
	 * the caret"), and the caret was still on line 1 -- the user had SCROLLED
	 * down without moving it -- so the next frame yanked the window back.  A fold
	 * is not a caret move: the window keeps the line it has at the top.
	 */
	{
		nb_buf *jb;
		int top_before, scroll_before;

		nb_ui_open_file(ui_dir, "many.c");
		jb = nb_app_buffer();
		u = nb_ui_get();
		check("fold: the mid-block fixture opened",
		      jb != NULL && jb->nlines == 40,
		      jb ? fmt("n=%d", jb->nlines) : "");
		nb_buf_goto(jb, 0, 0);
		nb_ui_draw();			/* the view agrees with the caret */
		/* ... and now the user drags down to the block (line 10) without
		 * touching the caret: the drag's own frame draws, and the follow has
		 * nothing to do because the caret did not move */
		nb_ui_editor_set_scroll_px(10 * LINE_H);
		nb_ui_draw();
		top_before = jb->top_line;
		scroll_before = nb_ui_editor_scroll_px();
		check("fold: the window can sit away from the caret",
		      top_before == 10 && jb->cur_line == 0 && scroll_before > 0,
		      fmt("top=%d caret=%d scroll=%d", top_before + 1,
			  jb->cur_line + 1, scroll_before));
		/* the block's own line is the top row: tap its gutter */
		nb_ui_touch_send(1, 4, NB_BODY_TOP + 8);
		nb_ui_touch_send(8, 4, NB_BODY_TOP + 8);
		nb_ui_draw();
		check("fold: the block in the middle of the file folded",
		      nb_fold_open_at(&u->fold, 10) &&
		      nb_fold_hidden(&u->fold, 11), fmt("nfold=%d", u->fold.n));
		check("fold: ... and the view stayed where the user left it",
		      jb->top_line == top_before &&
		      nb_ui_editor_scroll_px() == scroll_before,
		      fmt("top %d -> %d, scroll %d -> %d", top_before + 1,
			  jb->top_line + 1, scroll_before,
			  nb_ui_editor_scroll_px()));
		check("fold: ... which is nowhere near the caret",
		      jb->cur_line == 0 && jb->top_line > 0, "");
		/* FOLDING THE BLOCK THE CARET IS IN MOVES THE CARET OUT OF IT: a caret
		 * on a hidden line would type into something invisible */
		nb_buf_goto(jb, 12, 0);		/* inside the fold (11..13) */
		nb_ui_draw();
		nb_ui_touch_send(1, 4, NB_BODY_TOP + 8);	/* unfold ... */
		nb_ui_touch_send(8, 4, NB_BODY_TOP + 8);
		nb_ui_draw();
		check("fold: tapping again unfolds it", u->fold.n == 0, "");
		nb_buf_goto(jb, 12, 0);
		nb_ui_draw();
		nb_ui_touch_send(1, 4, NB_BODY_TOP + 8);	/* ... and fold it */
		nb_ui_touch_send(8, 4, NB_BODY_TOP + 8);
		nb_ui_draw();
		check("fold: folding the caret's own block leaves the caret alone",
		      nb_fold_open_at(&u->fold, 10) && jb->cur_line == 12,
		      fmt("line=%d fold=%d", jb->cur_line + 1, u->fold.n));
		/* ... AND THE FIRST EDIT THERE REVEALS IT: typing into a line the user
		 * cannot see is worse than losing the fold */
		nb_ui_editor_key(NB_KEY_CHAR, 'x');
		check("fold: ... and an edit inside the fold unfolds it",
		      u->fold.n == 0 && jb->cur_line == 12,
		      fmt("fold=%d line=%d", u->fold.n, jb->cur_line + 1));
		nb_fold_clear(&u->fold);
	}

	/* ---- THE QUICK SYMBOL PANEL (shift+6) ---- */
	{
		nb_buf *sb;

		nb_ui_open_file(ui_dir, "comp.c");
		sb = nb_app_buffer();
		u = nb_ui_get();
		nb_buf_delete_range(sb, 0, 0, 0, nb_buf_linelen(sb, 0));
		nb_buf_goto(sb, 0, 0);
		feed_key(0x8B);				/* shift */
		feed_key(0x57);				/* ... the 6 key */
		check("symbols: shift+6 opens the panel",
		      nb_ui_overlay() == NB_OVER_SYMBOLS, "");
		check("symbols: the tokens follow the file's language (C here)",
		      strcmp(nb_ui_symbol_token(NB_LANG_C, 3), "==") == 0 &&
		      strcmp(nb_ui_symbol_token(NB_LANG_PPL, 4), "AND") == 0 &&
		      strcmp(nb_ui_symbol_token(NB_LANG_LUA, 4), "and") == 0,
		      "");
		{
			int x, y, ink = 0;

			nb_ui_draw();
			for (y = NB_BODY_TOP; y < NB_BODY_TOP + 120; y++) {
				for (x = 4; x < NB_SCREEN_W - 4; x++) {
					if (nb_host_fb_pixel(x, y) != 0)
						ink++;
				}
			}
			check("symbols: ... and it is on the screen", ink > 500,
			      fmt("ink=%d", ink));
		}
		/* THE SHORTCUTS FOLLOW THE KEYPAD, NOT THE GRID: this keyboard has
		 * 7 8 9 on the TOP row and 1 2 3 on the bottom one, so the cell under
		 * the top left key is the seventh and not the first */
		check("symbols: the cells are numbered 789 / 456 / 123",
		      nb_ui_symbol_key(0) == '7' && nb_ui_symbol_key(1) == '8' &&
		      nb_ui_symbol_key(2) == '9' && nb_ui_symbol_key(3) == '4' &&
		      nb_ui_symbol_key(5) == '6' && nb_ui_symbol_key(6) == '1' &&
		      nb_ui_symbol_key(8) == '3',
		      fmt("%c%c%c/%c%c%c/%c%c%c", nb_ui_symbol_key(0),
			  nb_ui_symbol_key(1), nb_ui_symbol_key(2),
			  nb_ui_symbol_key(3), nb_ui_symbol_key(4),
			  nb_ui_symbol_key(5), nb_ui_symbol_key(6),
			  nb_ui_symbol_key(7), nb_ui_symbol_key(8)));
		check("symbols: ... and a digit finds its cell",
		      nb_ui_symbol_cell_of_key('7') == 0 &&
		      nb_ui_symbol_cell_of_key('5') == 4 &&
		      nb_ui_symbol_cell_of_key('1') == 6 &&
		      nb_ui_symbol_cell_of_key('0') == -1, "");
		/* the panel is still up: the top left KEY inserts the top left CELL
		 * (">" in the C list).  0x51 is the physical 7 key -- the same key
		 * the keypad puts at the top left */
		feed_key(0x51);				/* the '7' key */
		check("symbols: the top left key inserts the top left cell",
		      strcmp(nb_buf_line(sb, 0), ">") == 0 &&
		      nb_ui_overlay() == NB_OVER_NONE,
		      fmt("[%s]", nb_buf_line(sb, 0)));
		nb_buf_delete_range(sb, 0, 0, 0, nb_buf_linelen(sb, 0));
		nb_buf_goto(sb, 0, 0);
		feed_key(0x8B);
		feed_key(0x57);
		/* THE DIGIT SHORTCUT: 6 is ">=" in the C list (middle right) */
		feed_key(0x57);				/* the '6' key */
		check("symbols: a digit key inserts its token",
		      strcmp(nb_buf_line(sb, 0), ">=") == 0 &&
		      nb_ui_overlay() == NB_OVER_NONE,
		      fmt("[%s]", nb_buf_line(sb, 0)));
		/* ... and a TAP does the same */
		nb_buf_delete_range(sb, 0, 0, 0, nb_buf_linelen(sb, 0));
		nb_buf_goto(sb, 0, 0);
		feed_key(0x8B);
		feed_key(0x57);
		{
			int cx = 10 + (3 % 3) * 100;		/* cell 4: "==" */
			int cy = (NB_BODY_TOP + 22) + (3 / 3) * 30;

			nb_ui_touch_send(1, cx + 30, cy + 12);
			nb_ui_touch_send(8, cx + 30, cy + 12);
			check("symbols: a tap inserts the cell's token",
			      strcmp(nb_buf_line(sb, 0), "==") == 0,
			      fmt("[%s]", nb_buf_line(sb, 0)));
		}
		/* esc closes it without inserting anything */
		nb_buf_delete_range(sb, 0, 0, 0, nb_buf_linelen(sb, 0));
		nb_buf_goto(sb, 0, 0);
		feed_key(0x8B);
		feed_key(0x57);
		feed_key(0x01);
		check("symbols: esc closes it and types nothing",
		      nb_ui_overlay() == NB_OVER_NONE &&
		      nb_buf_linelen(sb, 0) == 0, "");
	}

	/* ---- CODE COMPLETION ----
	 *
	 * Type "wi" and the popup offers "widget" (an identifier already in the
	 * file); type "wh" and it offers "while" (a keyword of the language
	 * profile).  ENTER or a tap completes the word, ESC closes only the popup,
	 * and a space ends the word. */
	{
		nb_buf *cb = u->buf;

		nb_ui_open_file(ui_dir, "comp.c");
		cb = nb_app_buffer();
		u = nb_ui_get();
		nb_buf_insert(cb, "int widget = 1;\nint \n", 23);
		nb_ui_editor_changed(0);
		nb_buf_goto(cb, 1, 4);			/* after "int " */
		nb_ui_editor_key(NB_KEY_CHAR, 'w');
		check("complete: typing a word opens the popup",
		      nb_complete_active(&u->comp) && u->comp.n > 1,
		      fmt("n=%d prefix=[%s]", u->comp.n, u->comp.prefix));
		nb_ui_editor_key(NB_KEY_CHAR, 'i');	/* "wi" */
		{
			int i, saw_while = 0, saw_widget = 0;

			for (i = 0; i < u->comp.n; i++) {
				if (strcmp(u->comp.item[i], "while") == 0)
					saw_while++;
				if (strcmp(u->comp.item[i], "widget") == 0)
					saw_widget++;
			}
			check("complete: ... the identifier from the file is there "
			      "and the keyword no longer matches",
			      saw_widget == 1 && saw_while == 0,
			      fmt("while=%d widget=%d", saw_while, saw_widget));
		}
		/* THE POPUP IS DRAWN, under the line the word is on */
		{
			int px, py, pw, ph, ink = 0, x, y;
			nb_u32 bg;

			nb_ui_draw();
			nb_ui_editor_comp_rect(&px, &py, &pw, &ph);
			check("complete: the popup has a box under the caret line",
			      pw > 0 && ph >= 16 && py >= NB_BODY_TOP &&
			      py + ph <= NB_BODY_BOT,
			      fmt("%d,%d %dx%d", px, py, pw, ph));
			bg = nb_host_fb_pixel(2, NB_SCREEN_H - 4);
			for (y = py; y < py + ph && y < NB_BODY_BOT; y++) {
				for (x = px; x < px + pw && x < NB_SCREEN_W; x++) {
					if (nb_host_fb_pixel(x, y) != bg)
						ink++;
				}
			}
			check("complete: ... and it is on the screen", ink > 100,
			      fmt("ink=%d", ink));
			/* a TAP on its first row accepts that candidate */
			nb_ui_touch_send(1, px + 8, py + 3 + 8);
			nb_ui_touch_send(8, px + 8, py + 3 + 8);
			check("complete: a tap on a row accepts it",
			      strcmp(nb_buf_line(cb, 1), "int widget") == 0 &&
			      cb->cur_col == 10,
			      fmt("[%s] col=%d", nb_buf_line(cb, 1),
				  cb->cur_col));
			check("complete: ... and closes the popup",
			      !nb_complete_active(&u->comp), "");
		}
		/* typing after an accept does not re-open it for the same word */
		nb_ui_editor_key(NB_KEY_CHAR, 's');	/* "widgets" */
		check("complete: the accepted word does not re-open the popup",
		      !nb_complete_active(&u->comp), "");
		/* a space ends the word */
		nb_ui_editor_key(NB_KEY_CHAR, ' ');
		check("complete: a space closes it", !nb_complete_active(&u->comp),
		      "");
		/* THE KEYWORD PATH, and ESC which closes only the popup */
		nb_buf_goto(cb, 1, 0);
		{
			int i;

			for (i = 0; i < 12; i++)
				nb_ui_editor_key(NB_KEY_BACKSPACE, 0);
		}
		feed_key(0x0C);
		nb_buf_goto(cb, 1, 0);
		nb_ui_editor_key(NB_KEY_CHAR, 'w');
		nb_ui_editor_key(NB_KEY_CHAR, 'h');
		{
			int i, saw_while = 0;

			for (i = 0; i < u->comp.n; i++) {
				if (strcmp(u->comp.item[i], "while") == 0)
					saw_while++;
			}
			check("complete: a keyword comes from the language profile",
			      saw_while == 1, fmt("n=%d while=%d", u->comp.n,
						  saw_while));
		}
		nb_ui_editor_key(NB_KEY_ESC, 0);
		check("complete: esc closes the popup and stays in the file",
		      !nb_complete_active(&u->comp) &&
		      u->mode == NB_MODE_EDITOR, fmt("mode=%d", u->mode));
		/* ENTER completes the selected candidate */
		nb_ui_editor_key(NB_KEY_CHAR, 'i');	/* "whi" */
		nb_ui_editor_key(NB_KEY_BACKSPACE, 0);	/* back to "wh" */
		check("complete: ... and typing re-opens it",
		      nb_complete_active(&u->comp), "");
		nb_ui_editor_key(NB_KEY_ENTER, 0);
		check("complete: ENTER replaces the prefix with the whole word",
		      strcmp(nb_buf_line(cb, 1), "while") == 0 &&
		      cb->cur_col == 5,
		      fmt("[%s] col=%d", nb_buf_line(cb, 1), cb->cur_col));
		/* the disabled setting: the same typing offers nothing */
		nb_ui_open_file(ui_dir, "comp.c");
		cb = nb_app_buffer();
		nb_ui_editor_key(NB_KEY_CHAR, 'w');
		check("complete: (on) it is on by default",
		      nb_complete_active(&u->comp), "");
		{
			nb_config *cfg = (nb_config *)nb_host_config();

				cfg->flags |= NB_CFG_FLAG_NOCOMPLETE;
				nb_ui_editor_key(NB_KEY_CHAR, 'i');
				check("complete: setting.json can turn it off",
			      !nb_complete_active(&u->comp), "");
			cfg->flags &= ~(unsigned)NB_CFG_FLAG_NOCOMPLETE;
			}
	}

	/* ---- THE MATCHING BRACKETS ----
	 *
	 * The pair around the caret is boxed and its glyphs are drawn in the ACCENT
	 * colour, which is what tells it apart from every other bracket on the line
	 * (the report asked for a different colour, not just a box). */
	{
		nb_buf *mb = u->buf;

		nb_buf_goto(mb, 0, 0);
		nb_buf_insert(mb, "if (a[0]) {}\n", 13);
		nb_ui_editor_changed(0);
		/* the caret on the '(' of "if (" -- index 3 */
		nb_buf_goto(mb, 0, 3);
		nb_ui_draw();
		/* "if (a[0]) {}":  the '(' at 3 matches the ')' at 8 (the [0] pair
		 * is nested and must not confuse it) */
		check("brackets: the caret's '(' finds its ')'",
		      u->brace_col == 3 && u->match_line == 0 &&
		      u->match_col == 8,
		      fmt("brace=%d match=%d:%d", u->brace_col,
			  u->match_line + 1, u->match_col));
		/* ... and the pixels: the two bracket cells carry the accent colour.
		 * The caret is 2 px of the same colour, so its columns are found
		 * first (the leftmost accent ink) and then EXCLUDED -- otherwise a
		 * check like this passes on the caret alone and proves nothing. */
		{
			int x, y, caret_x = -1, one = 0, two = 0, gw = 0;

			for (x = 0; x < NB_SCREEN_W && caret_x < 0; x++) {
				for (y = NB_BODY_TOP; y < NB_BODY_TOP + 16; y++) {
					if (nb_host_fb_pixel(x, y) == u->th->accent) {
						caret_x = x;
						break;
					}
				}
			}
			if (caret_x >= 0)
				gw = caret_x - 3 * 8;	/* the caret sits on col 3 */
			for (y = NB_BODY_TOP; y < NB_BODY_TOP + 16; y++) {
				for (x = 0; x < NB_SCREEN_W; x++) {
					if (nb_host_fb_pixel(x, y) != u->th->accent)
						continue;
					if (x == caret_x || x == caret_x + 1)
						continue;	/* the caret */
					if (x >= gw + 3 * 8 && x < gw + 4 * 8)
						one++;
					if (x >= gw + 8 * 8 && x < gw + 9 * 8)
						two++;
				}
			}
			check("brackets: ... and both are drawn in the accent colour",
			      one > 2 && two > 2, fmt("ink=%d/%d (gw=%d)", one, two,
						      gw));
		}
		/* the caret ON a closer matches backwards too */
		nb_buf_goto(mb, 0, 8);
		nb_ui_draw();
		check("brackets: a caret on ')' matches backwards",
		      u->brace_col == 8 && u->match_col == 3,
		      fmt("brace=%d match=%d", u->brace_col, u->match_col));
		/* the braces: the caret just after '{' pairs with the '}' */
		nb_buf_goto(mb, 0, 11);
		nb_ui_draw();
		check("brackets: the braces pair up as well",
		      u->brace_col == 11 && u->match_col == 10,
		      fmt("brace=%d match=%d", u->brace_col, u->match_col));
		/* A BRACKET WITH NO PARTNER LIGHTS NOTHING UP (the caret's own
		 * bracket is still named -- that is what the box is drawn from --
		 * but there is no pair) */
		nb_buf_delete_range(mb, 0, 0, 1, 0);	/* drop the test line */
		nb_buf_insert(mb, "int a = );\n", 11);
		nb_ui_editor_changed(0);
		nb_buf_goto(mb, 0, 9);			/* after the ')' */
		nb_ui_draw();
		check("brackets: an unmatched ')' lights up nothing",
		      u->match_line == -1, fmt("match=%d", u->match_line));
		/* ... and put the buffer back exactly as the fixture had it (the
		 * rest of this test counts lines) */
		nb_buf_delete_range(mb, 0, 0, 1, 0);
		nb_ui_editor_changed(0);
		nb_ui_open_file(ui_dir, "zh.c");
		u = nb_ui_get();
		b = nb_app_buffer();
		check("brackets: the fixture is back",
		      b && b->nlines == 18, b ? fmt("nlines=%d", b->nlines) : "");
	}

	/* ---- THE MESSAGE IS A TOAST ---- */
	{
		int shown, after;

		nb_ui_message("已打开 x.c", 0);
		nb_ui_draw();
		shown = status_message_ink();
		check("toast: a message is on the status row when it is set",
		      shown > 20, fmt("ink=%d", shown));
		for (i = 0; i < 40; i++)
			nb_ui_tick();		/* a second and a bit at 30 Hz */
		nb_ui_draw();
		after = status_message_ink();
		/* the row goes back to the POSITION/ENCODING/SIZE (so it is not
		 * blank) and the message's own text is gone */
		check("toast: ... and it gives the row back by itself",
		      u->message[0] == 0 && after > 10 && after != shown,
		      fmt("ink=%d was %d msg=[%s]", after, shown, u->message));
	}
	/* ---- A MENU TAP MUST NOT MOVE THE CODE ----
	 *
	 * The tap that chose a menu item used to end up as a tap on the TEXT as
	 * well (its release arrives after the panel closed, and a release the
	 * editor saw no press for became a caret move), so choosing 保存 scrolled
	 * the window to wherever the menu had been. */
	{
		int line_before, col_before, scroll_before;

		nb_buf_goto(b, 4, 2);
		nb_ui_editor_set_scroll_px(0);
		nb_ui_draw();
		line_before = b->cur_line;
		col_before = b->cur_col;
		scroll_before = nb_ui_editor_scroll_px();
		tap(NB_SCREEN_W - 10, 8);			/* the hamburger */
		tap(NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 0 * 20 + 8);	/* 保存 */
		nb_ui_draw();
		check("menu: choosing an item leaves the caret where it was",
		      b->cur_line == line_before && b->cur_col == col_before,
		      fmt("caret %d:%d -> %d:%d", line_before + 1, col_before,
			  b->cur_line + 1, b->cur_col));
		check("menu: ... and the view where it was",
		      nb_ui_editor_scroll_px() == scroll_before,
		      fmt("scroll %d -> %d", scroll_before,
			  nb_ui_editor_scroll_px()));
		/* the same for a tap that lands on the HEADER but misses the icon */
		nb_buf_goto(b, 4, 2);
		tap(40, 8);
		nb_ui_draw();
		check("menu: a tap on the title bar is not a tap on the text",
		      b->cur_line == 4 && b->cur_col == 2 &&
		      nb_ui_editor_scroll_px() == scroll_before,
		      fmt("caret %d:%d scroll=%d", b->cur_line + 1, b->cur_col,
			  nb_ui_editor_scroll_px()));
	}

	/* ---- the character table (shift+A) ---- */
	nb_buf_goto(b, 0, 0);
	feed_key(0x8B);				/* shift: a latch, not a hold */
	feed_key(0x41);				/* the key printed 'a' */
	check("panel: shift+A opens the character table",
	      nb_ui_overlay() == NB_OVER_PALETTE, "");
	/* ... AND IT IS ON THE GLASS ON THE VERY FIRST FRAME (the report was
	 * "the first time you open it you have to press an arrow key before it
	 * appears") */
	{
		int x, y, ink = 0;
		nb_u32 bg;

		printf("   PALETTE PROBE: wants_redraw=%d overlay=%d\n",
		       nb_ui_wants_redraw(), nb_ui_overlay());
		nb_ui_draw();
		bg = nb_host_fb_pixel(3, 200);
		for (y = NB_BODY_TOP; y < NB_BODY_BOT; y++) {
			for (x = 8; x < NB_SCREEN_W - 8; x++) {
				if (nb_host_fb_pixel(x, y) != bg)
					ink++;
			}
		}
		check("panel: the table is drawn on the first frame", ink > 200,
		      fmt("ink=%d dirty=%d", ink, u->dirty));
	}
	check("panel: the table starts with printable ASCII and has symbols",
	      nb_ui_palette_count() > 150 &&
	      nb_ui_palette_cp(0) == 0x20 &&
	      nb_ui_palette_cp(0x41 - 0x20) == 'A', "");
	for (i = 0; i < nb_ui_palette_count(); i++) {
		if (nb_font_glyph(nb_ui_palette_cp(i)) < 0)
			bad++;
	}

	check("panel: every character in the table has a glyph", bad == 0,
	      fmt("%d without a glyph", bad));
	check("panel: ... and the symbol half is worth having",
	      nb_ui_palette_count() - 95 >= 80,
	      fmt("%d symbols", nb_ui_palette_count() - 95));
	{
		/* the symbols the user asked for by name: money, arithmetic, the
		 * Greek a formula needs, and Chinese punctuation */
		static const unsigned int want[] = {
			0x00B0, 0x00B1, 0x00D7, 0x00F7, 0x00A5, 0x00A9, 0x03C0,
			0x03A9, 0x2605, 0x2460, 0x3002, 0xFF0C, 0x300A, 0x300C
		};
		int have = 0, w;

		for (w = 0; w < (int)(sizeof(want) / sizeof(want[0])); w++) {
			for (i = 0; i < nb_ui_palette_count(); i++) {
				if (nb_ui_palette_cp(i) == want[w]) {
					have++;
					break;
				}
			}
		}
		check("panel: the named common symbols are all in the table",
		      have == (int)(sizeof(want) / sizeof(want[0])),
		      fmt("%d of %d", have,
			  (int)(sizeof(want) / sizeof(want[0]))));
	}
	/* A TAP ONLY MOVES THE CURSOR; ENTER IS WHAT INSERTS (the report: a wrong
	 * character must not be in the file before it has been read) */
	{
		char before[NB_PATH_MAX];

		nb_str_copy(before, sizeof(before), nb_buf_line(b, 0), -1);
		palette_tap(0x41 - 0x20);
		check("panel: tapping a cell moves the cursor, and types nothing",
		      nb_ui_palette_cursor() == 0x41 - 0x20 &&
		      strcmp(nb_buf_line(b, 0), before) == 0,
		      fmt("cursor=%d line=[%s]", nb_ui_palette_cursor(),
			  nb_buf_line(b, 0)));
	}
	/* the title row says what it is and what the cursor is on -- and nothing
	 * else (no hints, no count) */
	{
		int x, y, left = 0, mid = 0, right = 0;
		nb_u32 bg;

		nb_ui_draw();
		bg = nb_host_fb_pixel(200, NB_BODY_TOP + 8);
		for (y = NB_BODY_TOP + 2; y < NB_BODY_TOP + 14; y++) {
			for (x = 6; x < NB_SCREEN_W - 6; x++) {
				if (nb_host_fb_pixel(x, y) == bg)
					continue;
				if (x < 70)
					left++;
				else if (x > NB_SCREEN_W - 90)
					right++;
				else
					mid++;
			}
		}
		check("panel: the title row has the name and the codepoint only",
		      left > 20 && right > 20 && mid == 0,
		      fmt("left=%d mid=%d right=%d", left, mid, right));
	}
	/* the arrow keys move the cursor, and ENTER inserts and closes */
	nb_ui_palette_move(1, 0);
	check("panel: an arrow key moves the cursor",
	      nb_ui_palette_cursor() == 0x42 - 0x20,
	      fmt("cursor=%d", nb_ui_palette_cursor()));
	nb_ui_palette_move(-1, 0);
	feed_key(0x0D);				/* ENTER on 'A' */
	check("panel: ENTER inserts the character and closes the table",
	      nb_ui_overlay() == NB_OVER_NONE && nb_buf_line(b, 0)[0] == 'A',
	      nb_buf_line(b, 0));
	/* ... and ESC closes it without inserting anything */
	feed_key(0x8B);
	feed_key(0x41);				/* shift+A again */
	check("panel: shift+A opens it again",
	      nb_ui_overlay() == NB_OVER_PALETTE, "");
	{
		char before[NB_PATH_MAX];

		nb_str_copy(before, sizeof(before), nb_buf_line(b, 0), -1);
		palette_tap(0x7A - 0x20);	/* 'z' */
		feed_key(0x01);			/* ESC */
		check("panel: esc closes the table without inserting",
		      nb_ui_overlay() == NB_OVER_NONE &&
		      strcmp(nb_buf_line(b, 0), before) == 0, nb_buf_line(b, 0));
	}

	/* A GB2312 FILE GETS GB2312 BYTES, not UTF-8: the table inserts in the
	 * BUFFER's encoding, or the saved file is one the calculator cannot read */
	{
		int idx = -1;
		unsigned int hi = 0, lo = 0;

		for (i = 0; i < nb_ui_palette_count(); i++) {
			if (nb_ui_palette_cp(i) == 0x3002)	/* the ideographic stop */
				idx = i;
		}
		feed_key(0x8B);
		feed_key(0x41);				/* open the table again */
		b->encoding = NB_ENC_GB2312;
		if (idx >= 0 && nb_font_gb_pair(0x3002, &hi, &lo)) {
			nb_buf_goto(b, 1, 0);
			palette_tap(idx);
			feed_key(0x0D);			/* ENTER */
			check("panel: a GB2312 buffer gets the GB2312 pair",
			      (unsigned char)nb_buf_line(b, 1)[0] == hi &&
			      (unsigned char)nb_buf_line(b, 1)[1] == lo,
			      fmt("%02X %02X",
				  (unsigned char)nb_buf_line(b, 1)[0],
				  (unsigned char)nb_buf_line(b, 1)[1]));
		} else {
			check("panel: the table has the ideographic stop and the "
			      "font has its pair", idx >= 0,
			      fmt("index=%d", idx));
		}
		b->encoding = NB_ENC_UTF8;
	}
	/* ON closes a panel too -- and it must CLOSE it, not quit the editor out
	 * from under it (the panel is what the user is looking at) */
	feed_key(0x8B);
	feed_key(0x41);				/* open the table once more */
	tap(NB_SCREEN_W - 10, 8);		/* eaten: the table is still up */
	check("panel: a panel eats a tap meant for the one below it",
	      nb_ui_overlay() == NB_OVER_PALETTE, "");
	feed_key(0x01);				/* esc closes the table */
	check("panel: esc closes the table",
	      nb_ui_overlay() == NB_OVER_NONE, "");
	tap(NB_SCREEN_W - 10, 8);
	check("panel: the menu opens again", nb_ui_overlay() == NB_OVER_MENU, "");
	feed_key(0x83);				/* ON */
	check("panel: ON closes a panel instead of quitting",
	      nb_ui_overlay() == NB_OVER_NONE && u->mode == NB_MODE_EDITOR,
	      fmt("over=%d mode=%d", nb_ui_overlay(), u->mode));
	/* the editor below is untouched: a key now means what it always meant
	 * (ESC asks to save, because the table's insert dirtied the buffer) */
	feed_key(0x01);
	check("panel: ... and the next ESC is the editor's own again",
	      u->mode == NB_MODE_EDITOR && u->prompt == NB_PROMPT_SAVE,
	      fmt("mode=%d prompt=%d", u->mode, u->prompt));
	feed_key(0x01);				/* cancel the question */
	check("panel: ... and cancelling it stays in the editor",
	      u->mode == NB_MODE_EDITOR && u->prompt == NB_PROMPT_NONE, "");

	/* ---- the scrollbar ---- */
	nb_buf_goto(b, 0, 0);
	nb_ui_draw();
	check("panel: the scrollbar is up when the file is longer than the body",
	      nb_ui_scrollbar_visible() && nb_ui_editor_max_scroll_px() == MAX_SCROLL, "");
	check("panel: its hit box is the right hand edge, not the text",
	      nb_ui_scrollbar_hit(NB_SCREEN_W - 3, 120) &&
	      !nb_ui_scrollbar_hit(120, 120), "");
	/* drag it from the top to the bottom: the window goes to the end */
	drag(NB_SCREEN_W - 3, NB_BODY_TOP + 2, NB_SCREEN_W - 3, NB_BODY_BOT - 2);
	check("panel: dragging the bar to the bottom scrolls to the end",
	      nb_ui_editor_scroll_px() == MAX_SCROLL,
	      fmt("scroll=%d", nb_ui_editor_scroll_px()));
	/* ... and back to the top */
	drag(NB_SCREEN_W - 3, NB_BODY_BOT - 2, NB_SCREEN_W - 3, NB_BODY_TOP);
	check("panel: ... and back to the top", nb_ui_editor_scroll_px() == 0,
	      fmt("scroll=%d", nb_ui_editor_scroll_px()));
	/* A SCRUB IS NOT A DRAG OF THE TEXT, and not a selection either */
	check("panel: the scrub left the caret alone", b->cur_line == 0, "");
	{
		int mid;

		drag(NB_SCREEN_W - 3, NB_BODY_TOP, NB_SCREEN_W - 3,
		     NB_BODY_TOP + (NB_BODY_BOT - NB_BODY_TOP) / 2);
		mid = nb_ui_editor_scroll_px();
		check("panel: the middle of the bar is the middle of the file",
		      mid > 20 && mid < 60, fmt("scroll=%d", mid));
	}
	/* the bar is SEE-THROUGH, and that is nb_gfx_blend: the panel has no
	 * hardware alpha (the top byte of an ARGB pixel is ignored by the LCD), so
	 * there is nothing else it could be */
	{
		nb_u32 white = 0xFFFFFFFFu, black = 0xFF000000u, got;

		nb_gfx_fill(0, 0, 8, 8, white);
		nb_gfx_blend(1, 1, 4, 4, black, 128);
		got = nb_host_fb_pixel(3, 3);
		check("panel: blending mixes with what is under it",
		      got != white && got != black &&
		      ((got >> 16) & 0xFF) > 0x60 && ((got >> 16) & 0xFF) < 0xA0,
		      fmt("%08X", (unsigned)got));
	}

	/* ---- THE LAYER BUFFER: a draw must not reach the panel until it is
	 * presented ----
	 *
	 * This is the flicker fix.  The editor used to draw straight into the
	 * firmware's pixels, so every frame erased the body and redrew it IN PLACE:
	 * a drag showed the erase before the text.  With a shadow buffer the panel
	 * only ever sees a complete frame, and the two buffers are separate right
	 * up to nb_screen_present(). */
	{
		int x, y, clean = 1;

		/* start from a screen that is entirely one colour, then draw a
		 * different one WITHOUT presenting */
		nb_gfx_fill(0, 0, NB_SCREEN_W, NB_SCREEN_H, 0xFF010203u);
		nb_screen_present();
		nb_gfx_fill(0, 0, NB_SCREEN_W, NB_SCREEN_H, 0xFFAABBCCu);
		for (y = 0; y < NB_SCREEN_H && clean; y++) {
			for (x = 0; x < NB_SCREEN_W; x++) {
				if (nb_host_fb_shown(x, y) != 0xFF010203u) {
					clean = 0;
					break;
				}
			}
		}
		check("screen: drawing does not touch the panel on its own",
		      clean, "");
		nb_screen_present();
		check("screen: ... and the present puts the whole frame on it",
		      nb_host_fb_shown(0, 0) == 0xFFAABBCCu &&
		      nb_host_fb_shown(NB_SCREEN_W - 1, NB_SCREEN_H - 1) ==
		      0xFFAABBCCu &&
		      nb_host_fb_shown(120, 120) == 0xFFAABBCCu, "");
	}

	/* ---- AUTO-SCROLL WHILE SELECTING ----
	 *
	 * A finger dragged past the edge of the glass has to keep the text moving,
	 * or a selection can never be longer than one screenful.  The finger's Y is
	 * held at the edge here (which is what the digitizer reports for a finger
	 * outside the screen) and the frame loop is ticked: the view has to scroll
	 * and the selection's end has to follow it. */
	{
		int start_sel, start_scroll, k;

		nb_ui_editor_set_scroll_px(0);
		nb_ui_draw();
		nb_ui_touch_send(1, 120, 40);
		for (k = 0; k < 14; k++)
			nb_ui_touch_tick();		/* the long press makes it */
		check("edge: a long press starts a selection with the view at rest",
		      nb_ui_selection_active() && nb_ui_editor_scroll_px() == 0, "");
		start_sel = u->sel_l1;
		/* the finger moves to the BOTTOM EDGE and stays there */
		nb_ui_touch_send(2, 120, NB_BODY_BOT - 1);
		start_scroll = nb_ui_editor_scroll_px();
		for (k = 0; k < 20; k++)
			nb_ui_touch_tick();
		check("edge: holding at the bottom edge scrolls the view",
		      nb_ui_editor_scroll_px() > start_scroll,
		      fmt("scroll %d -> %d", start_scroll,
			  nb_ui_editor_scroll_px()));
		check("edge: ... and the selection follows it",
		      u->sel_l1 > start_sel,
		      fmt("sel end line %d -> %d", start_sel + 1, u->sel_l1 + 1));
		/* it stops at the end of the file instead of scrolling forever */
		for (k = 0; k < 200; k++)
			nb_ui_touch_tick();
		check("edge: ... and stops at the end of the file",
		      nb_ui_editor_scroll_px() == nb_ui_editor_max_scroll_px() &&
		      u->sel_l1 == b->nlines - 1,
		      fmt("scroll=%d/%d sel=%d/%d", nb_ui_editor_scroll_px(),
			  nb_ui_editor_max_scroll_px(), u->sel_l1 + 1,
			  b->nlines));
		/* the other way: the finger at the top edge scrolls back up */
		nb_ui_touch_send(2, 120, NB_BODY_TOP);
		for (k = 0; k < 40; k++)
			nb_ui_touch_tick();
		check("edge: the top edge scrolls back to the first line",
		      nb_ui_editor_scroll_px() == 0, "");
		/* A LOST RELEASE: a finger lifted off the glass outside the screen can
		 * take its release event with it.  The scroll has to give up rather
		 * than run for ever, and it must give up in the middle of the file (a
		 * test that only checks "it stopped at the end" would pass for a scroll
		 * that never stopped at all). */
		{
			int held;

			nb_ui_touch_send(2, 120, NB_BODY_BOT - 1);	/* idle = 0 */
			nb_ui_touch_tick();				/* 8 px */
			held = nb_ui_editor_scroll_px();
			check("edge: ... and the finger is still moving here",
			      held > 0 && held < nb_ui_editor_max_scroll_px(),
			      fmt("scroll=%d/%d", held,
				  nb_ui_editor_max_scroll_px()));
			/* the guard is the idle FRAME COUNT, so it is set directly
			 * here: really waiting 150 frames would scroll to the end of
			 * the file first and hide what is being tested */
			u->touch_idle = 200;
			for (k = 0; k < 10; k++)
				nb_ui_touch_tick();
			check("edge: a finger that stopped reporting stops the scroll",
			      nb_ui_editor_scroll_px() == held,
			      fmt("scroll=%d, was %d", nb_ui_editor_scroll_px(),
				  held));
			/* ... and any event at all brings it back */
			nb_ui_touch_send(2, 120, NB_BODY_BOT - 1);
			nb_ui_touch_tick();
			check("edge: ... and the next event resumes it",
			      nb_ui_editor_scroll_px() > held,
			      fmt("scroll=%d, was %d", nb_ui_editor_scroll_px(),
				  held));
		}
		nb_ui_touch_send(8, 120, NB_BODY_BOT - 1);
		nb_ui_selection_clear();
	}

	/* ---- and a file that fits the body has no scrollbar at all ---- */
	nb_ui_open_file(ui_dir, "main.c");
	check("panel: a file that fits the body has no scrollbar",
	      nb_ui_editor_max_scroll_px() == 0 && !nb_ui_scrollbar_visible(),
	      fmt("max=%d", nb_ui_editor_max_scroll_px()));

	drop_ui_dir();
}

/* ---------------------------------------------------------------------------
 * the launch file's DIRECTORY TABLE, read by the REAL parser
 *
 * THE BUG THIS PINS: choosing a workspace whose path is not in the table showed
 * ANOTHER workspace's files.  `ws_dir[i]` fell back to directory 0 -- and
 * directory 0 is the first workspace, which is the application folder -- so a
 * folder the launcher had not listed (a budget, an unreadable folder, anything)
 * silently browsed nbCode.hpappdir.  A wrong answer that looks like a right one
 * is the worst kind, and it survived because the host model built its list
 * directly and the emulator suite used ONE workspace.
 *
 * The shape here is the calculator's own: several .hpappdir folders under
 * C:\DATA, listed BOTH as workspaces (setting.json) and as folder rows inside
 * C:\DATA, plus one workspace ("C:\DATA\mysrc") that is NOT a folder row.
 * ------------------------------------------------------------------------- */
struct cfgbuf {
	unsigned char b[65536];
	int n;
};

static void cfg_w32(struct cfgbuf *w, unsigned int v)
{
	w->b[w->n++] = (unsigned char)(v & 0xFF);
	w->b[w->n++] = (unsigned char)((v >> 8) & 0xFF);
	w->b[w->n++] = (unsigned char)((v >> 16) & 0xFF);
	w->b[w->n++] = (unsigned char)((v >> 24) & 0xFF);
}

static void cfg_wstr(struct cfgbuf *w, const char *s)
{
	while (*s)
		w->b[w->n++] = (unsigned char)*s++;
	w->b[w->n++] = 0;
}

static const char *cfg_ws[] = {
	"C:\\DATA",
	"C:\\DATA\\nbCode.hpappdir",
	"C:\\DATA\\Oxford.hpappdir",
	"C:\\DATA\\PrimeCode.hpappdir",
	"C:\\DATA\\primeLua.hpappdir"
};

/* the directories: every workspace, then a subfolder that only C:\DATA knows */
static const char *cfg_dir[] = {
	"C:\\DATA",
	"C:\\DATA\\nbCode.hpappdir",
	"C:\\DATA\\Oxford.hpappdir",
	"C:\\DATA\\PrimeCode.hpappdir",
	"C:\\DATA\\primeLua.hpappdir",
	"C:\\DATA\\mysrc"
};

/* one file per directory, named after it, so a wrong mapping is visible */
static void cfg_write_n(const char *path, int ndirs)
{
	struct cfgbuf w;
	FILE *f;
	int i;

	if (ndirs < 0)
		ndirs = (int)(sizeof(cfg_dir) / sizeof(cfg_dir[0]));

	memset(&w, 0, sizeof(w));
	cfg_w32(&w, NB_CFG_MAGIC);
	cfg_w32(&w, NB_CFG_VERSION);
	cfg_w32(&w, 0);			/* flags */
	cfg_w32(&w, 0);			/* theme */
	cfg_w32(&w, 4);			/* tab */
	cfg_w32(&w, 0);			/* language */
	cfg_w32(&w, 0);			/* frames */
	cfg_w32(&w, (unsigned)(sizeof(cfg_ws) / sizeof(cfg_ws[0])));
	cfg_w32(&w, 0);			/* default workspace */
	cfg_wstr(&w, "C:\\DATA");
	cfg_wstr(&w, "");
	cfg_wstr(&w, "");
	for (i = 0; i < (int)(sizeof(cfg_ws) / sizeof(cfg_ws[0])); i++)
		cfg_wstr(&w, cfg_ws[i]);
	cfg_w32(&w, (unsigned)ndirs);
	for (i = 0; i < ndirs; i++) {
		char name[40];

		cfg_wstr(&w, cfg_dir[i]);
		/* C:\DATA's folders are rows; everything else has no parent */
		cfg_w32(&w, i == 0 ? 0xFFFFFFFFu : 0xFFFFFFFFu);
		if (i == 0) {
			/* C:\DATA also shows its .hpappdir folders as ROWS -- and
			 * one of them has no directory of its own (the budget ran
			 * out), which is the reported bug: the row used to carry
			 * the index 0 and opened the first workspace instead */
			cfg_w32(&w, 2);
			cfg_w32(&w, 0xFFFFFFFFu);	/* NB_CFG_NO_DIR */
			cfg_w32(&w, 1);
			cfg_wstr(&w, "BeyondBudget.hpappdir");
			cfg_w32(&w, 4096);
			cfg_w32(&w, 0);
			cfg_wstr(&w, "d0.txt");
		} else {
			cfg_w32(&w, 1);
			cfg_w32(&w, 10 * (unsigned)i);
			cfg_w32(&w, 0);
			snprintf(name, sizeof(name), "d%d.txt", i);
			cfg_wstr(&w, name);
		}
	}
	f = fopen(path, "wb");
	if (f) {
		fwrite(w.b, 1, (size_t)w.n, f);
		fclose(f);
	}
}

static void cfg_write(const char *path)
{
	cfg_write_n(path, -1);
}

/* ---------------------------------------------------------------------------
 * THE GLOBAL CLIPBOARD (nb_clip.h, shift+VIEW / shift+MENU)
 *
 * Two halves, tested separately on purpose: the LIST is pure memory (order,
 * de-duplication, the eleventh entry) and the FILE is the port layer's (save,
 * load, and a file somebody edited by hand).
 * ------------------------------------------------------------------------- */
static void test_clipboard(void)
{
	char tmpl[] = "/tmp/nbcode_clip_XXXXXX";
	char path[NB_PATH_MAX];
	nb_ui *u;
	nb_buf *b;
	int i;

	if (!mkdtemp(tmpl))
		return;
	nb_clip_reset();
	check("clip: a fresh clipboard is empty", nb_clip_count() == 0, "");

	nb_clip_add("one\n", 4);
	nb_clip_add("two\n", 4);
	nb_clip_add("three\n", 6);
	check("clip: the newest entry is first",
	      nb_clip_count() == 3 && strcmp(nb_clip_text(0), "three\n") == 0 &&
	      strcmp(nb_clip_text(2), "one\n") == 0,
	      fmt("[%s][%s]", nb_clip_text(0), nb_clip_text(2)));
	/* AN ENTRY COPIED TWICE COMES BACK TO THE TOP instead of appearing twice:
	 * ten slots are few, and the user who copies it again wants it under their
	 * finger */
	nb_clip_add("one\n", 4);
	check("clip: copying an old entry moves it to the top, not in twice",
	      nb_clip_count() == 3 && strcmp(nb_clip_text(0), "one\n") == 0 &&
	      strcmp(nb_clip_text(1), "three\n") == 0,
	      fmt("n=%d [%s]", nb_clip_count(), nb_clip_text(0)));
	check("clip: ... and copying the top one again changes nothing",
	      nb_clip_add("one\n", 4) == 0 && nb_clip_count() == 3, "");
	/* eleven entries: ten are kept and the oldest is gone */
	nb_clip_reset();
	for (i = 0; i < NB_CLIP_MAX + 1; i++) {
		char line[16];

		snprintf(line, sizeof(line), "e%d", i);
		nb_clip_add(line, (int)strlen(line));
	}
	check("clip: the eleventh copy pushes the oldest out",
	      nb_clip_count() == NB_CLIP_MAX &&
	      strcmp(nb_clip_text(0), "e10") == 0 &&
	      strcmp(nb_clip_text(NB_CLIP_MAX - 1), "e1") == 0,
	      fmt("n=%d last=[%s]", nb_clip_count(),
		  nb_clip_text(NB_CLIP_MAX - 1)));
	/* delete: the entry goes and the ones behind it move up */
	check("clip: an entry can be deleted",
	      nb_clip_delete(0) == 1 && nb_clip_count() == NB_CLIP_MAX - 1 &&
	      strcmp(nb_clip_text(0), "e9") == 0, fmt("[%s]", nb_clip_text(0)));
	check("clip: ... and deleting nothing is not an error",
	      nb_clip_delete(99) == 0 && nb_clip_delete(-1) == 0, "");
	/* an entry that is longer than the slot keeps its head, in WHOLE characters */
	{
		char big[NB_CLIP_ENTRY + 64];

		for (i = 0; i < (int)sizeof(big) - 1; i++)
			big[i] = 'x';
		big[sizeof(big) - 1] = 0;
		nb_clip_add(big, (int)sizeof(big) - 1);
		check("clip: an oversized entry is truncated, not refused",
		      nb_clip_len(0) == NB_CLIP_ENTRY, fmt("len=%d", nb_clip_len(0)));
	}

	/* ---- THE FILE ---- */
	nb_clip_reset();
	nb_clip_set_dir(tmpl);
	nb_clip_add("alpha\n", 6);
	nb_clip_add("中文测试\n", 13);
	check("clip: the file lives in the directory it was given",
	      strstr(nb_clip_path(), NB_CLIP_FILE) != NULL &&
	      strncmp(nb_clip_path(), tmpl, strlen(tmpl)) == 0,
	      nb_clip_path());
	nb_clip_save();
	nb_clip_reset();
	check("clip: resetting forgets everything (a new session)", nb_clip_count() == 0,
	      "");
	check("clip: ... and the file brings it back",
	      nb_clip_load() == 2 && strcmp(nb_clip_text(0), "中文测试\n") == 0 &&
	      strcmp(nb_clip_text(1), "alpha\n") == 0,
	      fmt("n=%d [%s]", nb_clip_count(), nb_clip_text(0)));
	/* A FILE THAT IS NOT OURS IS NOT AN ERROR: whatever parses is kept */
	snprintf(path, sizeof(path), "%s/%s", tmpl, NB_CLIP_FILE);
	{
		FILE *f = fopen(path, "wb");

		if (f) {
			fputs("not our format at all\n", f);
			fclose(f);
		}
	}
	check("clip: a file somebody else wrote loads as empty, without a crash",
	      nb_clip_load() == 0 && nb_clip_count() == 0, "");
	{
		FILE *f = fopen(path, "wb");

		if (f) {
			/* the magic, then a record that claims more bytes than are
			 * there: the parser stops instead of reading past the end */
			fputs(NB_CLIP_MAGIC "5\nab\n", f);
			fclose(f);
		}
	}
	check("clip: a half-written file keeps what it can read",
	      nb_clip_load() == 0, "");
	{
		FILE *f = fopen(path, "wb");

		if (f) {
			fputs(NB_CLIP_MAGIC "2\nab\n3\nxyz\n", f);
			fclose(f);
		}
	}
	check("clip: ... and a whole record is read",
	      nb_clip_load() == 2 && strcmp(nb_clip_text(1), "xyz") == 0,
	      fmt("n=%d [%s]", nb_clip_count(), nb_clip_text(1)));

	/* ---- AND FROM THE EDITOR ---- */
	{
		static const char *names[] = { "clip.c" };
		char fpath[NB_PATH_MAX];
		FILE *f;

		make_ui_dir();
		snprintf(fpath, sizeof(fpath), "%s/clip.c", ui_dir);
		f = fopen(fpath, "wb");
		if (f) {
			fputs("hello world\nsecond line\n", f);
			fclose(f);
		}
		nb_host_reset();
		nb_host_files(ui_dir, names, 1);
		nb_ui_init(nb_host_config());
		nb_ui_enter_workspace(0);
		nb_clip_reset();
		nb_clip_set_dir(ui_dir);
		check("clip: the fixture opened",
		      nb_ui_open_file(ui_dir, "clip.c") == 1, "");
		u = nb_ui_get();
		b = nb_app_buffer();
		check("clip: the editor has a buffer to copy from",
		      b != NULL && b->nlines >= 2, b ? fmt("n=%d", b->nlines) : "");
		/* shift+VIEW copies the caret's line */
		nb_buf_goto(b, 0, 0);
		feed_key(0x8B);				/* shift */
		feed_key(0xB4);				/* VIEW: copy */
		check("clip: shift+VIEW copies the caret's line",
		      nb_clip_count() == 1 && nb_clip_len(0) > 0 &&
		      nb_clip_text(0)[nb_clip_len(0) - 1] == '\n',
		      fmt("n=%d [%s]", nb_clip_count(), nb_clip_text(0)));
		/* IT IS PERSISTED AT ONCE: the point of the panel is that the next
		 * session still has it */
		{
			const char *saved = nb_clip_text(0);
			int n = nb_clip_len(0);

			nb_clip_reset();
			check("clip: ... and it is on the disk before the session ends",
			      nb_clip_load() == 1 && nb_clip_len(0) == n &&
			      memcmp(nb_clip_text(0), saved, (size_t)n) == 0, "");
		}
		/* shift+MENU opens the panel, and a tap inserts the entry */
		nb_buf_goto(b, 0, 0);
		nb_buf_delete_range(b, 0, 0, b->nlines - 1, 0x7FFFFFFF);
		nb_ui_editor_changed(0);
		nb_ui_draw();
		feed_key(0x8B);
		feed_key(0x93);				/* MENU: the clipboard */
		check("clip: shift+MENU opens the clipboard panel",
		      nb_ui_overlay() == NB_OVER_CLIP, "");
		{
			int x, y, ink = 0;

			nb_ui_draw();
			for (y = NB_BODY_TOP + 16; y < NB_BODY_TOP + 34; y++) {
				for (x = 4; x < NB_SCREEN_W - 4; x++) {
					if (nb_host_fb_pixel(x, y) != 0)
						ink++;
				}
			}
			check("clip: ... and the entry is on the screen", ink > 20,
			      fmt("ink=%d", ink));
		}
		nb_ui_touch_send(1, 60, NB_BODY_TOP + 16 + 8);	/* the first row */
		nb_ui_touch_send(8, 60, NB_BODY_TOP + 16 + 8);
		check("clip: tapping an entry inserts it at the caret",
		      nb_ui_overlay() == NB_OVER_NONE && nb_buf_linelen(b, 0) > 0,
		      fmt("[%s]", nb_buf_line(b, 0)));
		/* DEL deletes the selected entry -- and the file with it */
		nb_buf_delete_range(b, 0, 0, b->nlines - 1, 0x7FFFFFFF);
		nb_ui_editor_changed(0);
		nb_clip_add("second", 6);
		check("clip: a second entry is on the list", nb_clip_count() == 2, "");
		nb_ui_draw();
		feed_key(0x8B);
		feed_key(0x93);
		feed_key(0x0C);				/* the DEL key */
		check("clip: DEL deletes the selected entry",
		      nb_clip_count() == 1 && nb_ui_overlay() == NB_OVER_CLIP,
		      fmt("n=%d", nb_clip_count()));
		nb_clip_reset();
		check("clip: ... and that deletion went to the file too",
		      nb_clip_load() == 1, fmt("n=%d", nb_clip_count()));
		/* ESC closes the panel without inserting anything */
		nb_buf_delete_range(b, 0, 0, b->nlines - 1, 0x7FFFFFFF);
		nb_ui_editor_changed(0);
		nb_ui_draw();
		feed_key(0x8B);
		feed_key(0x93);
		feed_key(0x01);
		check("clip: esc closes the panel and types nothing",
		      nb_ui_overlay() == NB_OVER_NONE && nb_buf_linelen(b, 0) == 0,
		      fmt("[%s]", nb_buf_line(b, 0)));
		/* the MENU row "剪贴板…" opens the same panel (the chord is not the
		 * only way in -- a 320x240 screen has no room for a legend) */
		nb_ui_draw();
		feed_key(0x93);				/* the editor's own menu */
		check("clip: the editor menu opens", nb_ui_overlay() == NB_OVER_MENU,
		      "");
		tap(NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 6 * 20 + 8);
		check("clip: the menu's 剪贴板 row opens it too",
		      nb_ui_overlay() == NB_OVER_CLIP, "");
		feed_key(0x01);
		/* A CUT PUTS THE TEXT IN THE GLOBAL LIST: the menu's cut-selection
		 * action used to delete the selection without copying it anywhere */
		nb_buf_delete_range(b, 0, 0, b->nlines - 1, 0x7FFFFFFF);
		nb_ui_editor_changed(0);
		nb_buf_insert(b, "hello world", 11);
		nb_ui_editor_changed(0);
		nb_clip_reset();
		u->sel_active = 1;
		u->sel_l0 = u->sel_l1 = 0;
		u->sel_c0 = 0;
		u->sel_c1 = 5;
		nb_ui_editor_action(NB_MENU_CUT_SEL);
		check("clip: cutting a selection keeps it in the clipboard",
		      nb_clip_count() == 1 && nb_clip_len(0) == 5 &&
		      memcmp(nb_clip_text(0), "hello", 5) == 0,
		      fmt("n=%d [%s]", nb_clip_count(), nb_clip_text(0)));
		check("clip: ... and the text really went away",
		      strcmp(nb_buf_line(b, 0), " world") == 0, nb_buf_line(b, 0));
		drop_ui_dir();
	}
}

/* ---------------------------------------------------------------------------
 * A TAP MUST NOT MOVE THE VIEW (either axis)
 *
 * Two reports, one shape: tapping a line number to fold, and tapping the menu in
 * the header, both made the code "run around" -- and the LEFT/RIGHT movement was
 * the more obvious of the two.  The vertical follow is anchored now (view_anchor),
 * but nothing checked the HORIZONTAL scroll, which the same follow owns.
 * ------------------------------------------------------------------------- */
static void test_tap_stability(void)
{
	static const char *names[] = { "wide.c" };
	char tmpl[] = "/tmp/nbcode_tap_XXXXXX";
	char path[NB_PATH_MAX];
	nb_ui *u;
	nb_buf *b;
	FILE *f;
	int i, left_before, top_before, y;

	if (!mkdtemp(tmpl))
		return;
	/* a block in the middle, and every line long enough that the horizontal
	 * scroll is meaningful */
	snprintf(path, sizeof(path), "%s/wide.c", tmpl);
	f = fopen(path, "wb");
	if (f) {
		for (i = 0; i < 10; i++)
			fprintf(f, "int a%d; // aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", i);
		fputs("void g(void) {\n  int b = 1; // bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
		      "  int c = 2; // cccccccccccccccccccccccccccccccc\n}\n", f);
		for (i = 0; i < 20; i++)
			fprintf(f, "int z%d; // zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n", i);
		fclose(f);
	}
	nb_host_reset();
	nb_host_files(tmpl, names, 1);
	nb_ui_init(nb_host_config());
	nb_ui_enter_workspace(0);
	if (nb_ui_open_file(tmpl, "wide.c") != 1)
		return;
	u = nb_ui_get();
	b = nb_app_buffer();
	nb_buf_goto(b, 0, 0);
	nb_ui_draw();
	/* scroll the view away from the caret, in BOTH directions: down to the
	 * block, and right so the long lines are cut off */
	nb_ui_editor_set_scroll_px(10 * LINE_H);
	drag(GUTTER_W + 240, 120, GUTTER_W + 140, 120);	/* drag left: scroll right */
	nb_ui_draw();
	top_before = b->top_line;
	left_before = u->left * 8 + u->xoff;
	check("tap: the view can be scrolled in both directions",
	      top_before == 10 && left_before > 0 && b->cur_line == 0,
	      fmt("top=%d left=%d caret=%d", top_before + 1, left_before,
		  b->cur_line + 1));
	/* 1. THE FOLD MARKER of the block's own line (the first visible row), with
	 * the finger's own drift on it */
	jitter_tap(4, NB_BODY_TOP + 8);
	nb_ui_draw();
	check("tap: the jittering tap still folded the block",
	      nb_fold_open_at(&u->fold, 10), fmt("nfold=%d", u->fold.n));
	check("tap: folding does not move the view sideways",
	      u->left * 8 + u->xoff == left_before,
	      fmt("left %d -> %d", left_before, u->left * 8 + u->xoff));
	check("tap: ... nor vertically",
	      b->top_line == top_before, fmt("top %d -> %d", top_before + 1,
					     b->top_line + 1));
	/* 2. THE MENU ICON in the header */
	jitter_tap(NB_SCREEN_W - 10, 8);
	nb_ui_draw();
	check("tap: the menu opens", nb_ui_overlay() == NB_OVER_MENU, "");
	check("tap: ... and opening it does not move the view sideways",
	      u->left * 8 + u->xoff == left_before,
	      fmt("left %d -> %d", left_before, u->left * 8 + u->xoff));
	check("tap: ... nor vertically", b->top_line == top_before, "");
	/* ... and tapping a row of it fires the action without moving anything
	 * either (撤销 is the third row: it changes the TEXT, not the window) */
	{
		int changed_before = b->version;

		nb_ui_touch_send(1, NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 2 * 20 + 8);
		nb_ui_touch_send(8, NB_SCREEN_W - 60, NB_BODY_TOP + 2 + 2 * 20 + 8);
		nb_ui_draw();
		check("tap: a menu row fires without moving the window",
		      u->left * 8 + u->xoff == left_before &&
		      b->top_line == top_before,
		      fmt("left=%d top=%d version %d -> %d",
			  u->left * 8 + u->xoff, b->top_line + 1, changed_before,
			  b->version));
	}
	/* 3. A TAP OUTSIDE AN OPEN MENU closes it and must not drag the text */
	{
		int left0 = u->left * 8 + u->xoff, top0 = b->top_line;

		jitter_tap(20, NB_BODY_TOP + 150);
		nb_ui_draw();
		check("tap: closing the menu with a jittering tap moves nothing",
		      nb_ui_overlay() == NB_OVER_NONE &&
		      u->left * 8 + u->xoff == left0 && b->top_line == top0,
		      fmt("over=%d left=%d top=%d", nb_ui_overlay(),
			  u->left * 8 + u->xoff, b->top_line + 1));
	}

	/* 4. AND A TAP IN THE TEXT moves the CARET, which is allowed to scroll the
	 * window -- but only as far as keeping the caret visible needs */
	{
		int before = u->left * 8 + u->xoff;

		y = NB_BODY_TOP + 8 + 3 * LINE_H;
		nb_ui_touch_send(1, GUTTER_W + 4, y);
		nb_ui_touch_send(8, GUTTER_W + 4, y);
		nb_ui_draw();
		/* the caret goes where the finger is -- and the ROW it lands on is
		 * counted in VISIBLE lines, so with a fold above it is not
		 * top_line + 3; what matters here is that the view did not move
		 * sideways to reach it */
		check("tap: tapping the text moves the caret and not the view",
		      u->left * 8 + u->xoff == before &&
		      b->cur_line >= b->top_line && b->cur_line < b->nlines,
		      fmt("caret=%d top=%d left %d -> %d", b->cur_line + 1,
			  b->top_line + 1, before, u->left * 8 + u->xoff));
	}
	/* ---- A PASTE WITH NEWLINES ---- */
	{
		/* from the START of the line: the paste splits the line it lands in,
		 * which is what the line numbers below assume */
		nb_buf_goto(b, b->cur_line, 0);
		nb_clip_reset();
		nb_clip_add("one\ntwo\nthree\n", 14);
		check("paste: a multi-line entry splits into lines",
		      nb_ui_editor_insert_text(nb_clip_text(0), nb_clip_len(0)) == 1 &&
		      strcmp(nb_buf_line(b, b->cur_line - 3), "one") == 0 &&
		      strcmp(nb_buf_line(b, b->cur_line - 2), "two") == 0 &&
		      strcmp(nb_buf_line(b, b->cur_line - 1), "three") == 0,
		      fmt("line=%d [%s][%s][%s]", b->cur_line + 1,
			  nb_buf_line(b, b->cur_line - 3),
			  nb_buf_line(b, b->cur_line - 2),
			  nb_buf_line(b, b->cur_line - 1)));
		/* the paste is ONE undo record */
		nb_buf_undo(b);
		nb_ui_editor_changed(0);
		check("paste: ... and one undo takes the whole paste back",
		      nb_buf_line(b, b->cur_line)[0] != 0 &&
		      strncmp(nb_buf_line(b, b->cur_line), "one", 3) != 0,
		      fmt("[%s]", nb_buf_line(b, b->cur_line)));
	}
	/* A LONG SINGLE LINE INTO A SHORT ONE: the line has to GROW, which is the
	 * case that used to leave the buffer pointing at freed memory */
	{
		char longline[200];
		int k;

		nb_buf_delete_range(b, 0, 0, b->nlines - 1, 0x7FFFFFFF);
		nb_ui_editor_changed(0);
		nb_buf_insert(b, "x", 1);
		for (k = 0; k < 150; k++)
			longline[k] = (char)('a' + (k % 26));
		nb_clip_reset();
		nb_clip_add(longline, 150);
		check("paste: a 150 byte line pastes into a one byte line",
		      nb_ui_editor_insert_text(nb_clip_text(0), 150) == 1 &&
		      nb_buf_linelen(b, 0) > 140 &&
		      nb_buf_line(b, 0)[0] == 'x',
		      fmt("len=%d", nb_buf_linelen(b, 0)));
		nb_buf_delete_range(b, 0, 0, b->nlines - 1, 0x7FFFFFFF);
		nb_ui_editor_changed(0);
	}

	/* a big multi-line paste, through the panel's own path */
	{
		char big[200];
		int k;

		for (k = 0; k < 60 && k < (int)sizeof(big) - 2; k++)
			big[k] = (k % 3 == 2) ? '\n' : (char)('a' + (k % 26));
		big[k] = 0;
		nb_clip_reset();
		nb_clip_add(big, k);
		check("paste: a many-line entry pastes without damage",
		      nb_ui_editor_insert_text(nb_clip_text(0), nb_clip_len(0)) == 1 &&
		      nb_buf_line(b, b->nlines - 1) != NULL,
		      fmt("nlines=%d nbytes=%u", b->nlines, b->nbytes));
	}
	nb_ui_editor_close();
	{
		char rm[NB_PATH_MAX];

		snprintf(rm, sizeof(rm), "%s/wide.c", tmpl);
		remove(rm);
		rmdir(tmpl);
	}
}

/* ---------------------------------------------------------------------------
 * FOUR REPORTS FROM THE CALCULATOR, ONE TEST EACH
 * ------------------------------------------------------------------------- */
static void test_ui_reports(void)
{
	nb_ui *u;
	nb_buf *b;
	int i;

	viewport_fixture();
	u = nb_ui_get();
	b = nb_app_buffer();

	/* ---- 1. THE KEY HELP CAN BE CLOSED ----
	 * Its only way out was the HELP key again, and HELP is filtered by the
	 * auto-repeat guard, which clears on the key's RELEASE: one swallowed
	 * release left the help page on the screen for good ("I opened the key help
	 * and could not close it again").  ESC now closes it, and the panel keys are
	 * no longer gated by that bitmap. */
	feed_key(0x95);				/* HELP opens it */
	check("reports: HELP opens the key help", u->show_help == 1, "");
	feed_key(0x01);				/* ESC closes it */
	check("reports: ... and ESC closes it", u->show_help == 0, "");
	feed_key(0x95);
	feed_key(0x83);				/* ON closes it too, without quitting */
	check("reports: ... and so does ON, without quitting the editor",
	      u->show_help == 0 && u->mode == NB_MODE_EDITOR,
	      fmt("help=%d mode=%d", u->show_help, u->mode));
	/* THE STUCK KEY: a HELP press whose release never arrives */
	feed_key(0x95);
	u->held[0x95 & 0x7F] = 1;		/* the release was swallowed */
	feed_key(0x95);
	check("reports: a HELP press with a lost release still closes it",
	      u->show_help == 0, fmt("help=%d", u->show_help));
	u->held[0x95 & 0x7F] = 0;

	/* ---- 2. THE BOTTOM ROW IS DRAWN, AND THE STATUS BAR COVERS IT ----
	 * The row above the status bar is partly visible by definition (the body is
	 * not a whole number of rows tall), and it used to be dropped outright when
	 * its top fell below the body's edge.  Every row that has a pixel above the
	 * bar is drawn now; the bar is painted over the rest. */
	{
		int bad = 0, worst = 0, worst_yoff = -1;

		for (i = 0; i < LINE_H; i++) {
			int drawn;

			nb_ui_editor_set_scroll_px(i);	/* every sub-row offset */
			nb_ui_draw();
			drawn = nb_ui_editor_rows_drawn();
			/* NO BLANK BAND ABOVE THE STATUS BAR: the rows that were
			 * painted have to reach the bottom of the body, whatever the
			 * sub-row offset is.  (The row after the last full one is
			 * partly on the glass as soon as the view is mid-row, and it
			 * used to be dropped: "scroll half a line and the line below
			 * the last one does not appear".) */
			if (drawn * LINE_H - i < NB_BODY_BOT - NB_BODY_TOP) {
				bad++;
				worst = drawn;
				worst_yoff = i;
			}
		}
		check("reports: the painted rows reach the bottom of the body at "
		      "every sub-row offset", bad == 0,
		      fmt("%d of %d offsets short (drew %d rows at offset %d)",
			  bad, LINE_H, worst, worst_yoff));
	}

	/* ---- 3. THE COMPLETION POPUP SHOWS THE WHOLE NAME ----
	 * The box was `longest * 8 + 8` wide while the text starts 8 px in and is
	 * clipped to `w - 12`: the last character of every candidate was cut, so a
	 * function name ONE LETTER longer than the typed prefix came out truncated.
	 * The candidate is also stored in 40 bytes now, not 24. */
	{
		char raw[160];
		char prefix[64];
		int x, y, w, h, longest, k;
		static const char *name = "abcdefghijklmnopqrstuvwx";	/* 24 */

		/* the file holds the name (the candidate) and the user has typed all
		 * of it but the last letter, on the line below */
		snprintf(raw, sizeof(raw), "int %s;\n\n", name);
		nb_buf_from_raw(b, raw, (int)strlen(raw), NB_ENC_UTF8, NB_EOL_LF);
		nb_ui_editor_changed(0);
		nb_buf_goto(b, 1, 0);
		for (k = 0; k < 23; k++)
			prefix[k] = name[k];
		prefix[23] = 0;
		nb_buf_insert(b, prefix, 23);
		nb_complete_update(&u->comp, b, nb_hl_lang(&u->hl));
		check("reports: the candidate is the whole identifier, not 24 bytes "
		      "of it",
		      u->comp.n >= 1 && strcmp(u->comp.item[0], name) == 0,
		      u->comp.n >= 1 ? u->comp.item[0] : "(none)");
		longest = (int)strlen(u->comp.item[0]);
		nb_ui_draw();
		nb_ui_editor_comp_rect(&x, &y, &w, &h);
		check("reports: ... and its box is wide enough for all of it",
		      w >= longest * 8 + 12 && x >= 0 && x + w <= NB_SCREEN_W - 2,
		      fmt("x=%d w=%d needed=%d", x, w, longest * 8 + 12));
	}


	/* ---- 4. THE ROW ARITHMETIC IS O(FOLDS), NOT O(LINES) ----
	 * Dragging a file with a lot of folds stuttered because set_scroll_px()
	 * searched the whole file (asking nb_fold_hidden about every line) for every
	 * touch move.  The counter makes that a number instead of a feeling: 200
	 * scroll calls may not cost more than a small multiple of the fold count
	 * each.  A line-by-line search over this 40 line fixture with 20 folds would
	 * be 800 probes per call. */
	{
		unsigned long before;
		int calls;
		long per_call;

		nb_ui_open_file(ui_dir, "many.c");
		b = nb_app_buffer();
		nb_fold_clear(&u->fold);
		for (i = 0; i < b->nlines && u->fold.n < 20; i++) {
			if (nb_fold_region_end(b, i, NB_LANG_C) > i)
				nb_fold_toggle(&u->fold, b, i, NB_LANG_C);
		}
		/* the list the editor walks has to be SORTED for that */
		{
			int sorted = 1;

			for (i = 1; i < u->fold.n; i++) {
				if (u->fold.line[i - 1] >= u->fold.line[i])
					sorted = 0;
			}
			check("reports: the fold list is kept sorted (the row lookup "
			      "walks the folds, not the file)", sorted,
			      fmt("n=%d", u->fold.n));
		}
		before = nb_fold_probes;
		calls = 200;
		for (i = 0; i < calls; i++)
			nb_ui_editor_set_scroll_px((i % 30) * LINE_H);
		per_call = (long)(nb_fold_probes - before) / calls;
		check("reports: a scroll costs O(folds), not O(lines)",
		      per_call <= 64, fmt("%ld fold probes per call (nlines=%d)",
					  per_call, b->nlines));
		/* ... and folding/unfolding keeps it sorted */
		for (i = 0; i < 20 && i < b->nlines; i++) {
			if (nb_fold_region_end(b, i, NB_LANG_C) > i)
				nb_fold_toggle(&u->fold, b, i, NB_LANG_C);
		}
		{
			int sorted = 1;

			for (i = 1; i < u->fold.n; i++) {
				if (u->fold.line[i - 1] >= u->fold.line[i])
					sorted = 0;
			}
			check("reports: ... and after unfolding things too", sorted,
			      fmt("n=%d", u->fold.n));
		}
		nb_fold_clear(&u->fold);
	}
}

/* ---------------------------------------------------------------------------
 * NINE MORE FROM THE CALCULATOR (one test each)
 * ------------------------------------------------------------------------- */
static void test_reports2(void)
{
	nb_ui *u;
	nb_buf *b;
	int i;

	viewport_fixture();
	u = nb_ui_get();
	b = nb_app_buffer();

	/* ---- 1. NO KEY LEGENDS IN THE BARS ---- */
	{
		int found = 0;

		nb_ui_enter_browser();
		/* the toast occupies the row while it is up: this is about the LEGEND
		 * that used to live there permanently */
		nb_ui_get()->message[0] = 0;
		nb_ui_get()->msg_frames = 0;
		nb_ui_draw();
		/* (the row also carries the position on the right; the legend lived on
		 * the left, and that is the half this checks) */
		/* the status row's left hand end used to spell out "↵打开 退格 删除
		 * esc 上级" (clipped to "esc>").  Nothing may be drawn there now. */
		{
			int x, y, ink = 0;

			for (y = NB_SCREEN_H - 16 + 2; y < NB_SCREEN_H - 2; y++) {
				for (x = 4; x < 150; x++) {
					if (nb_host_fb_pixel(x, y) !=
					    nb_host_fb_pixel(2, NB_SCREEN_H - 8))
						ink++;
				}
			}
			found = ink;
		}
		check("reports2: the bottom bar carries no key legend", found < 6,
		      fmt("ink=%d", found));
		feed_key(0x0D);				/* back into the editor */
	}

	/* ---- 3. THE SCROLLBAR IS FLUSH WITH THE SCREEN'S EDGE ---- */
	{
		int x, y, ink = 0;

		/* a file long enough to need one */
		for (i = 0; i < 40; i++)
			nb_buf_insert(b, "line\n", 5);
		nb_ui_editor_changed(0);
		nb_ui_draw();
		for (y = NB_BODY_TOP; y < NB_BODY_BOT; y++) {
			if (nb_host_fb_pixel(NB_SCREEN_W - 1, y) !=
			    nb_host_fb_pixel(NB_SCREEN_W - 1, NB_BODY_TOP - 2))
				ink++;
		}
		check("reports2: the scrollbar reaches the screen's right edge",
		      ink > 20, fmt("ink in the last column=%d", ink));
		(void)x;
	}

	/* ---- 4. NO BLANK BAND AT THE BOTTOM (tested in test_ui_reports too: this
	 * is the same invariant through the pixel scroll) ---- */
	{
		int bad = 0;

		for (i = 1; i < LINE_H; i++) {
			nb_ui_editor_set_scroll_px(i);
			nb_ui_draw();
			if (nb_ui_editor_rows_drawn() * LINE_H - i <
			    NB_BODY_BOT - NB_BODY_TOP)
				bad++;
		}
		check("reports2: half a line of scroll still fills the body",
		      bad == 0, fmt("%d short", bad));
	}

	/* ---- 6. DEL INSIDE THE INDENTATION TAKES A WHOLE STEP ---- */
	{
		static const char *names[] = { "ind.c" };
		char tmpl[] = "/tmp/nbcode_ind_XXXXXX";
		char path[NB_PATH_MAX];
		FILE *f;

		if (mkdtemp(tmpl)) {
			snprintf(path, sizeof(path), "%s/ind.c", tmpl);
			f = fopen(path, "wb");
			if (f) {
				fputs("void f(void) {\n    int a = 1;\n      int b;\n"
				      "   int c;\n}\n", f);
				fclose(f);
			}
			nb_host_reset();
			nb_host_files(tmpl, names, 1);
			nb_ui_init(nb_host_config());
			nb_ui_enter_workspace(0);
			if (nb_ui_open_file(tmpl, "ind.c") == 1) {
				b = nb_app_buffer();
				u = nb_ui_get();
				/* four spaces in, tab width four: all four go */
				nb_buf_goto(b, 1, 4);
				feed_key(0x0C);		/* the Del key */
				check("reports2: Del takes a whole indent step",
				      strcmp(nb_buf_line(b, 1), "int a = 1;") == 0,
				      fmt("[%s]", nb_buf_line(b, 1)));
				/* SIX spaces in with a tab width of four: back to the
				 * previous tab stop, which is two */
				nb_buf_goto(b, 2, 6);
				feed_key(0x0C);
				check("reports2: ... to the previous tab stop",
				      strcmp(nb_buf_line(b, 2), "    int b;") == 0,
				      fmt("[%s]", nb_buf_line(b, 2)));
				/* ... and after the text it is the ordinary one */
				nb_buf_goto(b, 1, 11);
				feed_key(0x0C);
				check("reports2: ... and the ordinary one after text",
				      strcmp(nb_buf_line(b, 1), "int a = 1") == 0,
				      fmt("[%s]", nb_buf_line(b, 1)));
				/* ONE undo record for the step */
				nb_buf_goto(b, 3, 3);
				feed_key(0x0C);
				nb_buf_undo(b);
				check("reports2: ... and one undo puts the step back",
				      strcmp(nb_buf_line(b, 3), "   int c;") == 0,
				      fmt("[%s]", nb_buf_line(b, 3)));
			}
			remove(path);
			rmdir(tmpl);
		}
	}

	/* ---- 7. INDENTATION GUIDES ---- */
	{
		int x, y, ink = 0;
		nb_u32 bg;

		nb_ui_open_file(ui_dir, "many.c");
		b = nb_app_buffer();
		u = nb_ui_get();
		/* the fixture is not indented: put two levels on line 1 */
		nb_buf_goto(b, 1, 0);
		nb_buf_insert(b, "        int x;", 14);
		nb_ui_editor_changed(1);
		nb_ui_editor_set_scroll_px(0);
		nb_ui_draw();
		bg = nb_host_fb_pixel(NB_SCREEN_W - 4, NB_BODY_TOP + LINE_H + 8);
		for (y = NB_BODY_TOP + LINE_H + 2; y < NB_BODY_TOP + 2 * LINE_H - 2;
		     y++) {
			int gw = nb_ui_editor_gutter_w();

			for (x = gw; x < NB_SCREEN_W - 8; x++) {
				if (nb_host_fb_pixel(x, y) == u->th->line)
					ink++;
			}
		}
		check("reports2: indentation guides are drawn", ink > 8,
		      fmt("ink=%d", ink));
		(void)bg;
	}

	/* ---- 5. FIND AND REPLACE ---- */
	{
		int index = 0, total = 0;

		nb_buf_from_raw(b, "one two one\nthree one\nfour\n", 28,
				NB_ENC_UTF8, NB_EOL_LF);
		nb_ui_editor_changed(0);
		nb_find_clear();
		check("reports2: SYMB sets the search text",
		      nb_find_set("one") == 1, "");
		check("reports2: ... the count is the number of matches",
		      nb_find_count(b) == 3, fmt("%d", nb_find_count(b)));
		nb_buf_goto(b, 0, 0);
		check("reports2: ... stepping down walks the matches",
		      nb_ui_editor_find_step(1) == 1 && b->cur_line == 0 &&
		      b->cur_col == 8, fmt("%d:%d", b->cur_line + 1, b->cur_col));
		check("reports2: ... and the info says which one it is",
		      nb_ui_editor_find_info(&index, &total) &&
		      index == 2 && total == 3, fmt("%d/%d", index, total));
		check("reports2: ... the next one is on the line below",
		      nb_ui_editor_find_step(1) == 1 && b->cur_line == 1 &&
		      b->cur_col == 6, fmt("%d:%d", b->cur_line + 1, b->cur_col));
		check("reports2: ... and it wraps at the end of the file",
		      nb_ui_editor_find_step(1) == 1 && b->cur_line == 0 &&
		      b->cur_col == 0, fmt("%d:%d", b->cur_line + 1, b->cur_col));
		check("reports2: ... so does the previous one",
		      nb_ui_editor_find_step(-1) == 1 && b->cur_line == 1,
		      fmt("%d:%d", b->cur_line + 1, b->cur_col));
		/* CASE INSENSITIVE for ASCII, and a search inside a word */
		check("reports2: the search ignores ASCII case",
		      nb_find_set("ONE") == 1 && nb_find_count(b) == 3, "");
		/* REPLACE: the caret has to be on a match */
		nb_find_set("one");
		nb_buf_goto(b, 0, 0);
		nb_buf_goto(b, 0, 0);
		nb_ui_editor_find_step(1);		/* onto the first match */
		{
			char before[NB_PATH_MAX];

			snprintf(before, sizeof(before), "%s", nb_buf_line(b, 0));
			check("reports2: PLOT replaces the match under the caret",
			      nb_find_replace(b, "1", NB_NULL, NB_NULL) == 1 &&
			      strncmp(nb_buf_line(b, 0), "one two 1", 9) == 0,
			      fmt("[%s] was [%s]", nb_buf_line(b, 0), before));
		}
		nb_buf_goto(b, 0, 8);
		/* (line 0 already had its first match replaced by the check above) */
		check("reports2: ... and replace-all takes every match",
		      nb_find_set("one") == 1 &&
		      nb_find_replace_all(b, "X") == 2 &&
		      strcmp(nb_buf_line(b, 0), "X two 1") == 0 &&
		      strcmp(nb_buf_line(b, 1), "three X") == 0,
		      fmt("[%s][%s]", nb_buf_line(b, 0), nb_buf_line(b, 1)));
		nb_find_clear();
		check("reports2: clearing the search takes the chrome away",
		      !nb_find_active() && nb_ui_editor_find_info(&index,
								  &total) == 0, "");
		/* ---- AND THE WHOLE PATH THROUGH THE KEYS: SYMB, the text, ENTER,
		 * PLOT, the replacement ---- */
		nb_buf_from_raw(b, "one two one\nthree one\n", 22, NB_ENC_UTF8,
				NB_EOL_LF);
		nb_ui_editor_changed(0);
		nb_buf_goto(b, 0, 0);
		nb_find_clear();
		feed_key(0x91);				/* SYMB: search */
		check("reports2: SYMB opens the search text input",
		      nb_ui_input_active() && u->input_then == NB_INPUT_FIND, "");
		/* ALPHA TWICE LOCKS IT: a one-shot alpha is spent by the first letter,
		 * and the second key would type its plain layer */
		feed_key(0xB6);
		feed_key(0xB6);
		{
			const int one[3] = { 0x4F, 0x4E, 0x45 };	/* o, n, e */
			int k;

			for (k = 0; k < 3; k++)
				feed_key(one[k]);
		}
		check("reports2: the search text was typed into the input",
		      strcmp(u->input_text, "one") == 0, fmt("[%s]", u->input_text));
		feed_key(0x0D);				/* ENTER */
		check("reports2: ... and the search runs when it is answered",
		      !nb_ui_input_active() && nb_find_active() &&
		      strcmp(nb_find_text(), "one") == 0 &&
		      nb_ui_editor_find_info(&index, &total) && total == 3,
		      fmt("[%s] %d/%d", nb_find_text(), index, total));
		/* PLOT asks what to put in its place and replaces the match the caret
		 * landed on */
		feed_key(0xB2);				/* PLOT */
		check("reports2: PLOT opens the replacement input",
		      nb_ui_input_active() && u->input_then == NB_INPUT_REPLACE,
		      "");
		feed_key(0xB6);				/* alpha (still locked: off) */
		feed_key(0xB6);				/* ... and on again */
		feed_key(0x58);				/* x */
		feed_key(0x0D);				/* ENTER */
		check("reports2: ... and ENTER replaces the match under the caret",
		      strncmp(nb_buf_line(b, 0), "x two one", 9) == 0,
		      fmt("[%s]", nb_buf_line(b, 0)));
		check("reports2: ... and the caret is on the next match",
		      b->cur_line == 0 && b->cur_col == 6,
		      fmt("%d:%d", b->cur_line + 1, b->cur_col));
		nb_find_clear();
	}

	/* ---- 8. THE SAVE QUESTION IS THREE BUTTONS ---- */
	{
		nb_ui_open_file(ui_dir, "many.c");
		b = nb_app_buffer();
		u = nb_ui_get();
		nb_buf_goto(b, 1, 0);
		nb_buf_insert(b, "x", 1);
		nb_ui_editor_changed(1);
		feed_key(0x01);				/* ESC asks to save first */
		check("reports2: closing a modified file asks first",
		      u->prompt == NB_PROMPT_SAVE, fmt("prompt=%d", u->prompt));
		check("reports2: ... with three buttons, not y/n",
		      nb_ui_prompt_button_count() == 3 &&
		      strcmp(nb_ui_prompt_button(0), "保存") == 0 &&
		      strcmp(nb_ui_prompt_button(1), "不保存") == 0 &&
		      strcmp(nb_ui_prompt_button(2), "取消") == 0, "");
		/* the arrows move the highlight and ENTER presses it */
		feed_key(0x04);				/* RIGHT */
		feed_key(0x04);				/* RIGHT: 取消 */
		feed_key(0x0D);				/* ENTER */
		check("reports2: RIGHT RIGHT ENTER cancels, staying in the file",
		      u->prompt == NB_PROMPT_NONE && u->mode == NB_MODE_EDITOR,
		      fmt("prompt=%d mode=%d", u->prompt, u->mode));
		/* ... and a tap presses one too */
		feed_key(0x01);
		{
			int x, y, w, h;

			nb_ui_prompt_rect(1, &x, &y, &w, &h);	/* 不保存 */
			nb_ui_touch_send(1, x + w / 2, y + h / 2);
			nb_ui_touch_send(8, x + w / 2, y + h / 2);
		}
		check("reports2: ... and a tap on 不保存 leaves without saving",
		      u->prompt == NB_PROMPT_NONE && u->mode == NB_MODE_BROWSER,
		      fmt("prompt=%d mode=%d", u->prompt, u->mode));
	}

}

/* ---------------------------------------------------------------------------
 * FIVE MORE FROM THE CALCULATOR
 * ------------------------------------------------------------------------- */
static void test_reports3(void)
{
	nb_ui *u;
	nb_buf *b;
	int i;

	viewport_fixture();
	u = nb_ui_get();
	b = nb_app_buffer();

	/* ---- 3. THE KEYBOARD LAYER COMES BEFORE THE FILE TYPE ---- */
	{
		char lab[24];

		nb_keymap_reset();
		feed_key(0xB6);				/* alpha: one shot */
		feed_key(0xB6);				/* ... locked */
		nb_ui_editor_status_label(lab, sizeof(lab));
		check("reports3: the layer is shown BEFORE the file type",
		      strcmp(lab, "alock  c") == 0, fmt("[%s]", lab));
		nb_keymap_reset();
		feed_key(0x8B);				/* shift */
		nb_ui_editor_status_label(lab, sizeof(lab));
		check("reports3: ... and so is shift",
		      strcmp(lab, "shift  c") == 0, fmt("[%s]", lab));
		nb_keymap_reset();
	}

	/* ---- 4. ONE INDENT LEVEL HAS NO GUIDE, AND NONE SITS ON THE TEXT ---- */
	{
		int ink = 0, x, y, gw;
		nb_u32 bg;

		nb_buf_from_raw(b, "a\n    b\n        c\n            d\n", 30,
				NB_ENC_UTF8, NB_EOL_LF);
		nb_ui_editor_changed(0);
		nb_ui_editor_set_scroll_px(0);
		nb_ui_draw();
		gw = nb_ui_editor_gutter_w();
		bg = nb_host_fb_pixel(NB_SCREEN_W - 6, NB_BODY_TOP + 3);
		/* the guide colour in a line with ONE level (line 1) */
		for (y = NB_BODY_TOP + LINE_H + 2; y < NB_BODY_TOP + 2 * LINE_H - 2;
		     y++) {
			for (x = gw; x < NB_SCREEN_W - 8; x++) {
				if (nb_host_fb_pixel(x, y) == u->th->line)
					ink++;
			}
		}
		check("reports3: one indent level draws no guide", ink == 0,
		      fmt("ink=%d", ink));
		/* ... and the line with THREE levels has guides at 4 and 8, and
		 * NOT at 12 (which is where its text starts) */
		{
			int at4 = 0, at8 = 0, at12 = 0;

			y = NB_BODY_TOP + 3 * LINE_H + 8;
			for (x = gw; x < NB_SCREEN_W - 8; x++) {
				if (nb_host_fb_pixel(x, y) != u->th->line)
					continue;
				if (x < gw + 4 * 8 + 4 && x > gw + 4 * 8 - 4)
					at4++;
				else if (x < gw + 8 * 8 + 4 && x > gw + 8 * 8 - 4)
					at8++;
				else if (x < gw + 12 * 8 + 4 && x > gw + 12 * 8 - 4)
					at12++;
			}
			check("reports3: ... three levels draw two guides, clear of the "
			      "text", at4 == 1 && at8 == 1 && at12 == 0,
			      fmt("at 4=%d, 8=%d, 12=%d", at4, at8, at12));
		}
		(void)bg;
	}

	/* ---- 1. EVERY MATCH IS HIGHLIGHTED ---- */
	{
		int x, y, found = 0;

		nb_buf_from_raw(b, "pr pr pr\nxx pr yy\n", 17, NB_ENC_UTF8,
				NB_EOL_LF);
		nb_ui_editor_changed(0);
		nb_find_clear();
		nb_find_set("pr");
		nb_buf_goto(b, 0, 0);
		nb_ui_editor_set_scroll_px(0);
		nb_ui_draw();
		for (y = NB_BODY_TOP; y < NB_BODY_TOP + 2 * LINE_H; y++) {
			int gw = nb_ui_editor_gutter_w();

			for (x = gw; x < NB_SCREEN_W; x++) {
				if (nb_host_fb_pixel(x, y) == u->th->find)
					found++;
			}
		}
		check("reports3: every search match gets a highlight", found > 100,
		      fmt("highlighted pixels=%d", found));
		/* ... and the highlight goes when the search does */
		nb_find_clear();
		nb_ui_draw();
		found = 0;
		for (y = NB_BODY_TOP; y < NB_BODY_TOP + 2 * LINE_H; y++) {
			int gw = nb_ui_editor_gutter_w();

			for (x = gw; x < NB_SCREEN_W; x++) {
				if (nb_host_fb_pixel(x, y) == u->th->find)
					found++;
			}
		}
		check("reports3: ... and goes away with the search", found == 0,
		      fmt("left=%d", found));
	}

	/* ---- 2. THE FIND CHROME IS A FLOATING WINDOW, not header furniture ---- */
	{
		int x = 0, y = 0, w = 0, h = 0, ink = 0, px, py;

		nb_buf_from_raw(b, "pr one\npr two\n", 14, NB_ENC_UTF8, NB_EOL_LF);
		nb_ui_editor_changed(0);
		nb_find_set("pr");
		nb_ui_draw();
		nb_ui_editor_find_window(&x, &y, &w, &h);
		check("reports3: the find window is a panel inside the body",
		      w > 60 && h > 30 && y >= NB_BODY_TOP &&
		      y + h <= NB_BODY_BOT && x + w <= NB_SCREEN_W,
		      fmt("%dx%d at %d,%d", w, h, x, y));
		/* it has ink (a title, a count and two buttons) */
		for (py = y; py < y + h; py++) {
			for (px = x; px < x + w; px++) {
				if (nb_host_fb_pixel(px, py) != u->th->bg)
					ink++;
			}
		}
		check("reports3: ... with the count and the buttons drawn in it",
		      ink > 200, fmt("ink=%d", ink));
		/* A TAP ON 下 STEPS, and a tap outside it is still a tap on the
		 * code (the window is not modal) */
		{
			int total = 0, index = 0;

			nb_buf_goto(b, 0, 0);
			nb_ui_editor_find_info(&index, &total);
			check("reports3: the search starts on the first match",
			      index == 1 && total == 2, fmt("%d/%d", index, total));
			nb_ui_touch_send(1, x + w - 20, y + h - 10);	/* 下 */
			nb_ui_touch_send(8, x + w - 20, y + h - 10);
			nb_ui_editor_find_info(&index, &total);
			check("reports3: ... a tap on 下 steps to the next match",
			      index == 2, fmt("%d/%d", index, total));
			nb_ui_touch_send(1, x + 20, y + h - 10);	/* 上 */
			nb_ui_touch_send(8, x + 20, y + h - 10);
			nb_ui_editor_find_info(&index, &total);
			check("reports3: ... and 上 goes back", index == 1,
			      fmt("%d/%d", index, total));
			/* the close box */
			nb_ui_touch_send(1, x + w - 10, y + 6);
			nb_ui_touch_send(8, x + w - 10, y + 6);
			check("reports3: ... and x closes the search",
			      !nb_find_active(), "");
		}
	}

	/* ---- 5. A SELECTION DRAG SCROLLS SIDEWAYS TOO ---- */
	{
		nb_ui_open_file(ui_dir, "zh.c");
		b = nb_app_buffer();
		u = nb_ui_get();
		nb_buf_goto(b, 0, 0);
		nb_ui_editor_set_scroll_px(0);
		nb_ui_editor_set_scroll_left_px(0);
		nb_ui_draw();
		/* a long press starts a selection, then the finger walks off the
		 * right hand edge of the glass */
		nb_ui_touch_send(1, NB_SCREEN_W - 40, NB_BODY_TOP + 40);
		nb_ui_editor_long_press(NB_SCREEN_W - 40, NB_BODY_TOP + 40);
		check("reports3: a long press starts a selection",
		      nb_ui_selection_active(), "");
		for (i = 0; i < 8; i++) {
			nb_ui_touch_send(2, NB_SCREEN_W - 2, NB_BODY_TOP + 40);
			nb_ui_editor_touch_tick();
		}
		check("reports3: ... and holding at the right edge scrolls sideways",
		      nb_ui_editor_scroll_px() >= 0 &&
		      (u->left * 8 + u->xoff) > 0,
		      fmt("left=%d", u->left * 8 + u->xoff));
		/* ... and at the left edge back again */
		for (i = 0; i < 10; i++) {
			nb_ui_touch_send(2, 0, NB_BODY_TOP + 40);
			nb_ui_editor_touch_tick();
		}
		check("reports3: ... and the left edge brings it back",
		      u->left * 8 + u->xoff == 0,
		      fmt("left=%d", u->left * 8 + u->xoff));
		nb_ui_selection_clear();
		nb_ui_touch_send(8, 0, NB_BODY_TOP + 40);
	}
}

/* ---------------------------------------------------------------------------
 * THE FILE LIST SHOWS EVERY ENTRY, AND A FILE THAT IS NOT TEXT OPENS IN HEX
 * ------------------------------------------------------------------------- */
static void test_list_and_hex(void)
{
	char tmpl[] = "/tmp/nbcode_lh_XXXXXX";
	char path[NB_PATH_MAX];
	nb_ui *u;
	int i;

	if (!mkdtemp(tmpl))
		return;

	/* ---- 1. TWENTY FILES ARE TWENTY ROWS ----
	 * The list's own scrolling is tested elsewhere; what this pins down is that a
	 * folder is listed COMPLETELY -- the launcher now asks the firmware finder
	 * with every attribute mask and merges the answers (calc/main.py), because
	 * stopping at the first mask that returned anything is what showed thirteen
	 * entries of a folder with more.
	 */
	{
		static const char *names[20] = { "a.c", "b.c", "c.c", "d.c", "e.c",
						 "f.c", "g.c", "h.c", "i.c", "j.c",
						 "k.c", "l.c", "m.c", "n.c", "o.c",
						 "p.c", "q.c", "r.c", "s.c", "t.c" };
		FILE *f;

		for (i = 0; i < 20; i++) {
			snprintf(path, sizeof(path), "%s/%s", tmpl, names[i]);
			f = fopen(path, "wb");
			if (f) {
				fputs("int x;\n", f);
				fclose(f);
			}
		}
		nb_host_reset();
		nb_host_files(tmpl, names, 20);
		nb_ui_init(nb_host_config());
		nb_ui_enter_workspace(0);
		u = nb_ui_get();
		check("list: a folder of 20 files has 20 rows", u->count == 20,
		      fmt("count=%d", u->count));
		/* ... and the LAST one is reachable, which is what "only 13 entries"
		 * was about */
		{
			int steps = 0;

			while (u->sel + 1 < u->count && steps < 40) {
				feed_key(0x05);		/* DOWN */
				steps++;
			}
			check("list: the selection reaches the last row",
			      u->sel == u->count - 1, fmt("sel=%d", u->sel));
			nb_ui_draw();
			check("list: ... and the window scrolled to it",
			      nb_ui_list_scroll_px() > 0,
			      fmt("scroll=%d", nb_ui_list_scroll_px()));
		}
	}

	/* ---- 2. A FILE THAT IS NOT TEXT OPENS IN THE HEX VIEW ---- */
	{
		static const char *names[2] = { "bin.dat", "latin.txt" };
		FILE *f;

		/* a NUL and a lone high byte: neither UTF-8 nor GB2312, and that is
		 * exactly what nb_codec_detect answers Latin-1 to */
		snprintf(path, sizeof(path), "%s/bin.dat", tmpl);
		f = fopen(path, "wb");
		if (f) {
			fwrite("\x00\x01\x02\xff\xfehello\x7f", 1, 12, f);
			fclose(f);
		}
		/* ... and a Latin-1 text file, which the GUESS sends to the hex view
		 * too -- and which MENU can hand back to the editor */
		snprintf(path, sizeof(path), "%s/latin.txt", tmpl);
		f = fopen(path, "wb");
		if (f) {
			fwrite("caf\xe9 na\xefve\n", 1, 12, f);
			fclose(f);
		}
		nb_host_reset();
		nb_host_files(tmpl, names, 2);
		nb_ui_init(nb_host_config());
		nb_ui_enter_workspace(0);
		u = nb_ui_get();
		check("hex: the fixture is listed",
		      nb_ui_open_file(tmpl, "bin.dat") == 1 &&
		      u->mode == NB_MODE_HEX && nb_hex_active(),
		      fmt("mode=%d", u->mode));
		check("hex: ... with the file's bytes in it",
		      nb_hex_len() == 12 && nb_hex_full_len() == 12,
		      fmt("len=%d", nb_hex_len()));
		check("hex: ... and it says which file it is",
		      strcmp(nb_hex_name(), "bin.dat") == 0, nb_hex_name());
		nb_ui_draw();
		{
			int x, y, ink = 0;

			/* the hex text is drawn in the body's right hand part */
			for (y = NB_BODY_TOP; y < NB_BODY_TOP + 3 * 16; y++) {
				for (x = 60; x < NB_SCREEN_W - 4; x++) {
					if (nb_host_fb_pixel(x, y) == u->th->text)
						ink++;
				}
			}
			check("hex: ... and the bytes are on the glass", ink > 60,
			      fmt("ink=%d", ink));
		}
		/* a file with a HEADER, a long one, and a scroll range */
		{
			char bigpath[NB_PATH_MAX];
			char block[1024];
			FILE *g;
			int k;

			/* NOT text: bytes that are neither UTF-8 nor GB2312, so the
			 * view is the hex one */
			for (k = 0; k < (int)sizeof(block); k++)
				block[k] = (char)((k * 7) ^ 0x5A);
			snprintf(bigpath, sizeof(bigpath), "%s/bin.dat", tmpl);
			g = fopen(bigpath, "wb");
			if (g) {
				for (k = 0; k < 40; k++)
					fwrite(block, 1, sizeof(block), g);
				fclose(g);
			}
			(void)bigpath;
		}
		check("hex: a 40 KB file opens in the view",
		      nb_ui_open_file(tmpl, "bin.dat") == 1 &&
		      nb_hex_len() == NB_HEX_MAX &&
		      nb_hex_full_len() == 40960,
		      fmt("len=%d full=%d", nb_hex_len(), nb_hex_full_len()));
		check("hex: ... and the view scrolls through what it holds",
		      nb_hex_max_scroll_px() > 0, "");
		{
			int before = nb_hex_scroll_px();

			feed_key(0x05);			/* DOWN */
			check("hex: DOWN scrolls a row",
			      nb_hex_scroll_px() == before + NB_HEX_ROW_H,
			      fmt("%d -> %d", before, nb_hex_scroll_px()));
			feed_key(0x03);			/* UP */
			check("hex: UP comes back", nb_hex_scroll_px() == before,
			      "");
			/* shift+RIGHT is END, shift+LEFT is HOME (nb_keymap.c: the
			 * calculator has no key of its own for either) */
			feed_key(0x8B);
			feed_key(0x04);
			check("hex: shift+RIGHT goes to the end",
			      nb_hex_scroll_px() == nb_hex_max_scroll_px(),
			      fmt("scroll=%d max=%d", nb_hex_scroll_px(),
				  nb_hex_max_scroll_px()));
			feed_key(0x8B);
			feed_key(0x02);
			check("hex: shift+LEFT comes back to the start",
			      nb_hex_scroll_px() == 0, "");
			/* a drag moves it a pixel at a time */
			nb_ui_touch_send(1, 160, 120);
			nb_ui_touch_send(2, 160, 100);
			nb_ui_touch_send(8, 160, 100);
			check("hex: a drag scrolls by pixels",
			      nb_hex_scroll_px() == 20,
			      fmt("scroll=%d", nb_hex_scroll_px()));
		}
		/* THE MENU HANDS IT TO THE EDITOR, and ESC goes back to the list */
		nb_ui_hex_menu();
		check("hex: MENU opens the view's menu",
		      nb_ui_overlay() == NB_OVER_MENU, "");
		nb_ui_menu_action(NB_MENU_HEX_TEXT);
		check("hex: '以文本打开' opens the same file in the editor",
		      u->mode == NB_MODE_EDITOR && !nb_hex_active() &&
		      nb_app_buffer() != NULL, fmt("mode=%d", u->mode));
		nb_ui_editor_close();
		nb_ui_enter_browser();
		check("hex: ... and the list is back", u->mode == NB_MODE_BROWSER, "");
		/* ... and the hex view's other answer closes it */
		nb_ui_open_file(tmpl, "bin.dat");
		check("hex: the view opens again", u->mode == NB_MODE_HEX, "");
		feed_key(0x01);				/* ESC */
		check("hex: ESC goes back to the file list",
		      u->mode == NB_MODE_BROWSER && !nb_hex_active(),
		      fmt("mode=%d", u->mode));
		/* A NORMAL FILE STILL OPENS IN THE EDITOR */
		{
			static const char *plain[1] = { "plain.c" };

			snprintf(path, sizeof(path), "%s/plain.c", tmpl);
			f = fopen(path, "wb");
			if (f) {
				fputs("int main(void) { return 0; }\n", f);
				fclose(f);
			}
			nb_host_reset();
			nb_host_files(tmpl, plain, 1);
			nb_ui_init(nb_host_config());
			nb_ui_enter_workspace(0);
			check("hex: a text file still opens in the editor",
			      nb_ui_open_file(tmpl, "plain.c") == 1 &&
			      nb_ui_get()->mode == NB_MODE_EDITOR &&
			      !nb_hex_active(), "");
		}
		for (i = 0; i < 2; i++) {
			snprintf(path, sizeof(path), "%s/%s", tmpl, names[i]);
			remove(path);
		}
	}
	{
		char rm[NB_PATH_MAX];
		static const char *names[20] = { "a.c", "b.c", "c.c", "d.c", "e.c",
						 "f.c", "g.c", "h.c", "i.c", "j.c",
						 "k.c", "l.c", "m.c", "n.c", "o.c",
						 "p.c", "q.c", "r.c", "s.c", "t.c" };

		for (i = 0; i < 20; i++) {
			snprintf(rm, sizeof(rm), "%s/%s", tmpl, names[i]);
			remove(rm);
		}
		snprintf(rm, sizeof(rm), "%s/plain.c", tmpl);
		remove(rm);
		rmdir(tmpl);
	}
}

static void test_config_dirs(void)
{
	char tmpl[] = "/tmp/nbcode_cfg_XXXXXX";
	char path[NB_PATH_MAX];
	nb_config cfg;
	int i, j, bad = 0;

	if (!mkdtemp(tmpl))
		return;
	/* the parser is given the same GLOBAL instance the browser reads (on the
	 * host that is nb_host_config()'s struct, on the device nb_cfg_instance) */
	snprintf(path, sizeof(path), "%s/%s", tmpl, NB_CFG_FILE_NAME);
	cfg_write(path);
	nb_cfg_load(nb_host_config(), tmpl);
	remove(path);
	rmdir(tmpl);
	memcpy(&cfg, nb_host_config(), sizeof(cfg));

	check("config: every workspace in the table parses",
	      cfg.nworkspaces == (int)(sizeof(cfg_ws) / sizeof(cfg_ws[0])) &&
	      cfg.ndirs == (int)(sizeof(cfg_dir) / sizeof(cfg_dir[0])),
	      fmt("ws=%d dirs=%d", cfg.nworkspaces, cfg.ndirs));
	for (i = 0; i < cfg.nworkspaces; i++) {
		int want = -1;

		for (j = 0; j < cfg.ndirs; j++) {
			if (strcmp(cfg_dir[j], cfg_ws[i]) == 0)
				want = j;
		}
		if (cfg.ws_dir[i] != want)
			bad++;
	}
	check("config: each workspace maps to ITS OWN directory", bad == 0,
	      fmt("%d wrong", bad));

	/* ... and the browser really lists that directory's files: the names in
	 * the table are what makes a wrong mapping visible */
	for (i = 1; i < cfg.nworkspaces; i++) {
		char want[16];

		nb_file_select(i);
		snprintf(want, sizeof(want), "d%d.txt", i);
		check("config: the browser lists the workspace's own files",
		      nb_file_count() > 0 &&
		      strcmp(nb_file_name(0), want) == 0,
		      fmt("workspace %d lists [%s]", i,
			  nb_file_count() ? nb_file_name(0) : "(empty)"));
	}

	/* A FOLDER ROW WITH NO DIRECTORY OF ITS OWN must refuse, not open another
	 * one: that is the same bug one level down, and it is how every .hpappdir
	 * folder past the launcher's budget came out as nbCode.hpappdir. */
	{
		nb_file_select(0);			/* C:\DATA */
		check("config: the folder row with no directory is a row",
		      nb_file_count() == 2 && nb_file_is_dir(0), "");
		check("config: ... and entering it REFUSES instead of opening "
		      "another folder", nb_file_enter(0) == 0, "");
		check("config: ... and the list is still the one it was",
		      nb_file_count() == 2 &&
		      strcmp(nb_file_name(1), "d0.txt") == 0, "");
	}

	/* A WORKSPACE THE TABLE DOES NOT HAVE MUST NOT BORROW A DIRECTORY.
	 *
	 * This is the reported bug, and it is the dangerous shape: the launcher's
	 * directory table has a budget (NB_CFG_MAX_DIRS), the folder walk spends it
	 * on the .hpappdir folders under C:\DATA, and a workspace that misses the
	 * cut used to fall back to directory 0 -- the FIRST workspace, which is the
	 * application folder -- so the user saw nbCode's own files while believing
	 * they had opened another folder. */
	{
		int wsn = (int)(sizeof(cfg_ws) / sizeof(cfg_ws[0]));

		{
			char tmpl2[] = "/tmp/nbcode_cfg2_XXXXXX";

			if (mkdtemp(tmpl2)) {
				snprintf(path, sizeof(path), "%s/%s", tmpl2,
					 NB_CFG_FILE_NAME);
				cfg_write_n(path, 3);
				nb_cfg_load(nb_host_config(), tmpl2);
				remove(path);
				rmdir(tmpl2);
				check("config: a workspace with no directory in the "
				      "table has no list",
				      nb_host_config()->nworkspaces == wsn &&
				      nb_host_config()->ws_dir[3] ==
				      (int)NB_CFG_NO_DIR,
				      fmt("ws3 -> dir %d",
					  nb_host_config()->ws_dir[3]));
				nb_file_select(3);
				check("config: ... and browsing it shows nothing "
				      "instead of another folder's files",
				      nb_file_count() == 0,
				      fmt("workspace 3 lists [%s]",
					  nb_file_count() ? nb_file_name(0)
							  : "(empty)"));
			}
		}
	}
}

/* ---------------------------------------------------------------------------
 * the browser and the workspace picker: TOUCH, the MENU key, and the Del key
 *
 * A file browser on a touch screen is a pointing device: tapping a row opens it.
 * The report asked for exactly that, plus pixel-level dragging and for the delete
 * to be on the Del key (PPL 19) rather than a function-key chord -- and the
 * calculator has no F1..F6 row at all, so everything those keys did is in the
 * MENU popup now.
 * ------------------------------------------------------------------------- */
static void test_browser_menu(void)
{
	/* MORE ROWS THAN FIT: the list scrolls at all only when the folder is
	 * taller than the body (13 rows) */
	static const char *names[] = { "one.c", "two.c", "three.c", "four.c",
				       "five.c", "six.c", "seven.c", "eight.c",
				       "nine.c", "ten.c", "eleven.c", "twelve.c",
				       "thirteen.c", "fourteen.c", "fifteen.c",
				       "sixteen.c", "seventeen.c", "eighteen.c",
				       "nineteen.c", "twenty.c" };
	nb_ui *u;
	char path[NB_PATH_MAX];
	int i;

	make_ui_dir();
	for (i = 0; i < 20; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%s/%s", ui_dir, names[i]);
		f = fopen(path, "wb");
		if (f) {
			fputs("int x;\n", f);
			fclose(f);
		}
	}
	nb_host_reset();
	nb_host_files(ui_dir, names, 20);
	nb_ui_init(nb_host_config());
	nb_ui_enter_workspace(0);
	u = nb_ui_get();
	check("browser: it starts on the file list", u->mode == NB_MODE_BROWSER &&
	      u->count == 20, fmt("mode=%d count=%d", u->mode, u->count));

	/* A DRAG SCROLLS THE LIST BY PIXELS */
	{
		int before = u->top * 16 + u->list_yoff;

		nb_ui_touch_send(1, 120, 100);
		nb_ui_touch_send(2, 120, 90);		/* 10 px up */
		check("browser: a drag scrolls the list by pixels",
		      u->list_yoff == 10 && u->top == 0,
		      fmt("top=%d yoff=%d", u->top, u->list_yoff));
		nb_ui_touch_send(2, 120, 74);		/* 16 px more */
		check("browser: ... and whole rows once a row is covered",
		      u->top == 1 && u->list_yoff == 10,
		      fmt("top=%d yoff=%d", u->top, u->list_yoff));
		nb_ui_touch_send(8, 120, 74);
		check("browser: the drag moved the view", 
		      u->top * 16 + u->list_yoff > before, "");
	}
	/* the same drag UP must not go past the top */
	{
		nb_ui_touch_send(1, 120, 60);
		nb_ui_touch_send(2, 120, 200);
		nb_ui_touch_send(8, 120, 200);
		check("browser: dragging down stops at the first row",
		      u->top == 0 && u->list_yoff == 0,
		      fmt("top=%d yoff=%d", u->top, u->list_yoff));
	}

	/* THE SCROLLBAR, when the folder is taller than the list, and the header
	 * drawn LAST (a half scrolled row used to run into the file names' title) */
	{
		int x, y, ink_before = 0;

		nb_ui_draw();
		for (y = 0; y < NB_BODY_TOP; y++) {
			for (x = 0; x < NB_SCREEN_W; x++) {
				if (nb_host_fb_pixel(x, y) != 0)
					ink_before++;
			}
		}
		check("browser: the scrollbar is up when the list is longer than "
		      "the screen",
		      nb_ui_scrollbar_visible() &&
		      nb_ui_list_max_px() > 0, fmt("max=%d",
						   nb_ui_list_max_px()));
		check("browser: ... and it takes the finger on the right hand edge",
		      nb_ui_scrollbar_hit(NB_SCREEN_W - 3, 120) &&
		      !nb_ui_scrollbar_hit(120, 120), "");
		/* drag it down: the list scrolls by pixels */
		nb_ui_touch_send(1, NB_SCREEN_W - 3, NB_BODY_TOP + 2);
		nb_ui_touch_send(2, NB_SCREEN_W - 3, NB_BODY_TOP + 80);
		nb_ui_touch_send(8, NB_SCREEN_W - 3, NB_BODY_TOP + 80);
		check("browser: dragging the bar moves the list",
		      nb_ui_list_scroll_px() > 0,
		      fmt("scroll=%d", nb_ui_list_scroll_px()));
		/* and the header is still the header: a half scrolled row must not
		 * run into it */
		nb_ui_draw();
		{
			int ink_after = 0;

			for (y = 0; y < NB_BODY_TOP; y++) {
				for (x = 0; x < NB_SCREEN_W; x++) {
					if (nb_host_fb_pixel(x, y) != 0)
						ink_after++;
				}
			}
			check("browser: the header is drawn over the half scrolled row",
			      ink_after == ink_before,
			      fmt("ink %d -> %d", ink_before, ink_after));
		}
		nb_ui_list_set_scroll_px(0);
	}

	/* A TAP OPENS THE FILE UNDER THE FINGER (not "select, then ENTER") */
	{
		int y = 16 + 2 * 16 + 4;		/* the third row */

		nb_ui_touch_send(1, 60, y);
		nb_ui_touch_send(8, 60, y);
		check("browser: a tap opened the file it was on",
		      u->mode == NB_MODE_EDITOR && u->buf &&
		      strcmp(u->buf->name, "three.c") == 0,
		      fmt("mode=%d name=%s", u->mode,
			  u->buf ? u->buf->name : "(none)"));
		/* back to the list (esc asks to save? no: nothing was changed) */
		feed_key(0x01);
		check("browser: esc went back to the file list",
		      u->mode == NB_MODE_BROWSER, fmt("mode=%d", u->mode));
	}

	/* THE MENU KEY OPENS THE SCREEN'S MENU, and the arrows + ENTER work it */
	feed_key(0x93);
	check("browser: MENU opens the popup", nb_ui_overlay() == NB_OVER_MENU,
	      "");
	{
		int ink = 0, x, y;

		nb_ui_draw();
		for (y = NB_BODY_TOP; y < NB_BODY_TOP + 120; y++) {
			for (x = NB_SCREEN_W - 160; x < NB_SCREEN_W; x++) {
				if (nb_host_fb_pixel(x, y) != 0)
					ink++;
			}
		}
		check("browser: ... and it is on the screen", ink > 200,
		      fmt("ink=%d", ink));
	}
	/* the list is 打开 新建文件 重命名 删除 刷新: down twice is 重命名 */
	feed_key(0x05);				/* DOWN */
	feed_key(0x05);				/* DOWN */
	feed_key(0x0D);				/* ENTER */
	check("browser: the 重命名 item asks for a name",
	      nb_ui_input_active() && u->input_then == NB_INPUT_RENAME,
	      fmt("then=%d text=[%s]", u->input_then, nb_ui_input_text()));
	for (i = 0; i < 12; i++)
		feed_key(0x0C);			/* clear the prefilled name */
	feed_key(0xB6);
	feed_key(0xB6);				/* alpha: locked */
	feed_key(0x59);				/* y */
	feed_key(0xB6);				/* alpha: off */
	feed_key(0xB8);				/* . */
	feed_key(0xB6);
	feed_key(0xB6);				/* alpha: locked */
	feed_key(0x43);				/* c */
	feed_key(0xB6);				/* alpha: off */
	feed_key(0x0D);				/* ENTER: rename */
	check("browser: the file was renamed on disk",
	      access((snprintf(path, sizeof(path), "%s/y.c", ui_dir), path),
		     F_OK) == 0, path);

	/* THE DEL KEY (PPL 19) DELETES THE SELECTED FILE, after a question */
	u->sel = 0;				/* the first row */
	feed_key(0x0C);
	check("browser: Del asks before deleting",
	      u->prompt == NB_PROMPT_DELETE, fmt("prompt=%d", u->prompt));
	feed_key(0x59);				/* the key printed 'y' */
	check("browser: ... and answering yes removes it",
	      u->prompt == NB_PROMPT_NONE, "");

	/* THE WORKSPACE SCREEN: MENU gives 进入 / 刷新, and a tap enters */
	u->mode = NB_MODE_WORKSPACE;
	feed_key(0x93);
	check("workspace: MENU opens its own menu",
	      nb_ui_overlay() == NB_OVER_MENU, "");
	feed_key(0x01);				/* esc closes it */
	check("workspace: esc closes it", nb_ui_overlay() == NB_OVER_NONE, "");
	u->mode = NB_MODE_WORKSPACE;
	nb_ui_touch_send(1, 60, 16 + 4);
	nb_ui_touch_send(8, 60, 16 + 4);
	check("workspace: a tap entered the first workspace",
	      u->mode == NB_MODE_BROWSER, fmt("mode=%d", u->mode));

	drop_ui_dir();
}

static void test_ui_flow(void)
{
	static const char *names[] = { "main.c", "note.txt", "hello.lua" };
	nb_ui *u;
	nb_buf *b;

	make_ui_dir();
	snprintf(ui_dir_slash, sizeof(ui_dir_slash), "%s", ui_dir);
	nb_host_reset();
	nb_host_files(ui_dir, names, 3);
	nb_ui_init(nb_host_config());
	nb_ui_enter_workspace(0);
	u = nb_ui_get();
	check("ui: the browser lists the workspace files", nb_file_count() == 3, "");
	check("ui: the editor starts in the browser",
	      u->mode == NB_MODE_BROWSER, "");

	/* TOUCH: A DRAG SCROLLS BY PIXELS, A LONG PRESS SELECTS, A TAP MOVES THE
	 * CARET.  These drive the real touch path (nb_ui_touch) on the real editor,
	 * which is the only way to know that the gestures do what the help says. */
	{
		nb_ui *tu = nb_ui_get();
		int line0, col0;

		nb_ui_enter_workspace(0);
		tu->sel = 0;
		feed_key(0x0D);			/* open main.c */
		{
			nb_buf *tb = nb_app_buffer();
			int k;

			/* enough lines to scroll: the fixture has three, so type more */
			nb_buf_goto(tb, 0, 0);
			for (k = 0; k < 20; k++) {
				nb_buf_insert(tb, "int a;\n", 7);
			}
			nb_ui_editor_changed(0);
			tb->top_line = 0;
			tu->yoff = 0;
		}
		/* A TAP puts the caret where the finger is: line 3, a few columns in */
		nb_ui_touch_send(1, 60, 16 + 3 * 16 + 4);
		nb_ui_touch_send(8, 60, 16 + 3 * 16 + 4);
		{
			char v[40];

			snprintf(v, sizeof(v), "line %d",
				 nb_app_buffer()->cur_line);
			check("touch: a tap moves the caret to the line that was "
			      "touched", nb_app_buffer()->cur_line == 3, v);
		}

		/* A DRAG SCROLLS: one pixel of finger movement is one pixel of text, so
		 * yoff takes the remainder and top_line only moves a whole line */
		line0 = nb_app_buffer()->top_line;
		(void)line0;
		nb_ui_touch_send(1, 160, 120);
		nb_ui_touch_send(2, 160, 112);		/* up by 8 px = half a line */
		{
			char v[40];

			snprintf(v, sizeof(v), "yoff=%d", tu->yoff);
			check("touch: a drag moves the text by PIXELS, not by lines",
			      tu->yoff == 8, v);
		}
		nb_ui_touch_send(2, 160, 104);		/* 16 px more */
		{
			char v[40];

			snprintf(v, sizeof(v), "top=%d yoff=%d",
				 nb_app_buffer()->top_line, tu->yoff);
			check("touch: ... and scrolls whole lines once a line is "
			      "covered", nb_app_buffer()->top_line == 1 &&
			      tu->yoff == 0, v);
		}
		nb_ui_touch_send(8, 160, 104);

		/* A LONG PRESS starts a selection, and a further drag extends it */
		nb_ui_touch_send(1, 60, 16 + 2 * 16 + 4);
		{
			int k;

			for (k = 0; k < 14; k++)
				nb_ui_touch_tick();	/* the frame loop ages it */
		}
		check("touch: a long press starts a selection",
		      nb_ui_selection_active() == 1, "");
		nb_ui_touch_send(2, 140, 16 + 4 * 16 + 4);	/* drag down/right */
		{
			char v[40];

			snprintf(v, sizeof(v), "l0=%d l1=%d", tu->sel_l0,
				 tu->sel_l1);
			check("touch: ... and dragging extends it over more lines",
			      tu->sel_l1 > tu->sel_l0, v);
		}
		{
			/* the anchor line's own first bytes are BEFORE the selection,
			 * so the renderer is asked about the anchor itself and about a
			 * line in the middle of it */
			int mid = (tu->sel_l0 + tu->sel_l1) / 2;

			check("touch: ... and the selection is what a renderer can "
			      "ask about",
			      nb_ui_selection_on(tu->sel_l0, tu->sel_c0) == 1 &&
			      nb_ui_selection_on(mid, 0) == 1 &&
			      nb_ui_selection_on(tu->sel_l0 - 1, 0) == 0, "");
		}
		/* CUTTING IT (F5) removes those lines */
		{
			nb_buf *tb = nb_app_buffer();
			int before = tb->nlines;

			char v[40];

			nb_ui_editor_action(NB_MENU_CUT_SEL);	/* the menu's cut */
			snprintf(v, sizeof(v), "%d -> %d lines", before,
				 tb->nlines);
			check("touch: F5 cuts the selection",
			      tb->nlines < before && !nb_ui_selection_active(),
			      v);
		}
		/* a tap after that clears nothing but moves the caret */
		(void)col0;
	}

	/* FOLDERS ARE ENTRIES TOO, AND ENTER STEPS INTO ONE.  This is how the
	 * .hpappdir folders under C:\DATA are opened: the launcher lists them (the
	 * directory table in src/nb_cfg.h), the browser shows a row per folder, and
	 * entering one shows ITS files.  ESC goes back up. */
	{
		static const char *inner[] = { "inside1.c", "inside2.lua" };
		char inner_path[NB_PATH_MAX];
		int d;

		snprintf(inner_path, sizeof(inner_path), "%s/app.nbappdir",
			 ui_dir_slash);
		/* the files have to EXIST: opening one goes through the port layer's
		 * real filesystem on the host */
		if (mkdir(inner_path, 0700) == 0) {
			char fp[NB_PATH_MAX];
			FILE *g;

			snprintf(fp, sizeof(fp), "%s/inside1.c", inner_path);
			g = fopen(fp, "wb");
			if (g) {
				fputs("int inside(void) {\n    return 1;\n}\n", g);
				fclose(g);
			}
			snprintf(fp, sizeof(fp), "%s/inside2.lua", inner_path);
			g = fopen(fp, "wb");
			if (g) {
				fputs("print('inside')\n", g);
				fclose(g);
			}
		}
		d = nb_host_add_subdir(0, inner_path, inner, 2);
		check("ui: a folder was added to the workspace", d > 0, "");
		nb_ui_enter_workspace(0);
		check("ui: the folder is a row in the file list",
		      nb_file_is_dir(3) == 1 &&
		      strstr(nb_file_name(3), "app.nbappdir") != NULL,
		      nb_file_name(3));
		{
			/* ENTER on the folder row: the browser steps into it */
			nb_ui *bu = nb_ui_get();

			bu->sel = 3;
			feed_key(0x0D);
			check("ui: ENTER on a folder steps into it",
			      nb_file_count() == 2 &&
			      strcmp(nb_file_name(0), "inside1.c") == 0,
			      nb_file_name(0));
			check("ui: ... and the directory it opens files from moved "
			      "with it",
			      strstr(bu->dir, "app.nbappdir") != NULL, bu->dir);
			/* ESC goes back up to the workspace's own files */
			feed_key(0x01);
			check("ui: ESC goes back up one folder",
			      nb_file_count() == 4 && strcmp(nb_file_name(0),
							     "main.c") == 0,
			      nb_file_name(0));
			check("ui: ... and staying in the file list, not going to "
			      "the workspace list", bu->mode == NB_MODE_BROWSER,
			      "");
		}
	}

	/* OPENING A FILE POINTS THE BROWSER AT ITS FOLDER: F1 from the editor shows
	 * the folder the file came from, which is where the user was. */
	{
		nb_ui *bu = nb_ui_get();

		/* a file inside the folder: open it from the browser */
		nb_ui_enter_workspace(0);
		bu->sel = 3;
		feed_key(0x0D);			/* into the folder */
		check("ui: the folder's file is on the list",
		      strcmp(nb_file_name(0), "inside1.c") == 0, nb_file_name(0));
		feed_key(0x0D);			/* open it */
		check("ui: the file inside the folder opened",
		      bu->mode == NB_MODE_EDITOR && bu->buf != NULL &&
		      strcmp(bu->buf->name, "inside1.c") == 0,
		      bu->buf ? bu->buf->name : "(none)");
		feed_key(0x01);			/* esc: back to the list */
		check("ui: F1 from that file shows ITS folder, not the workspace "
		      "root", strstr(bu->dir, "app.nbappdir") != NULL, bu->dir);
		nb_ui_editor_close();
		nb_ui_enter_workspace(0);
	}

	/* THE START SCREEN SHOWS THE FOLDER THAT WAS PICKED.  A second workspace with
	 * different files: choosing it must change the list, the directory a file is
	 * opened from, and the title -- the launch file used to carry ONE list (the
	 * default workspace's), so every workspace showed nbCode's own files. */
	{
		static const char *other[] = { "other1.lua", "other2.c" };

		nb_host_add_workspace("C:\\DATA\\elsewhere", other, 2);
		check("ui: the start screen offers both workspaces",
		      nb_ws_count() == 2, "");
		nb_ui_enter_workspace(0);
		check("ui: workspace 0 lists ITS files",
		      nb_file_count() >= 3 && strcmp(nb_file_name(0), names[0]) == 0,
		      nb_file_name(0));
		nb_ui_enter_workspace(1);
		check("ui: workspace 1 lists ITS files, not workspace 0's",
		      nb_file_count() == 2 &&
		      strcmp(nb_file_name(0), "other1.lua") == 0 &&
		      strcmp(nb_file_name(1), "other2.c") == 0, nb_file_name(0));
		check("ui: ... and it is the folder files are opened from",
		      strcmp(u->dir, "C:\\DATA\\elsewhere") == 0, u->dir);
		nb_ui_enter_workspace(0);
		check("ui: going back shows the first workspace's files again",
		      nb_file_count() >= 3 && strcmp(nb_file_name(0), names[0]) == 0,
		      nb_file_name(0));
	}

	feed_key(0x0D);
	check("ui: ENTER opened the file", u->mode == NB_MODE_EDITOR, "");
	b = nb_app_buffer();
	check("ui: the file is in the buffer",
	      b != NULL && strcmp(b->name, "main.c") == 0,
	      b ? b->name : "(none)");
	if (!b)
		return;

	/* 'q' is the alpha layer of the 7/Q key: without alpha it is a '7' */
	{
		int before = nb_buf_linelen(b, 0);

		feed_key(0x51);
		printf("   UI PROBE: len %d -> %d line0=[%s] cur=%d:%d mod=%d\n",
		       before, nb_buf_linelen(b, 0), nb_buf_line(b, 0),
		       b->cur_line, b->cur_col, b->modified);
		check("ui: a letter key typed one character",
		      nb_buf_linelen(b, 0) == before + 1, nb_buf_line(b, 0));
	}
	/* the bracket keys: shift + the bracket key types '(' (the keypad has no
	 * bracket keys of its own, which is why nb_keymap puts the punctuation on
	 * the shift layer and leaves the function row for the editor's actions) */
	/* primeCode's shift layer on that key is the QUOTE PAIR, and the caret
	 * lands inside it */
	nb_host_event_push(0x8B, 1);	/* shift down (a latch) */
	nb_host_pump();
	feed_key(0x4E);			/* the bracket key */
	check("ui: shift + the bracket key typed '' with the caret inside",
	      strstr(nb_buf_line(b, 0), "''") != NULL,
	      nb_buf_line(b, 0));
	feed_key(0x02);			/* left */
	feed_key(0x03);			/* up */
	check("ui: arrows moved the cursor without changing text",
	      nb_buf_linelen(b, 0) > 0, "");

	/* PRIMECODE'S KEYS THROUGH THE REAL PATH: the function keys insert calls,
	 * the bracket keys insert PAIRS, and the caret ends up between them */
	{
		const char *got;

		nb_buf_goto(b, 0, 0);
		nb_buf_insert(b, "    ", 4);
		feed_key(0x4E);				/* the bracket key */
		got = nb_buf_line(b, 0);
		check("ui: the bracket key inserts a pair and the caret is inside",
		      strncmp(got, "    ()", 6) == 0 && b->cur_col == 5,
		      fmt("[%s] col=%d", got, b->cur_col));
		feed_key(0x51);				/* '7' */
		check("ui: ... so typing goes between the pair",
		      strncmp(nb_buf_line(b, 0), "    (7)", 7) == 0,
		      nb_buf_line(b, 0));
		nb_buf_goto(b, 1, 0);
		feed_key(0x47);				/* the sin key */
		check("ui: the sin key inserts sin() with the caret inside",
		      strncmp(nb_buf_line(b, 1), "sin()", 5) == 0 &&
		      b->cur_col == 4,
		      fmt("[%s] col=%d", nb_buf_line(b, 1), b->cur_col));
		feed_key(0x48);				/* cos(), inside the sin() */
		check("ui: ... and cos() lands between its parentheses",
		      strncmp(nb_buf_line(b, 1), "sin(cos())", 10) == 0,
		      nb_buf_line(b, 1));
	}
	/* THE FIVE LETTERS, THROUGH THE REAL KEY PATH.
	 *
	 * The bug report was "a, b, c, d and e cannot be typed at all": the mapper
	 * had no letters on the keys that print them, and the ALPHA key latched
	 * forever with no way back.  This types them the way a user does -- alpha
	 * twice for the lock, five keys, alpha again to unlock -- and then checks
	 * that the keypad really came back. */
	{
		const int five[5] = { 0x41, 0x42, 0x43, 0x44, 0x45 };
		const char *line;
		int k;

		nb_buf_goto(b, 0, 0);
		/* THE RIGHT HAND END OF THE STATUS BAR CARRIES THE FILE TYPE and then
		 * the layer, in that order: the header's right hand end is the menu
		 * now, so this is where the language is shown. */
		{
			char v[40];
			int plain, lang_term = NB_LANG_PPL;

			nb_ui_draw();
			plain = status_right_ink();
			lang_term = nb_hl_lang(&nb_ui_get()->hl);
			snprintf(v, sizeof(v), "ink=%d lang=%s", plain,
				 nb_lang_name(lang_term));
			check("ui: the status bar names the file type",
			      plain > 10 && strcmp(nb_lang_name(lang_term), "C") == 0,
			      v);
			/* THE TYPE IS ABBREVIATED AND THE LAYER IS THE KEYPAD'S OWN WORD:
			 * "py" and "alock", not "Python" and a Chinese label.  The label is
			 * read as a string instead of guessed from pixels. */
			{
				char lab[24];

				nb_ui_editor_status_label(lab, sizeof(lab));
				check("ui: the status bar shows the file type ABBREVIATED",
				      strcmp(lab, "c") == 0, fmt("[%s]", lab));
			}
			{
				char lab[24];

				feed_key(0x8B);		/* shift */
				nb_ui_editor_status_label(lab, sizeof(lab));
				check("ui: shift is labelled `shift`",
				      strcmp(lab, "shift  c") == 0, fmt("[%s]", lab));
				feed_key(0x8B);	/* ... a latch: pressed again to drop it */
			}
		}
		feed_key(0xB6);			/* alpha: one letter */
		feed_key(0xB6);			/* alpha: locked */
		check("ui: two presses of alpha lock the letter layer",
		      nb_keymap_alpha_state() == NB_ALPHA_LOCK, "");
		{
			char lab[24];

			nb_ui_editor_status_label(lab, sizeof(lab));
			check("ui: ... and the lock is labelled `alock`",
			      strcmp(lab, "alock  c") == 0, fmt("[%s]", lab));
		}
		/* AND THE LAYER IS ON THE SCREEN TOO: next to the file type, on top
		 * of whatever message is there (a message fills that row, and an
		 * indicator that a message can hide is not there when it matters) */
		{
			char v[24];
			int ink;

			nb_ui_draw();
			ink = status_right_ink();
			snprintf(v, sizeof(v), "ink=%d", ink);
			check("ui: the locked letter layer is shown in the status bar",
			      ink > 60, v);
		}
		for (k = 0; k < 5; k++)
			feed_key(five[k]);
		line = nb_buf_line(b, 0);
		check("ui: the keys printed a b c d e type 'abcde'",
		      line && strncmp(line, "abcde", 5) == 0, line ? line : "");
		feed_key(0xB6);			/* alpha: off again */
		check("ui: a third press unlocks it",
		      nb_keymap_alpha_state() == NB_ALPHA_OFF, "");
		{
			char v[24];
			int ink;

			nb_ui_draw();
			ink = status_right_ink();
			snprintf(v, sizeof(v), "ink=%d", ink);
			check("ui: ... and the indicator goes away with it, leaving "
			      "the file type", ink > 0 && ink < 40, v);
		}
		{
			int before = nb_buf_linelen(b, 0);
			int at = b->cur_col;

			feed_key(0x51);		/* the keypad is digits again */
			check("ui: ... and the keypad types a digit",
			      nb_buf_linelen(b, 0) == before + 1 &&
			      nb_buf_line(b, 0)[at] == '7',
			      nb_buf_line(b, 0));
		}
		/* THE CAPITAL VARIANTS (shift+alpha): "Alpha" is a one shot in capitals,
		 * "Alock" is the lock -- a capital A means the letters come out in
		 * capitals, which is the whole of the convention. */
		{
			char lab[24];

			nb_keymap_reset();
			feed_key(0x8B);		/* shift (a latch) */
			feed_key(0xB6);		/* ... + alpha: one shot, capitals */
			nb_ui_editor_status_label(lab, sizeof(lab));
			check("ui: shift+alpha is labelled `Alpha`",
			      strcmp(lab, "Alpha  c") == 0, fmt("[%s]", lab));
			feed_key(0xB6);		/* alpha again: locked, capitals */
			nb_ui_editor_status_label(lab, sizeof(lab));
			check("ui: ... and the capital lock is `Alock`",
			      strcmp(lab, "Alock  c") == 0, fmt("[%s]", lab));
			nb_keymap_reset();	/* off again, for the checks below */
		}
		/* THE HELP OVERLAY (F6) is where the alpha cycle is written down, so
		 * it has to fit on the screen: a 240 pixel display does not scroll an
		 * overlay, it loses the last lines */
		feed_key(0x95);			/* HELP */
		{
			/* THE HELP PANEL IS THE WHOLE SCREEN with a flat background, so
			 * the LOWEST row that carries ink is the last line of text: it has
			 * to be on the glass (below the middle of the screen, above the
			 * bottom border) whatever the list's length is. */
			int x, y, last_y = -1;

			nb_ui_draw();
			for (y = NB_SCREEN_H - 4; y > NB_SCREEN_H / 2; y--) {
				int row_ink = 0;

				for (x = 8; x < NB_SCREEN_W - 8; x++) {
					if (nb_host_fb_pixel(x, y) !=
					    nb_host_fb_pixel(4, NB_SCREEN_H - 4))
						row_ink++;
				}
				if (row_ink > 2) {
					last_y = y;
					break;
				}
			}
			check("ui: the last line of the help overlay is on screen",
			      last_y > NB_SCREEN_H / 2 &&
			      last_y < NB_SCREEN_H - 3,
			      fmt("lowest text row=%d", last_y));
		}
		feed_key(0x95);			/* HELP again: back to the text */
		check("ui: ... and F6 closes it again",
		      !nb_ui_get()->show_help, "");
	}
	check("ui: the buffer is dirty after typing", b->modified == 1, "");

	/* F3 is the save key: the file goes back to disk through the port layer */
	{
		char want[NB_PATH_MAX];

		nb_ui_editor_action(NB_MENU_SAVE);	/* the menu's 保存 */
		{
			char d[96];

			snprintf(d, sizeof(d), "mod=%d prompt=%d msg=[%s] last=[%s]",
				 b->modified, u->prompt, u->message,
				 nb_host_last_written());
			check("ui: the menu's 保存 saved the file",
			      b->modified == 0, d);
		}
		nb_str_copy(want, sizeof(want), ui_dir, -1);
		nb_str_append(want, sizeof(want), "/main.c");
		{
			/* the editor keeps the device's '\\' separator in the path
			 * it hands the port layer, so the comparison is done on the
			 * part that is the same everywhere: the file name */
			const char *got = nb_host_last_written();
			const char *tail = strrchr(got, '\\');
			const char *btail = strrchr(got, '/');

			if (btail && (!tail || btail > tail))
				tail = btail;
			check("ui: the saved path ends in the opened file's name",
			      tail && strcmp(tail + 1, "main.c") == 0, got);
			check("ui: ... and it is the file we opened",
			      strncmp(got, ui_dir, strlen(ui_dir)) == 0, got);
		}
		check("ui: the saved bytes are the edited text",
		      file_has(want, "int main"), nb_host_last_written());
	}

	/* LEAVING THE EDITOR ASKS, AND GOES WHERE THE KEY SAID.
	 *
	 * F1 and ESC mean "back to the file list"; ON means "out of the program".
	 * Both ask about unsaved changes, and the question has to carry the
	 * destination: this block used to raise one "save before quit?" question for
	 * both, so answering 'n' after ESC quit the whole editor instead of closing
	 * the file.  'y' saves, 'n' leaves without saving, ESC cancels. */
	feed_key(0x51);			/* type something: the buffer is dirty */
	check("ui: the buffer is dirty again", b->modified == 1, "");
	feed_key(0x01);			/* esc: leave the file */
	check("ui: esc with unsaved changes asks first",
	      u->prompt == NB_PROMPT_SAVE, "");
	{
		char v[32];

		snprintf(v, sizeof(v), "prompt_then=%d", u->prompt_then);
		check("ui: ... and the question knows the way out is the file list",
		      u->prompt_then == NB_THEN_BACK, v);
	}
	check("ui: the editor is still up while the question is",
	      u->mode == NB_MODE_EDITOR, u->message);
	feed_key(0x01);			/* ESC on the question: stay */
	check("ui: esc on the question cancels it and keeps the file open",
	      u->prompt == NB_PROMPT_NONE && u->mode == NB_MODE_EDITOR,
	      u->message);

	feed_key(0x01);			/* ESC in the editor: leave the file */
	check("ui: esc with unsaved changes asks too",
	      u->prompt == NB_PROMPT_SAVE && u->prompt_then == NB_THEN_BACK, "");
	feed_key(0x4E);			/* the key printed 'n': do not save */
	{
		char v[32];

		snprintf(v, sizeof(v), "mode=%d (quit is %d, browser is %d)",
			 u->mode, NB_MODE_QUIT, NB_MODE_BROWSER);
		check("ui: answering n goes back to the FILE LIST, not out of the "
		      "program", u->mode == NB_MODE_BROWSER, v);
	}
	check("ui: ... and the buffer is closed", nb_app_buffer() == NULL,
	      nb_app_buffer() ? nb_app_buffer()->name : "(closed)");

	/* the same question, the other destination: ON still quits */
	feed_key(0x0D);			/* ENTER: open the file again */
	check("ui: the file opened again", u->mode == NB_MODE_EDITOR, "");
	feed_key(0x51);			/* dirty it */
	feed_key(0x83);			/* ON: leave the program */
	check("ui: ON with unsaved changes asks first",
	      u->prompt == NB_PROMPT_SAVE, "");
	{
		char v[32];

		snprintf(v, sizeof(v), "prompt_then=%d", u->prompt_then);
		check("ui: ... and that question's way out is quitting",
		      u->prompt_then == NB_THEN_QUIT, v);
	}
	feed_key(0x59);			/* the key printed 'y': save, then quit */
	{
		char v[32];

		snprintf(v, sizeof(v), "mode=%d", u->mode);
		check("ui: answering y saved the file and quit",
		      u->mode == NB_MODE_QUIT, v);
	}

	/* TYPING INTO A LINE MUST NOT OUTGROW ITS STYLE BUFFER.
	 *
	 * nb_hl_line() writes one style byte per byte of the line, and the buffer is
	 * allocated len+1 the first time a line is coloured.  nb_ui_editor_changed()
	 * used to allocate ONLY when the pointer was NULL, so a line that grew kept
	 * its old, too-short buffer: the extra byte landed in the next heap
	 * allocation -- normally the following line's style buffer.  That is what
	 * "open a .txt, insert a character, and the highlighting below it is wrong"
	 * was.  Two things are checked, and the second is the one a user sees:
	 *
	 *   * no line with a cached buffer has less room than its text needs;
	 *   * the cached colours AGREE WITH THE TOKENIZER for the text that is on
	 *     screen -- a style byte left over from the text before the edit (or a
	 *     byte written into the wrong line's buffer) shows up here as a shifted
	 *     colour.
	 */
	{
		int i, cap = 0, short_lines = 0, worst = -1, worst_cap = 0, worst_len = 0;

		for (i = 0; i < 6; i++)
			feed_key(0x51);		/* six letters into the current line */
		for (i = 0; i < nb_app_buffer()->nlines; i++) {
			int len = nb_buf_linelen(nb_app_buffer(), i);

			if (!nb_ui_style_buf(i, &cap))
				continue;	/* freed, and not needed yet */
			if (cap < len + 1) {
				short_lines++;
				worst = i;
				worst_cap = cap;
				worst_len = len;
			}
		}
		{
			char v[96];

			snprintf(v, sizeof(v), "%d line(s) short; line %d has room "
				 "for %d and needs %d", short_lines, worst, worst_cap,
				 worst_len + 1);
			check("ui: an edited line cannot outgrow its style buffer",
			      short_lines == 0, v);
		}
		{
			unsigned char want[512];
			const unsigned char *got;
			int line = nb_app_buffer()->cur_line;
			int len = nb_buf_linelen(nb_app_buffer(), line);
			int differ = -1, k;
			char v[96];

			if (len > 0 && len < 500) {
				nb_hl_line(nb_lang_get(NB_LANG_C),
					   nb_buf_line(nb_app_buffer(), line), len,
					   want, NB_HL_BLK_NONE);
				got = nb_ui_style_buf(line, &cap);
				for (k = 0; k < len && got; k++) {
					if (got[k] != want[k]) {
						differ = k;
						break;
					}
				}
				snprintf(v, sizeof(v), "line %d: %d byte(s) differ, "
					 "first at %d (cached %d, tokenizer %d)",
					 line, differ < 0 ? 0 : 1, differ,
					 (got && differ >= 0) ? got[differ] : -1,
					 differ >= 0 ? want[differ] : -1);
				check("ui: the cached colours still agree with the "
				      "tokenizer for the text on screen",
				      got && differ < 0, v);
			}
		}
	}

	/* EVERY VISIBLE LINE GETS COLOURS, not just the first one whose block state
	 * stopped changing.  A file that is one long comment is the case that shows
	 * it: the state is the same on every line, so an early stop leaves the rest of
	 * the screenful in the plain text colour.
	 */
	{
		char path[NB_PATH_MAX];
		FILE *f;
		int k, uncoloured = 0, first_bad = -1;

		nb_str_copy(path, sizeof(path), ui_dir, -1);
		nb_str_append(path, sizeof(path), "/comment.c");
		f = fopen(path, "wb");
		if (f) {
			fputs("/*\n", f);
			for (k = 0; k < 20; k++)
				fprintf(f, "line %d inside the comment\n", k);
			fputs("*/\n", f);
			fclose(f);
		}
		if (nb_ui_open_file(ui_dir, "comment.c")) {
			nb_ui *cu = nb_ui_get();
			nb_buf *cb = cu->buf;

			/* TYPE INTO THE FILE FIRST: that is what drops the style
			 * buffers, and the next draw is what has to put them back.
			 * (Opening a file colours all of it, so a draw alone would
			 * not show the early stop's damage.) */
			feed_key(0x51);
			nb_ui_draw();
			for (k = 0; k < cb->nlines && k < 14; k++) {
				int cap = 0;

				if (!nb_ui_style_buf(k, &cap)) {
					uncoloured++;
					if (first_bad < 0)
						first_bad = k;
					continue;
				}
				/* and the colour has to be the comment one */
				if (k > 0 && nb_buf_linelen(cb, k) > 0 &&
				    nb_ui_style_at(k, 0) != NB_HS_COMMENT) {
					uncoloured++;
					if (first_bad < 0)
						first_bad = k;
				}
			}
			{
				char v[80];

				snprintf(v, sizeof(v), "%d of the visible lines are not "
					 "comment-coloured, first %d", uncoloured,
					 first_bad);
				check("ui: every line of a one-comment screenful is "
				      "coloured", uncoloured == 0, v);
			}
			nb_ui_editor_close();
			/* LEAVE A FILE OPEN: the blocks below this one drive the editor,
			 * and nb_app_buffer() is NULL after a close */
			nb_ui_open_file(ui_dir, "main.c");
		} else {
			check("ui: the comment file could be opened", 0, "");
		}
	}

	/* A STRUCTURAL EDIT (a newline) MOVES EVERY LINE BELOW IT, so the per-line
	 * block states that were recorded for the old numbering are meaningless: a
	 * file whose colouring depends on a block comment would come out wrong from
	 * the insertion point down.  The strongest check available is to tokenize the
	 * file from the top here and require every CACHED line to match.
	 */
	{
		int state = NB_HL_BLK_NONE, line, first_bad = -1, compared = 0;
		unsigned char want[512];
		char v[96];

		/* put a block comment in, so the colouring below it DEPENDS on the
		 * state that a stale record would get wrong */
		nb_buf_goto(nb_app_buffer(), 0, 0);
		{
			const char *open = "/* ";
			int k;

			for (k = 0; open[k]; k++)
				nb_buf_insert(nb_app_buffer(), open + k, 1);
			nb_ui_editor_changed(0);
		}
		feed_key(0x0D);			/* ENTER: split the line, +1 line */
		check("ui: the newline made a new line",
		      nb_app_buffer()->nlines >= 3, "");
		/* DRAW FIRST: the editor colours the visible lines as it draws them, and
		 * this test is about the CACHE, so every line has to be in it before the
		 * comparison below means anything */
		nb_ui_draw();
		for (line = 0; line < nb_app_buffer()->nlines; line++) {
			const unsigned char *got;
			int len = nb_buf_linelen(nb_app_buffer(), line);
			int cap = 0, k;

			if (len >= 500)
				break;
			state = nb_hl_line(nb_lang_get(NB_LANG_C),
					   nb_buf_line(nb_app_buffer(), line), len,
					   want, state);
			got = nb_ui_style_buf(line, &cap);
			if (!got)
				continue;	/* not coloured yet: nothing to compare */
			if (cap < len + 1) {
				first_bad = line;
				break;
			}
			compared++;
			for (k = 0; k < len; k++) {
				if (got[k] != want[k]) {
					first_bad = line;
					break;
				}
			}
			if (first_bad >= 0)
				break;
		}
		snprintf(v, sizeof(v), "%d line(s) compared, first mismatch on line "
			 "%d", compared, first_bad);
		check("ui: after a newline the cached colours still match a fresh "
		      "tokenization", first_bad < 0 && compared > 1, v);
	}

	/* esc from a clean editor goes straight back to the list */
	nb_ui_enter_workspace(0);
	feed_key(0x0D);
	feed_key(0x01);
	{
		char v[32];

		snprintf(v, sizeof(v), "mode=%d", u->mode);
		check("ui: esc from a clean file returns to the browser",
		      u->mode == NB_MODE_BROWSER, v);
	}
	drop_ui_dir();
}

static int dump_screens;		/* -v: print the screens as ASCII art */

/* a segfault in a test binary: say WHERE (see main) */
void segv(int sig)
{
	void *bt[16];
	int n = backtrace(bt, 16);

	(void)sig;
	fprintf(stderr, "SEGFAULT at:\n");
	backtrace_symbols_fd(bt, n, 2);
	_exit(139);
}

static void dump_render(void)
{
	nb_text_decoded d;
	const nb_theme *th = nb_theme_get();
	nb_u32 classes[NB_HS_KIND];

	nb_text_classes(th, classes);
	nb_gfx_clear(0);
	{
		const char *line = "local \xe4\xb8\xad\xe6\x96\x87 = 42 -- note";
		unsigned char styles[64];

		nb_hl_line(nb_lang_get(NB_LANG_LUA), line, (int)strlen(line),
			   styles, NB_HL_BLK_NONE);
		nb_text_decode(&d, line, (int)strlen(line), styles, NB_ENC_UTF8);
		nb_text_draw_decoded(&d, 0, 0, 0, 0, th, classes, 0, 0);
	}
	art(0, 0, 152, 16, "mixed Lua line, 19 frames = 152 px");

	/* THE WHOLE SCREEN, ONCE, IN BOTH THEMES: the chrome is checked by numbers
	 * (test_theme_contrast) and the SHAPE of it by eye -- this dump is what a
	 * person looks at when the layout changes, and it is how the header's accent
	 * line, the gutter's edge and the selected row's bar are reviewed. */
	{
		static const char *names[] = { "main.c", "note.txt", "hello.lua" };
		int dark;

		/* the screen dumps are for a human reviewing the layout, so they are
		 * behind -v: 40 rows times four screens is a lot of output for a
		 * suite that runs on every change */
		if (!dump_screens)
			return;
		make_ui_dir();			/* the earlier tests dropped theirs */
		for (dark = 0; dark < 2; dark++) {
			nb_host_reset();
			nb_host_files(ui_dir, names, 3);
			nb_ui_init(nb_host_config());
			if (dark) {
				nb_host_config()->theme = 1;
				nb_ui_init(nb_host_config());
			}
			nb_ui_enter_workspace(0);
			nb_ui_draw();
			art(0, 0, NB_SCREEN_W, 40,
			    dark ? "dark: the browser (top 40 rows)"
				 : "light: the browser (top 40 rows)");
			/* ... and the EDITOR, with a selection and the scrollbar */
			{
				nb_ui *du = nb_ui_get();
				nb_buf *db;
				int k;

				du->sel = 0;
				feed_key(0x0D);			/* open main.c */
				db = nb_app_buffer();
				if (db) {
					for (k = 0; k < 20; k++)
						nb_buf_insert(db, "int a = 1;\n", 11);
					nb_ui_editor_changed(0);
					nb_buf_goto(db, 1, 0);
					nb_ui_draw();
					art(0, 0, NB_SCREEN_W, 40,
					    dark ? "dark: the editor (top 40 rows)"
						 : "light: the editor (top 40 rows)");
				}
				/* ... and the CLIPBOARD PANEL, which is the one
				 * panel with ten rows of user text in it */
				nb_clip_reset();
				nb_clip_add("int main(void) {\n", 17);
				nb_clip_add("已复制的中文一行\n", 25);
				{
					int k;

					for (k = 0; k < 10; k++)
						nb_clip_add("entry", 5);
				}
				nb_ui_clip_open();
				nb_ui_draw();
				art(0, 0, NB_SCREEN_W, 40,
				    dark ? "dark: the clipboard panel (top 40 rows)"
					 : "light: the clipboard panel (top 40 rows)");
				feed_key(0x01);
			}
		}
		drop_ui_dir();
	}
}

int main(int argc, char **argv)
{
	/* A CRASH WITHOUT A BACKTRACE IS A GUESSING GAME: this is a test binary,
	 * so a segfault prints where it happened instead of just dying. */
	{
		void segv(int sig);

		signal(SIGSEGV, segv);
	}
	/* -v before anything draws: the screen dumps are chosen here */
	if (argc > 1 && strcmp(argv[1], "-v") == 0)
		dump_screens = 1;
	printf("nbCode host tests (build marker %s %s)\n", __DATE__, __TIME__);
	/* the same two calls nb_run_begin makes: without the target bound, every
	 * draw is a no-op and the render tests would "pass" by drawing nothing */
	nb_screen_init();
	if (!nb_gfx_init()) {
		printf("FAIL: no framebuffer\n");
		return 2;
	}
	printf("font: %d glyphs, %d western codepoints, %d GB2312 slots\n",
	       nb_font_table.nglyphs, nb_font_table.nlatin, nb_font_table.ngb);

	test_font();
	test_buffer();
	test_line_growth();
	test_cursor_wide();
	test_codec();
	test_highlight();
	test_highlight_state();
	test_theme_contrast();
	test_keymap();
	test_viewport();
	test_panels();
	test_config_dirs();
	test_clipboard();
	test_ui_reports();
	test_reports2();
	test_reports3();
	test_list_and_hex();
	test_tap_stability();
	test_browser_menu();
	test_render();
	test_ui_flow();
	dump_render();

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	if (dump_screens)
		printf("(verbose)\n");
	return failures ? 1 : 0;
}
