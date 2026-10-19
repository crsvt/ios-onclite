/*
 * onclite probe entry: arm64 Linux Image header so LK (aboot) treats us
 * like a kernel. LK enters at EL1, MMU/D-cache off, x0 = DTB pointer.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset, as the stock kernel */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from LK */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	bl	probe_main
3:	wfe
	b	3b

/*
 * PSCI CPU_ON target. x0 = context id = cpu index (1..7). If TZ starts the
 * core at EL2, drop to EL1 (no stage 2, AArch64 EL1, no traps) like the
 * XNU sweet secondary path. secondary_main gets (index, CurrentEL at entry).
 */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x20, x0
	mrs	x21, CurrentEL
	cmp	x21, #0x8
	b.ne	5f
	mov	x1, #(1 << 31)		/* HCR_EL2.RW */
	msr	hcr_el2, x1
	msr	vttbr_el2, xzr
	mov	x1, #0x33ff		/* CPTR_EL2 RES1 bits, no traps */
	msr	cptr_el2, x1
	msr	mdcr_el2, xzr
	mov	x1, #3			/* EL1 physical timer/counter access */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mov	x1, #0x0800		/* SCTLR_EL1: RES1 bits 29:28,23:22,20,11 */
	movk	x1, #0x30d0, lsl #16
	msr	sctlr_el1, x1
	adr	x1, 5f
	msr	elr_el2, x1
	mov	x1, #0x3c5		/* DAIF masked, AArch64 EL1h */
	msr	spsr_el2, x1
	isb
	eret
5:	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x20, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	mov	x0, x20
	mov	x1, x21
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). IRQs return; everything else stops on screen.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret
