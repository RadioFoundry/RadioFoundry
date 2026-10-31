// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The crash log's view sections (docs/spec/debuglink.md 3.19, crash-log.md): the record kept from before this start,
// and why this start happened. Without the radio core, so a second SoC's debug link server (core/rx2/rx2_dl.c) shows
// the same sections. Without the crash log they say "crash.built=0" and "reset.built=0".
#include "config.h"
#include "debuglink/debuglink.h"

#if CONFIG_CRASH_LOG
#include "crash/crash.h"

void debuglink_view_crash(dl_text_t *t)
{
	const crash_record_t *r = crash_last();
	dl_text(t, "crash.built=1\ncrash.valid=%u\n", r != NULL);
	if (r == NULL)
		return;
	char task[CRASH_TASK_LEN + 1];
	for (int k = 0; k < CRASH_TASK_LEN; k++)
		task[k] = r->task[k];
	task[CRASH_TASK_LEN] = 0;
	dl_text(t, "crash.arch=%u\ncrash.kind=%u\ncrash.cause=%u\ncrash.flags=0x%x\ncrash.count=%u\n", r->arch, r->kind,
		(unsigned)r->cause, r->flags, r->count);
	dl_text(t, "crash.pc=0x%08x\ncrash.lr=0x%08x\ncrash.sp=0x%08x\ncrash.psr=0x%08x\n", (unsigned)r->pc,
		(unsigned)r->lr, (unsigned)r->sp, (unsigned)r->psr);
	dl_text(t, "crash.status=0x%08x,0x%08x,0x%08x,0x%08x\n", (unsigned)r->status[0], (unsigned)r->status[1],
		(unsigned)r->status[2], (unsigned)r->status[3]);
	dl_text(t, "crash.task=%s\ncrash.uptime_ms=%u\ncrash.build=%08x\n", task, (unsigned)r->uptime_ms,
		(unsigned)r->build);
	for (int k = 0; k < 14; k++)
		dl_text(t, "crash.r%d=0x%08x\n", k, (unsigned)r->r[k]);
	if (r->flags & CRASH_F_STACK) {
		dl_text(t, "crash.stack=");
		for (int k = 0; k < CRASH_STACK_WORDS; k++)
			dl_text(t, k ? ",%08x" : "%08x", (unsigned)r->stack[k]);
		dl_text(t, "\n");
	}
	// The other tasks: "crash.t<k>=<name>,<where>"; the name may hold a space, never a comma
	const unsigned n = r->ntasks < CRASH_TASKS ? r->ntasks : CRASH_TASKS;
	dl_text(t, "crash.tasks=%u\ncrash.tasks_more=%u\n", n, r->more_tasks);
	for (unsigned k = 0; k < n; k++) {
		char name[CRASH_TAG_LEN + 1];
		for (int c = 0; c < CRASH_TAG_LEN; c++)
			name[c] = r->tasks[k].name[c] == ',' ? '_' : r->tasks[k].name[c];
		name[CRASH_TAG_LEN] = 0;
		dl_text(t, "crash.t%u=%s,%08x\n", k, name, (unsigned)r->tasks[k].where);
	}
}

// Why this start happened, with or without a record (crash-log.md 1.3). A section of its own: a full crash section
// with the stack and the task snapshot already comes close to a reply's size
void debuglink_view_reset(dl_text_t *t)
{
	const crash_reset_t *x = crash_reset();
	dl_text(t, "reset.built=1\nreset.cause=%s\nreset.flags=0x%08x\nreset.uptime_ms=%u\nreset.warm=%u\n",
		crash_reset_name(x->cause), (unsigned)x->flags, (unsigned)x->uptime_ms, (unsigned)x->warm);
}

#else

void debuglink_view_crash(dl_text_t *t)
{
	dl_text(t, "crash.built=0\n");
}

void debuglink_view_reset(dl_text_t *t)
{
	dl_text(t, "reset.built=0\n");
}

#endif
