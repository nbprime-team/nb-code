/*
 * nb_list.c -- the workspaces and files the Python launcher handed over.
 *
 * WHY THE LIST COMES FROM PYTHON: the firmware's directory enumeration
 * (_wfindfirst / _wfindnext) lives in the MicroPython application, not in a
 * loaded image -- a shellcode-loaded ELF has the file API and nothing to walk a
 * directory with.  So the launcher lists, and this file is the read-only view
 * the UI browses.  It costs one flat block of memory that the launcher already
 * built, and it means nbCode can open any file the launcher can see.
 *
 * The list is only replaced by the launcher (a fresh run), so there is no
 * "refresh" path here beyond remembering which workspace is being shown; the UI
 * asks for the list of the workspace it is in.
 */
#include "nb_common.h"
#include "nb_port.h"
#include "nb_cfg.h"

/* The list lives in the config the launcher wrote (port/nb_cfgfile.c): the
 * firmware's directory enumeration is in the MicroPython application, not in a
 * loaded image, so the launcher lists and the editor browses. */

static const nb_config *conf(void)
{
	return nb_config_get();
}

/* A CONFIG WITH NO WORKSPACE STILL HAS ONE.  The launcher always lists at least
 * the application folder, so this only happens when nbcode.cfg is missing (the
 * image was copied without it, or it could not be written) -- and in that case
 * the useful answer is "the folder the editor itself lives in", not an empty
 * start screen with nothing to select. */
static void ensure_workspace(void)
{
	nb_config *c = &nb_cfg_instance;

	if (c->nworkspaces > 0)
		return;
	nb_str_copy(c->workspace[0], NB_CFG_PATH_MAX, nb_cfg_dir_hint(), -1);
	c->nworkspaces = 1;
	c->default_workspace = 0;
	if (!c->cwd[0])
		nb_str_copy(c->cwd, sizeof(c->cwd), nb_cfg_dir_hint(), -1);
}

int nb_ws_count(void)
{
	ensure_workspace();
	return conf()->nworkspaces;
}

const char *nb_ws_dir(int index)
{
	const nb_config *c = conf();

	if (!c || index < 0 || index >= c->nworkspaces)
		return "";
	return c->workspace[index];
}

const char *nb_ws_label(int index)
{
	const char *dir = nb_ws_dir(index);
	const char *last = dir;
	int i;

	/* the last path component is the label: "C:\DATA\nbCode.hpappdir" reads
	 * as "nbCode.hpappdir", which is what a user recognises */
	for (i = 0; dir[i]; i++) {
		if (dir[i] == '\\' || dir[i] == '/')
			last = dir + i + 1;
	}
	return last;
}

const char *nb_ws_default(void)
{
	const nb_config *c = conf();

	if (!c)
		return "";
	if (c->default_workspace < 0 || c->default_workspace >= c->nworkspaces)
		return nb_ws_dir(0);
	return nb_ws_dir(c->default_workspace);
}

/* WHICH DIRECTORY IS ON SCREEN.
 *
 * The launcher sends a TABLE OF DIRECTORIES (src/nb_cfg.h): the workspaces, and
 * the folders inside them, so the browser can both show a folder row and step
 * into it -- including the .hpappdir folders under C:\DATA.  Everything the
 * browser asks for is about the directory it is showing, and `ws_shown` only
 * remembers which workspace that belongs to (it is what F1 goes back to). */
static int dir_shown = -1;	/* -1 = nothing on screen (see nb_file_select) */
static int ws_shown;

static const nb_cfg_entry *entry_at(int index);

static const nb_cfg_dir *dir_at(int index)
{
	const nb_config *c = conf();

	if (!c || index < 0 || index >= c->ndirs)
		return NB_NULL;
	return &c->dir[index];
}

int nb_file_select(int index)
{
	const nb_config *c = conf();

	if (!c || index < 0 || index >= c->nworkspaces)
		return 0;
	ws_shown = index;
	if (c->ws_dir[index] == (int)NB_CFG_NO_DIR) {
		/* NO DIRECTORY FOR THIS WORKSPACE: show NOTHING.  Leaving the
		 * previous directory up would be the same bug as falling back to
		 * directory 0 -- files on screen that belong to another folder,
		 * with nothing to say they do. */
		dir_shown = -1;
		return 0;
	}
	dir_shown = c->ws_dir[index];
	return 1;
}

int nb_file_selected(void)
{
	return ws_shown;
}

/* ENTER ON A FOLDER: show it.  Returns 0 when the entry is not a directory. */
int nb_file_enter(int index)
{
	const nb_cfg_entry *e = entry_at(index);

	if (!e || !e->isdir)
		return 0;
	if ((int)e->bytes < 0 || (int)e->bytes >= (conf() ? conf()->ndirs : 0))
		return 0;
	dir_shown = (int)e->bytes;
	return 1;
}

/* GO UP ONE FOLDER.  Returns 0 at the top of the workspace, which is the
 * browser's cue to go back to the workspace list instead. */
int nb_file_up(void)
{
	const nb_cfg_dir *d = dir_at(dir_shown);

	if (!d || d->parent == NB_CFG_NO_DIR ||
	    (int)d->parent >= (conf() ? conf()->ndirs : 0))
		return 0;
	dir_shown = (int)d->parent;
	return 1;
}

/* SHOW THE DIRECTORY THIS PATH BELONGS TO.  Called when a file is opened, so that
 * leaving the editor with F1 lands in the folder the file came from rather than in
 * the workspace root.  Returns 0 when the table has no such directory. */
int nb_file_show(const char *path)
{
	const nb_config *c = conf();
	int d;

	if (!c || !path)
		return 0;
	for (d = 0; d < c->ndirs; d++) {
		if (nb_strcmp(c->names + c->dir[d].path, path) == 0) {
			dir_shown = d;
			return 1;
		}
	}
	return 0;
}

int nb_file_in_dir(void)
{
	const nb_cfg_dir *d = dir_at(dir_shown);

	return d ? 1 : 0;
}

/* entry `index` of the directory on screen, or NULL */
static const nb_cfg_entry *entry_at(int index)
{
	const nb_config *c = conf();
	const nb_cfg_dir *d = dir_at(dir_shown);
	int first;

	if (!c || !d || index < 0 || index >= (int)d->count)
		return NB_NULL;
	first = (int)d->first + index;
	if (first < 0 || first >= c->nfiles)
		return NB_NULL;
	return &c->file[first];
}

int nb_file_count(void)
{
	const nb_cfg_dir *d = dir_at(dir_shown);

	return d ? (int)d->count : 0;
}

const char *nb_file_name(int index)
{
	const nb_config *c = conf();
	const nb_cfg_entry *e = entry_at(index);

	return (c && e) ? c->names + e->name : "";
}

unsigned int nb_file_bytes(int index)
{
	const nb_cfg_entry *e = entry_at(index);

	return e ? e->bytes : 0;
}

int nb_file_is_dir(int index)
{
	const nb_cfg_entry *e = entry_at(index);

	return e ? (int)e->isdir : 0;
}

/* the directory on screen, as a device path: this is what the browser joins a
 * file name to when it opens one */
const char *nb_file_current_dir(void)
{
	const nb_config *c = conf();
	const nb_cfg_dir *d = dir_at(dir_shown);

	if (!c || !d)
		return c ? c->cwd : "";
	return c->names + d->path;
}

/* the workspace the shown directory belongs to (0 when there is no table) */
int nb_file_workspace(void)
{
	return ws_shown;
}
