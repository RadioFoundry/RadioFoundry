/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2017 C-SKY Microsystems Co., Ltd. All rights reserved.
 * Copyright (c) 2026 The RadioFoundry Authors
 *
 * FreeRTOS port for the C-SKY CK803S in the HR_C7000, adapted from FreeRTOS-Kernel
 * portable/ThirdParty/CDK/T-HEAD_CK802 port.c (MIT; see portmacro.h for the changes). The tick is the SoC's timer 1
 * (platform/hrc7000/time.c), its interrupt goes through the common entry in portasm.S like every other.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 * to permit persons to whom the Software is furnished to do so, subject to the following conditions: the above
 * copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "config.h"
#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
#include "app/watchdog.h"
#endif

extern void vPortStartTask(void);

// Not 0 until the scheduler starts, so leaving a critical section before then never enables interrupts
volatile uint32_t ulCriticalNesting = 0x9999u;
volatile uint32_t ulPortPendingYield;
volatile uint32_t ulPortYieldRequired;

// PSR of a new task: supervisor (bit 31), exceptions enabled (bit 8), interrupts enabled (bit 6)
#define TASK_INITIAL_PSR	0x80000140u

static void prvTaskExitError(void)
{
	// A task returned from its function: a bug. Stop here (visible with a debugger)
	portDISABLE_INTERRUPTS();
	for (;;) {
	}
}

// Frame (low to high): r0 .. r13, r15, PSR, PC: what portasm.S saves and restores
StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters)
{
	StackType_t *stk = pxTopOfStack;

	*(--stk) = (StackType_t)pxCode;			// PC
	*(--stk) = (StackType_t)TASK_INITIAL_PSR;	// PSR
	*(--stk) = (StackType_t)prvTaskExitError;	// r15 (link)
	for (int r = 13; r >= 1; r--) {
		*(--stk) = (StackType_t)(r * 0x01010101u);	// r13 .. r1, recognizable in a dump
	}
	*(--stk) = (StackType_t)pvParameters;		// r0
	return stk;
}

BaseType_t xPortStartScheduler(void)
{
	hrc_tick_start(configTICK_RATE_HZ);
	ulCriticalNesting = 0;
	vPortStartTask();
	return pdFALSE;
}

void vPortEndScheduler(void)
{
}

void vPortEnterCritical(void)
{
	portDISABLE_INTERRUPTS();
	ulCriticalNesting++;
}

void vPortExitCritical(void)
{
	if (ulCriticalNesting == 0) {
		for (;;) {	// unbalanced exit
		}
	}
	ulCriticalNesting--;
	if (ulCriticalNesting == 0) {
		portENABLE_INTERRUPTS();
		if (ulPortPendingYield) {
			ulPortPendingYield = 0;
			portYIELD();
		}
	}
}

// Timer 1 handler (registered by hrc_tick_start). The hang check runs here, not in the tick hook: xTaskResumeAll also
// calls the hook, from a task, for ticks that came while the scheduler was suspended, and the check needs the frame
// the interrupt entry saved
void vPortTickHandler(void)
{
	hrc_timer_ack(TIMER_TICK);
#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
	watchdog_tick();	// a record before the watchdog's reset (crash-log.md 1.1)
#endif
	if (xTaskIncrementTick() != pdFALSE) {
		ulPortYieldRequired = 1;
	}
}
