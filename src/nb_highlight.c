/*
 * nb_highlight.c -- the syntax colouring tokenizer.
 *
 * One pass per line, byte in / style byte out, with the multi-line state passed
 * in and out as an int (NB_HL_BLK_*).  The tokenizer knows nothing about themes:
 * it emits NB_HS_* classes and nb_text.c maps them to colours, so a new theme is
 * a table and not a change here.
 *
 * What it recognises
 *   * line comments in three dialects ("//", "--", "#") and C style block
 *     comments, including nesting;
 *   * Lua long brackets: --[[ ]] as a comment, [[ ]] as a string, with levels;
 *   * Python triple quoted strings, and decorators;
 *   * strings with backslash escapes, and the fact that a quote inside a comment
 *     is not a string -- the bug every naive highlighter has;
 *   * words: keywords, control flow, constants and library names, classified by
 *     table with a small memo in front;
 *   * numbers: 12, 0x1F, 1.5e-3, 1_000, 3.14f;
 *   * operators and punctuation;
 *   * CJK and every other high byte as opaque TEXT: a Chinese identifier must
 *     not break the tokenizer and a Chinese comment must stay a comment.
 */
#include "nb_highlight.h"
#include "nb_buffer.h"

/* ---------------------------------------------------------------------------
 * the profiles
 * ------------------------------------------------------------------------- */
static const nb_lang_profile PROF_PPL = {
	NB_LANG_PPL, "PPL", "ppl",
	1, 0, 0,			/* "//" comments */
	1, 0, 0,			/* C block comments */
	1,				/* a leading # is firmware syntax */
	1, 1, 1,
	/* declarations */
	"EXPORT LOCAL CONST VIEW SYMB PLOT NUM CASE IFERR LAMBDA "
	"export local const view symb plot num case iferr lambda",
	/* control flow */
	"IF THEN ELSE END FOR FROM TO STEP DO WHILE REPEAT UNTIL BREAK "
	"RETURN KILL BEGIN TRY CATCH AND OR NOT XOR "
	"if then else end for from to step do while repeat until break "
	"return kill begin try catch and or not xor",
	/* constants */
	"TRUE FALSE PI EULER INF NAN true false pi euler inf nan",
	/* library calls */
	"PRINT MSGBOX INPUT AFiles Programs TEXTOUT RECT LINE PIXON PIXOFF "
	"MAKEMAT MAKELIST SIZE DIM EDITMAT SUBMAT INVERT DET RANK TRN "
	"EVAL GETKEY TICKS WAIT BEEP RANDOM "
	"print msgbox input AFiles Programs textout rect line pixon pixoff "
	"makemat makelist size dim editmat submat invert det rank trn "
	"eval getkey ticks wait beep random"
};

static const nb_lang_profile PROF_LUA = {
	NB_LANG_LUA, "Lua", "lua",
	0, 1, 0,			/* "--" comments */
	0, 1, 0,			/* Lua long brackets */
	0,
	1, 1, 1,
	"local function return end then do repeat until in "
	"local function return end then do repeat until in",
	"if else elseif while for break goto and or not in "
	"if else elseif while for break goto and or not in",
	"true false nil _G _VERSION true false nil _G _VERSION",
	"print pairs ipairs type tostring tonumber require error pcall xpcall "
	"assert select setmetatable getmetatable rawget rawset next "
	"string math table io os coroutine utf8 debug "
	"print pairs ipairs type tostring tonumber require error pcall xpcall "
	"assert select setmetatable getmetatable rawget rawset next "
	"string math table io os coroutine utf8 debug"
};

static const nb_lang_profile PROF_PY = {
	NB_LANG_PY, "Python", "py",
	0, 0, 1,			/* "#" comments */
	0, 0, 1,			/* triple quotes */
	0,
	1, 1, 1,
	"def class import from as global nonlocal lambda with async await del pass "
	"def class import from as global nonlocal lambda with async await del pass",
	"if elif else for while try except finally raise return yield break "
	"continue in is not and or assert "
	"if elif else for while try except finally raise return yield break "
	"continue in is not and or assert",
	"True False None self cls NotImplemented Ellipsis "
	"True False None self cls NotImplemented Ellipsis",
	"print len range str int float list dict set tuple open enumerate zip "
	"sum min max abs sorted reversed map filter isinstance getattr setattr "
	"hasattr super repr format type id input "
	"print len range str int float list dict set tuple open enumerate zip "
	"sum min max abs sorted reversed map filter isinstance getattr setattr "
	"hasattr super repr format type id input"
};

static const nb_lang_profile PROF_C = {
	NB_LANG_C, "C", "c",
	1, 0, 0,
	1, 0, 0, 1,			/* block comments + leading # directives */
	1, 1, 1,
	"int char short long unsigned signed float double void struct union "
	"enum typedef static const volatile register inline extern "
	"int char short long unsigned signed float double void struct union "
	"enum typedef static const volatile register inline extern",
	"if else for while do switch case default return break continue goto "
	"sizeof "
	"if else for while do switch case default return break continue goto "
	"sizeof",
	"NULL true false NULL true false",
	"printf sprintf snprintf malloc free memcpy memset strlen strcmp "
	"nb_alloc nb_free "
	"printf sprintf snprintf malloc free memcpy memset strlen strcmp "
	"nb_alloc nb_free"
};

/* plain text: no keywords and no comments, but strings and numbers keep their
 * colour so a .txt of code is still readable */
static const nb_lang_profile PROF_TXT = {
	NB_LANG_TXT, "Text", "txt",
	0, 0, 0,
	0, 0, 0, 0,
	1, 1, 0,
	"", "", "", ""
};

const nb_lang_profile *nb_lang_get(nb_lang lang)
{
	switch (lang) {
	case NB_LANG_PPL:	return &PROF_PPL;
	case NB_LANG_LUA:	return &PROF_LUA;
	case NB_LANG_PY:	return &PROF_PY;
	case NB_LANG_C:		return &PROF_C;
	case NB_LANG_TXT:
	default:		return &PROF_TXT;
	}
}

const char *nb_lang_name(nb_lang lang)
{
	return nb_lang_get(lang)->name;
}

/* the short one for the status bar: four characters of a 40 column row that also
 * carries the file size, the position and the keyboard layer */
const char *nb_lang_abbrev(nb_lang lang)
{
	return nb_lang_get(lang)->abbrev;
}

/* the tail of `name` from its last '.' */
static const char *nb_suffix(const char *name)
{
	const char *dot = name;
	int i;

	for (i = 0; name[i]; i++) {
		if (name[i] == '.')
			dot = name + i;
	}
	return dot;
}

static int suffix_is(const char *name, const char *want)
{
	const char *s = nb_suffix(name);
	int i;

	for (i = 0; want[i] || s[i]; i++) {
		char a = s[i];

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != want[i])
			return 0;
	}
	return 1;
}

nb_lang nb_lang_from_name(const char *name)
{
	if (suffix_is(name, ".ppl") || suffix_is(name, ".hpppl") ||
	    suffix_is(name, ".hpappprgm") || suffix_is(name, ".hpappnote"))
		return NB_LANG_PPL;
	if (suffix_is(name, ".lua"))
		return NB_LANG_LUA;
	if (suffix_is(name, ".py"))
		return NB_LANG_PY;
	if (suffix_is(name, ".c") || suffix_is(name, ".h") ||
	    suffix_is(name, ".cc") || suffix_is(name, ".cpp") ||
	    suffix_is(name, ".hpp") || suffix_is(name, ".s"))
		return NB_LANG_C;
	return NB_LANG_TXT;
}

/* ---------------------------------------------------------------------------
 * word classification with a memo in front of the tables
 *
 * The Python PrimeCode measured this: on a forty word table a linear scan is up
 * to forty string compares per token, and a repeating token (the same variable
 * three times in a line) pays it every time.  The memo is 128 entries of
 * (length, first three bytes, class): a hit is a few compares, a miss falls back
 * to the scan.  It cannot go stale -- the same bytes always map to the same
 * class -- so it needs no invalidation when the file changes.
 * ------------------------------------------------------------------------- */
#define MEMO_MAX 128
static struct memo_entry {
	unsigned char len;
	unsigned char head[3];
	unsigned char cls;
} memo[MEMO_MAX];
static int memo_next;

/* is `word` (n bytes) in the space separated list? */
static int in_list(const char *list, const char *word, int n)
{
	const char *p = list;

	while (*p) {
		int i = 0, k, same = 1;

		while (p[i] && p[i] != ' ')
			i++;
		if (i == n) {
			for (k = 0; k < n; k++) {
				if (p[k] != word[k]) {
					same = 0;
					break;
				}
			}
			if (same)
				return 1;
		}
		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}
	return 0;
}

static int word_class(const nb_lang_profile *prof, const char *w, int n)
{
	int i;

	if (n <= 0 || n > 32)
		return NB_HS_TEXT;
	for (i = 0; i < MEMO_MAX; i++) {
		if (memo[i].len == (unsigned char)n && memo[i].cls &&
		    memo[i].head[0] == (unsigned char)w[0] &&
		    memo[i].head[1] == (unsigned char)(n > 1 ? w[1] : 0) &&
		    memo[i].head[2] == (unsigned char)(n > 2 ? w[2] : 0))
			return memo[i].cls;
	}
	{
		int cls = NB_HS_TEXT;

		if (in_list(prof->constants, w, n))
			cls = NB_HS_CONST;
		else if (in_list(prof->control, w, n))
			cls = NB_HS_CTRL;
		else if (in_list(prof->keywords, w, n))
			cls = NB_HS_KEY;
		else if (in_list(prof->libs, w, n))
			cls = NB_HS_LIB;
		memo[memo_next].len = (unsigned char)n;
		memo[memo_next].head[0] = (unsigned char)w[0];
		memo[memo_next].head[1] = (unsigned char)(n > 1 ? w[1] : 0);
		memo[memo_next].head[2] = (unsigned char)(n > 2 ? w[2] : 0);
		memo[memo_next].cls = (unsigned char)cls;
		memo_next = (memo_next + 1) % MEMO_MAX;
		return cls;
	}
}

/* ---------------------------------------------------------------------------
 * the line tokenizer
 * ------------------------------------------------------------------------- */
static void fill(unsigned char *s, int from, int to, int cls)
{
	int i;

	for (i = from; i < to; i++)
		s[i] = (unsigned char)cls;
}

/* Lua long bracket opener at i: '[' '='*n '['.  Returns n, or -1. */
static int lua_long_open(const char *t, int len, int i)
{
	int level = 0, k;

	if (i >= len || t[i] != '[')
		return -1;
	k = i + 1;
	while (k < len && t[k] == '=') {
		level++;
		k++;
	}
	if (k < len && t[k] == '[')
		return level;
	return -1;
}

/* a closer of exactly `level` at i, or 0 */
static int lua_long_close(const char *t, int len, int i, int level)
{
	int k, n = 0;

	if (i >= len || t[i] != ']')
		return 0;
	k = i + 1;
	while (k < len && t[k] == '=') {
		n++;
		k++;
	}
	if (n == level && k < len && t[k] == ']')
		return n + 2;
	return 0;
}

int nb_hl_line(const nb_lang_profile *prof, const char *t, int len,
	       unsigned char *styles, int state)
{
	const nb_lang_profile *p = prof;
	int i = 0, bol = 1;
	int in_block = state;		/* NB_HL_BLK_*: NONE when not inside one */
	int block_level = 0;		/* nesting depth for C blocks, or the
					 * '=' level of a Lua long bracket */

	fill(styles, 0, len, NB_HS_TEXT);

	/* a PPL line whose first character is '#' is firmware syntax, not code:
	 * #pragma, #PYTHON, #end.  Colouring the whole line as preprocessor is
	 * what PrimeCode does and it makes those blocks obvious at a glance. */
	if (p->ppl_syntax_at && len > 0 && t[0] == '#') {
		fill(styles, 0, len, NB_HS_PREPROC);
		return NB_HL_BLK_NONE;
	}

	while (i < len) {
			/* ---- still inside a block comment / long string ---- */
		if (in_block != NB_HL_BLK_NONE) {
			int cls = (in_block == NB_HL_BLK_C ||
				   in_block == NB_HL_BLK_LUA_COMMENT)
				? NB_HS_COMMENT : NB_HS_STRING;

			if (in_block == NB_HL_BLK_C) {
				if (i + 1 < len && t[i] == '*' && t[i + 1] == '/') {
					styles[i] = styles[i + 1] =
						(unsigned char)cls;
					i += 2;
					if (--block_level <= 0) {
						in_block = NB_HL_BLK_NONE;
						block_level = 0;
					}
					continue;
				}
				if (i + 1 < len && t[i] == '/' && t[i + 1] == '*') {
					styles[i] = styles[i + 1] =
						(unsigned char)cls;
					block_level++;
					i += 2;
					continue;
				}
				styles[i] = (unsigned char)cls;
				i++;
				continue;
			}
			if (in_block == NB_HL_BLK_LUA_LONG ||
			    in_block == NB_HL_BLK_LUA_COMMENT) {
				int close = (i + 1 < len)
					? lua_long_close(t, len, i, block_level)
					: 0;

				if (close) {
					fill(styles, i, i + close, cls);
					i += close;
					in_block = NB_HL_BLK_NONE;
					continue;
				}
				styles[i] = (unsigned char)cls;
				i++;
				continue;
			}
			/* Python triple quote */
			{
				char q = (in_block == NB_HL_BLK_PY_TRIPLE_D)
					? '"' : '\'';
				int k = i, found = 0;

				while (k + 2 < len) {
					if (t[k] == q && t[k + 1] == q &&
					    t[k + 2] == q) {
						found = 1;
						break;
					}
					k++;
				}
				if (found) {
					fill(styles, i, k + 3, cls);
					i = k + 3;
					in_block = NB_HL_BLK_NONE;
				} else {
					fill(styles, i, len, cls);
					i = len;
				}
				continue;
			}
		}

		/* ---- whitespace ---- */
		if (t[i] == ' ' || t[i] == '\t') {
			i++;
			continue;
		}

		/* ---- C block comment ---- */
		if (p->block_c_style && i + 1 < len && t[i] == '/' &&
		    t[i + 1] == '*') {
			styles[i] = styles[i + 1] = NB_HS_COMMENT;
			in_block = NB_HL_BLK_C;
			block_level = 1;
			i += 2;
			bol = 0;
			continue;
		}

		/* ---- Lua: "--" comments, long comments and long strings ---- */
		if (p->block_lua_long && i + 1 < len && t[i] == '-' &&
		    t[i + 1] == '-') {
			int level = lua_long_open(t, len, i + 2);

			if (level >= 0) {
				int j = i + 3 + level;
				int k = j, close = 0;
				int save_level = level;

				while (k < len) {
					close = lua_long_close(t, len, k, level);
					if (close)
						break;
					k++;
				}
				if (close) {
					fill(styles, i, k + close, NB_HS_COMMENT);
					i = k + close;
					in_block = NB_HL_BLK_NONE;
					block_level = 0;
				} else {
					fill(styles, i, len, NB_HS_COMMENT);
					in_block = NB_HL_BLK_LUA_COMMENT;
					block_level = save_level;
					i = len;
				}
				bol = 0;
				continue;
			}
			fill(styles, i, len, NB_HS_COMMENT);
			i = len;
			bol = 0;
			continue;
		}
		if (p->block_lua_long && t[i] == '[') {
			int level = lua_long_open(t, len, i);

			if (level >= 0) {
				int j = i + 2 + level;
				int k = j, close = 0;

				while (k < len) {
					close = lua_long_close(t, len, k, level);
					if (close)
						break;
					k++;
				}
				if (close) {
					fill(styles, i, k + close, NB_HS_STRING);
					i = k + close;
					in_block = NB_HL_BLK_NONE;
					block_level = 0;
				} else {
					fill(styles, i, len, NB_HS_STRING);
					in_block = NB_HL_BLK_LUA_LONG;
					block_level = level;
					i = len;
				}
				bol = 0;
				continue;
			}
		}

		/* ---- "#" comments (Python) ---- */
		if (p->line_comment_hash && t[i] == '#') {
			fill(styles, i, len, NB_HS_COMMENT);
			i = len;
			break;
		}
		/* ---- "//" comments ---- */
		if (p->line_comment_slash && i + 1 < len && t[i] == '/' &&
		    t[i + 1] == '/') {
			fill(styles, i, len, NB_HS_COMMENT);
			i = len;
			break;
		}

		/* ---- a Python decorator ---- */
		if (p->py_triple && t[i] == '@' && bol) {
			int j = i + 1;

			while (j < len && (NB_ISWORD((unsigned char)t[j]) ||
					   t[j] == '.'))
				j++;
			fill(styles, i, j, NB_HS_PREPROC);
			i = j;
			bol = 0;
			continue;
		}

		/* ---- a C preprocessor directive: the whole line ---- */
		if (p->block_c_style && t[i] == '#' && bol) {
			fill(styles, i, len, NB_HS_PREPROC);
			i = len;
			break;
		}

		/* ---- strings ---- */
		if ((p->string_dquote && t[i] == '"') ||
		    (p->string_squote && t[i] == '\'')) {
			char q = t[i];
			int j = i + 1;

			/* Python triple quote */
			if (p->py_triple && j + 1 < len && t[j] == q &&
			    t[j + 1] == q) {
				int k = j + 2, found = 0;

				while (k + 2 < len) {
					if (t[k] == q && t[k + 1] == q &&
					    t[k + 2] == q) {
						found = 1;
						break;
					}
					k++;
				}
				fill(styles, i, len, NB_HS_STRING);
				if (found) {
					i = k + 3;
				} else {
					in_block = (q == '"') ?
						NB_HL_BLK_PY_TRIPLE_D :
						NB_HL_BLK_PY_TRIPLE_S;
					i = len;
				}
				bol = 0;
				continue;
			}
			while (j < len) {
				if (p->escape_backslash && t[j] == '\\') {
					j += 2;
					continue;
				}
				if (t[j] == q) {
					j++;
					break;
				}
				j++;
			}
			if (j > len)
				j = len;
			fill(styles, i, j, NB_HS_STRING);
			i = j > i ? j : i + 1;
			bol = 0;
			continue;
		}

		/* ---- numbers ---- */
		if (NB_ISDIGIT((unsigned char)t[i]) ||
		    (t[i] == '.' && i + 1 < len &&
		     NB_ISDIGIT((unsigned char)t[i + 1]))) {
			int j = i;

			if (t[j] == '0' && j + 1 < len &&
			    (t[j + 1] == 'x' || t[j + 1] == 'X')) {
				j += 2;
				while (j < len && (NB_ISHEX((unsigned char)t[j]) ||
						   t[j] == '_'))
					j++;
			} else if (t[j] == '0' && j + 1 < len &&
				   (t[j + 1] == 'b' || t[j + 1] == 'B')) {
				j += 2;
				while (j < len && (t[j] == '0' || t[j] == '1' ||
						   t[j] == '_'))
					j++;
			} else {
				while (j < len && (NB_ISDIGIT((unsigned char)t[j]) ||
						   t[j] == '_' || t[j] == '.'))
					j++;
				if (j < len && (t[j] == 'e' || t[j] == 'E')) {
					int k = j + 1;

					if (k < len && (t[k] == '+' || t[k] == '-'))
						k++;
					if (k < len &&
					    NB_ISDIGIT((unsigned char)t[k])) {
						j = k;
						while (j < len &&
						       NB_ISDIGIT((unsigned char)t[j]))
							j++;
					}
				}
			}
			while (j < len && (t[j] == 'f' || t[j] == 'F' ||
					   t[j] == 'u' || t[j] == 'U' ||
					   t[j] == 'l' || t[j] == 'L'))
				j++;
			fill(styles, i, j, NB_HS_CONST);
			i = j;
			bol = 0;
			continue;
		}

		/* ---- words: identifiers and every high byte as opaque text ---- */
		if (NB_ISWORD((unsigned char)t[i])) {
			int j = i;

			while (j < len && NB_ISWORD((unsigned char)t[j]))
				j++;
			fill(styles, i, j, word_class(p, t + i, j - i));
			i = j;
			bol = 0;
			continue;
		}

		/* ---- operators and punctuation ---- */
		styles[i] = NB_HS_OP;
		i++;
		bol = 0;
	}

	return in_block;
}

/* ---------------------------------------------------------------------------
 * the editor-facing entry point
 * ------------------------------------------------------------------------- */
void nb_hl_init(nb_hl *h, const nb_lang_profile *prof)
{
	/* safe on ANY memory (a fresh stack struct included): nothing is read */
	nb_memset(h, 0, sizeof(*h));
	h->magic = NB_HL_MAGIC;
	h->prof = prof;
}

void nb_hl_open(nb_hl *h, const nb_lang_profile *prof,
		const struct nb_buf *buf)
{
	/* KEEP THE STATE ARRAY IF THERE IS ONE.  It is nb_hl_lines()'s scratch (one
	 * byte per line: the block state that line starts in) sized to a file, not
	 * to a language -- so opening the next file must not throw the pointer away.
	 * Zeroing the whole struct did exactly that (a leak per opened file), and
	 * the array nb_hl_lines() then allocated in its place was never initialised:
	 * the state line 0 "started in" was whatever the heap held, and a byte that
	 * looked like NB_HL_BLK_LUA_LONG or a Python triple quote paints THE WHOLE
	 * FILE in the string colour -- green.  That was "some files open all
	 * green": the file was read as one long, unterminated string. */
	unsigned char *keep = NB_NULL;
	int cap = 0;

	if (h->magic == NB_HL_MAGIC) {
		keep = h->state_line;
		cap = h->state_cap;
	}
	nb_memset(h, 0, sizeof(*h));
	h->magic = NB_HL_MAGIC;
	h->prof = prof;
	h->state_line = keep;
	h->state_cap = cap;
	if (keep && cap > 0)
		keep[0] = NB_HL_BLK_NONE;
	nb_hl_reset(h, buf);
}

void nb_hl_free(nb_hl *h)
{
	if (h->magic == NB_HL_MAGIC && h->state_line)
		nb_free(h->state_line);
	h->magic = 0;
	h->state_line = NB_NULL;
	h->state_cap = 0;
}

void nb_hl_reset(nb_hl *h, const struct nb_buf *buf)
{
	h->buf = buf;
	h->state_valid = 0;
	h->last_line = -1;
	if (h->state_line && h->state_cap > 0)
		h->state_line[0] = NB_HL_BLK_NONE;
}

void nb_hl_shift(nb_hl *h, int line)
{
	if (line < 0)
		line = 0;
	h->last_line = line - 1;	/* so nb_hl_lines() starts at `line` ... */
	h->state_valid = 1;		/* ... and trusts the state recorded there */
}

nb_lang nb_hl_lang(const nb_hl *h)
{
	return h->prof->lang;
}

void nb_hl_lines(nb_hl *h, const struct nb_buf *buf, unsigned char **styles,
		 int *caps, int first, int last, int stop_when_stable)
{
	int i, state;

	if (!buf || buf->nlines <= 0)
		return;
	if (!h->state_line || h->state_cap < buf->nlines + 2) {
		int cap = buf->nlines + 64;
		int had = h->state_line ? h->state_cap : 0;
		unsigned char *n = (unsigned char *)nb_realloc(
			h->state_line, (unsigned int)cap);
		int k;

		if (!n)
			return;
		h->state_line = n;
		h->state_cap = cap;
		/* EVERY STATE BYTE MEANS SOMETHING, so the fresh ones have to say
		 * "no block is open".  realloc does not zero, and a state byte that
		 * nobody has written yet is read by the very next line's tokenizer:
		 * garbage there is a whole file coloured as a string or a comment. */
		for (k = had; k < cap; k++)
			h->state_line[k] = NB_HL_BLK_NONE;
	}
	if (first < 0)
		first = 0;
	if (last >= buf->nlines)
		last = buf->nlines - 1;
	if (last < first)
		return;

	/* the state line `first` starts in must be known: it is recorded when the
	 * line before it was coloured, so an edit far above the cached line means
	 * starting over from the top (cheap: this is one byte per line) */
	if (!h->state_valid || first > h->last_line + 1)
		first = 0;
	state = h->state_line ? h->state_line[first] : NB_HL_BLK_NONE;

	for (i = first; i <= last; i++) {
		const char *t = nb_buf_line(buf, i);
		int len = nb_buf_linelen(buf, i);
		int end;

		/* THE BUFFER HAS TO BE BIG ENOUGH FOR THE LINE AS IT IS NOW.  It is
		 * allocated len+1 the first time and REALLOCATED when the line has
		 * grown since: one style byte is written per byte of the line, so a
		 * buffer that is a byte short writes one byte into the next heap
		 * allocation -- the following line's style buffer, which is how
		 * "insert a character and the highlighting below it shifts" happens. */
		if (!styles[i] || caps[i] < len + 1) {
			unsigned char *n = (unsigned char *)nb_realloc(
				styles[i], (unsigned int)len + 1);

			if (!n)
				return;
			styles[i] = n;
			caps[i] = len + 1;
		}
		end = nb_hl_line(h->prof, t, len, styles[i], state);
		if (i + 1 < h->state_cap)
			h->state_line[i + 1] = (unsigned char)end;
		h->last_line = i;
		h->state_valid = 1;
		if (end == state && stop_when_stable)
			break;		/* the block state did not change, so
					 * every later line is still correct */
		state = end;
	}
}
