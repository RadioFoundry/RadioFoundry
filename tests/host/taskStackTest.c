// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's tasks view section (core/debuglink/debuglink_view_tasks.c, docs/spec/debuglink.md 3.7): every
// task's least free stack as keys, the least of them, names made into keys. The kernel side (core/app/task_stack.c)
// is faked: the host tests have no RTOS
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.h"

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                \
			printf(__VA_ARGS__);                                                                       \
			printf("\n");                                                                              \
		}                                                                                                  \
	} while (0)

// --- the fake task table ---

static task_stack_t fake[TASK_STACK_MAX];
static unsigned fake_n, fake_untracked, asked_max;

unsigned task_stack_snapshot(task_stack_t *out, unsigned max, unsigned *untracked)
{
	unsigned n = fake_n < max ? fake_n : max;

	asked_max = max;
	memcpy(out, fake, n * sizeof(*out));
	*untracked = fake_untracked;
	return n;
}

void task_stack_heap(uint32_t *free_now, uint32_t *least_free)
{
	*free_now = 9000;
	*least_free = 6000;
}

static void add(const char *name, uint32_t free_bytes)
{
	snprintf(fake[fake_n].name, sizeof(fake[fake_n].name), "%s", name);
	fake[fake_n].free_bytes = free_bytes;
	fake_n++;
}

static char out[1024];

static const char *render(void)
{
	dl_text_t t;

	dl_text_init(&t, out, sizeof(out) - 1);
	debuglink_view_tasks(&t);
	out[t.len] = '\0';
	return out;
}

static int has(const char *line)
{
	char want[96];

	snprintf(want, sizeof(want), "%s\n", line);
	if (strncmp(out, want, strlen(want)) == 0) {
		return 1;
	}
	snprintf(want, sizeof(want), "\n%s\n", line);
	return strstr(out, want) != NULL;
}

// --- tests ---

static void testEmpty(void)
{
	fake_n = 0;
	fake_untracked = 0;
	render();
	CHECK(strcmp(out, "tasks.count=0\ntasks.untracked=0\ntasks.heap_free=9000\ntasks.heap_min_free=6000\n") == 0,
	      "empty table: '%s'", out);
	CHECK(asked_max == TASK_STACK_MAX, "the whole table asked for: %u", asked_max);
}

static void testTasks(void)
{
	fake_n = 0;
	fake_untracked = 0;
	add("start", 472);
	add("radio", 812);
	add("audio", 196);
	add("IDLE", 300);
	render();
	CHECK(has("tasks.count=4"), "count: '%s'", out);
	CHECK(has("tasks.untracked=0"), "untracked: '%s'", out);
	CHECK(has("tasks.min_free=196"), "least free: '%s'", out);
	CHECK(has("tasks.min_task=audio"), "least free task: '%s'", out);
	CHECK(has("tasks.free.start=472") && has("tasks.free.radio=812") && has("tasks.free.audio=196") &&
	      has("tasks.free.IDLE=300"), "every task: '%s'", out);
	// In the order the kernel created them
	CHECK(strstr(out, "tasks.free.start") < strstr(out, "tasks.free.radio") &&
	      strstr(out, "tasks.free.audio") < strstr(out, "tasks.free.IDLE"), "creation order: '%s'", out);
}

static void testTieKeepsFirst(void)
{
	fake_n = 0;
	add("ui", 64);
	add("host", 64);
	render();
	CHECK(has("tasks.min_task=ui"), "a tie names the first: '%s'", out);
}

static void testNamesAsKeys(void)
{
	fake_n = 0;
	add("Tmr Svc", 900);	// FreeRTOS's timer task
	add("", 10);
	add("a.b=c", 20);
	add("ABCDEFGHIJKL", 30);	// as long as the field holds
	render();
	CHECK(has("tasks.free.Tmr_Svc=900"), "space made '_': '%s'", out);
	CHECK(has("tasks.free._=10"), "an empty name is '_': '%s'", out);
	CHECK(has("tasks.min_task=_"), "least free with an empty name: '%s'", out);
	CHECK(has("tasks.free.a_b_c=20"), "'.' and '=' made '_': '%s'", out);
	CHECK(has("tasks.free.ABCDEFGHIJKL=30"), "a full-length name: '%s'", out);
}

static void testUntrackedAndFull(void)
{
	fake_n = 0;
	for (unsigned i = 0; i < TASK_STACK_MAX; i++) {
		char name[13];
		snprintf(name, sizeof(name), "task%02u", i);
		add(name, 1000u + i);
	}
	fake_untracked = 3;
	render();
	CHECK(has("tasks.count=16"), "a full table: '%s'", out);
	CHECK(has("tasks.untracked=3"), "tasks past the table counted: '%s'", out);
	CHECK(has("tasks.min_free=1000") && has("tasks.min_task=task00"), "least of a full table: '%s'", out);
	CHECK(has("tasks.free.task15=1015"), "the last task fits one reply: '%s'", out);
	CHECK(strlen(out) < 600, "a full table stays well inside a reply: %u bytes", (unsigned)strlen(out));
}

int main(void)
{
	testEmpty();
	testTasks();
	testTieKeepsFirst();
	testNamesAsKeys();
	testUntrackedAndFull();
	printf("taskStackTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
