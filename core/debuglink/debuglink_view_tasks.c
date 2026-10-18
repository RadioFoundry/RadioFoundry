// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The tasks view section: every FreeRTOS task's least free stack since it started (core/app/task_stack.c), so a test
// can see a task running low before it overflows. Measured, where make stack-check (docs/dev/stack-check.md) computes.
#include "config.h"
#include "debuglink/debuglink.h"
#include "app/task_stack.h"

// A task name as a key: FreeRTOS names may hold spaces ("Tmr Svc"), keys hold letters, digits and '_'
static void key_name(char *out, const char *name)
{
	unsigned k = 0;

	for (; name[k] != '\0' && k < TASK_STACK_NAME_LEN; k++) {
		char c = name[k];
		int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		out[k] = ok ? c : '_';
	}
	if (k == 0) {
		out[k++] = '_';
	}
	out[k] = '\0';
}

void debuglink_view_tasks_format(const task_stack_t *ts, unsigned n, unsigned untracked, dl_text_t *t)
{
	char key[TASK_STACK_NAME_LEN + 1];
	unsigned least = 0;

	dl_text(t, "tasks.count=%u\ntasks.untracked=%u\n", n, untracked);
	for (unsigned i = 1; i < n; i++) {
		if (ts[i].free_bytes < ts[least].free_bytes) {
			least = i;
		}
	}
	if (n > 0) {
		key_name(key, ts[least].name);
		dl_text(t, "tasks.min_free=%u\ntasks.min_task=%s\n", (unsigned)ts[least].free_bytes, key);
	}
	for (unsigned i = 0; i < n; i++) {
		key_name(key, ts[i].name);
		dl_text(t, "tasks.free.%s=%u\n", key, (unsigned)ts[i].free_bytes);
	}
}

void debuglink_view_tasks(dl_text_t *t)
{
#if CONFIG_TASK_STACKS
	task_stack_t ts[TASK_STACK_MAX];	// 272 bytes on the caller's stack (the host link or the HTTP server)
	unsigned untracked = 0;
	unsigned n = task_stack_snapshot(ts, TASK_STACK_MAX, &untracked);

	debuglink_view_tasks_format(ts, n, untracked, t);
#else
	dl_text(t, "tasks.built=0\n");
#endif
}
