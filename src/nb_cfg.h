/*
 * nb_cfg.h -- how the Python launcher and nbCode.elf agree on a run.
 *
 * A FILE, NOT A MEMORY BLOCK, and that is a decision that cost three rewrites.
 *
 * The launcher is Python; the editor is C; they have to agree on a handful of
 * settings, a workspace to browse and a file to open.  The obvious mechanism is
 * a struct in memory, handed over in r0 (primeLua's launch config) or written
 * into a buffer the image carries.  Both work -- until the two sides disagree
 * about a struct's layout, which they did, repeatedly: the launcher's model of
 * nb_config and the compiler's differed at the tail of the structure, so the
 * editor read a pointer where a count belonged, found a zero magic, and quit
 * with a config it could not use.  Every variant of that bug looks the same from
 * the device: a black screen and "no config block" in the log.
 *
 * A FILE HAS NO LAYOUT TO DISAGREE ABOUT.  The launcher writes
 *
 *     nbCode.hpappdir\nbcode.cfg     (magic, version, 6 words, then NUL-terminated
 *                                   UTF-8 workspace paths)
 *
 * and the editor reads it through the firmware's own file API -- the same API it
 * uses for the source files it edits, so there is no second mechanism, and a
 * short file or a wrong magic is detected instead of being read as pointers.  It
 * is also the format a human can look at with the calculator's own editor.
 *
 * THIS IS NOT WHERE SETTINGS LIVE.  The user's settings are setting.json, which
 * the launcher reads and this file carries the RESULT of (theme, tab width,
 * highlighting).  setting.json stays the launcher's business so that a syntax
 * error in it cannot brick the editor.
 */
#define NB_CFG_MAGIC		0x6763626Eu	/* "nbcg" */
#define NB_CFG_VERSION		4

/* THE FILE'S NAME, BY BARE NAME.  MicroPython resolves a relative file name
 * against the application folder, which is where the launcher writes it -- and
 * the image cannot ask where it was loaded from, so a full device path would be
 * a second thing for the two sides to agree about.  calc/main.py names the same
 * string (CFG_FILE); tools/make_dist.py reads it out of here and refuses to
 * package the two if they ever disagree. */
#define NB_CFG_FILE_NAME	"nbcode.cfg"

/* the header's six words, after magic/version/size */
#define NB_CFG_MAX_WS		8
#define NB_CFG_PATH_MAX		256

#ifndef NB_CFG_H
#define NB_CFG_H

#include "nb_common.h"

/* ---------------------------------------------------------------------------
 * THE FILE (little-endian, no padding, no pointers):
 *
 *   u32 magic        NB_CFG_MAGIC ("nbcg")
 *   u32 version      NB_CFG_VERSION (4: a table of DIRECTORIES, each with its
 *                    own entries, so the browser can show folders and step into
 *                    them -- including the .hpappdir folders under C:\DATA)
 *   u32 flags        NB_CFG_FLAG_*
 *   u32 theme        0 light, 1 dark
 *   u32 tab_width    2..8 (clamped on read)
 *   u32 language     NB_LANG_*, 0 = from the file name
 *   u32 frames       a frame budget for this run, 0 = unlimited (a test, or
 *                    "just show me the screen")
 *   u32 nworkspaces  0..NB_CFG_MAX_WS
 *   u32 default_ws   which workspace the UI opens on
 *   str cwd          the workspace the names below are relative to
 *   str open_file    a file to open straight away, or "" for the browser
 *   str logpath      where the durable log goes ("" disables it)
 *   then nworkspaces NUL-terminated UTF-8 device paths
 *
 * Everything the editor needs that is NOT here it derives: the log path and the
 * settings path are the workspace's own directory, the file LIST is browsed by
 * ... nothing: the firmware's directory API is in the MicroPython application,
 * not in a loaded image, so the launcher writes the listing into the file
 * as well:
 *
 *   u32 ndirs, then ndirs entries of:
 *       str path            the directory, as a device path
 *       u32 parent          its parent's index, or 0xFFFFFFFF at the top
 *       u32 nentries, then nentries entries of
 *           { u32 size, u32 isdir, str name }
 *
 *   A FILE entry's name is relative to its directory, because that is what the
 *   browser joins it to when it opens one.  A DIRECTORY entry's name is the path
 *   of the subdirectory it points at (and its `size` field carries that
 *   directory's index), which is how choosing a folder in the list works.  The
 *   first ndirs... the workspaces are the first entries: `ws_dir[i]` says which
 *   directory each workspace starts at.
 *
 *   (A single flat list was the format's first shape -- it made every workspace
 *   show the same files -- and a per-workspace list was the second, which could
 *   not show a folder at all.)
 *
 * A SHORT OR MISSING FILE IS NOT FATAL: the editor starts with its defaults and
 * says so, because a launcher that cannot write the file is exactly the case
 * where a black screen would be worst.
 * ------------------------------------------------------------------------- */
#define NB_CFG_FLAG_READONLY	0x0001
#define NB_CFG_FLAG_NOHIGHLIGHT	0x0002
#define NB_CFG_FLAG_NOLINENUM	0x0004
/* bit 3: the completion popup is OFF.  A flag word whose unused bits an older
 * launcher leaves clear means "on" by default here, which is what a user who
 * never edits setting.json should get. */
#define NB_CFG_FLAG_NOCOMPLETE	0x0008
/* bit 4: the INDENTATION GUIDES are OFF (they are on by default, like the rest:
 * see the note above) */
#define NB_CFG_FLAG_NOGUIDES	0x0010
#define NB_CFG_MAX_FILES	512	/* entries, across every directory */
#define NB_CFG_NAME_MAX		64	/* one name/path in the pool below */
#define NB_CFG_MAX_NAMES	512
#define NB_CFG_MAX_DIRS		64	/* a workspace plus the folders in it: 24 was
					 * not enough for C:\DATA's .hpappdir folders,
					 * and the rows that missed the cut fell back to
					 * the application folder (see NB_CFG_NO_DIR) */

/* The parsed form, in memory, filled by port/nb_cfgfile.c.  Strings are copied
 * into the fixed buffers below so nothing points into a released buffer.
 *
 * THE FILE LIST IS PER WORKSPACE.  Each entry names its own workspace's directory
 * (see nb_list.c), so the start screen can show what is really in the folder the
 * user picked.  The names live in one pool shared by every workspace -- 8 x 256
 * byte-name arrays would be 1.5 MB of the image's memory, and the pool is 16 KB --
 * and `name` is the BYTE OFFSET of the name inside it. */
typedef struct nb_cfg_entry {
	unsigned short name;		/* offset into nb_config.names */
	unsigned int bytes;		/* a file's size, or a subdirectory's index */
	unsigned int isdir;
} nb_cfg_entry;

/* one directory: its path, where its entries are, and its parent (0xFFFFFFFF at
 * the top, which is what "go up" checks before it leaves the workspace) */
typedef struct nb_cfg_dir {
	unsigned short path;		/* offset into nb_config.names */
	unsigned short first;		/* index into nb_config.file */
	unsigned short count;
	unsigned int parent;
} nb_cfg_dir;

#define NB_CFG_NO_DIR	0xFFFFFFFFu

typedef struct nb_config {
	unsigned int magic;
	unsigned int version;
	unsigned int flags;
	int theme;
	int tab_width;
	int language;
	unsigned int frames;

	int nworkspaces;
	char workspace[NB_CFG_MAX_WS][NB_CFG_PATH_MAX];
	int default_workspace;

	/* THE DIRECTORY TABLE.  `dir[i].path` and every entry name are offsets into
	 * the shared pool below; `dir[i].first/count` say which entries it owns. */
	int ndirs;
	nb_cfg_dir dir[NB_CFG_MAX_DIRS];
	int nfiles;			/* entries in use, across every directory */
	nb_cfg_entry file[NB_CFG_MAX_FILES];
	int ws_dir[NB_CFG_MAX_WS];	/* the directory each workspace starts at */
	char names[NB_CFG_MAX_NAMES * NB_CFG_NAME_MAX];
	int names_used;			/* bytes of the pool in use */

	/* where the file we are editing lives: the workspace the launcher had
	 * selected (the editor resolves relative names against it) */
	char cwd[NB_CFG_PATH_MAX];

	/* the file to open at startup, or "" to show the browser */
	char open_file[NB_CFG_PATH_MAX];

	/* the durable log's path, or "" for none */
	char logpath[NB_CFG_PATH_MAX];
} nb_config;

/* The directory the launch file is read from.  ON THE DEVICE this is the flat
 * application folder, and the launcher writes nbcode.cfg into the same one; on a
 * host it is overridden by the harness, which is why it is a variable rather
 * than a #define. */
extern const char *nb_cfg_dir_hint(void);

/* read + parse; returns 0 when the file is missing or unusable, and fills the
 * defaults in that case (never a half-parsed config) */
int nb_cfg_load(nb_config *c, const char *dir);
void nb_cfg_defaults(nb_config *c);

/* the one instance (port/nb_fw.c defines it) */
extern nb_config nb_cfg_instance;

#endif /* NB_CFG_H */
