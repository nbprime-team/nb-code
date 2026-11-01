/*
 * nb_codec.h -- file bytes <-> text: detection, decoding, re-encoding.
 *
 * See nb_codec.c for why detection has to be this careful (a GB2312 tree that
 * silently becomes UTF-8 is data damage).
 */
#ifndef NB_CODEC_H
#define NB_CODEC_H

#include "nb_common.h"
#include "nb_font.h"

/* how many bytes the character at p occupies (1..4).  `avail` is what is left
 * in the string, so a truncated sequence at the end of a line is one byte and
 * cannot walk off it. */
int nb_codec_utf8_seq(const unsigned char *p, int avail);

/* UTF-8 (or a lone high byte) -> codepoint; *len receives the bytes consumed.
 * A malformed sequence yields 0xFFFD, which the renderer draws as a box. */
unsigned int nb_codec_utf8_decode(const unsigned char *p, int *len);

/* codepoint -> UTF-8 bytes; returns the byte count, 0 if it does not fit */
int nb_codec_encode(unsigned int cp, char *out, int cap);

/* codepoint -> bytes in a FILE's encoding (GB2312 gets its pair, anything
 * unrepresentable becomes '?').  This is what the character palette inserts. */
int nb_codec_char_to_bytes(unsigned int cp, nb_encoding enc, char *out,
			   int cap);

/* what the file at `raw` looks like.  *bom_len (optional) receives the number
 * of leading bytes that are a BOM and must not become text. */
nb_encoding nb_codec_detect(const char *raw, int len, int *bom_len);

/* Same test, but for text the editor is HOLDING rather than a file on disk.
 * On save this decides whether a buffer that was opened as UTF-8 has since
 * acquired bytes that only make sense as GB2312 (the user typed Chinese, or
 * pasted it from a GB2312 file).  Without it, opening a fresh ASCII file and
 * typing Chinese would write a file that no other tool on the calculator can
 * read. */
nb_encoding nb_codec_detect_text(const char *raw, int len);

/* the encoding a NEW file should be saved as, from its name */
nb_encoding nb_codec_for_name(const char *name);

/* The line splitter: hands each line to `sink`, in order, without copying.
 * Returns the number of lines, or -1 when the sink said stop. */
typedef int (*nb_line_sink)(void *ctx, const char *text, int len, int index);
int nb_codec_split(const char *raw, int len, nb_line_sink sink, void *ctx,
		   nb_encoding enc, int *lines_out);

/* one line's text -> bytes in `enc` (always NUL safe: the caller sizes cap).
 * *lossy receives the number of characters that had no representation. */
int nb_codec_line_to_bytes(const char *text, int len, nb_encoding enc,
			   char *out, int cap, int *lossy);

#endif /* NB_CODEC_H */
