// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Every FreeRTOS task's least free stack since it started, for the debug link's "tasks" view section
// (docs/spec/debuglink.md 3.7). The kernel has no task list without its trace facility, so the boards'
// FreeRTOSConfig.h pass each task's creation and deletion here (traceTASK_CREATE / traceTASK_DELETE) and this keeps
// the handles. Included from FreeRTOSConfig.h, so it uses no FreeRTOS type.
#ifndef TASK_STACK_H
#define TASK_STACK_H

#include <stdint.h>

#define TASK_STACK_MAX		16	// tasks tracked; more are counted in task_stack_snapshot's untracked
#define TASK_STACK_NAME_LEN	12	// configMAX_TASK_NAME_LEN on every board

typedef struct {
	char name[TASK_STACK_NAME_LEN + 1];	// NUL terminated
	uint32_t free_bytes;			// the least free stack seen (the high water mark), in bytes
} task_stack_t;

// The kernel's hooks, called inside its critical section with the new or deleted task's TCB
void task_stack_created(void *task);
void task_stack_deleted(void *task);

// Copy up to max tasks into out, in creation order; returns how many. *untracked: tasks created while the table was
// full. Walks each task's stack with the scheduler suspended, one task at a time (firmware only)
unsigned task_stack_snapshot(task_stack_t *out, unsigned max, unsigned *untracked);
// The RTOS heap (every task stack and queue): its free bytes now and the least since start (firmware only)
void task_stack_heap(uint32_t *free_now, uint32_t *least_free);
// Slot i's handle (a TCB), NULL for an empty slot or i >= TASK_STACK_MAX. No lock: the crash capture
// (core/crash/crash_capture.c) reads the table with interrupts off
void *task_stack_handle(unsigned i);

#endif
