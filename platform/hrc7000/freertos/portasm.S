/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2017 C-SKY Microsystems Co., Ltd. All rights reserved.
 * Copyright (c) 2026 The OpenDigitalRadio Authors
 *
 * FreeRTOS context switch and the common interrupt entry for the CK803S in the HR_C7000, adapted from FreeRTOS-Kernel
 * portable/ThirdParty/CDK/T-HEAD_CK802 portasm.S (MIT). Changes: the PIC end-of-interrupt write (0x04 to PIC_COW1,
 * docs/spec/hardware-dm32.md 4.1) at the start of every interrupt; the vector number is zero-extended and bounds
 * checked; a context switch asked for by a handler (ulPortYieldRequired) is done once at the end of the entry; the
 * yield saves the caller's PSR before interrupts are turned off. Placed in RAM (.ramfunc) with the slot interrupt
 * handlers so the interrupt path never waits for the XIP flash.
 *
 * Frame on the task stack (17 words, low to high): r0 .. r13, r15, PSR, PC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 * to permit persons to whom the Software is furnished to do so, subject to the following conditions: the above
 * copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

	.section .ramfunc, "ax"
	.align	2

/* Restore the frame at sp and return into it */
	.type	port_restore, @function
port_restore:
	ldw	r0, (sp, 64)
	mtcr	r0, epc
	ldw	r0, (sp, 60)
	mtcr	r0, epsr
	ldw	r15, (sp, 56)
	ldm	r0-r13, (sp)
	addi	sp, 68
	rte

/* First task: interrupts off until the rte loads the task's PSR */
	.global	vPortStartTask
	.type	vPortStartTask, @function
vPortStartTask:
	psrclr	ie
	lrw	r4, pxCurrentTCB
	ldw	r4, (r4, 0)
	ldw	sp, (r4, 0)
	br	port_restore

/* Task-level yield: called like a function, returns to the caller when the task runs again */
	.global	vPortYield
	.type	vPortYield, @function
vPortYield:
	subi	sp, 68
	stm	r0-r13, (sp)
	stw	r15, (sp, 56)
	stw	r15, (sp, 64)		/* resume at the return address */
	mfcr	r0, psr
	stw	r0, (sp, 60)		/* the caller's PSR, interrupts as they were */
	psrclr	ie
	lrw	r2, pxCurrentTCB
	ldw	r3, (r2, 0)
	stw	sp, (r3, 0)
	jbsr	vTaskSwitchContext
	lrw	r4, pxCurrentTCB
	ldw	r4, (r4, 0)
	ldw	sp, (r4, 0)
	br	port_restore

/* Every user interrupt (vectors 32 .. 95). Interrupts are off (the CPU cleared PSR.IE on entry) and stay off: no
 * nesting. The handler runs on the interrupt stack with r0 = PIC source (vector - 32) */
	.global	hrc_irq_entry
	.type	hrc_irq_entry, @function
hrc_irq_entry:
	subi	sp, 68
	stm	r0-r13, (sp)
	stw	r15, (sp, 56)
	mfcr	r0, epsr
	stw	r0, (sp, 60)
	mfcr	r0, epc
	stw	r0, (sp, 64)
	psrset	ee			/* a fault in the handler is then a fault, not an unrecoverable error */

	lrw	r7, pxCurrentTCB
	ldw	r7, (r7, 0)
	stw	sp, (r7, 0)
	lrw	r0, __irq_stack_top
	mov	sp, r0

	lrw	r1, 0x17000010		/* PIC_COW1: end of interrupt */
	movi	r2, 4
	stw	r2, (r1, 0)

	mfcr	r0, psr
	lsri	r0, r0, 16
	zextb	r0, r0
	subi	r0, 32
	movi	r1, 64
	cmphs	r0, r1
	bt	1f
	lrw	r1, hrc_irq_table
	lsli	r2, r0, 2
	addu	r1, r1, r2
	ldw	r1, (r1, 0)
	jsr	r1
1:
	lrw	r1, ulPortYieldRequired
	ldw	r2, (r1, 0)
	cmpnei	r2, 0
	bf	2f
	movi	r2, 0
	stw	r2, (r1, 0)
	jbsr	vTaskSwitchContext
2:
	lrw	r7, pxCurrentTCB
	ldw	r7, (r7, 0)
	ldw	sp, (r7, 0)
	br	port_restore

/* CPU exceptions 1 .. 31: every register into a frame like the interrupt entry's, below the stack pointer the fault
 * left (the CPU pushes nothing itself), then hrc_fault(frame, vector) on a stack of its own (irq.c), which keeps the
 * crash record and restarts, or stops for a debugger. A stack pointer that can't take the frame faults again with
 * exceptions off: an unrecoverable error, and the radio stays stopped as before */
	.global	hrc_fault_entry
	.type	hrc_fault_entry, @function
hrc_fault_entry:
	subi	sp, 68
	stm	r0-r13, (sp)
	stw	r15, (sp, 56)
	mfcr	r0, epsr
	stw	r0, (sp, 60)
	mfcr	r0, epc
	stw	r0, (sp, 64)
	mov	r0, sp
	mfcr	r1, psr
	lsri	r1, r1, 16
	zextb	r1, r1
	lrw	r2, hrc_fault_sp
	ldw	r2, (r2, 0)
	mov	sp, r2
	jbsr	hrc_fault
3:	br	3b
