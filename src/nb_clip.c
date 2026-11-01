/*
 * nb_clip.c -- the global clipboard (nb_clip.h explains what it is for).
 *
 * TWO HALVES, DELIBERATELY SEPARATE: the LIST (pure memory, no I/O, testable on
 * its own) and the FILE (the port layer's slurp/spit).  Nothing here allocates:
 * ten entries of NB_CLIP_ENTRY bytes are 1930 bytes of .bss, which is less than
 * one screen line of undo history and cannot fail at the moment the user presses
 * copy.
 */
#include "nb_clip.h"
#include "nb_port.h"

static char entry[NB_CLIP_MAX][NB_CLIP_ENTRY + 1];
static int len[NB_CLIP_MAX];
static int count;			/* entries in use, newest at index 0 */
static char dir[NB_PATH_MAX];		/* where the file goes ("" = memory only) */
static char path[NB_PATH_MAX];

void nb_clip_reset(void)
{
	int i;

	for (i = 0; i < NB_CLIP_MAX; i++) {
		entry[i][0] = 0;
		len[i] = 0;
	}
	count = 0;
}

int nb_clip_count(void)
{
	return count;
}

const char *nb_clip_text(int i)
{
	if (i < 0 || i >= count)
		return "";
	return entry[i];
}

int nb_clip_len(int i)
{
	if (i < 0 || i >= count)
		return 0;
	return len[i];
}

static int same(int i, const char *text, int n)
{
	if (len[i] != n)
		return 0;
	if (n <= 0)
		return 1;
	return nb_memcmp(entry[i], text, (unsigned int)n) == 0;
}

int nb_clip_add(const char *text, int n)
{
	int i;

	if (!text || n <= 0)
		return 0;
	if (n > NB_CLIP_ENTRY)
		n = NB_CLIP_ENTRY;		/* the tail is dropped, and the
						 * status line says so when it
						 * happens (see nb_ui_editor.c) */
	/* ALREADY ON TOP: nothing moved, and the caller does not have to write the
	 * file again for a copy the user just repeated */
	if (count > 0 && same(0, text, n))
		return 0;
	/* ... and an entry that is already in the list comes back to the top
	 * instead of being stored twice: ten slots are few, and the user who
	 * copies the same line again wants it where their finger is */
	for (i = 0; i < count; i++) {
		if (same(i, text, n)) {
			int j;

			for (j = i; j > 0; j--) {
				nb_memcpy(entry[j], entry[j - 1],
					  (unsigned int)len[j - 1] + 1);
				len[j] = len[j - 1];
			}
			nb_memcpy(entry[0], text, (unsigned int)n);
			entry[0][n] = 0;
			len[0] = n;
			return 1;
		}
	}
	/* the new one at the top, the rest shifted down, the oldest dropped */
	{
		int last = (count < NB_CLIP_MAX) ? count : NB_CLIP_MAX - 1;

		for (i = last; i > 0; i--) {
			nb_memcpy(entry[i], entry[i - 1],
				  (unsigned int)len[i - 1] + 1);
			len[i] = len[i - 1];
		}
		if (count < NB_CLIP_MAX)
			count++;
	}
	nb_memcpy(entry[0], text, (unsigned int)n);
	entry[0][n] = 0;
	len[0] = n;
	return 1;
}

int nb_clip_delete(int i)
{
	int j;

	if (i < 0 || i >= count)
		return 0;
	for (j = i; j + 1 < count; j++) {
		nb_memcpy(entry[j], entry[j + 1],
			  (unsigned int)len[j + 1] + 1);
		len[j] = len[j + 1];
	}
	count--;
	entry[count][0] = 0;
	len[count] = 0;
	return 1;
}

int nb_clip_top(int i)
{
	char tmp[NB_CLIP_ENTRY + 1];
	int n, j;

	if (i <= 0 || i >= count)
		return 0;
	n = len[i];
	nb_memcpy(tmp, entry[i], (unsigned int)n + 1);
	for (j = i; j > 0; j--) {
		nb_memcpy(entry[j], entry[j - 1], (unsigned int)len[j - 1] + 1);
		len[j] = len[j - 1];
	}
	nb_memcpy(entry[0], tmp, (unsigned int)n + 1);
	len[0] = n;
	return 1;
}

/* ---------------------------------------------------------------------------
 * the file
 * ------------------------------------------------------------------------- */
void nb_clip_set_dir(const char *d)
{
	int n = 0;

	dir[0] = 0;
	path[0] = 0;
	if (!d || !d[0])
		return;
	/* ONE trailing separator, and never two: the path is built by appending the
	 * file name, and "dir\\\file" is a spelling the firmware's file API does
	 * not accept (the same rule nb_file.c follows for the log) */
	while (d[n] && n < NB_PATH_MAX - 1) {
		dir[n] = d[n];
		n++;
	}
	dir[n] = 0;
	while (n > 1 && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
		n--;
	dir[n] = 0;
	if (!dir[0])
		return;
	nb_str_copy(path, sizeof(path), dir, -1);
	nb_str_append(path, sizeof(path), "\\");
	nb_str_append(path, sizeof(path), NB_CLIP_FILE);
}

const char *nb_clip_path(void)
{
	return path;
}

int nb_clip_save(void)
{
	void *h;
	char head[16];
	int i;

	if (!path[0])
		return NB_NO;
	h = nb_file_open(path, "wb");
	if (!h)
		return NB_NO;
	nb_file_write(h, NB_CLIP_MAGIC, (int)nb_strlen(NB_CLIP_MAGIC));
	for (i = 0; i < count; i++) {
		nb_fmt_int(head, sizeof(head), len[i]);
		nb_file_write(h, head, (int)nb_strlen(head));
		nb_file_write(h, "\n", 1);
		nb_file_write(h, entry[i], len[i]);
		nb_file_write(h, "\n", 1);
	}
	nb_file_close(h);
	return NB_YES;
}

int nb_clip_load(void)
{
	char *raw;
	const char *err = "";
	int total = 0, i, magic, got = 0;

	nb_clip_reset();
	if (!path[0])
		return 0;
	raw = nb_file_slurp(path, &total, &err);
	if (!raw)
		return 0;
	magic = (int)nb_strlen(NB_CLIP_MAGIC);
	/* A FILE THAT IS NOT OURS IS NOT AN ERROR: it is a clipboard that has not
	 * been written yet, or one a user edited by hand.  Whatever parses is kept
	 * and the rest is dropped -- losing a clipboard is not worth a message. */
	if (total < magic || nb_memcmp(raw, NB_CLIP_MAGIC, (unsigned int)magic) != 0) {
		nb_free(raw);
		return 0;
	}
	i = magic;
	while (got < NB_CLIP_MAX && i < total) {
		int v = 0, digits = 0;

		while (i < total && raw[i] >= '0' && raw[i] <= '9' && digits < 4) {
			v = v * 10 + (raw[i] - '0');
			i++;
			digits++;
		}
		if (digits == 0 || i >= total || raw[i] != '\n')
			break;
		i++;
		if (v < 0 || v > NB_CLIP_ENTRY || i + v > total)
			break;
		nb_memcpy(entry[got], raw + i, (unsigned int)v);
		entry[got][v] = 0;
		len[got] = v;
		got++;
		i += v;
		if (i < total && raw[i] == '\n')
			i++;
		else
			break;
	}
	nb_free(raw);
	count = got;
	return got;
}
