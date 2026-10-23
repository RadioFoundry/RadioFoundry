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
#if CONFIG_TASK_STACKS
#include "app/task_stack.h"
#ifndef configIDLE_TASK_NAME
#define configIDLE_TASK_NAME	"IDLE"	// the kernel's default, which only tasks.c sees
#endif
#endif

static void irq_off(void)
{
	taskDISABLE_INTERRUPTS();
}

// NUL padded, not necessarily terminated (the record's fields are exactly len)
static void put_name(char *out, unsigned len, const char *name)
{
	for (unsigned k = 0; k < len; k++) {
		out[k] = *name;
		if (*name)
			name++;
	}
}

uint32_t crash_stack_return(uint32_t sp, unsigned words)
{
	const uint32_t odd = hal_crash_arch() == CRASH_ARCH_CM4;	// Thumb return addresses have bit 0 set
	for (unsigned k = 0; k < words; k++, sp += 4u) {
		if ((sp & 3u) || !hal_crash_ram_addr(sp) || !hal_crash_ram_addr(sp + 3u))
			return 0;
		const uint32_t w = *(const volatile uint32_t *)(uintptr_t)sp;
		if ((w & 1u) == odd && hal_crash_code_addr(w & ~1u))
			return w;
	}
	return 0;
}

#if CONFIG_TASK_STACKS
// The other tasks, from the debug link's task table: not the running one (the record's registers are its, and on an
// interrupt its saved context is old), nor the idle task, which only ever waits for work. Not inlined: its frame stays
// off crash_commit's, which an assertion on the idle task's small stack has under it all the way to the restart
__attribute__((noinline)) static void put_tasks(crash_record_t *r)
{
	const TaskHandle_t self = xTaskGetCurrentTaskHandle();
	for (unsigned i = 0; i < TASK_STACK_MAX; i++) {
		const TaskHandle_t t = (TaskHandle_t)task_stack_handle(i);
		if (t == NULL || t == self)
			continue;
		const char *name = pcTaskGetName(t);
		if (strcmp(name, configIDLE_TASK_NAME) == 0)
			continue;
		if (r->ntasks == CRASH_TASKS) {
			if (r->more_tasks < 0xFFu)
				r->more_tasks++;
			continue;
		}
		crash_task_t *e = &r->tasks[r->ntasks++];
		put_name(e->name, CRASH_TAG_LEN, name);
		e->where = hal_crash_task_where(t);
	}
}
#endif

void crash_commit(crash_record_t *r)
{
	irq_off();
	board_rf_safe();	// again if the platform's fault handler already did: it costs a few register writes
	r->uptime_ms = hal_millis();
	if (!(r->flags & CRASH_F_ISR) && r->task[0] == 0) {
		TaskHandle_t t = xTaskGetCurrentTaskHandle();	// NULL before the scheduler's first task
		if (t != NULL)
			put_name(r->task, CRASH_TASK_LEN, pcTaskGetName(t));
	}
	// The words at sp, if sp is in RAM: a bad sp (the fault itself, often) must not fault again here
	const uint32_t span = 4u * CRASH_STACK_WORDS;
	if ((r->sp & 3u) == 0 && hal_crash_ram_addr(r->sp) && hal_crash_ram_addr(r->sp + span - 1u)) {
		memcpy(r->stack, (const void *)(uintptr_t)r->sp, span);
		r->flags |= CRASH_F_STACK;
	}
#if CONFIG_TASK_STACKS
	put_tasks(r);
#endif
	crash_record_t *store = hal_crash_store();
	crash_seal(r, store);
	if (store != NULL)
		*store = *r;
	hal_crash_restart();
}

// Static, not on the stack: an assertion or the stack overflow hook runs on whatever task hit it, the idle task's
// 512 bytes included, so the record can't take 256 bytes there (make stack-check)
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
		put_name(r->task, CRASH_TASK_LEN, task);
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
