/*
 * nb_host_ext.c -- the parts of the host port that need the editor's own state
 * (nb_host.h declares them; they cannot live in nb_port_host.c because that file
 * must stay independent of the UI layer).
 */
#include "nb_common.h"
#include "nb_host.h"
#include "nb_port.h"
#include "nb_buffer.h"
#include "nb_ui.h"

/* implemented by nb_app.c, declared here to keep nb_port.h free of the editor */
void nb_app_headless(int frames);
int nb_app_headless_mode(void);
int nb_app_frame_count(void);

/* the host test's entry point: run `frames` frames of the real loop */
int nb_host_run(int frames)
{
	nb_app_headless(frames);
	nb_run();
	return 0;
}

/* drain the host event queue into the UI without running a frame: a test that
 * wants to type does not need the frame loop's sleep */
void nb_host_pump(void)
{
	nb_event ev;

	while (nb_input_poll(&ev)) {
		if (ev.type == NB_EV_KEY)
			nb_ui_key(&ev);
		else if (ev.type == NB_EV_TOUCH)
			nb_ui_touch(&ev);
	}
}

/* one frame, for a test that wants to inspect the state in between */
int nb_host_step(void)
{
	return nb_run_step();
}

int nb_host_begin(void)
{
	return nb_run_begin();
}

void nb_host_end(void)
{
	nb_run_end(0);
}
