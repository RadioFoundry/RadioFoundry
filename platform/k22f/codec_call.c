// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The codec call veneer (codec_call.h). The whole function is assembly at file scope: no compiler-generated frame,
// no assumptions about the caller's stack layout (docs/spec/gotchas.md 4.1).
#include "codec_call.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only

__asm__(
	"	.section .text.k22f_codec_call,\"ax\",%progbits\n"
	"	.syntax unified\n"
	"	.thumb\n"
	"	.thumb_func\n"
	"	.global k22f_codec_call\n"
	"	.type k22f_codec_call, %function\n"
	"k22f_codec_call:\n"
	"	push	{r4, r5, r6, r7, lr}\n"
	"	mov	r7, sp\n"		// frame: restored before returning, whatever the callee did
	"	orr	r4, r0, #1\n"		// Thumb bit
	"	mov	r5, r1\n"
	"	cmp	r2, #12\n"
	"	it	hi\n"
	"	movhi	r2, #12\n"
	"	subs	r6, r2, #4\n"		// stack argument count
	"	it	lt\n"
	"	movlt	r6, #0\n"
	"	sub	r12, sp, r6, lsl #2\n"
	"	bic	r12, r12, #7\n"	// AAPCS: 8-byte aligned at the call
	"	mov	sp, r12\n"
	"	movs	r3, #0\n"
	"1:	cmp	r3, r6\n"
	"	bge	2f\n"
	"	add	r0, r5, r3, lsl #2\n"
	"	ldr	r0, [r0, #16]\n"
	"	str	r0, [sp, r3, lsl #2]\n"
	"	adds	r3, r3, #1\n"
	"	b	1b\n"
	"2:	ldm	r5, {r0, r1, r2, r3}\n"	// reads 4 words even for fewer arguments: callers pass a full array
	"	blx	r4\n"
	"	mov	sp, r7\n"
	"	pop	{r4, r5, r6, r7, pc}\n"
	"	.size k22f_codec_call, . - k22f_codec_call\n"
	"	.text\n"
);

#endif	// CONFIG_DMR
