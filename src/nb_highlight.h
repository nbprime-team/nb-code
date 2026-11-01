/*
 * nb_highlight.h -- syntax colouring, one style byte per source byte.
 *
 * The output is a style byte per BYTE of the line (nb_hs_*), not per character:
 * a Chinese character is two or three style bytes with the same value, which
 * costs the same memory as a per-character array and needs no mapping back to
 * cursor columns.  A 1000 byte line is 1000 bytes, allocated once per line.
 *
 * THE MULTI-LINE PROBLEM, AND THE CHEAP FIX
 * A block comment, a Lua long string or a Python triple quote spans lines, so
 * line N's colouring depends on the state at the end of line N-1.  The classic
 * answers are "reparse the whole file on every keystroke" (a visible stall on
 * this CPU) or "keep a full state per line" (memory).  nbCode keeps one BYTE per
 * line -- the block state that line STARTS in -- so re-highlighting can begin
 * at any line, and an edit only re-colours from the line it touched, plus the
 * lines after it while the block state keeps changing.  Typing `--[[` therefore
 * repaints the rest of the file once, and typing a letter repaints one line.
 *
 * LANGUAGE PROFILES are tables, not code paths: nb_lang_get() picks one from the
 * file name (setting.json's "language" overrides it) and the same tokenizer with
 * different punctuation, comments and keyword sets serves all of them.
 */
#ifndef NB_HIGHLIGHT_H
#define NB_HIGHLIGHT_H

#include "nb_common.h"

struct nb_buf;				/* nb_buffer.h: only pointers here */

/* the style classes, in the order the themes define them */
enum {
	NB_HS_TEXT = 0,		/* plain code */
	NB_HS_KEY,		/* declaration keyword: local, def, EXPORT ... */
	NB_HS_CTRL,		/* control flow: if, for, end, return ... */
	NB_HS_CONST,		/* number, true/false/nil/None */
	NB_HS_STRING,		/* quotes and long strings */
	NB_HS_OP,		/* operators, punctuation */
	NB_HS_LIB,		/* standard library call: print, AFiles ... */
	NB_HS_COMMENT,		/* line and block comments */
	NB_HS_PREPROC,		/* #pragma / #include / @decorator */
	NB_HS_KIND		/* count: one theme entry per class */
};

/* one language's punctuation and keyword sets (the keyword strings are space
 * separated lists; see nb_highlight.c) */
typedef struct nb_lang_profile {
	nb_lang lang;
	const char *name;		/* the full name (information panel) */
	const char *abbrev;		/* the short one (status bar): PY, C, Lua */

	int line_comment_slash;		/* 1 = "//" starts a comment */
	int line_comment_dash;		/* 1 = "--" starts a comment */
	int line_comment_hash;		/* 1 = "#" starts a comment */
	int block_c_style;		/* 1 = C block comments */
	int block_lua_long;		/* 1 = Lua long brackets, comment + string */
	int py_triple;			/* 1 = Python triple quoted strings */
	int ppl_syntax_at;		/* 1 = a leading '#' makes the line
					 * firmware syntax, not code */

	int string_dquote, string_squote;
	int escape_backslash;

	const char *keywords;		/* NB_HS_KEY */
	const char *control;		/* NB_HS_CTRL */
	const char *constants;		/* NB_HS_CONST */
	const char *libs;		/* NB_HS_LIB */
} nb_lang_profile;

const nb_lang_profile *nb_lang_get(nb_lang lang);
nb_lang nb_lang_from_name(const char *name);		/* by extension */
const char *nb_lang_name(nb_lang lang);
/* THE STATUS BAR HAS ROOM FOR FOUR CHARACTERS, NOT FOR "Python": the short name
 * is what the file type indicator uses (see nb_ui_editor.c).  It is also what the
 * file type is CALLED by everyone who edits it. */
const char *nb_lang_abbrev(nb_lang lang);

/* the block kinds a line can start in (the value stored per line) */
#define NB_HL_BLK_NONE		0
#define NB_HL_BLK_C		1
#define NB_HL_BLK_LUA_LONG	2
#define NB_HL_BLK_PY_TRIPLE_D	3
#define NB_HL_BLK_PY_TRIPLE_S	4
#define NB_HL_BLK_LUA_COMMENT	5	/* --[[ ... ]]: a long bracket that is
					 * a COMMENT, which the continuation
					 * lines have to know (they are grey,
					 * not string-coloured) */

/* set by nb_hl_init() and checked by nb_hl_open(): it is what makes it safe to
 * call nb_hl_open() on a highlighter that has never been initialised (the struct
 * lives inside the UI, which is zeroed at startup, but a stack one is not) */
#define NB_HL_MAGIC	0x484C4248u	/* "HLBH" */

typedef struct nb_hl {
	unsigned int magic;
	const nb_lang_profile *prof;
	const struct nb_buf *buf;	/* only for bookkeeping */
	int last_line;			/* last line whose state was recorded */
	int state_valid;
	unsigned char *state_line;	/* one byte per line: the state it
					 * starts with (+1 slot) */
	int state_cap;
} nb_hl;

void nb_hl_init(nb_hl *h, const nb_lang_profile *prof);
void nb_hl_free(nb_hl *h);

/* A FILE WAS OPENED: pick its language, forget every cached per-line state, and
 * KEEP the state array (it is sized to a file, and allocating a fresh one per
 * open both leaked the old one and left the new one uninitialised -- which
 * painted whole files in the string colour).  This is the call the editor makes;
 * nb_hl_init() is only for a struct that has never been used. */
void nb_hl_open(nb_hl *h, const nb_lang_profile *prof,
		const struct nb_buf *buf);

/* a new file was opened into `buf`: forget every cached state */
void nb_hl_reset(nb_hl *h, const struct nb_buf *buf);

/* THE LINE NUMBERING BELOW `line` CHANGED (a newline went in or came out).  The
 * record for `line` itself is still right -- a split line starts in the state its
 * first half did -- and every record below it is meaningless now, so the next
 * nb_hl_lines() re-colours from here to the bottom. */
void nb_hl_shift(nb_hl *h, int line);

nb_lang nb_hl_lang(const nb_hl *h);

/* THE HIGH LIGHTER OWNS THE STYLE BUFFERS' SIZES.  `styles[i]` holds one byte per
 * byte of line i, and `caps[i]` says how many bytes that buffer can hold --
 * nb_hl_lines() grows it when the line needs more.  THE CAPACITY ARRAY IS NOT
 * OPTIONAL POLITENESS: without it, a line that GREW kept its old, too-small
 * buffer, and the extra style byte went into the next heap allocation -- usually
 * the following line's buffer -- so typing one character into a .txt file made
 * the colours on the line below shift.  Every caller passes a caps array that it
 * zeroes when it frees a buffer. */
/* Colour one line into `styles` (which must hold len+1 bytes) and return the
 * block state the line ENDS in.  No cache, no allocation: this is the unit the
 * host tests drive. */
int nb_hl_line(const nb_lang_profile *prof, const char *text, int len,
	       unsigned char *styles, int in_block);

/* Colour lines [first..last].  `styles` is one pointer per line (NULL entries
 * are allocated).  With stop_when_stable set, the walk stops as soon as the
 * block state stops changing -- the keystroke path. */
void nb_hl_lines(nb_hl *h, const struct nb_buf *buf, unsigned char **styles,
		 int *caps, int first, int last, int stop_when_stable);

#endif /* NB_HIGHLIGHT_H */
