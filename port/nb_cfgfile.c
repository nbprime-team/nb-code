/*
 * nb_cfgfile.c -- read the launch file (src/nb_cfg.h) into nb_config.
 *
 * The launcher writes "nbcode.cfg" next to the image; this reads it with the
 * firmware's own file API and fills a nb_config.  Everything here is defensive
 * on purpose: a truncated file, a file from another version and a file that is
 * not there at all all have to end in "the editor starts with defaults", never
 * in a read past the end of a buffer.
 *
 * THE FORMAT IS SELF-DESCRIBING AND FLAT: a header of words, then
 * NUL-terminated UTF-8 strings.  No pointers, no struct layout shared between
 * Python and C, and nothing to get out of step when a field is added -- which is
 * the point (see the header's comment for what the memory-block version cost).
 */
#include "nb_common.h"
#include "nb_cfg.h"
#include "nb_port.h"

/* A whole-file read: the launcher keeps this under 64 KB, and refusing a bigger
 * one is cheaper than trusting it. */
#define NB_CFG_MAX_BYTES	(64 * 1024)

struct reader {
	const char *data;
	int len;
	int pos;
	int bad;
};

static unsigned int rd_u32(struct reader *r)
{
	unsigned int v;

	if (r->pos + 4 > r->len) {
		r->bad = 1;
		return 0;
	}
	v = (unsigned int)(unsigned char)r->data[r->pos] |
	    ((unsigned int)(unsigned char)r->data[r->pos + 1] << 8) |
	    ((unsigned int)(unsigned char)r->data[r->pos + 2] << 16) |
	    ((unsigned int)(unsigned char)r->data[r->pos + 3] << 24);
	r->pos += 4;
	return v;
}

/* a NUL-terminated string into a fixed buffer; returns its length, or -1 when
 * the file ran out first */
static int rd_str(struct reader *r, char *out, int cap)
{
	int n = 0;

	while (r->pos < r->len && r->data[r->pos] != 0) {
		if (n < cap - 1)
			out[n] = r->data[r->pos];
		n++;
		r->pos++;
	}
	if (r->pos >= r->len) {
		r->bad = 1;
		return -1;
	}
	r->pos++;			/* the NUL */
	out[n < cap ? n : cap - 1] = 0;
	return n;
}

void nb_cfg_defaults(nb_config *c)
{
	int i;

	nb_memset(c, 0, sizeof(*c));
	c->magic = NB_CFG_MAGIC;
	c->version = NB_CFG_VERSION;
	c->theme = 0;
	c->tab_width = 4;
	c->language = 0;
	c->nworkspaces = 0;
	c->default_workspace = 0;
	c->nfiles = 0;
	c->ndirs = 0;
	c->names_used = 0;
	for (i = 0; i < NB_CFG_MAX_WS; i++)
		c->ws_dir[i] = (int)NB_CFG_NO_DIR;
}

/* the same folder, allowing for case and a trailing separator */
static int path_same(const char *a, const char *b)
{
	int i = 0, j = 0;

	while (a[i] && b[j]) {
		char ca = a[i], cb = b[j];

		if (ca == '/')
			ca = '\\';
		if (cb == '/')
			cb = '\\';
		if (ca >= 'A' && ca <= 'Z')
			ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (char)(cb - 'A' + 'a');
		if (ca != cb)
			return 0;
		i++;
		j++;
	}
	/* one trailing separator is not a difference */
	if (a[i] == '\\' || a[i] == '/')
		i++;
	if (b[j] == '\\' || b[j] == '/')
		j++;
	return a[i] == 0 && b[j] == 0;
}

int nb_cfg_load(nb_config *c, const char *dir)
{
	char *raw;
	int len = 0;
	const char *err = "";
	struct reader r;
	int i;

	nb_cfg_defaults(c);

	/* TWO SPELLINGS OF THE SAME FILE, because the two sides of this handoff
	 * disagree about what a path is.
	 *
	 * The launcher writes nbcode.cfg with a BARE NAME: MicroPython's file API
	 * resolves relative names against the application folder and refuses an
	 * absolute "C:\DATA\..." path outright (OSError 0 -- measured on hardware).
	 * The image reads it with the FIRMWARE's file API, which is the opposite:
	 * it wants the full device path.  `dir` is the application folder the
	 * launcher handed over in r0 (src/nb_cfg.h, port/nb_fw.c), so the device
	 * spelling is tried first and the bare name second; a launcher that knew
	 * neither leaves the second one to try.  If both fail the editor starts
	 * with its defaults and the log says so -- never a black screen.
	 */
	raw = NB_NULL;
	if (dir && dir[0]) {
		char beside[NB_PATH_MAX];

		nb_str_copy(beside, sizeof(beside), dir, -1);
		nb_str_append(beside, sizeof(beside), "\\");
		nb_str_append(beside, sizeof(beside), NB_CFG_FILE_NAME);
		raw = nb_file_slurp(beside, &len, &err);
		if (raw)
			nb_log("cfg: read by device path\n");
	}
	if (!raw)
		raw = nb_file_slurp(NB_CFG_FILE_NAME, &len, &err);
	if (!raw) {
		nb_log("cfg: no ");
		nb_log(NB_CFG_FILE_NAME);
		nb_log(" (");
		nb_log(err ? err : "?");
		nb_log("): defaults\n");
		return 0;
	}
	if (len > NB_CFG_MAX_BYTES) {
		nb_free(raw);
		nb_log("cfg: too big: defaults\n");
		return 0;
	}

	r.data = raw;
	r.len = len;
	r.pos = 0;
	r.bad = 0;
	{
		unsigned int magic = rd_u32(&r);
		unsigned int version = rd_u32(&r);

		if (magic != NB_CFG_MAGIC) {
			nb_log_hex("cfg: bad magic ", magic);
			nb_free(raw);
			nb_cfg_defaults(c);
			return 0;
		}
		if (version != NB_CFG_VERSION) {
			/* a different revision: refuse rather than misread */
			nb_log_hex("cfg: version ", version);
			nb_free(raw);
			nb_cfg_defaults(c);
			return 0;
		}
	}
	c->flags = rd_u32(&r);
	c->theme = (int)rd_u32(&r);
	c->tab_width = (int)rd_u32(&r);
	c->language = (int)rd_u32(&r);
	c->frames = rd_u32(&r);
	c->nworkspaces = (int)rd_u32(&r);
	c->default_workspace = (int)rd_u32(&r);
	rd_str(&r, c->cwd, NB_CFG_PATH_MAX);
	rd_str(&r, c->open_file, NB_CFG_PATH_MAX);
	rd_str(&r, c->logpath, NB_CFG_PATH_MAX);

	if (c->tab_width < 2 || c->tab_width > 8)
		c->tab_width = 4;
	if (c->theme != 1)
		c->theme = 0;
	if (c->language < 0 || c->language > 5)
		c->language = 0;
	if (c->nworkspaces < 0 || c->nworkspaces > NB_CFG_MAX_WS)
		c->nworkspaces = 0;
	if (c->default_workspace < 0 ||
	    c->default_workspace >= c->nworkspaces)
		c->default_workspace = 0;

	for (i = 0; i < c->nworkspaces; i++) {
		if (rd_str(&r, c->workspace[i], NB_CFG_PATH_MAX) < 0)
			break;
	}
	if (i < c->nworkspaces)
		c->nworkspaces = i;

	/* THE DIRECTORY TABLE: the launcher lists every folder it wants the browser
	 * to be able to show -- the workspaces, and the folders inside them -- and
	 * says where each one's entries are.  A DIRECTORY entry carries its own
	 * index in the `bytes` field, which is how choosing a folder in the list
	 * works. */
	{
		unsigned int ndirs = rd_u32(&r), d;
		int k;

		if (ndirs > NB_CFG_MAX_DIRS) {
			nb_log_num("cfg: too many directories, dropping=",
				   ndirs - NB_CFG_MAX_DIRS);
			ndirs = NB_CFG_MAX_DIRS;
			r.bad = 1;
		}
		c->ndirs = (int)ndirs;
		c->nfiles = 0;
		c->names_used = 0;
		for (d = 0; d < ndirs; d++) {
			char path[NB_CFG_PATH_MAX];
			int room, need;
			unsigned int parent;
			int n, made = 0;

			if (rd_str(&r, path, (int)sizeof(path)) < 0)
				break;
			parent = rd_u32(&r);
			n = (int)rd_u32(&r);
			if (n < 0)
				n = 0;
			need = (int)nb_strlen(path) + 1;
			room = (int)sizeof(c->names) - c->names_used;
			if (room < need) {
				nb_log("cfg: the name pool is full: directories dropped\n");
				r.bad = 1;
				break;
			}
			c->dir[d].path = (unsigned short)c->names_used;
			nb_str_copy(c->names + c->names_used, room, path, -1);
			c->names_used += need;
			c->dir[d].parent = parent;
			c->dir[d].first = (unsigned short)c->nfiles;
			c->dir[d].count = 0;
			for (k = 0; k < n; k++) {
				unsigned int bytes = rd_u32(&r);
				unsigned int isdir = rd_u32(&r);
				char name[NB_CFG_NAME_MAX];
				int nroom, nneed;

				if (rd_str(&r, name, (int)sizeof(name)) < 0)
					break;
				nneed = (int)nb_strlen(name) + 1;
				if (c->nfiles >= NB_CFG_MAX_FILES)
					continue;
				nroom = (int)sizeof(c->names) - c->names_used;
				if (nroom < nneed)
					continue;
				c->file[c->nfiles].name =
					(unsigned short)c->names_used;
				c->file[c->nfiles].bytes = bytes;
				c->file[c->nfiles].isdir = isdir;
				nb_str_copy(c->names + c->names_used, nroom,
					    name, -1);
				c->names_used += nneed;
				c->nfiles++;
				made++;
			}
			c->dir[d].count = (unsigned short)made;
			if (k < n) {
				nb_log_num("cfg: directory truncated, dropped=",
					   (unsigned)(n - k));
				r.bad = 1;
			}
		}
		if (d < ndirs) {
			c->ndirs = (int)d;
			r.bad = 1;
		}
	}

	/* WHICH DIRECTORY EACH WORKSPACE STARTS AT: the first directory whose path
	 * is the workspace, or the first directory at all when the launcher named
	 * none. */
	/* NO MATCH MEANS NO LIST, NEVER "the first directory".
	 *
	 * Directory 0 is the first WORKSPACE, which in practice is the application
	 * folder: falling back to it made every workspace the launcher had no
	 * directory for browse nbCode.hpappdir's own files.  A user cannot tell that
	 * apart from a working folder -- they see files, just the wrong ones -- and
	 * it is what the report "choosing another .hpappdir always turns into
	 * nbcode.hpappdir" was.
	 *
	 * The comparison is deliberately forgiving about what does not change which
	 * folder is meant: case, and a trailing separator. */
	for (i = 0; i < c->nworkspaces; i++) {
		int d;

		c->ws_dir[i] = (int)NB_CFG_NO_DIR;
		for (d = 0; d < c->ndirs; d++) {
			const char *p = c->names + c->dir[d].path;

			if (path_same(p, c->workspace[i])) {
				c->ws_dir[i] = d;
				break;
			}
		}
		nb_log_num("cfg: workspace -> dir ", (unsigned)c->ws_dir[i]);
	}

	/* a cwd the launcher left empty means "the workspace it selected" */
	if (!c->cwd[0] && c->nworkspaces > 0 &&
	    c->default_workspace < c->nworkspaces)
		nb_str_copy(c->cwd, sizeof(c->cwd),
			    c->workspace[c->default_workspace], -1);

	nb_log_num("cfg: workspaces=", (unsigned)c->nworkspaces);
	nb_log_num("cfg: files=", (unsigned)c->nfiles);
	nb_log_num("cfg: names=", (unsigned)c->names_used);
	nb_log_num("cfg: dirs=", (unsigned)c->ndirs);
	nb_log_num("cfg: theme=", (unsigned)c->theme);
	nb_log_num("cfg: tab=", (unsigned)c->tab_width);
	nb_free(raw);
	if (r.bad) {
		/* a truncated file: keep what parsed (the header is intact, and a
		 * partial list is better than none) but say so */
		nb_log("cfg: truncated after the header\n");
	}
	return 1;
}
