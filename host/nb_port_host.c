/*
 * nb_port_host.c -- the PC implementation of nb_port.h (see nb_host.h).
 *
 * Everything here is deliberately dull: a framebuffer in a static array, an
 * event queue a test fills by hand, and plain stdio for files.  If a host test
 * fails, the failure is in the editor, not in this file -- that is the whole
 * point of keeping the layering honest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nb_common.h"
#include "nb_port.h"
#include "nb_cfg.h"
#include "nb_host.h"

/* ---------------------------------------------------------------------------
 * memory: the C heap
 * ------------------------------------------------------------------------- */
void *nb_alloc(unsigned int n)
{
	return malloc(n ? n : 1);
}

void *nb_realloc(void *p, unsigned int n)
{
	return realloc(p, n ? n : 1);
}

void nb_free(void *p)
{
	free(p);
}

void *nb_memcpy(void *d, const void *s, unsigned int n)
{
	return memcpy(d, s, n);
}

void *nb_memset(void *d, int c, unsigned int n)
{
	return memset(d, c, n);
}

int nb_memcmp(const void *a, const void *b, unsigned int n)
{
	return memcmp(a, b, n);
}

unsigned int nb_strlen(const char *s)
{
	return (unsigned int)strlen(s);
}

int nb_strcmp(const char *a, const char *b)
{
	return strcmp(a, b);
}

/* ---------------------------------------------------------------------------
 * the screen
 * ------------------------------------------------------------------------- */
/* THE HOST MODELS THE DEVICE'S DOUBLE BUFFER (port/nb_screen.c): `back` is what
 * nb_gfx draws into, `fb` is what the panel shows after a present.  The device
 * gained the shadow buffer because drawing straight into the visible pixels
 * flickers (every frame erases and redraws the body in place); keeping the two
 * apart here is what lets a host test SEE that a draw does not touch the screen
 * until it is presented. */
static nb_u32 fb[NB_SCREEN_W * NB_SCREEN_H];
static nb_u32 back[NB_SCREEN_W * NB_SCREEN_H];
static int fb_ready;

const nb_u32 *nb_host_fb(void)
{
	return fb;
}

nb_u32 nb_host_fb_pixel(int x, int y)
{
	if (x < 0 || y < 0 || x >= NB_SCREEN_W || y >= NB_SCREEN_H)
		return 0;
	return back[y * NB_SCREEN_W + x];
}

nb_u32 nb_host_fb_shown(int x, int y)
{
	if (x < 0 || y < 0 || x >= NB_SCREEN_W || y >= NB_SCREEN_H)
		return 0;
	return fb[y * NB_SCREEN_W + x];
}

int nb_screen_init(void)
{
	memset(fb, 0, sizeof(fb));
	memset(back, 0, sizeof(back));
	fb_ready = 1;
	return 1;
}

void nb_screen_free(void)
{
	/* the host's buffer is a static array: nothing to give back */
}

nb_u32 *nb_screen_back(void)
{
	return back;
}

int nb_screen_width(void)
{
	return NB_SCREEN_W;
}

int nb_screen_height(void)
{
	return NB_SCREEN_H;
}

void nb_screen_present(void)
{
	/* one pass, like the device: this is the frame the panel would show */
	memcpy(fb, back, sizeof(fb));
	fb_ready = 1;
}

/* ---------------------------------------------------------------------------
 * input: a queue the test fills
 * ------------------------------------------------------------------------- */
#define HOST_Q 256
static nb_event q[HOST_Q];
static int q_head, q_tail;

void nb_host_event_push(int code, int down)
{
	int n = (q_head + 1) % HOST_Q;

	if (n == q_tail)
		return;
	q[q_head].type = NB_EV_KEY;
	q[q_head].code = code;
	q[q_head].down = down;
	q[q_head].x = q[q_head].y = 0;
	q[q_head].action = 0;
	q_head = n;
}

void nb_host_event_touch(int x, int y, int action)
{
	int n = (q_head + 1) % HOST_Q;

	if (n == q_tail)
		return;
	q[q_head].type = NB_EV_TOUCH;
	q[q_head].code = 0;
	q[q_head].down = (action == 1);
	q[q_head].x = x;
	q[q_head].y = y;
	q[q_head].action = action;
	q_head = n;
}

int nb_input_poll(nb_event *ev)
{
	if (q_tail == q_head)
		return 0;
	*ev = q[q_tail];
	q_tail = (q_tail + 1) % HOST_Q;
	return 1;
}

int nb_input_install(void)
{
	return 1;
}

void nb_input_remove(void)
{
}

int nb_input_hooked(void)
{
	return 1;
}

void nb_sleep(int ms)
{
	(void)ms;
}

/* a clock the tests can move: seconds since the test started */
static long fake_now = 1770000000L;

long nb_time_now(void)
{
	return fake_now++;
}

/* ---------------------------------------------------------------------------
 * files: the host's own filesystem
 * ------------------------------------------------------------------------- */
/* the device hands nbCode paths like "C:\\DATA\\nbCode.hpappdir\\a.c"; the host's
 * filesystem wants '/'.  Translating here keeps every caller platform blind. */
static const char *host_path(const char *path)
{
	static char buf[NB_PATH_MAX];
	int i;

	nb_str_copy(buf, sizeof(buf), path, -1);
	for (i = 0; buf[i]; i++) {
		if (buf[i] == '\\')
			buf[i] = '/';
	}
	if (buf[0] == '/' && buf[1] == '/')
		return buf + 1;		/* a UNC-looking device path: drop one */
	return buf;
}

void *nb_file_open(const char *path, const char *mode)
{
	return fopen(host_path(path), mode);
}

int nb_file_read(void *fh, char *buf, int len)
{
	return (int)fread(buf, 1, (size_t)len, (FILE *)fh);
}

int nb_file_write(void *fh, const char *buf, int len)
{
	return (int)fwrite(buf, 1, (size_t)len, (FILE *)fh);
}

int nb_file_close(void *fh)
{
	return fclose((FILE *)fh);
}

int nb_file_size(const char *path)
{
	FILE *f = fopen(host_path(path), "rb");
	long n;

	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (int)n;
}

int nb_file_remove(const char *path)
{
	return remove(host_path(path)) == 0;
}

char *nb_file_slurp(const char *path, int *len, const char **err)
{
	FILE *f = fopen(host_path(path), "rb");
	char *buf;
	long n;

	if (err)
		*err = "open failed";
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || n > NB_FILE_MAX) {
		fclose(f);
		if (err)
			*err = "too big";
		return NULL;
	}
	buf = (char *)malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		if (err)
			*err = "no memory";
		return NULL;
	}
	if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		if (err)
			*err = "read failed";
		return NULL;
	}
	buf[n] = 0;
	fclose(f);
	*len = (int)n;
	return buf;
}

static char last_written[NB_PATH_MAX];

int nb_file_spit(const char *path, const char *data, int len, const char **err)
{
	FILE *f = fopen(host_path(path), "wb");

	if (err)
		*err = "";
	if (!f) {
		if (err)
			*err = "cannot open for write";
		return 0;
	}
	if (len > 0 && fwrite(data, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		if (err)
			*err = "write failed";
		return 0;
	}
	fclose(f);
	nb_str_copy(last_written, sizeof(last_written), path, -1);
	return 1;
}

const char *nb_host_last_written(void)
{
	return last_written;
}

/* ---------------------------------------------------------------------------
 * logging: straight to stdout, prefixed so a test can grep it
 * ------------------------------------------------------------------------- */
void nb_log(const char *s)
{
	fputs("[nbcode] ", stdout);
	fputs(s, stdout);
	fflush(stdout);
}

void nb_log_num(const char *tag, unsigned int v)
{
	printf("[nbcode] %s%u\n", tag, v);
	fflush(stdout);
}

void nb_log_hex(const char *tag, unsigned int v)
{
	printf("[nbcode] %s0x%08X\n", tag, v);
	fflush(stdout);
}

void nb_set_logpath(const char *p)
{
	(void)p;
}

void nb_logfile(const char *s)
{
	fputs(s, stdout);
}

/* on the host there is no reboot to survive: a milestone is just a log line,
 * and the tests can grep for it */
void nb_milestone(const char *s)
{
	printf("[milestone] %s\n", s);
}

/* the host has no log ring: the exit status is a value, not a line of text */
void nb_log_ret(int status)
{
	printf("[nbcode] RT_RET:%d\n", status);
}

/* ---------------------------------------------------------------------------
 * the launch config and the file list
 * ------------------------------------------------------------------------- */
/* THE HOST HAS NO SEPARATE FILE LIST.  The workspaces and the file list live in
 * the launch config and nowhere else, on the calculator too (src/nb_list.c reads
 * nb_config_get()), so the host fills the same struct the launcher's file would
 * fill instead of keeping a private copy that can drift from it. */
static char open_path[NB_PATH_MAX];

/* ONE CONFIG INSTANCE, THE SAME ONE THE CALCULATOR HAS.  port/nb_fw.c defines
 * nb_cfg_instance; on the host it is this variable, so src/nb_list.c and
 * src/nb_app.c run unchanged against whichever side is loaded. */
nb_config nb_cfg_instance;

/* the device's folder hint, as a host path the tests can override (the weak
 * symbol in port/nb_fw.c is not in the host link) */
const char *nb_cfg_dir_fallback = ".";

const char *nb_cfg_dir_hint(void)
{
	return nb_cfg_dir_fallback;
}

nb_config *nb_host_config(void)
{
	return &nb_cfg_instance;
}

const nb_config *nb_config_get(void)
{
	return &nb_cfg_instance;
}

/* Stand in for the launcher: one workspace, the listed names, sizes measured on
 * the real filesystem (the host cwd is where the test put them). */
/* ---- the host's launch config, with the launcher's DIRECTORY TABLE ---------
 *
 * The real launcher lists the folders it wants the browser to show (src/nb_cfg.h:
 * the directory table); these build the same shape from a name list, so the host
 * tests drive the same code the calculator runs. */
static int host_add_dir(nb_config *cfg, const char *path, unsigned int parent)
{
	int d = cfg->ndirs;
	int room, need;

	if (d >= NB_CFG_MAX_DIRS)
		return -1;
	need = (int)strlen(path) + 1;
	room = (int)sizeof(cfg->names) - cfg->names_used;
	if (room < need)
		return -1;
	cfg->dir[d].path = (unsigned short)cfg->names_used;
	nb_str_copy(cfg->names + cfg->names_used, room, path, -1);
	cfg->names_used += need;
	cfg->dir[d].parent = parent;
	cfg->dir[d].first = (unsigned short)cfg->nfiles;
	cfg->dir[d].count = 0;
	cfg->ndirs++;
	return d;
}

static void host_add_file(nb_config *cfg, int d, const char *name,
			  const char *dir)
{
	char full[NB_PATH_MAX];
	int need = (int)strlen(name) + 1;

	if (d < 0 || cfg->nfiles >= NB_CFG_MAX_FILES)
		return;
	if (cfg->names_used + need > (int)sizeof(cfg->names))
		return;
	nb_str_copy(cfg->names + cfg->names_used,
		    (int)sizeof(cfg->names) - cfg->names_used, name, -1);
	cfg->file[cfg->nfiles].name = (unsigned short)cfg->names_used;
	cfg->names_used += need;
	if (dir && dir[0] && strcmp(dir, ".") != 0)
		snprintf(full, sizeof(full), "%s/%s", dir, name);
	else
		nb_str_copy(full, sizeof(full), name, -1);
	cfg->file[cfg->nfiles].bytes = (unsigned int)nb_file_size(full);
	cfg->file[cfg->nfiles].isdir = 0;
	cfg->nfiles++;
	cfg->dir[d].count++;
}

void nb_host_files(const char *dir, const char *const *nm, int n)
{
	nb_config *cfg = &nb_cfg_instance;
	int i, d;

	nb_cfg_defaults(cfg);
	nb_str_copy(cfg->cwd, sizeof(cfg->cwd), dir, -1);
	nb_str_copy(cfg->workspace[0], NB_CFG_PATH_MAX, dir, -1);
	cfg->nworkspaces = 1;
	cfg->default_workspace = 0;
	d = host_add_dir(cfg, dir, NB_CFG_NO_DIR);
	cfg->ws_dir[0] = d;
	for (i = 0; i < n; i++)
		host_add_file(cfg, d, nm[i], dir);
	nb_file_select(0);
}

/* Install a SECOND workspace with its own file list.  The start screen must show
 * each folder's own files; it used to show the first folder's for every choice,
 * because the launch file carried one list (see src/nb_cfg.h). */
void nb_host_add_workspace(const char *dir, const char *const *nm, int n)
{
	nb_config *cfg = &nb_cfg_instance;
	int w, i, d;

	if (cfg->nworkspaces >= NB_CFG_MAX_WS)
		return;
	w = cfg->nworkspaces++;
	nb_str_copy(cfg->workspace[w], NB_CFG_PATH_MAX, dir, -1);
	d = host_add_dir(cfg, dir, NB_CFG_NO_DIR);
	cfg->ws_dir[w] = d;
	for (i = 0; i < n; i++)
		host_add_file(cfg, d, nm[i], dir);
}

/* Install a FOLDER inside a workspace: the browser shows it as a row and steps
 * into it (that is how the .hpappdir folders under C:\DATA are opened).
 *
 * The rows of a directory must be CONTIGUOUS in the entry table (the format says
 * "first + count", which is what the launcher writes and what keeps the image's
 * lookup cheap), so the folder's own row goes into the parent FIRST -- while the
 * parent's range still ends at the end of the table -- and the folder's contents
 * follow it. */
int nb_host_add_subdir(int ws, const char *path, const char *const *nm, int n)
{
	nb_config *cfg = &nb_cfg_instance;
	unsigned short row;
	int i, d, pd, need;

	if (ws < 0 || ws >= cfg->nworkspaces)
		return 0;
	pd = cfg->ws_dir[ws];
	need = (int)strlen(path) + 1;
	if (cfg->names_used + need > (int)sizeof(cfg->names))
		return 0;
	if (cfg->nfiles >= NB_CFG_MAX_FILES || cfg->ndirs >= NB_CFG_MAX_DIRS)
		return 0;
	row = (unsigned short)cfg->names_used;
	nb_str_copy(cfg->names + cfg->names_used,
		    (int)sizeof(cfg->names) - cfg->names_used, path, -1);
	cfg->names_used += need;

	d = cfg->ndirs;
	cfg->file[cfg->nfiles].name = row;
	cfg->file[cfg->nfiles].bytes = (unsigned int)d;	/* the child's index */
	cfg->file[cfg->nfiles].isdir = 1;
	cfg->nfiles++;
	cfg->dir[pd].count++;

	cfg->dir[d].path = row;			/* the same path string */
	cfg->dir[d].parent = (unsigned int)pd;
	cfg->dir[d].first = (unsigned short)cfg->nfiles;
	cfg->dir[d].count = 0;
	cfg->ndirs++;
	for (i = 0; i < n; i++)
		host_add_file(cfg, d, nm[i], path);
	return d;
}

void nb_host_reset(void)
{
	nb_cfg_defaults(&nb_cfg_instance);
	memset(fb, 0, sizeof(fb));
	q_head = q_tail = 0;
	last_written[0] = 0;
	open_path[0] = 0;
}

/* ---------------------------------------------------------------------------
 * exit
 * ------------------------------------------------------------------------- */
static int exit_status;

void nb_run_finish(int status)
{
	exit_status = status;
	nb_input_remove();
}

int nb_host_exit_status(void)
{
	return exit_status;
}
