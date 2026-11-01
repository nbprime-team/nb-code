/*
 * nb_input.h -- the keyboard/touch backend's contract (see port/nb_input_fw.c).
 *
 * THE MECHANISM IS PRIMELUA'S, WHICH IS PURE DOOM'S
 * The firmware's input comes out of one function pointer slot
 * (0x307FBFA0 on the G1): the OS calls get_event through a 16 byte stub, and
 * patching the first two words of that stub to {ldr pc, [pc, #-4]; <address>}
 * makes every input event arrive in this image instead.  DOOM has done exactly
 * this on this machine for years, primeLua reconstructed it and verified it (see
 * primeLua/README.md and port/plua_input.c), and nbCode copies it rather than
 * inventing a third variant:
 *
 *   * the hook is an ordinary C function entered by a BRANCH, so it returns with
 *     the LR it was called with;
 *   * it calls the firmware's get_event through nbCode's own svc wrapper (the
 *     kernel takes the SVC return address off the stack, so that call returns
 *     into the hook);
 *   * the patch is written with DOOM's ritual: IRQs masked, the translation
 *     table base poisoned to quiesce the core, the write, then clean+invalidate
 *     D-cache / invalidate I-cache / drain the write buffer, then the TTBR put
 *     back.  On this core a write to firmware code followed by a whole-D-cache
 *     invalidate WITHOUT the quiesce is a reset, and primeLua lost two sessions
 *     learning that;
 *   * the events are DECODED on the OS's input thread and queued for the editor.
 *
 * THE STATE LAYOUT IS A CONTRACT WITH calc/main.py: the launcher has to be able
 * to disarm the hook and put the firmware's own bytes back after nbCode returns
 * (including after a crash), and it finds the state by its magic.  The offsets
 * below are asserted at compile time so a drift is a build error, not a write
 * into the wrong field of a live input path.
 */
#ifndef NB_INPUT_H
#define NB_INPUT_H

#include "nb_common.h"

#define NB_IN_MAGIC		"NBINPUT"

/* where the firmware's get_event stub is, and the patch's two words */
#define NB_FW_EVENT_SLOT	0x307FBFA0u
#define NB_SLOT_TRAP		0xE51FF004u	/* ldr pc, [pc, #-4] */

/* the firmware's own stub, word for word, used when the slot holds OUR trap but
 * no trustworthy original was captured (a session that died before it could
 * restore).  Writing these back gives a WORKING get_event; NOPs would break
 * input for every application until the next reboot. */
#define NB_STUB0	0xE52D0004u	/* push {r0} */
#define NB_STUB1	0xE52DE004u	/* push {lr} */
#define NB_STUB2	0xEF01003Fu	/* svc  0x1003F */
#define NB_STUB3	0xE49D0004u	/* pop  {r0} */

/* the raw get_event frame (measured on the device; primetcc rt/hp_input.h) */
#define NB_EV_TYPE	4
#define NB_EV_TOUCH_N	24
#define NB_EV_ITEM	28
#define NB_EV_ITEM_SZ	12
#define NB_EV_KEY_FLAG	28
#define NB_EV_KEY_ID	34
#define NB_EV_TYPE_KEY	0x00100010u
#define NB_EV_TYPE_TOUCH 15u
#define NB_EV_KEY_PRESS	16u
#define NB_EV_KEY_RELEASE 0x100000u
#define NB_EV_ACT_PRESS	1u
#define NB_EV_ACT_MOVE	2u
#define NB_EV_ACT_RELEASE 8u

#define NB_IN_QUEUE	64

struct nb_in_item {
	unsigned char type;		/* 1 key, 2 touch */
	unsigned char down;
	unsigned char action;
	unsigned char pad;
	unsigned short key;
	unsigned short x, y;
};

/* the shared state (one instance, in this image's .data).  The first fields are
 * the launcher's contract: it reads saved[]/saved_valid and writes armed. */
struct nb_in_state {
	char magic[8];			/* "NBINPUT" */
	unsigned hook_fn;		/* our entry (R_ARM_RELATIVE) */
	unsigned saved[4];		/* the firmware's own slot bytes */
	int saved_valid;
	volatile int armed;		/* 32: 1 while nbCode is listening */
	/* 36: HOW MANY HOOK CALLS ARE IN FLIGHT.
	 *
	 * The OS's input thread calls get_event through this hook, and the call it
	 * makes -- the firmware's own get_event -- BLOCKS until an event arrives.
	 * So when the editor exits, that thread is usually still inside this image,
	 * sitting in that blocked call.  The launcher may not free the image while
	 * this is non-zero: the thread would resume in freed memory on the next
	 * event, and on this calculator that is a reboot.  (It was: "press esc at
	 * the workspace list and the machine restarts".) */
	volatile int hook_busy;
	int install_count, remove_count;
	int hook_calls, dropped, bad_type;
	int key_press, key_release, touch_press, touch_move, touch_release;
	volatile int head, tail;
	struct nb_in_item q[NB_IN_QUEUE];
};

extern struct nb_in_state nb_in_state;

#endif /* NB_INPUT_H */
