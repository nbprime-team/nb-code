/*
 * nb_app.c -- the main loop, and the one place the launch sequence lives.
 *
 * THE SEQUENCE (this is what the Python launcher expects, see nb_cfg.h):
 *
 *   1. nb_run_begin()   the entry's C half: light up the screen, take the config
 *                       block the launcher left in r0, install the input hook
 *                       (the DOOM mechanism), and open the file the launcher was
 *                       asked to open -- or show the workspace picker.
 *   2. nb_run_step()    one frame: drain the event queue, act, redraw if anything
 *                       changed, yield.  The Python side is not involved at all
 *                       while this runs: nbCode owns the screen and the keyboard.
 *   3. nb_run_end()     hand the firmware's input slot back (even after an
 *                       error), close the file, and return a status the
 *                       launcher prints.
 *
 * WHY THE LOOP YIELDS
 * hp_svc_get_event is what the input hook is called FROM, so an idle editor must
 * still let the OS's input thread run: nb_sleep(ms) is that yield, and it is also
 * where the emulator's event injection happens (tests/emu_arm2.py feeds the queue
 * inside sleep).  A tight loop with no sleep starves input and burns the battery.
 */
#include "nb_ui.h"
#include "nb_gfx.h"
#include "nb_clip.h"
#include "nb_port.h"
#include "nb_codec.h"

/* the frame period: ~30 frames/s.  The screen only repaints when something
 * changed, so this is a latency budget, not a workload */
#define FRAME_MS	32

/* the loop's safety net: a headless test (or a scripted emulator run) must end
 * even if nothing quits.  0 disables it. */
static int max_frames;
static int frame_count;

/* the stack pointer, on the device only (the host has no such register) */
static void milestone_stack(const char *what)
{
#if defined(__arm__)
	register unsigned sp __asm__("sp");
	char line[48];

	nb_str_copy(line, sizeof(line), "milestone: ", -1);
	nb_str_append(line, sizeof(line), what);
	nb_str_append(line, sizeof(line), " sp=0x");
	{
		char hex[12];

		nb_fmt_hex(hex, sizeof(hex), sp, 8);
		nb_str_append(line, sizeof(line), hex);
	}
	nb_milestone(line);
#else
	(void)what;
#endif
}

int nb_run_begin(void)
{
	nb_ui *u = nb_ui_get();
	const nb_config *cfg;
	int have_cfg;
	int have_screen;

	/* THE CONFIG FIRST, because it carries the DURABLE LOG'S PATH -- and that
	 * path is what makes every milestone below survive a crash.  (The milestone
	 * for the screen used to be written before the path was known, so on a
	 * machine that rebooted there it was silently dropped.)
	 *
	 * A missing or broken config is NOT fatal: the editor starts with its
	 * defaults and says so, because a launcher that could not write the file
	 * (or a user who copied the image without it) is exactly the case where a
	 * black screen would be worst.  Without a path the milestones go nowhere,
	 * but the launcher's own log still records what the launcher did. */
	have_cfg = nb_cfg_load(&nb_cfg_instance, nb_cfg_dir_hint());
	cfg = nb_config_get();
	if (cfg->logpath[0])
		nb_set_logpath(cfg->logpath);
	nb_log_num("cfg loaded=", (unsigned)have_cfg);
	nb_log_num("cfg frames=", cfg->frames);
	nb_log_hex("cfg flags=", cfg->flags);
	nb_log_num("cfg theme=", (unsigned)cfg->theme);
	nb_log_num("cfg tab=", (unsigned)cfg->tab_width);
	nb_log_num("cfg workspaces=", (unsigned)cfg->nworkspaces);
	nb_log_num("cfg files=", (unsigned)cfg->nfiles);
	nb_milestone("milestone: config loaded");

	/* THE GLOBAL CLIPBOARD'S FILE LIVES NEXT TO THE LOG (nb_clip.h).  The log
	 * path is the only path the launcher tells the editor -- at run time it is
	 * already open from inside the application folder -- so the clipboard is
	 * that path with the file name replaced.  No log path means no clipboard
	 * file: the panel still works, the list just does not outlive the session. */
	if (cfg->logpath[0]) {
		char dir[NB_PATH_MAX];
		int i, cut = -1;

		for (i = 0; cfg->logpath[i] && i < NB_PATH_MAX - 1; i++) {
			if (cfg->logpath[i] == '\\' || cfg->logpath[i] == '/')
				cut = i;
		}
		if (cut > 0) {
			nb_str_copy(dir, sizeof(dir), cfg->logpath, cut);
			nb_clip_set_dir(dir);
		}
	}
	nb_log_num("clip entries=", (unsigned)nb_clip_load());
	if (cfg->logpath[0])
		nb_logfile("nbCode " NB_VERSION " start");

	/* THEN THE SCREEN, AND NOT FATALLY.  nb_screen_init() either finds the
	 * firmware's framebuffer or says no; drawing without it writes a frame into
	 * whatever the wrong pointer points at, which on this machine is a reset.
	 * This is where the editor died before, on the first frame.
	 *
	 * BOTH checks: nb_screen_init() looks at the firmware's own structures, and
	 * nb_gfx_init() is what every primitive actually draws through (it hands
	 * out NB_GFX_FAIL when the surface is not usable).  The primitives guard a
	 * null buffer themselves, so the worst case would be a blank screen -- but
	 * the editor should say why, not sit there black. */
	have_screen = nb_screen_init();
	if (!have_screen || nb_gfx_init() != NB_GFX_OK) {
		nb_logfile("nbCode: stopped: no framebuffer to draw into");
		nb_milestone("milestone: screen FAILED, stopping");
		return 0;
	}
	nb_milestone("milestone: screen ready");

	nb_ui_init(cfg);
	nb_milestone("milestone: ui ready");

	/* the input hook.  It is installed BEFORE the first frame, because the
	 * user's first key press must not be lost; if it cannot be installed the
	 * editor still runs (the launcher can feed keys through the config's
	 * fallback), but the log says why. */
	if (nb_input_install()) {
		nb_log("nbCode: input hook armed\n");
		nb_milestone("milestone: input hook armed");
	} else {
		nb_log("nbCode: input hook NOT armed\n");
		nb_milestone("milestone: input hook NOT armed");
	}

	/* a frame budget from the launcher (see nb_cfg.h): a bounded session for
	 * an automated check, 0 for a normal one */
	if (cfg->frames > 0) {
		nb_app_headless((int)cfg->frames);
		nb_log_num("frame budget=", cfg->frames);
	}
	if (cfg->nworkspaces == 0) {
		/* no config file: offer the application folder, which is where the
		 * editor itself lives and usually where the sources are */
			nb_log("cfg: no workspace in the config: using the app folder\n");
		nb_log("cfg: cwd at startup was ");
		{
			extern const char *nb_cfg_dir_hint(void);

			nb_log(nb_cfg_dir_hint());
		}
		nb_log("\n");
	}

	/* the file the launcher was asked to open, if any */
	if (cfg->open_file[0]) {
		char dir[NB_PATH_MAX], name[NB_PATH_MAX];
		const char *p = cfg->open_file;
		int i, last = -1;

		for (i = 0; p[i]; i++) {
			if (p[i] == '\\' || p[i] == '/')
				last = i;
		}
		if (last >= 0) {
			nb_str_copy(dir, sizeof(dir), p, last);
			nb_str_copy(name, sizeof(name), p + last + 1, -1);
			nb_ui_enter_workspace(cfg->default_workspace);
			if (!nb_ui_open_file(dir, name))
				nb_ui_enter_browser();
		} else {
			nb_ui_enter_workspace(cfg->default_workspace);
			if (!nb_ui_open_file(cfg->cwd, p))
				nb_ui_enter_browser();
		}
	} else {
		nb_ui_enter_workspace(cfg->default_workspace);
		nb_ui_message("请选择文件", 0);
	}
	u->dirty = 1;
	nb_milestone("milestone: first frame about to be drawn");
	/* HOW MUCH STACK IS LEFT is the one thing this editor cannot see and the
	 * one thing that has taken it down: the image runs INLINE, on the Python
	 * application's thread stack (primeLua allocates a 512 KB stack of its own
	 * for exactly this reason -- see its plua_run.c), so a frame that is a few
	 * hundred bytes deeper than the last one can be the difference between a
	 * running editor and a calculator that reboots.  The stack pointer at
	 * start-up and at the first frame is written to the durable log, so a crash
	 * report carries the depth it reached. */
	milestone_stack("start-up");
	return 1;
}

int nb_run_step(void)
{
	nb_ui *u = nb_ui_get();
	nb_event ev;

	/* everything the firmware has queued this frame */
	while (nb_input_poll(&ev)) {
		if (ev.type == NB_EV_KEY)
			nb_ui_key(&ev);
		else if (ev.type == NB_EV_TOUCH)
			nb_ui_touch(&ev);
	}
	nb_ui_tick();
	if (nb_ui_wants_redraw()) {
		nb_ui_draw();
		if (frame_count == 0)
			milestone_stack("after the first frame");
	}
	nb_sleep(FRAME_MS);
	frame_count++;
	/* the frame counter is published every 16 frames: on the device it is the
	 * only sign of life an idle editor gives, and a run that never reaches its
	 * frame budget (see nb_cfg.h) is visible in the log as a stuck count */
	if (max_frames > 0 && (frame_count & 15) == 0) {
		nb_log_num("frame count=", (unsigned)frame_count);
	}
	if (max_frames > 0 && frame_count >= max_frames) {
		nb_log("nbCode: frame limit reached\n");
		u->mode = NB_MODE_QUIT;
	}
	return nb_ui_finished();
}

void nb_run_end(int status)
{
	nb_ui *u = nb_ui_get();

	/* Hand the firmware's input path back FIRST: the launcher gives this image's
	 * memory back next, and an OS whose get_event slot still points into a freed
	 * block is a dead keyboard until the next reset.
	 *
	 * The milestones are here for the same reason as the ones at startup: this
	 * is the path a crash after quitting takes, and without them the durable log
	 * simply stops after the last startup line.  (The launcher's own log records
	 * what IT did next, so the two together say whether a reboot happened inside
	 * the image's teardown or after the launcher had finished.) */
	nb_milestone("milestone: exiting");
	nb_input_remove();
	nb_milestone("milestone: disarmed (the launcher puts the slot back)");
	nb_ui_free();
	nb_screen_free();
	nb_milestone("milestone: buffers freed");
	nb_log("nbCode: exit\n");
	(void)u;
	nb_log_ret(status);
	nb_run_finish(status);
	nb_milestone("milestone: returned to the launcher");
}

void nb_run(void)
{
	int status = 0;

	if (!nb_run_begin())
		status = 2;
	else
		while (!nb_run_step())
			;
	nb_run_end(status);
}

/* ---------------------------------------------------------------------------
 * what the host tests and the emulator harness call
 * ------------------------------------------------------------------------- */
void nb_app_headless(int frames)
{
	max_frames = frames;
}

nb_buf *nb_app_buffer(void)
{
	return nb_ui_get()->buf;
}

int nb_app_max_frames(void)
{
	return max_frames;
}

void nb_app_reset_frame_count(void)
{
	frame_count = 0;
}

int nb_app_frame_count(void)
{
	return frame_count;
}

int nb_app_headless_mode(void)
{
	return max_frames > 0;
}
