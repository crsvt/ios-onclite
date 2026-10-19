/*
 * Stand-in "kernel" for testing the onclite shim without xnu: entered by
 * PSCI CPU_ON on the Gold core like xnu's __start (EL1, MMU/caches off,
 * x0 = boot_args PA, no stack). Position independent.
 */
	.section .text.head, "ax"
	.globl _stub_entry
_stub_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	bl	stub_main
1:	wfe
	b	1b
