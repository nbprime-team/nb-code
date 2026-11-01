/*
 * nb_keymap.c -- device scan code -> what the editor does.
 *
 * THE TWO TABLES, AGAIN
 * The HP Prime has two unrelated key numbering schemes in this workspace:
 *
 *   device scan codes   0x01 esc, 0x02 left, 0x03 up, 0x04 right, 0x05 down,
 *                       0x0C backspace, 0x0D enter, 0x20 space, 0x83 ON,
 *                       0x8B shift, 0xB6 alpha, 0x91/0xB2/0xB3/0xB4/0xB5/0x93
 *                       for symb/plot/num/view/cas/menu, and the number/symbol
 *                       keys (0x30..0x5A, 0xB7..0xB9) whose ALPHA layer is a
 *                       letter
 *   PPL GETKEY          0..50, where 4 is ESC and 2 is UP
 *
 * nbCode uses the DEVICE codes, because that is what comes out of the firmware's
 * get_event and what the input hook queues.  Getting them confused is not
 * theoretical: with the GETKEY numbering, device 0x04 (right arrow) reads as
 * "escape", so the right arrow quits (primeLua's README records exactly that
 * bug, and g1fm's too).
 *
 * THERE IS ONE TABLE, AND ITS ALPHA COLUMN IS THE KEYBOARD'S OWN
 * Every key that carries a letter carries exactly three faces: what it types
 * plain, what it types with SHIFT, and the small letter printed on it for the
 * ALPHA layer.  All three live in `keys[]` below, so a key is described in one
 * place and nothing has to be inferred from another table.
 *
 * THE ALPHA LETTERS ARE NOT A GUESS, AND THIS IS HOW THEY WERE DERIVED
 * They are the letters PRINTED on the keys, taken from the Python PrimeCode
 * (which is what this editor is modelled on) and cross-checked against the
 * hardware-verified device table:
 *
 *   * primeCode's keys.py has a letter per PPL GETKEY id: id 32 is 'q' (and the
 *     device table in primetcc/rt/hp_input.h says the same thing in words: the
 *     7 key is "7/Q", 0x51);
 *   * the same file gives the GETKEY ids of the whole number pad, and those line
 *     up one for one with the device codes 0x47..0x5A in the firmware's scan
 *     order -- sin/cos/tan/ln/log/x^2/+- at 0x47..0x4D, the numbers, then
 *     comma/bracket/EEX before them;
 *   * the GETKEY *shift* layer confirms it independently where it can be checked
 *     against the real calculator: id 24 is ln and shifts to exp(), id 26 is x^2
 *     and shifts to sqrt(), id 20 is x^y and shifts to NTHROOT, id 18 shifts to
 *     the degree-minute-second symbol -- all of which are the printed shift
 *     functions of those keys;
 *   * so the letters run a b c d e down the keys at 0x41..0x45 (the calculator's
 *     vars/toolbox/units/define/a-b-c keys, which type nothing of their own),
 *     then one letter per key in scan order, and the keypad letters are
 *     q r s / u v w / y z # -- NOT the q w e / r t y / u i o of a QWERTY guess.
 *
 * The earlier version of this file had exactly that guess, which is why the
 * letters a, b, c, d and e could not be typed at all (nothing was mapped to
 * 0x41..0x45, and the keys that were mapped to a..e were the wrong ones).  The
 * hardware has the last word; tests/test_host.c pins all 32 letters.
 *
 * ALPHA AND SHIFT ARE THE TWO SECOND-LEVEL KEYS, AND THEY BEHAVE AS ON THE DEVICE
 *   ALPHA once      the next letter key types its letter, then alpha is off
 *   ALPHA twice     LOCKED: every letter key types its letter until alpha is
 *                   pressed a third time (this is what makes typing a word
 *                   possible; a one-shot alpha is for one letter)
 *   SHIFT           A LATCH, spent by the next key: press it and it stays on
 *                   (the status line says "shift"), press it again to switch it
 *                   off.  primeCode's k_shi is exactly this ((k_shi+1) % 2, then
 *                   cleared by the key it was pressed for) and the calculator's
 *                   own shift indicator works the same way -- a modifier that
 *                   has to be HELD while pressing another key on a calculator
 *                   with a touch screen is not usable with one hand.
 *   SHIFT+ALPHA     the same cycle in capitals (one-shot, then locked)
 *   SHIFT+key       the shifted character of the key: digits give punctuation,
 *                   and shift+ON is a real tab
 * and alpha wins over shift on a key that carries a letter, which is how the
 * calculator works too (primeCode's k_alp cannot be non-zero and still get the
 * plain shift layer).  Together those two rules make every printable ASCII byte
 * reachable, capitals included -- tests/test_host.c checks the whole set.
 *
 * THE EDITING CHORDS ARE primeCode's (keys.py, ids in brackets):
 *   shift+left [7]         the start of the line (k_shi -> gb = 0)
 *   shift+right [8]        the end of the line (k_shi -> gb = len(txt))
 *   shift+backspace [19]   delete the character UNDER the cursor (forward
 *                          delete; the plain key is the backspace)
 *   shift+A                the character table (see nb_ui_overlay.c): nbCode's
 *                          own addition, because the keypad cannot type the
 *                          symbols a C file or a note wants.  Capital A is still
 *                          one tap away in the table, and every OTHER letter key
 *                          still takes its capital from shift+alpha.
 *
 * WHAT A KEY MEANS is decided in two steps: nb_keymap_modifier() consumes the
 * modifier keys (shift, alpha) and updates the state, then nb_keymap() answers
 * for the key itself.  Keeping them apart is what lets a test drive the mapper
 * with a plain list of scan codes.
 */
#include "nb_ui.h"
#include "nb_input.h"

/* device scan codes (port/hp_input.h, and primetcc/rt/hp_input.h has the full,
 * hardware-verified table) */
#define K_ESC		0x01
#define K_LEFT		0x02
#define K_UP		0x03
#define K_RIGHT		0x04
#define K_DOWN		0x05
#define K_BACKSPACE	0x0C
#define K_ENTER		0x0D
#define K_SPACE		0x20
#define K_ON		0x83
#define K_SHIFT		0x8B
#define K_ALPHA		0xB6
#define K_SYMB		0x91
#define K_PLOT		0xB2
#define K_NUM		0xB3
#define K_VIEW		0xB4
#define K_CAS		0xB5
#define K_MENU		0x93
#define K_APPS		0xB1
#define K_LETTER_A	0x41	/* the key printed 'a' */
#define K_SHIFT6	0x57	/* the '6' key: shift+6 is the symbol panel */
#define K_HELP		0x95

/* ONE ROW PER KEY THAT TYPES SOMETHING, AND THE LAYERS ARE primeCode's.
 *
 *   plain  what the key types with no modifier (primeCode's `norm`): the keypad
 *          itself, the function keys as CALLS -- sin() -- and the bracket keys as
 *          a PAIR.
 *   shift  the shifted layer (primeCode's `shi`): the inverse functions
 *          (asin(), exp(), sqrt()...) and the pairs {} [] ''.
 *   alpha  the letter printed on the key (see the derivation above).
 *
 * A string, not a character: "sin()" and "()" are what those keys are FOR, and
 * the editor puts the caret between a pair after inserting it (see
 * nb_ui_editor_ins).  An empty string means the key types nothing in that layer.
 *
 * WHERE THIS DIFFERS FROM primeCode, AND WHY: its shift layer leaves six keys
 * EMPTY (or holding a symbol a C file cannot use) and it has no way at all to
 * type `$ & < > ? @ \ | ~`.  Those six keys carry the punctuation a C file needs
 * instead, so the whole of printable ASCII stays reachable -- tests/test_host.c
 * walks every layer and insists on all 95 bytes.  primeCode's own layer is kept
 * wherever it is usable (the inverse functions, the pairs, :=, 0x... spelled as
 * the two characters here, °′″). */
static const struct {
	int code;
	const char *plain, *shift;
	const char *alpha;
} keys[] = {
	/* the five keys that only carry a letter */
	{ 0x41, "",    "",   "a" },		/* vars */
	{ 0x42, "",    "",   "b" },		/* toolbox */
	{ 0x43, "",    "",   "c" },		/* units */
	{ 0x44, "x",   "|",  "d" },		/* define: shift types | (its old
						 * home, shift+6, is the symbol
						 * panel now) */
	{ 0x45, "/",   "\xc2\xb0\xe2\x80\xb2\xe2\x80\xb3", "e" },	/* a b/c: °′″ */

	{ 0x46, "^",   "~",  "f" },		/* x^y */
	{ 0x47, "sin()", "asin()", "g" },	/* sin */
	{ 0x48, "cos()", "acos()", "h" },	/* cos */
	{ 0x49, "tan()", "atan()", "i" },	/* tan */
	{ 0x4A, "ln()",  "exp()",  "j" },	/* ln */
	{ 0x4B, "log()", "alog()", "k" },	/* log */
	{ 0x4C, "^2",  "sqrt()", "l" },		/* x^2 */
	{ 0x4D, "-",   "abs()",  "m" },		/* +/- */
	{ 0x4E, "()",  "''",     "n" },		/* () -- a PAIR */
	{ 0x4F, ",",   "\\",   "o" },		/* comma */
	{ 0x50, "*10^", ":=",    "p" },		/* EEX */

	{ 0x51, "7",   "&",  "q" },		/* 7  -- the key primetcc's table calls 7/Q */
	{ 0x52, "8",   "{}", "r" },
	{ 0x53, "9",   "!",  "s" },
	{ 0x54, "/",   "%",  "t" },		/* divide */
	{ 0x55, "4",   "$",  "u" },
	{ 0x56, "5",   "[]", "v" },
	{ 0x57, "6",   "|",  "w" },
	{ 0x58, "*",   "<",  "x" },		/* multiply */
	{ 0x59, "1",   ">",  "y" },
	{ 0x5A, "2",   "?",  "z" },
	{ 0x33, "3",   "@",  "#" },		/* 3 */
	{ 0xB7, "-",   "`",  ":" },		/* minus */
	{ 0x30, "0",   ":",  "\"" },		/* 0 */
	{ 0xB8, ".",   "=",  "." },		/* period */
	{ 0x20, " ",   "_",  " " },		/* space */
	{ 0xB9, "+",   ";",  ";" }		/* add */
};

/* A key whose insertion ENDS with one of these gets the caret put INSIDE it:
 * primeCode's `if adde[-1] in ')]}"': gb -= 1`, which is what makes `()` and
 * `sin()` and `{}` usable instead of annoying.  Only for a snippet (more than
 * one character): a lone ')' typed on purpose must not move the caret. */
static const char *closers = ")]}\"'";

static int shift_down;
static int alpha_state;			/* NB_ALPHA_OFF / _ONCE / _LOCK */
static int alpha_upper;			/* SHIFT was held when alpha was pressed */

/* the ALPHA key: OFF -> one letter -> locked -> OFF, and SHIFT+ALPHA chooses the
 * capital variant of whichever of those it lands on.  A press cycles; the
 * release does nothing, because the layer stays set after the key comes up (a
 * one-shot alpha that ended on release could never be used at all). */
static void alpha_press(void)
{
	if (alpha_state == NB_ALPHA_LOCK)
		alpha_state = NB_ALPHA_OFF;
	else
		alpha_state++;
	alpha_upper = shift_down ? 1 : 0;
}

int nb_keymap_modifier(int code, int down)
{
	if (code == K_SHIFT) {
		/* A TOGGLE, not a hold (see the note at the top): the press flips
		 * it and the release is ignored. */
		if (down)
			shift_down = !shift_down;
		return 1;
	}
	if (code == K_ALPHA) {
		if (down)
			alpha_press();
		return 1;
	}
	return 0;
}

int nb_keymap_held_shift(void)
{
	return shift_down;
}

int nb_keymap_alpha_state(void)
{
	return alpha_state;
}

int nb_keymap_alpha_upper(void)
{
	return alpha_upper;
}

/* New session, clean slate: a run that ended with ALPHA locked must not start
 * with it locked (nb_ui_init calls this; a disabled keymap or a half-pressed
 * modifier is how a fresh file gets typed in the wrong layer). */
void nb_keymap_reset(void)
{
	shift_down = 0;
	alpha_state = NB_ALPHA_OFF;
	alpha_upper = 0;
}

static nb_key_meaning key_meaning(int code);

/* THE LATCH IS SPENT BY THE NEXT KEY THAT MEANS SOMETHING.  This is the wrapper:
 * the worker below answers what the key types, this one clears the shift latch
 * afterwards -- one exit point, which is why the table has a function of its own
 * rather than a `shift_down = 0` on a dozen return paths.  SHIFT and ALPHA are
 * exempt (they are the modifiers themselves: shift+alpha is how capitals are
 * selected). */
nb_key_meaning nb_keymap(int code)
{
	nb_key_meaning m = key_meaning(code);

	if (m.what != NB_KEY_NONE && code != K_SHIFT && code != K_ALPHA)
		shift_down = 0;
	return m;
}

static nb_key_meaning key_meaning(int code)
{
	nb_key_meaning m;
	int i;

	m.what = NB_KEY_NONE;
	m.ch[0] = 0;
	m.ch[1] = m.ch[2] = m.ch[3] = 0;
	m.ins = NB_NULL;

	switch (code) {
	case K_LEFT:
		/* primeCode: shift+left goes to the start of the line */
		m.what = shift_down ? NB_KEY_HOME : NB_KEY_LEFT;
		return m;
	case K_RIGHT:
		m.what = shift_down ? NB_KEY_END : NB_KEY_RIGHT;
		return m;
	case K_UP:	m.what = NB_KEY_UP;	return m;
	case K_DOWN:	m.what = NB_KEY_DOWN;	return m;
	case K_BACKSPACE:
		/* primeCode: shift+backspace deletes FORWARD */
		m.what = shift_down ? NB_KEY_DELETE : NB_KEY_BACKSPACE;
		return m;
	case K_ENTER:	m.what = NB_KEY_ENTER;	return m;
	case K_ESC:	m.what = NB_KEY_ESC;	return m;
	case K_ON:
		/* tab: the ON key with shift, so a source file can still be
		 * indented with a real tab character when the user wants one */
		m.what = shift_down ? NB_KEY_TAB : NB_KEY_ON;
		return m;
	default:	break;
	}

	/* THE TWO FUNCTION KEYS THIS CALCULATOR'S KEYBOARD ACTUALLY HAS A USE FOR.
	 * There is no F1..F6 row: what used to hang off one is in the MENU popup
	 * now, and HELP is help.  Apps/Home/Symb/Plot/Num/View/CAS do nothing (see
	 * nb_ui.h). */
	switch (code) {
	case K_MENU:
		/* shift+MENU IS THE GLOBAL CLIPBOARD (nb_clip.h): the panel lists the
		 * last ten things the user copied, from any file.  Plain MENU stays
		 * the screen's own popup, which is where the copy/cut actions live. */
		m.what = shift_down ? NB_KEY_CLIP : NB_KEY_MENU;
		return m;
	case K_HELP:	m.what = NB_KEY_HELP;	return m;
	case K_SYMB:
		/* SYMB SEARCHES (the key is labelled with a magnifying glass on the
		 * device, which is the one key whose job needs no explanation) */
		m.what = NB_KEY_FIND;
		return m;
	case K_PLOT:
		/* PLOT REPLACES: the pair of them is the search bar of a desktop
		 * editor, spread over the two keys the keyboard still had free */
		m.what = NB_KEY_REPLACE;
		return m;
	case K_VIEW:
		/* shift+VIEW IS COPY -- the selection when there is one, otherwise
		 * the caret's line.  It is the one action worth a chord of its own:
		 * copying is what a user does constantly and the menu needs three
		 * presses to reach it.  Plain VIEW still does nothing (nb_ui.h). */
		if (shift_down)
			m.what = NB_KEY_COPY;
		return m;
	default:	break;
	}

	/* SHIFT+A OPENS THE CHARACTER TABLE (nb_ui_overlay.c).  It has to be here,
	 * before the character layer: shift+A would otherwise be a capital, and the
	 * table is where the user reaches every symbol the keypad cannot type.  With
	 * ALPHA on, shift+this key is still a capital A -- the table does not steal
	 * the alpha layer. */
	if (shift_down && alpha_state == NB_ALPHA_OFF && code == K_LETTER_A) {
		m.what = NB_KEY_PALETTE;
		return m;
	}
	/* SHIFT+6 IS THE QUICK SYMBOL PANEL: `>= <= != &&` are what a keypad cannot
	 * type and what every other line needs, and this key was the one free one
	 * whose shifted layer ('|') is also on the tan key -- so nothing becomes
	 * unreachable (see the coverage test). */
	if (shift_down && alpha_state == NB_ALPHA_OFF && code == K_SHIFT6) {
		m.what = NB_KEY_SYMBOLS;
		return m;
	}

	/* the character keys */
	for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
		const char *text = NB_NULL;

		if (keys[i].code != code)
			continue;
		if (alpha_state != NB_ALPHA_OFF && keys[i].alpha[0]) {
			/* the printed letter, in capitals if alpha was selected
			 * with SHIFT.  Only a single lower case letter has a
			 * capital; the six non-letters keep what they are ('#',
			 * '"', ';', ':', '.', space). */
			if (alpha_upper && keys[i].alpha[0] >= 'a' &&
			    keys[i].alpha[0] <= 'z' && keys[i].alpha[1] == 0)
				m.ch[0] = (char)(keys[i].alpha[0] - 32);
			else
				m.ch[0] = keys[i].alpha[0];
			/* one-shot alpha is spent by the key it was set for (and
			 * LOCK is not: that is the whole difference) */
			if (alpha_state == NB_ALPHA_ONCE)
				alpha_state = NB_ALPHA_OFF;
			m.what = NB_KEY_CHAR;
			return m;
		}
		if (shift_down && keys[i].shift[0])
			text = keys[i].shift;
		else if (keys[i].plain[0])
			text = keys[i].plain;
		if (!text)
			return m;	/* nothing on this key in this layer */
		if (text[0] && text[1] == 0) {
			m.ch[0] = text[0];	/* one character: the plain way */
		} else {
			m.ins = text;		/* a snippet: sin(), (), {} */
		}
		m.what = NB_KEY_CHAR;
		return m;
	}

	return m;
}

/* does an insertion want the caret INSIDE it (see `closers`)? */
int nb_key_wants_caret_inside(const char *text, int len)
{
	int i;

	if (!text || len < 2)
		return 0;
	for (i = 0; closers[i]; i++) {
		if (text[len - 1] == closers[i])
			return 1;
	}
	return 0;
}
