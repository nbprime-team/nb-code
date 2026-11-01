/*
 * nb_svc.s -- nbCode's ARM layer: the firmware SVC wrappers, the cp15 sequence
 * that makes a patch to the firmware's input slot safe, and the ELF entry point.
 *
 * FIRMWARE CALLING CONVENTION (the same one DOOM, primetcc and primeLua use):
 *
 *     push {r0}
 *     push {lr}
 *     svc  <service id>
 *
 * The firmware's SVC dispatcher finds the argument block through the pushed r0,
 * calls the service with r0-r3, and returns through the pushed LR -- so to the
 * caller this looks like an ordinary AAPCS call with up to four integer
 * arguments and the result in r0.
 *
 * TARGET
 * ARM926EJ-S (ARMv5TEJ), soft float, position independent.  The image is loaded
 * by the calculator's MicroPython shellcode loader, which applies R_ARM_RELATIVE
 * relocations only, hence -fPIC everywhere and no absolute addresses in this
 * file: literals are PC-relative through ldr =sym.
 */
	.arm
	.syntax unified
	.text

/* ===========================================================================
 * the entry point
 *
 * The loader calls it with r0 = a pointer to the APPLICATION FOLDER as a
 * NUL-terminated UTF-8 device path, or 0 when the launcher does not know it.
 * That is the one string the two sides have to agree about live, and it is kept
 * to a string on purpose: a struct in memory is what went wrong before (see
 * src/nb_cfg.h), and a string has no layout to disagree about.
 *
 * The launch parameters themselves are the FILE "nbcode.cfg" the launcher wrote
 * next to the image.  The folder path matters because the FIRMWARE's fopen wants
 * an absolute path while MicroPython's own file API refuses them -- each side
 * uses its own spelling of the same file, and this is how the image learns the
 * one it needs.
 *
 * The entry saves the callee-saved registers the loader expects to survive,
 * aligns the stack, records r0 in nb_entry_arg and calls nb_run().
 * =========================================================================== */
	.globl nb_entry
	.type nb_entry, %function
nb_entry:
	/* the loader's shellcode trusts AAPCS: everything callee-saved goes back
	 * the way it came in, or the launcher's Python call frame is corrupted */
	push	{r4, r5, r6, r7, r8, r9, r10, r11, ip, lr}
	/* ARM926 data aborts on unaligned LDRD/STRD, and the firmware does not
	 * promise more than 4-byte alignment: align to 8 before using the stack */
	bic	sp, sp, #7
	sub	sp, sp, #8
	ldr	r1, =nb_entry_arg
	str	r0, [r1]
	bl	nb_run
	add	sp, sp, #8
	pop	{r4, r5, r6, r7, r8, r9, r10, r11, ip, pc}

/* the launcher's application-folder pointer, as handed over in r0.  Read (and
 * copied) by nb_cfg_dir_hint() the first time it is needed; the launcher frees
 * the block when the run is over, so nothing holds this pointer for long. */
	.globl nb_entry_arg
	.data
	.align 2
nb_entry_arg:
	.word 0

	.text

/* ===========================================================================
 * files.  The firmware's fopen wants UTF-16 paths and "rb"/"wb+" modes; the
 * translation lives in C (port/nb_file.c) and these are the bare calls.
 * =========================================================================== */
	.globl hp_svc_fopen
hp_svc_fopen:
	push	{r0}
	push	{lr}
	svc	0x1026f
	bx	lr

	.globl hp_svc_fclose
hp_svc_fclose:
	push	{r0}
	push	{lr}
	svc	0x100ca
	bx	lr

	.globl hp_svc_fseek
hp_svc_fseek:
	push	{r0}
	push	{lr}
	svc	0x100cf
	bx	lr

	.globl hp_svc_ftell
hp_svc_ftell:
	push	{r0}
	push	{lr}
	svc	0x100d0
	bx	lr

	.globl hp_svc_fread
hp_svc_fread:
	push	{r0}
	push	{lr}
	svc	0x100d4
	bx	lr

	.globl hp_svc_fwrite
hp_svc_fwrite:
	push	{r0}
	push	{lr}
	svc	0x100d7
	bx	lr

	.globl hp_svc_filesize
hp_svc_filesize:
	push	{r0}
	push	{lr}
	svc	0x100cb
	bx	lr

	.globl hp_svc_remove
hp_svc_remove:
	push	{r0}
	push	{lr}
	svc	0x10274
	bx	lr

/* ===========================================================================
 * heap and scheduling
 * =========================================================================== */
	.globl hp_svc_malloc
hp_svc_malloc:
	push	{r0}
	push	{lr}
	svc	0x10037
	bx	lr

	.globl hp_svc_calloc
hp_svc_calloc:
	push	{r0}
	push	{lr}
	svc	0x10038
	bx	lr

	.globl hp_svc_realloc
hp_svc_realloc:
	push	{r0}
	push	{lr}
	svc	0x10039
	bx	lr

	.globl hp_svc_free
hp_svc_free:
	push	{r0}
	push	{lr}
	svc	0x1003a
	bx	lr

	.globl hp_svc_sleep
hp_svc_sleep:
	push	{r0}
	push	{lr}
	svc	0x10008
	bx	lr

/* get_lcd() -> the LCD structure, whose first field is the framebuffer pointer
 * (port/nb_screen.c reads it once and keeps it) */
	.globl hp_svc_getlcd
hp_svc_getlcd:
	push	{r0}
	push	{lr}
	svc	0x1008d
	bx	lr

/* get_event(ev): THIS IS CALLED BY THE INPUT HOOK, from the OS's own input
 * thread, inside the hook's blocking call.  The kernel takes the SVC's return
 * address off the stack, so the call returns straight back into the hook --
 * which is why the hook can call this instead of jumping to a copy of the
 * firmware's stub. */
	.globl hp_svc_get_event
hp_svc_get_event:
	push	{r0}
	push	{lr}
	svc	0x1003f
	bx	lr

/* ===========================================================================
 * the write to the firmware's get_event slot, done the way DOOM does it
 *
 * nb_slot_patch(dst, src, nwords): IRQs masked, the translation table base
 * poisoned to quiesce the core, the words copied, then clean+invalidate
 * D-cache, invalidate I-cache, drain the write buffer, and the TTBR put back.
 *
 * Every CP15 operation here is copied from puredoom.elf's aggressive_memcpy,
 * including the TTBR poison: on this core, a write to firmware code followed by
 * a whole-D-cache invalidate WITHOUT it is a reset, and with it is what PureDOOM
 * has been doing on this machine all along.  primeLua lost two sessions to the
 * other order (see primeLua/README.md), so this is not a preference.
 * =========================================================================== */
	.globl nb_slot_patch
	.type nb_slot_patch, %function
nb_slot_patch:
	mov	r3, #0
	mrs	r4, cpsr
	orr	r5, r4, #0x80		/* mask IRQ: the patch is two words and an
					 * interrupt between them runs the trap
					 * half-written */
	msr	cpsr_c, r5
	mrc	p15, 0, r6, c3, c0, 0	/* save the translation table base */
	mvn	r5, #0
	mcr	p15, 0, r5, c3, c0, 0	/* quiesce (DOOM's poison) */
1:
	ldr	r5, [r1], #4
	str	r5, [r0], #4
	subs	r2, r2, #1
	bne	1b
	mcr	p15, 0, r3, c7, c10, 1	/* clean+invalidate D-cache */
	mcr	p15, 0, r3, c7, c5, 0	/* invalidate I-cache */
	mcr	p15, 0, r3, c7, c10, 4	/* drain the write buffer */
	mcr	p15, 0, r6, c3, c0, 0	/* translation table base back */
	msr	cpsr_c, r4
	bx	lr

/* the same cache maintenance for a plain data write (no quiesce: the target is
 * writable memory, not firmware code) */
	.globl nb_flush_dcache
nb_flush_dcache:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 1
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

/* ===========================================================================
 * division helpers: -nostdlib means libgcc's __aeabi_* are linked in, but the
 * compiler also emits calls to these on this target, and the firmware's idea of
 * "divide by zero" is a reset.  Integers only (soft float throughout).
 * =========================================================================== */
	.globl __aeabi_idiv0
__aeabi_idiv0:
	mov	r0, #0
	bx	lr

	.globl __aeabi_ldiv0
__aeabi_ldiv0:
	mov	r0, #0
	mov	r1, #0
	bx	lr
