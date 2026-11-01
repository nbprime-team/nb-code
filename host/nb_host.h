/*
 * nb_host.h -- the PC side of the port layer, for the host tests.
 *
 * The editor is built unmodified for the host: same nb_buffer.c, same
 * highlighter, same nb_ui.c, same nb_app.c.  Only the bottom layer differs
 * (this file), which is what makes a host test evidence about the real editor
 * rather than about a model of it.
 *
 * A test drives it by pushing events and stepping frames:
 *
 *     nb_host_reset();
 *     nb_host_event_push(0x0D, 1);        // ENTER, down
 *     nb_app_step();
 *     nb_host_fb_pixel(x, y)              // what the screen now shows
 */
#ifndef NB_HOST_H
#define NB_HOST_H

#include "nb_common.h"
#include "nb_cfg.h"

/* forget everything: framebuffer, event queue, file list, config */
void nb_host_reset(void);

/* push one key event (device scan code; see nb_input.h for the table) */
void nb_host_event_push(int code, int down);
void nb_host_event_touch(int x, int y, int action);

/* the simulated screen */
nb_u32 nb_host_fb_pixel(int x, int y);
/* what the PANEL shows: the same buffer after the last present.  The two differ
 * on purpose (the device draws into a shadow buffer and blits it, which is the
 * flicker fix), so a test can tell "drawn" from "on screen". */
nb_u32 nb_host_fb_shown(int x, int y);
const nb_u32 *nb_host_fb(void);

/* the launch config the editor will read (call before nb_app_init) */
nb_config *nb_host_config(void);

/* install a file list: `dir` is the workspace, names are shown in order */
void nb_host_files(const char *dir, const char *const *names, int n);

/* append another workspace with its own list: the start screen shows one folder's
 * files at a time, and choosing a different folder has to change them */
void nb_host_add_workspace(const char *dir, const char *const *names, int n);

/* append a FOLDER inside workspace `ws`, with its own files; returns its index */
int nb_host_add_subdir(int ws, const char *path, const char *const *names, int n);

/* read a real "nbcode.cfg" with the same parser the calculator uses (dir is the
 * folder holding it); returns what nb_cfg_load returns */
int nb_host_load_cfg(const char *dir);

/* what the editor wrote, if anything (the fake filesystem is the host's own
 * /tmp directory, so a test can also just read the file) */
const char *nb_host_last_written(void);

/* the editor's own state, for assertions */
struct nb_buf *nb_app_buffer(void);

/* drain the event queue into the UI without running a frame */
void nb_host_pump(void);

/* the run's three pieces, so a test can watch the state between frames */
int nb_host_begin(void);
int nb_host_step(void);
int nb_host_run(int frames);
void nb_host_end(void);

#endif /* NB_HOST_H */
