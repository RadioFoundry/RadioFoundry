// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The crash capture (core/crash/crash_capture.c, docs/spec/crash-log.md 8) on a stand-in RTOS (stubs/crash_rtos/): every way
// in (a platform's fault record, an assertion, a stack overflow, running out of heap, an assertion inside the capture)
// turns the transmitter off (board_rf_safe) with interrupts off, before the record is stored and before the restart.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "crash/crash.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// What happened, in order: R board_rf_safe, S the store handed out, X the restart
static char trace[16];
static unsigned trace_len;
static bool rf_irq_off;		// interrupts were off at the first board_rf_safe
static jmp_buf restarted;
static crash_record_t store;

bool fake_irq_off;

// crash_capture.c's FreeRTOS hooks (the RTOS declares them in the firmware)
void vApplicationStackOverflowHook(TaskHandle_t task, char *name);
void vApplicationMallocFailedHook(void);

static void note(char c)
{
	if (trace_len < sizeof trace - 1)
		trace[trace_len++] = c;
}

void board_rf_safe(void)
{
	if (strchr(trace, 'R') == NULL)
		rf_irq_off = fake_irq_off;
	note('R');
}

crash_record_t *hal_crash_store(void) { note('S'); return &store; }
uint8_t hal_crash_arch(void) { return CRASH_ARCH_CK803; }
bool hal_crash_ram_addr(uint32_t a) { (void)a; return false; }	// no stack copy: host pointers don't fit in 32 bits
bool hal_crash_code_addr(uint32_t a) { (void)a; return false; }
uint32_t hal_millis(void) { return 1234; }

void hal_crash_restart(void)
{
	note('X');
	longjmp(restarted, 1);
}

static char task_name[] = "radio";
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task_name; }
char *pcTaskGetName(TaskHandle_t t) { return t; }

static void begin(void)
{
	memset(trace, 0, sizeof trace);
	trace_len = 0;
	rf_irq_off = false;
	fake_irq_off = false;
	memset(&store, 0, sizeof store);
}

// The transmitter went off first, with interrupts off, and the restart came last
static void check_order(const char *what, uint8_t kind)
{
	const char *r = strchr(trace, 'R'), *s = strchr(trace, 'S'), *x = strchr(trace, 'X');
	bool ok = trace[0] == 'R' && r != NULL && x != NULL && x[1] == '\0' && (s == NULL || r < s) && rf_irq_off;
	if (!ok)
		printf("  %s: trace \"%s\", interrupts %s at the first RF off\n", what, trace, rf_irq_off ? "off" : "on");
	CHECK(ok);
	if (kind != 0)
		CHECK(crash_valid(&store) && store.kind == kind);
}

static void test_commit(void)
{
	begin();
	crash_record_t r;
	memset(&r, 0, sizeof r);
	r.arch = CRASH_ARCH_CK803;
	r.kind = CRASH_KIND_FAULT;
	r.cause = 2;
	if (setjmp(restarted) == 0)
		crash_commit(&r);
	check_order("fault", CRASH_KIND_FAULT);
	CHECK(store.uptime_ms == 1234 && strncmp(store.task, "radio", CRASH_TASK_LEN) == 0);
}

static void test_stack(void)
{
	begin();
	if (setjmp(restarted) == 0)
		vApplicationStackOverflowHook(NULL, task_name);
	check_order("stack overflow", CRASH_KIND_STACK);
	CHECK(strncmp(store.task, "radio", CRASH_TASK_LEN) == 0);
}

// After the first soft capture the next one is taken for one inside the capture and restarts at once: the transmitter
// still goes off first
static void test_nested(void)
{
	begin();
	if (setjmp(restarted) == 0)
		crash_assert(712);
	check_order("nested assert", 0);
	CHECK(strcmp(trace, "RX") == 0);
	begin();
	if (setjmp(restarted) == 0)
		vApplicationMallocFailedHook();
	check_order("nested heap", 0);
}

int main(void)
{
	test_commit();
	test_stack();
	test_nested();
	printf("crashCaptureTest: %s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
