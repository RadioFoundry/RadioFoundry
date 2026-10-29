// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The task table behind the "tasks" view section (task_stack.h). The hooks run inside the kernel's critical section;
// the snapshot reads the table with the scheduler suspended, so no task is deleted or freed while its stack is walked
// (the idle task frees a deleted task's memory, and it can't run then).
#include <string.h>
#include "app/task_stack.h"
#include "FreeRTOS.h"
#include "task.h"

_Static_assert(configMAX_TASK_NAME_LEN <= TASK_STACK_NAME_LEN + 1, "task_stack_t.name is shorter than a task name");

static TaskHandle_t tasks[TASK_STACK_MAX];
static unsigned untracked_count;

void task_stack_created(void *task)
{
	for (unsigned i = 0; i < TASK_STACK_MAX; i++) {
		if (tasks[i] == NULL) {
			tasks[i] = (TaskHandle_t)task;
			return;
		}
	}
	untracked_count++;
}

void task_stack_deleted(void *task)
{
	for (unsigned i = 0; i < TASK_STACK_MAX; i++) {
		if (tasks[i] == (TaskHandle_t)task) {
			tasks[i] = NULL;
			return;
		}
	}
}

void *task_stack_handle(unsigned i)
{
	return i < TASK_STACK_MAX ? tasks[i] : NULL;
}

unsigned task_stack_snapshot(task_stack_t *out, unsigned max, unsigned *untracked)
{
	unsigned n = 0;

	for (unsigned i = 0; i < TASK_STACK_MAX && n < max; i++) {
		// One task per suspension: walking a stack takes up to a few hundred microseconds
		vTaskSuspendAll();
		TaskHandle_t h = tasks[i];
		if (h != NULL) {
			const char *name = pcTaskGetName(h);
			strncpy(out[n].name, name != NULL ? name : "?", TASK_STACK_NAME_LEN);
			out[n].name[TASK_STACK_NAME_LEN] = '\0';
			out[n].free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(h) * (uint32_t)sizeof(StackType_t);
			n++;
		}
		(void)xTaskResumeAll();
	}
	if (untracked != NULL) {
		*untracked = untracked_count;
	}
	return n;
}

void task_stack_heap(uint32_t *free_now, uint32_t *least_free)
{
	*free_now = (uint32_t)xPortGetFreeHeapSize();
	*least_free = (uint32_t)xPortGetMinimumEverFreeHeapSize();
}
