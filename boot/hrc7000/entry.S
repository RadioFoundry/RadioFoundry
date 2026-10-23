/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* HR_C7000 first stage entry (boot/<radio>/boot.c, stage.c; boot/README.md): every HR_C7000 board with a first stage,
 * on each of its SoCs. The IAP
 * loader jumps to 0x0300C000, the first byte of this sector. On a normal power-on it does so only if the application's
 * first 4 bytes are 2C 11 01 C0 (right after an update it jumps without looking): those are the vendor application's
 * entry, "lrw r1, [0x0300C0B0]; mtcr r1, psr". The
 * same 6 bytes start this stage, with a harmless PSR (supervisor, interrupts off) in that literal.
 *
 * BOOT_WINDOW = 0: no application behind the stage, the handshake is offered forever.
 * BOOT_WINDOW > 0: boot_resume jumps to APP_RESUME, the application's first byte (ODR at 0x0300D000 sets up PSR,
 * stack and RAM itself, platform/hrc7000/crt0.S). */
	.section .entry, "ax"
	.align	2
	.global	boot_entry
boot_entry:
	.short	0x112C, 0xC001, 0x6420
	br	1f
	.org	0xB0
	.long	0x80000100
1:	psrclr	ie
	lrw	r0, 0x0004F000		/* below the stub's own stack top (0x50000); the stub sets its own */
	mov	sp, r0
	jbsr	boot_main
2:	br	2b

	.global	boot_resume
boot_resume:
#if BOOT_WINDOW
	lrw	r0, APP_RESUME
	jmp	r0
#else
	rts
#endif
