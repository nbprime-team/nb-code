/*
 * nb_fw.c -- the firmware-side implementation of nb_port.h.
 *
 * One file for the pieces that are a few lines each (heap, clock, log, config,
 * exit); the interesting parts have their own translation units:
 *
 *   nb_svc.s      the SVC wrappers, the input-slot patch and the entry point
 *   nb_file.c     the UTF-16 file API and the write-through-temp save
 *   nb_screen.c   the framebuffer behind one blit per frame
 *   nb_input_fw.c the get_event hook (DOOM's mechanism)
 */
#include "nb_common.h"
#include "nb_port.h"
#include "nb_cfg.h"
#include "nb_input.h"

/* the log file's path arrives in the launch config; nb_run_begin passes it */
void nb_set_logpath(const char *p);

/* from nb_svc.s */
unsigned hp_svc_malloc(unsigned n);
unsigned hp_svc_calloc(unsigned n, unsigned sz);
unsigned hp_svc_realloc(unsigned p, unsigned n);
void hp_svc_free(unsigned p);
void hp_svc_sleep(unsigned ms);
unsigned hp_svc_gettime(unsigned buf);

/* ---------------------------------------------------------------------------
 * memory: straight to the firmware heap.
 *
 * The firmware's allocator guarantees 4-byte alignment only, which matters on
 * this CPU: an unaligned LDRD/STRD is a Data Abort, and on this machine a Data
 * Abort is a reset.  nb_realloc therefore rounds its size up to a multiple of
 * four and only ever hands back 4-byte aligned pointers.
 * ------------------------------------------------------------------------- */
void *nb_alloc(unsigned int n)
{
	void *p;

	if (n == 0)
		n = 1;
	p = (void *)(unsigned long)hp_svc_malloc((n + 3) & ~3u);
	return p;
}

void *nb_realloc(void *p, unsigned int n)
{
	if (!p)
		return nb_alloc(n);
	if (n == 0)
		n = 1;
	return (void *)(unsigned long)hp_svc_realloc((unsigned)p, (n + 3) & ~3u);
}

void nb_free(void *p)
{
	if (p)
		hp_svc_free((unsigned)(unsigned long)p);
}

/* ---------------------------------------------------------------------------
 * the string core, written out because -nostdlib and -fno-builtin mean there is
 * no libc to call (and calling one would pull in newlib's malloc).
 * ------------------------------------------------------------------------- */
void *nb_memcpy(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *nb_memset(void *dst, int c, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

int nb_memcmp(const void *a, const void *b, unsigned int n)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;

	while (n--) {
		if (*x != *y)
			return (int)*x - (int)*y;
		x++;
		y++;
	}
	return 0;
}

unsigned int nb_strlen(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
}

int nb_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ---------------------------------------------------------------------------
 * THE LAUNCH CONFIG COMES FROM A FILE, not from a memory block.
 *
 * The launcher (Python) used to hand the editor a struct in memory -- in r0, or
 * written into a buffer the image carried.  Both were tried and both failed the
 * same way: the two sides disagreed about the layout, the editor read a pointer
 * where a count belonged, found a zero magic, and quit with "no config block"
 * while the screen stayed black.  Writing it into the image's own .bss tail was
 * worse still -- that region is cleared by the loader and reused by the firmware
 * heap, so the config read back as zeros.
 *
 * A file has no layout to disagree about, so the launcher writes nbcode.cfg (see
 * src/nb_cfg.h) and port/nb_cfgfile.c parses it defensively: a short file, a
 * wrong magic, or no file at all ends in "the editor starts with its defaults",
 * which is the only acceptable failure mode for a launch handoff.
 *
 * What is left here is the ONE instance the rest of the editor reads.
 * ------------------------------------------------------------------------- */
nb_config nb_cfg_instance;

/* The application folder, as a device path.
 *
 * THE TWO SIDES USE DIFFERENT SPELLINGS OF THE SAME FILE, and neither can use
 * the other's: MicroPython's file API resolves a BARE name against the
 * application folder and refuses "C:\DATA\..." outright (OSError 0 -- measured),
 * while the FIRMWARE's fopen wants the full device path.  The launcher writes
 * nbcode.cfg by bare name; the image has to open it by device path, and the image
 * cannot work out where it was loaded from.
 *
 * So the launcher hands the folder over in r0 (nb_svc.s stores it in
 * nb_entry_arg) and it is copied here the first time it is needed -- the launcher
 * frees that block when the run is over.  The build-time constant below is the
 * fallback for a launcher that does not know either.
 *
 * Weak, so a host harness can point the same code at its own temporary tree
 * (tests/test_emu.py does; nothing on the device defines it). */
__attribute__((weak)) const char *nb_cfg_dir_fallback =
	"C:\\DATA\\nbCode.hpappdir";

/* set by nb_svc.s from r0 at entry */
extern const char *nb_entry_arg;

static char app_dir[NB_PATH_MAX];

const char *nb_cfg_dir_hint(void)
{
	if (!app_dir[0] && nb_entry_arg && nb_entry_arg[0]) {
		nb_str_copy(app_dir, sizeof(app_dir), nb_entry_arg, -1);
		nb_log("cfg: app folder from the launcher: ");
		nb_log(app_dir);
		nb_log("\n");
	}
	return app_dir[0] ? app_dir : nb_cfg_dir_fallback;
}

const nb_config *nb_config_get(void)
{
	return &nb_cfg_instance;
}

/* ---------------------------------------------------------------------------
 * logging
 *
 * nb_log() writes into a ring inside the image (magic "NBCORING"), which the
 * launcher polls while nbCode runs -- the same streaming channel primeLua uses
 * for print().  nb_logfile() appends to a file, which survives a reset: that is
 * the one that matters when the machine dies mid-edit.
 * ------------------------------------------------------------------------- */
/* how much of the durable log is kept (see nb_logfile) */
#define LOG_KEEP	16384

struct nb_ring {
	char magic[8];
	volatile int count;
	char data[8192];
};

/* The ring's address is published by the image through the log itself: the
 * launcher finds the magic "NBCORING" in the image file, which is exact because
 * the ring is initialized data and its magic is its first byte. */
struct nb_ring nb_ring __attribute__((used)) = { "NBCORING", 0, { 0 } };

void nb_log(const char *s)
{
	int n = (int)nb_strlen(s);
	int i;

	for (i = 0; i < n; i++)
		nb_ring.data[(nb_ring.count + i) & 8191] = s[i];
	nb_ring.count += n;
}

/* "<tag><number>\n" -- the tag comes through whole and the number is DECIMAL,
 * because this is the line a person reads in nbcode.log when the editor does not
 * start.  (An earlier version kept eight characters of the tag and printed the
 * value as hex, which turned "cfg: workspaces=1" into "cfg: wor00000001".) */
#define NB_LOG_TAG_MAX	32

static void log_tagged(const char *tag, unsigned int v, int base, int width)
{
	static const char *digits = "0123456789ABCDEF";
	char b[NB_LOG_TAG_MAX + 12];
	char num[12];
	int i = 0, n = 0, j;

	while (tag && *tag && i < NB_LOG_TAG_MAX)
		b[i++] = *tag++;
	if (v == 0) {
		num[n++] = '0';
	} else {
		while (v) {
			num[n++] = digits[v % (unsigned)base];
			v /= (unsigned)base;
		}
	}
	while (n < width)
		num[n++] = '0';
	for (j = n - 1; j >= 0; j--)
		b[i++] = num[j];
	b[i++] = '\n';
	b[i] = 0;
	nb_log(b);
}

void nb_log_num(const char *tag, unsigned int v)
{
	log_tagged(tag, v, 10, 0);
}

void nb_log_hex(const char *tag, unsigned int v)
{
	log_tagged(tag, v, 16, 8);
}

/* The durable log: a real file, written by the image itself, CLOSED before
 * returning.  Two reasons it is a file and not just the ring: a reboot clears
 * the ring, and MicroPython's console is not flushed when the machine dies
 * mid-run -- so "it rebooted" has to leave a trace on the flash.  (primeLua's
 * plua_diag does exactly this for the same reason.) */
static char logpath[NB_PATH_MAX];

void nb_set_logpath(const char *p)
{
	nb_str_copy(logpath, sizeof(logpath), p ? p : "", -1);
}

void nb_logfile(const char *s)
{
	char *old;
	int oldlen = 0;
	const char *err = "";
	void *h;
	int n;

	if (!logpath[0] || !s)
		return;
	/* read what is there, append, write it back: the firmware's fopen accepts
	 * "wb+" and nothing that could append in place reliably */
	old = nb_file_slurp(logpath, &oldlen, &err);
	h = nb_file_open(logpath, "wb+");
	if (!h) {
		if (old)
			nb_free(old);
		return;
	}
	/* KEEP THE TAIL OF WHAT IS THERE, and 16 KB of it.  This used to keep 2 KB
	 * and DROP EVERYTHING once the file was bigger: the launcher writes its
	 * start-up lines into the same file, so a slightly longer start-up silently
	 * wiped the durable log -- and a durable log that erases itself is worse than
	 * none, because the lines a crash is diagnosed from are the ones that just
	 * disappeared. */
	if (old && oldlen > 0) {
		int len = (int)nb_strlen(s) + 1;
		int keep = LOG_KEEP - len;

		if (keep < 0)
			keep = 0;
		if (oldlen > keep) {
			nb_file_write(h, old + (oldlen - keep), keep);
			n = keep;
		} else {
			nb_file_write(h, old, oldlen);
			n = oldlen;
		}
	} else {
		n = 0;
	}
	if (old)
		nb_free(old);
	{
		int len = (int)nb_strlen(s);

		nb_file_write(h, s, len);
		nb_file_write(h, "\n", 1);
	}
	nb_file_close(h);
}

/* A MILESTONE: one line, straight to the durable log, at a point the editor
 * must get past to work.
 *
 * WHY THIS IS NOT JUST nb_log(): the ring is the console, and on this machine
 * the run is one blocking debug call -- the launcher reads the ring only after
 * the editor returns.  So when the calculator FAULTS instead of returning, the
 * ring is never read and the only trace left is what the image wrote to the
 * flash itself.  That is how a reboot after "nbCode 1.0 start" gave no clue about
 * where it died; milestones cost one small file append each and turn the next
 * crash into a list of what was working.
 *
 * Kept few and short on purpose: nb_logfile() reads the file, appends and writes
 * it back (the firmware's fopen cannot append in place), so this is a real cost
 * -- about six lines per run is worth it, one per frame would not be. */
void nb_milestone(const char *s)
{
	if (!logpath[0] || !s)
		return;
	nb_logfile(s);
}

/* the launcher needs the ring's address; the entry hands it back through the
 * state block it polls (see calc/main.py: it finds "NBCORING" in the image, the
 * same way primeLua's launcher finds "PLUARING") */
void *nb_ring_addr(void)
{
	return (void *)&nb_ring;
}

/* the exit status, published where the launcher's ring reader can see it: the
 * debug interface's return value is unreliable on this hardware, so primeLua
 * writes "RT_RET:<n>" into its output ring and the launcher parses that.  nbCode
 * does the same, and this is the function that appends it. */
void nb_log_ret(int status)
{
	char b[16];
	int i = 0, n = 0, neg = 0;
	unsigned v;

	if (status < 0) {
		neg = 1;
		v = (unsigned)(-status);
	} else {
		v = (unsigned)status;
	}
	do {
		b[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && n < 14);
	nb_log("RT_RET:");
	if (neg)
		nb_log("-");
	while (n > 0) {
		char c[2];

		c[0] = b[--n];
		c[1] = 0;
		nb_log(c);
		i++;
	}
	nb_log("\n");
}


/* yield: hp_svc_get_event is what the input hook is called FROM, so an idle
 * editor must still let the OS's input thread run -- and on the emulator this
 * call is where injected events are delivered, which is what makes the
 * emulator tests' key injection work at all */
void nb_sleep(int ms)
{
	hp_svc_sleep((unsigned)ms);
}

