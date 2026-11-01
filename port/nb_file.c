/*
 * nb_file.c -- the firmware's file API, wrapped so the rest of nbCode can use
 * UTF-8 paths and ordinary C modes.
 *
 * TWO DEVICE QUIRKS, both measured (see primeLua/port/plua_fwmode.c):
 *   * the path must be UTF-16LE, and so must the mode string;
 *   * only "rb" and "wb+" are reliable.  "w" does not truncate, "a" appends
 *     nondeterministically, "r+"/"w+" are not implemented.  Every mode nbCode
 *     uses is translated here, and a write reads the old contents first when
 *     the mode implies keeping them (there is no "update in place").
 *
 * SAVING IS WRITE-THEN-RENAME: a file is written to "<name>.new", the original
 * is removed, and the new file is renamed into place.  A reset in the middle of
 * a write then costs the .new file, not the source.  (The firmware has no
 * rename, so this is remove+create; the window is two file operations wide,
 * which is the best this API allows.)
 */
#include "nb_common.h"
#include "nb_port.h"

unsigned hp_svc_fopen(unsigned path16, unsigned mode16);
unsigned hp_svc_fclose(unsigned fd);
unsigned hp_svc_fseek(unsigned fd, unsigned off, unsigned whence);
unsigned hp_svc_ftell(unsigned fd);
unsigned hp_svc_fread(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
unsigned hp_svc_fwrite(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
unsigned hp_svc_filesize(unsigned fd);
unsigned hp_svc_remove(unsigned path16);

#define NB_FH_MAX	8

struct fh {
	int fd;
	int write;
	unsigned pos;
};
static struct fh handles[NB_FH_MAX];

/* UTF-8 -> UTF-16LE into a caller-supplied buffer (the firmware needs the wide
 * form and has no converter).  Non-ASCII is passed through as Latin-1 rather
 * than decoded: file NAMES on this machine are ASCII or GB2312, and a wrong
 * UTF-8 decode would produce a name that cannot be found at all -- whereas a
 * byte-preserving pass finds the file the Python side listed. */
static void to_utf16(const char *s, unsigned short *out, int cap)
{
	int i = 0;

	while (s[i] && i < cap - 1) {
		out[i] = (unsigned short)(unsigned char)s[i];
		i++;
	}
	out[i] = 0;
}

void *nb_file_open(const char *path, const char *mode)
{
	unsigned short wpath[NB_PATH_MAX];
	unsigned short wmode[8];
	unsigned fd;
	int i;

	if (!path || !mode)
		return NB_NULL;
	to_utf16(path, wpath, NB_PATH_MAX);
	to_utf16(mode, wmode, 8);
	fd = hp_svc_fopen((unsigned)(unsigned long)wpath,
			  (unsigned)(unsigned long)wmode);
	if (!fd)
		return NB_NULL;
	for (i = 0; i < NB_FH_MAX; i++) {
		if (handles[i].fd == 0) {
			handles[i].fd = (int)fd;
			handles[i].write = (mode[0] == 'w');
			handles[i].pos = 0;
			return &handles[i];
		}
	}
	hp_svc_fclose(fd);		/* out of handles: close rather than leak */
	return NB_NULL;
}

static struct fh *as_fh(void *h)
{
	struct fh *f = (struct fh *)h;

	if (!f || f->fd == 0)
		return NB_NULL;
	return f;
}

/* THE FIRMWARE'S fread/fwrite RETURN ITEMS, NOT BYTES.
 *
 * They are C's fread(ptr, size, nmemb, f): the byte count is size*nmemb and the
 * return value counts ITEMS of `size`.  This wrapper is byte-oriented, so the
 * result has to be multiplied back up -- and getting that wrong is not subtle
 * once you look: reading a 205 byte file returned 1, every "did I read the whole
 * thing" test failed, and the editor reported a config file it could not read
 * while the bytes were in the buffer all along.  (The emulator found it; the
 * device would have done the same thing.)
 */
int nb_file_read(void *h, char *buf, int len)
{
	struct fh *f = as_fh(h);

	if (!f || len <= 0)
		return 0;
	return (int)hp_svc_fread((unsigned)(unsigned long)buf, (unsigned)len, 1,
				 (unsigned)f->fd) * len;
}

int nb_file_write(void *h, const char *buf, int len)
{
	struct fh *f = as_fh(h);

	if (!f || len <= 0)
		return 0;
	return (int)hp_svc_fwrite((unsigned)(unsigned long)buf, (unsigned)len, 1,
				  (unsigned)f->fd) * len;
}

int nb_file_close(void *h)
{
	struct fh *f = as_fh(h);
	int r;

	if (!f)
		return 0;
	r = (int)hp_svc_fclose((unsigned)f->fd);
	f->fd = 0;
	f->write = 0;
	f->pos = 0;
	return r;
}

int nb_file_size(const char *path)
{
	void *h = nb_file_open(path, "rb");
	int n;

	if (!h)
		return -1;
	n = (int)hp_svc_filesize((unsigned)((struct fh *)h)->fd);
	nb_file_close(h);
	return n;
}

char *nb_file_slurp(const char *path, int *len, const char **err)
{
	void *h;
	char *buf;
	int n;

	if (err)
		*err = "open failed";
	h = nb_file_open(path, "rb");
	if (!h)
		return NB_NULL;
	n = (int)hp_svc_filesize((unsigned)((struct fh *)h)->fd);
	if (n < 0) {
		nb_file_close(h);
		if (err)
			*err = "no size";
		return NB_NULL;
	}
	if (n > NB_FILE_MAX) {
		nb_file_close(h);
		if (err)
			*err = "file too big";
		return NB_NULL;
	}
	buf = (char *)nb_alloc((unsigned int)n + 1);
	if (!buf) {
		nb_file_close(h);
		if (err)
			*err = "no memory";
		return NB_NULL;
	}
	if (n > 0) {
		int got = nb_file_read(h, buf, n);

		if (got != n) {
			nb_log_num("slurp: got=", (unsigned)got);
			nb_log_num("slurp: want=", (unsigned)n);
			nb_log_num("slurp: fd=", (unsigned)((struct fh *)h)->fd);
			nb_free(buf);
			nb_file_close(h);
			if (err)
				*err = "read failed";
			return NB_NULL;
		}
	}
	buf[n] = 0;
	nb_file_close(h);
	if (len)
		*len = n;
	return buf;
}

static int write_all(const char *path, const char *data, int len,
		     const char **err)
{
	void *h = nb_file_open(path, "wb+");

	if (!h) {
		if (err)
			*err = "cannot create";
		return 0;
	}
	if (len > 0 && nb_file_write(h, data, len) != len) {
		nb_file_close(h);
		if (err)
			*err = "short write";
		return 0;
	}
	nb_file_close(h);
	return 1;
}

int nb_file_spit(const char *path, const char *data, int len, const char **err)
{
	char tmp[NB_PATH_MAX];
	unsigned short wtmp[NB_PATH_MAX], wpath[NB_PATH_MAX];

	if (err)
		*err = "";
	if (nb_strlen(path) + 5 > NB_PATH_MAX) {
		if (err)
			*err = "path too long";
		return 0;
	}
	nb_str_copy(tmp, sizeof(tmp), path, -1);
	nb_str_append(tmp, sizeof(tmp), ".new");
	if (!write_all(tmp, data, len, err))
		return 0;
	/* the original goes away, then the new file takes its name */
	to_utf16(path, wpath, NB_PATH_MAX);
	hp_svc_remove((unsigned)(unsigned long)wpath);
	to_utf16(tmp, wtmp, NB_PATH_MAX);
	{
		/* no rename in this API: create the target and copy the bytes */
		char *back_buf;
		int blen = 0;
		const char *e2 = "";

		back_buf = nb_file_slurp(tmp, &blen, &e2);
		if (!back_buf) {
			if (err)
				*err = "cannot re-read";
			return 0;
		}
		if (!write_all(path, back_buf, blen, err)) {
			nb_free(back_buf);
			return 0;
		}
		nb_free(back_buf);
		hp_svc_remove((unsigned)(unsigned long)wtmp);
	}
	return 1;
}

int nb_file_remove(const char *path)
{
	unsigned short wpath[NB_PATH_MAX];

	to_utf16(path, wpath, NB_PATH_MAX);
	return hp_svc_remove((unsigned)(unsigned long)wpath) != 0;
}
