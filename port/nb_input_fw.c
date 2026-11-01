/*
 * nb_input_fw.c -- the firmware's input path, intercepted the way DOOM does it.
 *
 * THE MECHANISM (see nb_input.h for the layout contract with the launcher)
 *
 *   * the firmware reaches get_event through one function pointer slot
 *     (0x307FBFA0); patching its first two words to {ldr pc, [pc, #-4];
 *     <address>} makes every input event arrive in this image instead, and the
 *     hook returns with the LR it was called with because it is entered by a
 *     BRANCH;
 *   * the hook calls the firmware's get_event through nbCode's own SVC wrapper.
 *     The kernel takes the SVC's return address off the stack, so that call
 *     comes back into the hook -- the hook never jumps into a copy of the
 *     firmware's stub;
 *   * the events are DECODED here, on the OS's input thread, and queued for the
 *     editor.  DOOM's hook does the same kind of work on that thread and this
 *     machine is fine with it; primeLua's first version copied raw events to a
 *     ring out of a belief that it was not, and that belief cost it a rewrite;
 *   * the patch is written with the CP15 ritual in nb_svc.s (quiesce, write,
 *     clean+invalidate, TTBR back).  The other orders reset this calculator.
 *
 * THE LAUNCHER CAN ALWAYS DISARM IT.  It finds the state by its magic and
 * writes `armed = 0`; nbCode also disarms it on the way out (nb_run_end), so a
 * clean exit never leaves the OS pointing into a freed image.
 */
#include "nb_common.h"
#include "nb_input.h"
#include "nb_port.h"

unsigned hp_svc_get_event(unsigned ev);
void nb_slot_patch(unsigned *dst, const unsigned *src, int nwords);

/* the shared state: field order is the launcher's contract (see nb_input.h) */
struct nb_in_state nb_in_state __attribute__((used)) = {
	/* spelled out per character so the struct needs no string copy at
	 * startup -- and so it is renamed by hand when the project is (this line
	 * silently kept the old name through a mechanical rename once, and the
	 * launcher then could not find the state at all) */
	.magic = { 'N', 'B', 'I', 'N', 'P', 'U', 'T', 0 },
};

/* The raw event frame, measured on the device (primetcc rt/hp_input.h):
 *   ev->[4]  = type: 0x00100010 key, 15 touch
 *   key:       [28] = 16 press / 0x100000 release, [34] = u16 scan code
 *   touch:     [24] = u16 item count (<= 8), item i at [28 + 12i]:
 *              [0] u32 action (1 press, 2 move, 8 release), [4] u16 valid flag,
 *              [6] x, [8] y
 */
static void push_item(struct nb_in_state *s, int type, int down, int action,
		      int key, int x, int y)
{
	int h = s->head;
	int n = (h + 1) & (NB_IN_QUEUE - 1);
	struct nb_in_item *it;

	if (n == s->tail) {
		s->dropped++;
		return;			/* full: drop the newest */
	}
	it = &s->q[h];
	it->type = (unsigned char)type;
	it->down = (unsigned char)down;
	it->action = (unsigned char)action;
	it->pad = 0;
	it->key = (unsigned short)key;
	it->x = (unsigned short)x;
	it->y = (unsigned short)y;
	s->head = n;
}

static void nb_input_hook_body(void *ev);

/* THE HOOK ENTRY: reached through the patched slot with r0 = the event buffer
 * the OS passed to get_event.
 *
 * THE COUNTER MATTERS AS MUCH AS THE WORK.  This runs on the OS's input thread,
 * and the call it makes below BLOCKS until an event arrives -- which means that
 * for most of a session that thread is inside this function.  hook_busy is what
 * tells the launcher that, so it does not free the image out from under a thread
 * that is about to resume in it (see nb_input.h: that rebooted the calculator
 * when the user quit).  One increment, one decrement, one exit path -- which is
 * why the body is a function of its own rather than a dozen early returns. */
void nb_input_hook(void *ev)
{
	struct nb_in_state *s = &nb_in_state;

	s->hook_busy++;
	nb_input_hook_body(ev);
	s->hook_busy--;
}

static void nb_input_hook_body(void *ev)
{
	struct nb_in_state *s = &nb_in_state;
	const unsigned char *e = (const unsigned char *)ev;

	hp_svc_get_event((unsigned)(unsigned long)ev);
	s->hook_calls++;
	if (!s->armed) {
		return;
	}
	{
		unsigned type = *(const unsigned *)(e + NB_EV_TYPE);

		if (type == NB_EV_TYPE_KEY) {
			unsigned flag = *(const unsigned *)(e + NB_EV_KEY_FLAG);
			unsigned short kid = *(const unsigned short *)
				(e + NB_EV_KEY_ID);
			int down;

			if (kid == 0)
				return;
			if (flag == NB_EV_KEY_PRESS)
				down = 1;
			else if (flag == NB_EV_KEY_RELEASE)
				down = 0;
			else {
				s->bad_type++;
				return;
			}
			if (down)
				s->key_press++;
			else
				s->key_release++;
			push_item(s, 1, down, 0, kid, 0, 0);
			return;
		}
		if (type != NB_EV_TYPE_TOUCH) {
			s->bad_type++;
			return;
		}
		{
			int n = *(const unsigned short *)(e + NB_EV_TOUCH_N);
			int i, lim = n > 8 ? 8 : n;

			if (n <= 0)
				return;
			/* the main loop only wants the LAST contact of a frame:
			 * a finger produces dozens of moves per second and the
			 * queue is 64 items deep */
			for (i = 0; i < lim; i++) {
				const unsigned char *p = e + NB_EV_ITEM +
					NB_EV_ITEM_SZ * i;
				unsigned act = *(const unsigned *)p;
				unsigned short x, y;

				if (*(const unsigned short *)(p + 4) != 0)
					continue;
				x = *(const unsigned short *)(p + 6);
				y = *(const unsigned short *)(p + 8);
				if (act == NB_EV_ACT_PRESS) {
					s->touch_press++;
					push_item(s, 2, 1, NB_EV_ACT_PRESS, 0, x, y);
				} else if (act == NB_EV_ACT_MOVE) {
					s->touch_move++;
					push_item(s, 2, 1, NB_EV_ACT_MOVE, 0, x, y);
				} else if (act == NB_EV_ACT_RELEASE) {
					s->touch_release++;
					push_item(s, 2, 0, NB_EV_ACT_RELEASE, 0, x,
						  y);
				}
			}
		}
	}
}

int nb_input_install(void)
{
	struct nb_in_state *s = &nb_in_state;
	unsigned patch[2];
	int i;

	if (!s->hook_fn)
		s->hook_fn = (unsigned)(unsigned long)nb_input_hook;
	if (!s->saved_valid) {
		volatile unsigned *slot = (volatile unsigned *)NB_FW_EVENT_SLOT;

		if (slot[0] == NB_SLOT_TRAP) {
			/* a previous session died inside the hook and left our
			 * trap in place: the firmware's known stub is the only
			 * trustworthy starting point */
			s->saved[0] = NB_STUB0;
			s->saved[1] = NB_STUB1;
			s->saved[2] = NB_STUB2;
			s->saved[3] = NB_STUB3;
		} else {
			for (i = 0; i < 4; i++)
				s->saved[i] = slot[i];
		}
		s->saved_valid = 1;
	}
	s->head = s->tail = 0;
	s->armed = 1;
	s->install_count++;
	patch[0] = NB_SLOT_TRAP;
	patch[1] = s->hook_fn;
	nb_slot_patch((unsigned *)NB_FW_EVENT_SLOT, patch, 2);
	if (*(volatile unsigned *)NB_FW_EVENT_SLOT != NB_SLOT_TRAP) {
		s->armed = 0;
		return 0;		/* the patch did not take */
	}
	return 1;
}

void nb_input_remove(void)
{
	struct nb_in_state *s = &nb_in_state;

	/* STOP LISTENING, AND STOP THERE: the slot itself goes back from the
	 * LAUNCHER (calc/main.py: stand_down_input), not from here.
	 *
	 * This function used to write the firmware's bytes back with the CP15
	 * ritual -- the same ritual nb_input_install() uses, which demonstrably
	 * works.  At the END of a session it rebooted the calculator: the durable
	 * log's last line was "milestone: exiting", i.e. this call.  The difference
	 * between install and remove is the state of the machine: at install the
	 * OS's input thread is parked in the FIRMWARE's get_event, and at remove it
	 * is parked inside THIS image (that is where it spends the whole session,
	 * blocked in get_event).  Whatever the mechanism, the evidence is that
	 * rewriting firmware code here is not safe, and primeLua's notes say the
	 * same thing from the other direction ("safe on the thread that ran the
	 * interpreter") -- its launcher writes the slot back from Python after
	 * every run, and has done since its own in-image versions killed the
	 * machine's input path.
	 *
	 * LEAVING IT ARMED IS NOT A RISK: the hook always calls the firmware's
	 * get_event first, so a DISARMED hook is transparent -- the OS keeps
	 * receiving its events whether or not the slot has been put back.  The
	 * launcher restores it a moment later, from the path that is proven here,
	 * and it holds this image's memory until no hook call is inside it. */
	s->armed = 0;
	s->head = 0;
	s->tail = 0;			/* and drop whatever is queued */
	s->remove_count++;
}

int nb_input_hooked(void)
{
	struct nb_in_state *s = &nb_in_state;

	return s->armed && *(volatile unsigned *)NB_FW_EVENT_SLOT ==
		NB_SLOT_TRAP;
}

/* the editor's side: one event per call, decoded out of the ring */
int nb_input_poll(nb_event *ev)
{
	struct nb_in_state *s = &nb_in_state;

	if (s->tail == s->head)
		return 0;
	{
		const struct nb_in_item *it = &s->q[s->tail];

		s->tail = (s->tail + 1) & (NB_IN_QUEUE - 1);
		ev->type = (it->type == 1) ? NB_EV_KEY : NB_EV_TOUCH;
		ev->code = it->key;
		ev->down = it->down;
		ev->x = it->x;
		ev->y = it->y;
		ev->action = it->action;
	}
	return 1;
}

void nb_run_finish(int status)
{
	(void)status;
	/* the launcher reads the status from the ring ("RT_RET") -- the debug
	 * interface's return value is unreliable on this hardware (primetcc
	 * measured it reporting 2 whatever main returns) */
}
