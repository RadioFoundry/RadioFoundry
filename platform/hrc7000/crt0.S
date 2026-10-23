/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* HR_C7000 / CK803S start-up (docs/spec/hardware-dm32.md 1.2, 2). The Baofeng bootloader jumps to the first byte of
 * the application at 0x0300C000 (no header, no vector table there), so Reset_Handler is linked first. It runs from
 * XIP flash with whatever the bootloader left: interrupts off, supervisor mode, our stack, the RAM sections copied,
 * .bss cleared, then C. Vectors are set up later from C (irq.c) because the table lives in RAM. */

	.section .entry, "ax"	// not .text.*: -ffunction-sections names a C function reset() .text.reset
	.align	2
	.global	Reset_Handler
	.type	Reset_Handler, @function
Reset_Handler:
	psrclr	ie
	lrw	r0, 0x80000100		/* supervisor, exceptions enabled, interrupts off (hardware-dm32.md 1.2) */
	mtcr	r0, psr
	lrw	r0, __stack_top
	mov	sp, r0

	/* RAM code (.ramfunc) and initialized data: copy from their load addresses in flash */
	lrw	r1, __ramfunc_load
	lrw	r2, __ramfunc_start
	lrw	r3, __ramfunc_end
	bsr	copy_words
	lrw	r1, __data_load
	lrw	r2, __data_start
	lrw	r3, __data_end
	bsr	copy_words

	/* .bss and the low-RAM zero section */
	movi	r0, 0
	lrw	r2, __bss_start
	lrw	r3, __bss_end
	bsr	zero_words
	lrw	r2, __ram_lo_bss_start
	lrw	r3, __ram_lo_bss_end
	bsr	zero_words

	jbsr	main
1:	br	1b

/* copy words from r1 to r2 until r2 reaches r3 */
copy_words:
	cmphs	r2, r3
	bt	2f
	ldw	r0, (r1, 0)
	stw	r0, (r2, 0)
	addi	r1, 4
	addi	r2, 4
	br	copy_words
2:	rts

/* store r0 from r2 until r2 reaches r3 */
zero_words:
	cmphs	r2, r3
	bt	3f
	stw	r0, (r2, 0)
	addi	r2, 4
	br	zero_words
3:	rts

	.size	Reset_Handler, . - Reset_Handler
