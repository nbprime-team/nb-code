/*
 * nb_port.h -- everything nbCode needs from the calculator, in one place.
 *
 * Two implementations exist and the build picks one:
 *
 *   port/nb_port_fw.c   the real thing: firmware SVCs, the ARGB framebuffer,
 *                       the DOOM-style get_event hook, the debug console ring
 *   host/nb_port_host.c the PC: stdio files, an in-memory framebuffer, a
 *                       scripted key queue, stdout
 *
 * Nothing above this layer includes a firmware header or knows which one it
 * is talking to, which is what makes `make test-host` and `make test-emu` the
 * same code path as the calculator instead of a description of it.
 *
 * The launch config (port/nb_cfg.h) is the other half of the contract with the
 * Python launcher: it carries the workspace, the settings and the file list,
 * because the C side cannot enumerate a directory.
 */
#ifndef NB_PORT_H
#define NB_PORT_H

#include "nb_common.h"
#include "nb_cfg.h"

/* ---------------------------------------------------------------------------
 * files.  The firmware's own API is UTF-16 and only reliably supports "rb" and
 * "wb+" (measured; see primeLua's plua_fwmode.c), so every mode is translated
 * here and the rest of the editor speaks UTF-8 paths and "rb"/"wb".
 * ------------------------------------------------------------------------- */
void *nb_file_open(const char *path, const char *mode);		/* NULL = failed */
int nb_file_read(void *fh, char *buf, int len);
int nb_file_write(void *fh, const char *buf, int len);
int nb_file_close(void *fh);
int nb_file_size(const char *path);
int nb_file_remove(const char *path);

/* Read a whole file (NB_FILE_MAX cap).  Returns a malloc'd buffer and sets
 * *len, or NULL; *err receives a short reason for the status bar. */
char *nb_file_slurp(const char *path, int *len, const char **err);

/* Write a whole file atomically enough for a calculator: write a temporary
 * ".tmp" next to the target, then remove the target and rename the temp over
 * it.  A half-written source file after a reset is the one outcome an editor
 * must not have. */
int nb_file_spit(const char *path, const char *data, int len, const char **err);

/* ---------------------------------------------------------------------------
 * the workspace list the launcher handed us (nb_cfg.h) -- read-only here
 * ------------------------------------------------------------------------- */
const char *nb_ws_dir(int index);
const char *nb_ws_label(int index);
int nb_ws_count(void);
const char *nb_ws_default(void);

/* the file list of the workspace we are in */
int nb_file_count(void);
const char *nb_file_name(int index);
unsigned int nb_file_bytes(int index);
int nb_file_is_dir(int index);
int nb_file_select(int index);		/* show this workspace's folder */
int nb_file_selected(void);
int nb_file_enter(int index);		/* step into the folder on this row */
int nb_file_up(void);			/* go up one folder; 0 at the top */
int nb_file_in_dir(void);
int nb_file_show(const char *path);	/* show the folder a path lives in */
int nb_file_workspace(void);
const char *nb_file_current_dir(void);

/* ---------------------------------------------------------------------------
 * screen: one ARGB8888 framebuffer, 320x240, drawn by nb_gfx.c and pushed with
 * one blit per frame (the pattern FileManager/primeLua use -- per-primitive
 * writes to the visible buffer flicker).
 * ------------------------------------------------------------------------- */
int nb_screen_init(void);
void nb_screen_present(void);
/* hand the shadow buffer back: the calculator needs the memory while nbCode is
 * not running (the device port allocates it in nb_screen_init; the host port,
 * which keeps its own array, has nothing to free) */
void nb_screen_free(void);
nb_u32 *nb_screen_back(void);
int nb_screen_width(void);
int nb_screen_height(void);

/* ---------------------------------------------------------------------------
 * input.  The calculator's key path is primeLua's (PureDOOM's mechanism): patch
 * the firmware's get_event slot with a trampoline into this image, decode the
 * event on the OS's input thread, and queue items for the editor.  nb_input.c
 * holds the queue; port/nb_input_fw.c holds the trap.
 * ------------------------------------------------------------------------- */
int nb_input_install(void);
void nb_input_remove(void);
int nb_input_hooked(void);

/* one queued item: a key event or a touch.  `code` is the DEVICE scan code
 * (hp_input.h), never the PPL GETKEY number -- mixing the two tables is the
 * bug that made a right arrow quit another app in this workspace. */
#define NB_EV_NONE	0
#define NB_EV_KEY	1
#define NB_EV_TOUCH	2

typedef struct nb_event {
	int type;
	int code;			/* key scan code */
	int down;			/* key: press(1)/release(0) */
	int x, y;			/* touch: screen pixels */
	int action;			/* touch: 1 press, 2 move, 8 release */
} nb_event;

int nb_input_poll(nb_event *ev);	/* 1 = filled, 0 = nothing waiting */
void nb_sleep(int ms);			/* yield: the firmware needs it */

/* the firmware's clock (svc 0x100A5 -- primeLua measured the layout): used to
 * time the frame loop and to put a real time in the status bar.  Returns Unix
 * seconds, or -1 when the machine does not answer. */
long nb_time_now(void);

/* ---------------------------------------------------------------------------
 * the debug console ring: nbCode's stdout.  The launcher polls it while the
 * editor runs, exactly as primeLua's launcher does, so a crash tells the user
 * what nbCode was doing instead of leaving a black screen.
 * ------------------------------------------------------------------------- */
void nb_log(const char *s);
/* "<tag><value>\n" appended to the ring: nb_log_num for anything a person reads
 * (counts, frames), nb_log_hex for a bit pattern where hex is the readable form
 * (a magic number, an address). */
void nb_log_num(const char *tag, unsigned int v);
void nb_log_hex(const char *tag, unsigned int v);

/* append a line to the durable log file (it survives a reset: MicroPython's
 * console does not).  The path comes from the launch config. */
void nb_logfile(const char *s);
void nb_set_logpath(const char *p);

/* a milestone: a short line written STRAIGHT to the durable log at a point the
 * editor has to get past.  The ring cannot help here -- when the calculator
 * faults, the launcher never returns from its one blocking call and never reads
 * the ring, so the flash is the only place a trail survives a reboot. */
void nb_milestone(const char *s);

/* ---------------------------------------------------------------------------
 * exit: hand the firmware's input slot back BEFORE returning to the launcher.
 * every exit path goes through here (nb_app.c), so a crash inside the editor
 * cannot leave the OS's input path pointing at an unloaded image.
 * ------------------------------------------------------------------------- */
void nb_run_finish(int status);

/* the device build publishes its exit code through the log ring ("RT_RET:<n>"),
 * because the firmware's debug interface reports a wrong return value on this
 * hardware; the host build has no ring and does nothing. */
void nb_log_ret(int status);

/* the launch config, filled by port/nb_entry.s from r0 */
const nb_config *nb_config_get(void);

/* ---------------------------------------------------------------------------
 * the run itself (nb_app.c).  nb_entry.s calls nb_run() and returns what it
 * returns; the three pieces are also what the host tests and the emulator
 * harness drive one frame at a time.
 * ------------------------------------------------------------------------- */
void nb_run(void);			/* begin + loop + end */
int nb_run_begin(void);			/* 0 = the config was unusable */
int nb_run_step(void);			/* one frame; 1 = finished */
void nb_run_end(int status);
void nb_app_headless(int frames);	/* stop after N frames (tests) */
int nb_app_headless_mode(void);
void nb_app_reset_frame_count(void);
int nb_app_frame_count(void);
struct nb_buf *nb_app_buffer(void);

/* version string, shown in the header and written to the log */
#define NB_VERSION	"1.0"

#endif /* NB_PORT_H */
