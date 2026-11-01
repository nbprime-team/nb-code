/*
 * nb_clip.h -- THE GLOBAL CLIPBOARD: ten entries, shared by every file, kept on
 * the disk.
 *
 * WHAT IT IS FOR.  The editor had a clipboard per BUFFER (nb_buffer.c's single
 * slot: what a cut put there, for the paste right after it), which cannot carry
 * anything from one file to the next and forgets a copy the moment the file is
 * closed.  This is the other kind: the user copies with `shift`+VIEW (or from the
 * menu), opens the panel with `shift`+MENU, and gets back a LIST of the last ten
 * things they copied -- from any file, in any session, because it is written to a
 * file next to nbcode.log.
 *
 * THE ORDER IS THE POINT: index 0 is the newest, an entry that is copied twice
 * moves back to the top instead of appearing twice, and the eleventh copy pushes
 * the oldest one out.  That is the whole policy, and it is what makes the panel a
 * "what did I just copy" list rather than a junk drawer.
 */
#ifndef NB_CLIP_H
#define NB_CLIP_H

#include "nb_common.h"

#define NB_CLIP_MAX	10		/* entries kept; the 11th drops the last */
#define NB_CLIP_ENTRY	192		/* bytes of one entry (a long line of
					 * Chinese is 3 bytes a character) */

/* THE FILE: a magic line, then one record per entry, `<bytes>\n<bytes>\n`.  The
 * length prefix is what makes it BINARY SAFE: an entry can hold anything the
 * editor can hold, newlines and all (a copied line ends with one), and a
 * half-written file still parses up to the record that was cut. */
#define NB_CLIP_MAGIC	"NBCLIP1\n"
#define NB_CLIP_FILE	"nbclip.txt"

void nb_clip_reset(void);			/* forget everything (no file) */
int nb_clip_count(void);
const char *nb_clip_text(int i);		/* never NULL; "" when out of range */
int nb_clip_len(int i);

/* Add an entry at the top.  Returns 1 when the list changed, 0 when `text` is
 * empty or already the newest entry (in which case nothing moved). */
int nb_clip_add(const char *text, int len);
int nb_clip_delete(int i);			/* 1 when it went */
int nb_clip_top(int i);				/* move an entry to the top */

/* where the file is.  `path` is the DIRECTORY the durable log lives in (the
 * application folder), which is the only path the editor is told about; an empty
 * directory means "memory only" (a device whose launcher gave no log path). */
void nb_clip_set_dir(const char *dir);
const char *nb_clip_path(void);

int nb_clip_load(void);				/* entries read, 0 when none */
int nb_clip_save(void);				/* NB_YES/NB_NO (never fatal) */

#endif /* NB_CLIP_H */
