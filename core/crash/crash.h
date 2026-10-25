// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The crash log (CONFIG_CRASH_LOG, docs/spec/crash-log.md). A fault, a stack overflow, running out of heap, a failed
// assertion or a hang fills a crash_record_t (hal_crash.h), keeps it where a reset doesn't clear it and restarts the radio. At
// the next start crash_init() takes it up; the UI shows it and the crash report (CONFIG_CRASH_REPORT) can send it.
//
// The capture side (crash_commit and the hooks) runs with interrupts off on a stack of its own and calls nothing that
// waits. The rest is for the start-up and the UI task.
#ifndef RFOUNDRY_CRASH_H
#define RFOUNDRY_CRASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hal_crash.h"

// --- After the restart ---

// At start-up, before anything shows it: a valid record in the store becomes crash_last()
void crash_init(void);
// The record kept from before this start, NULL if there is none
const crash_record_t *crash_last(void);
// Set CRASH_F_SEEN / CRASH_F_SENT on the kept record, here and in the store
void crash_mark(uint8_t flags);
// Forget the record (the store too)
void crash_clear(void);

// Why this start happened (crash-log.md 1.3), as crash_init found it
typedef struct {
	uint8_t cause;			// CRASH_RESET_*
	uint32_t flags;			// the chip's reset flags register, 0 on a chip without one
	uint32_t uptime_ms;		// how long the run before lasted, 0 when the memory was lost
	uint32_t warm;			// starts since the memory last lost its contents (power-on: 0)
} crash_reset_t;
const crash_reset_t *crash_reset(void);
// The cause from the chip's flags (CRASH_RESET_*), whether the memory was kept and whether software marked the reset.
// A pure function: the host tests run it
uint8_t crash_reset_cause(uint8_t chip, bool kept, bool soft);
// The cause in a word: "power", "software", "watchdog", "brownout", "pin", "warm", "unknown"
const char *crash_reset_name(uint8_t cause);
// From the tick: the uptime, kept for the next start
void crash_alive(uint32_t now_ms);

// --- Capture (firmware only: crash_capture.c) ---

// The platform filled arch-specific fields (kind, cause, pc, lr, sp, psr, r, status, ISR flag); this adds the task,
// the uptime and the stack words, seals the record, stores it and restarts the radio
void crash_commit(crash_record_t *r) __attribute__((noreturn));
// The first word in words words up from sp that looks like a return address (in code, bit 0 as the arch has it), 0
// if none. Each word is checked to be in RAM first. For the platforms' hal_crash_task_where
uint32_t crash_stack_return(uint32_t sp, unsigned words);
#define CRASH_TASK_SCAN_WORDS	24	// how far up a waiting task's stack its kernel call's return address is looked for
// configASSERT (the platforms' FreeRTOSConfig.h): the line, the caller as pc
void crash_assert(unsigned line) __attribute__((noreturn));
// The FreeRTOS hooks: CRASH_KIND_STACK (the task's name) or CRASH_KIND_HEAP (task NULL: the running one)
void crash_rtos(uint8_t kind, const char *task) __attribute__((noreturn));

// --- The record (pure functions; the host tests run them) ---

uint32_t crash_crc32(const void *data, size_t len);
// Magic, version, size and CRC all right
bool crash_valid(const crash_record_t *r);
// Fill magic, version, size, build and count (from prev, the store's old record, may be NULL), then the CRC
void crash_seal(crash_record_t *r, const crash_record_t *prev);
// The CRC again after a change (the flags)
void crash_reseal(crash_record_t *r);
// The commit hash's first 8 hex digits as a number; *dirty when the build had local changes. 0 when unknown
uint32_t crash_build_id(const char *version, bool *dirty);

// The report: a compact binary form for the air (docs/spec/crash-log.md 4). Returns its length, 0 if max is too small
// for the fixed part. Stack words that point into code follow, as many as fit (at most CRASH_PACK_RET_MAX)
#define CRASH_UDP_PORT		4090	// the report's source and destination port (crash-log.md 5.1)
#define CRASH_PACK_VERSION	1	// the report's own layout: the record's task snapshot (version 2) isn't in it
#define CRASH_PACK_FIXED	76
#define CRASH_PACK_RET_MAX	12
#define CRASH_PACK_MAX		(CRASH_PACK_FIXED + 4 * CRASH_PACK_RET_MAX)
size_t crash_pack(const crash_record_t *r, uint8_t *out, size_t max, bool (*is_code)(uint32_t addr));

// One line for the screen: "Fault 3 at 0800C1A4 in radio", "Stack overflow in ui", "Assert line 712 at 03012A40",
// "Hang at 03012A40 for 3000 ms in radio"
void crash_summary(const crash_record_t *r, char *out, size_t len);
// What kind of crash, in words ("Fault", "Stack overflow", "Out of memory", "Assert", "Hang", "Crash")
const char *crash_kind_name(uint8_t kind);

// --- The fault trace (crash_trace.c, CONFIG_FAULT_TRACE, with or without CONFIG_CRASH_LOG) ---

// A platform's fault handler prints CRASH_TRACE_LINES of these on the host port, one a second, before it restarts:
// "\r\n#RD0FAULT1 n=01 v=<8 hex> pc= psr= lr= sp= r0= ... *<CRC-16/CCITT-FALSE, 4 hex>\r\n". The CRC covers the
// bytes from '#' up to and including the space before '*'. Three seconds stays inside the watchdog's timeout
#define CRASH_TRACE_SYNC	"#RD0FAULT1"
#define CRASH_TRACE_LINES	3u
typedef struct {
	uint32_t vector, pc, psr, lr, sp;
	const uint32_t *r;	// the general registers the entry saved, r0 up
	uint8_t nr;		// how many (16 at most)
} crash_trace_t;
// One line through out, a polled writer that works with interrupts off
void crash_trace_line(const crash_trace_t *t, uint8_t n, void (*out)(const char *s));
uint16_t crash_trace_crc(const char *s);

#endif
