// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The crash log's capture side (crash.h): what every crash adds to the platform's registers, and the FreeRTOS and
// assert hooks. Runs with interrupts off, from a fault handler or a hook; reads the RTOS's current task but takes no
// lock and never waits. The transmitter goes off first (board_rf_safe, crash-log.md 8): a crash while transmitting
// must not leave the PA keyed through the capture, the restart and a start-up that may not finish. The host test
// (crashCaptureTest) runs it on a stand-in RTOS.
#include <string.h>
#include "config.h"
#include "crash/crash.h"
#include "hal_time.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"

static void irq_off(void)
{
	taskDISABLE_INTERRUPTS();
}

// NUL padded, not necessarily terminated (the record's field is exactly CRASH_TASK_LEN)
static void put_name(char out[CRASH_TASK_LEN], const char *name)
{
	for (unsigned k = 0; k < CRASH_TASK_LEN; k++) {
		out[k] = *name;
		if (*name)
			name++;
	}
}

void crash_commit(crash_record_t *r)
{
	irq_off();
	board_rf_safe();	// again if the platform's fault handler already did: it costs a few register writes
	r->uptime_ms = hal_millis();
	if (!(r->flags & CRASH_F_ISR) && r->task[0] == 0) {
		TaskHandle_t t = xTaskGetCurrentTaskHandle();	// NULL before the scheduler's first task
		if (t != NULL)
			put_name(r->task, pcTaskGetName(t));
	}
	// The words at sp, if sp is in RAM: a bad sp (the fault itself, often) must not fault again here
	const uint32_t span = 4u * CRASH_STACK_WORDS;
	if ((r->sp & 3u) == 0 && hal_crash_ram_addr(r->sp) && hal_crash_ram_addr(r->sp + span - 1u)) {
		memcpy(r->stack, (const void *)(uintptr_t)r->sp, span);
		r->flags |= CRASH_F_STACK;
	}
	crash_record_t *store = hal_crash_store();
	crash_seal(r, store);
	if (store != NULL)
		*store = *r;
	hal_crash_restart();
}

// Static, not on the stack: an assertion or the stack overflow hook runs on whatever task hit it, the idle task's
// 512 bytes included, so the record can't take 188 bytes there (make stack-check)
static crash_record_t soft_rec;
static bool soft_busy;

__attribute__((noreturn)) static void soft_capture(uint8_t kind, uint32_t cause, uint32_t pc, const char *task)
{
	irq_off();
	board_rf_safe();
	if (soft_busy)
		hal_crash_restart();	// an assertion inside the capture: the first record is half made, restart
	soft_busy = true;
	crash_record_t *r = &soft_rec;
	memset(r, 0, sizeof *r);
	volatile uint32_t here = 0;
	r->arch = hal_crash_arch();
	r->kind = kind;
	r->cause = cause;
	r->pc = pc;
	r->lr = pc;
	r->sp = (uint32_t)(uintptr_t)&here;	// this function's frame: near enough for the stack words
	if (task != NULL)
		put_name(r->task, task);
	crash_commit(r);
}

void crash_assert(unsigned line)
{
	soft_capture(CRASH_KIND_ASSERT, line, (uint32_t)(uintptr_t)__builtin_return_address(0), NULL);
}

void crash_rtos(uint8_t kind, const char *task)
{
	soft_capture(kind, 0, (uint32_t)(uintptr_t)__builtin_return_address(0), task);
}

// The FreeRTOS hooks (configCHECK_FOR_STACK_OVERFLOW, configUSE_MALLOC_FAILED_HOOK); without the crash log the
// platforms' own stop the radio
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	crash_rtos(CRASH_KIND_STACK, name);
}

void vApplicationMallocFailedHook(void)
{
	crash_rtos(CRASH_KIND_HEAP, NULL);
}
