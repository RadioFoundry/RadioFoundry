// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The crash log's record and its start-up side (crash.h). No RTOS calls: the host tests run all of it.
#include <string.h>
#include "config.h"
#include "crash/crash.h"
#include "debuglink/dl_fmt.h"

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "host"
#endif

static crash_record_t last;
static bool have_last;
static crash_reset_t reset;
static crash_boot_t *boot;	// the kept words, NULL before crash_init

uint32_t crash_crc32(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t crc = 0xFFFFFFFFu;
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));	// bitwise: no table in flash
	}
	return ~crc;
}

static uint32_t record_crc(const crash_record_t *r)
{
	return crash_crc32(r, offsetof(crash_record_t, crc));
}

bool crash_valid(const crash_record_t *r)
{
	return r != NULL && r->magic == CRASH_MAGIC && r->version == CRASH_VERSION && r->size == sizeof(*r) &&
		r->crc == record_crc(r);
}

uint32_t crash_build_id(const char *version, bool *dirty)
{
	uint32_t id = 0;
	int n = 0;
	for (; n < 8; n++) {
		char c = version[n];
		uint32_t d;
		if (c >= '0' && c <= '9')
			d = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (uint32_t)(c - 'A' + 10);
		else
			break;
		id = id << 4 | d;
	}
	if (n == 0 || (version[n] != '\0' && version[n] != '-'))
		id = 0;		// "host", "unknown": not a commit
	*dirty = strstr(version, "-dirty") != NULL;
	return id;
}

void crash_seal(crash_record_t *r, const crash_record_t *prev)
{
	r->magic = CRASH_MAGIC;
	r->version = CRASH_VERSION;
	r->size = sizeof(*r);
#ifdef RFOUNDRY_GIT_ID
	// The firmware: the Makefile worked crash_build_id(RFOUNDRY_GIT) out already (no parser in flash)
	const bool dirty = RFOUNDRY_GIT_DIRTY;
	r->build = RFOUNDRY_GIT_ID;
#else
	bool dirty;
	r->build = crash_build_id(RFOUNDRY_GIT, &dirty);
#endif
	if (dirty)
		r->flags |= CRASH_F_DIRTY;
	// In a row: the last crash of this build was never looked at (a crash at every start counts up)
	if (crash_valid(prev) && prev->build == r->build && !(prev->flags & CRASH_F_SEEN))
		r->count = (uint8_t)(prev->count < 0xFFu ? prev->count + 1u : 0xFFu);
	else
		r->count = 1;
	r->crc = record_crc(r);
}

void crash_reseal(crash_record_t *r)
{
	r->crc = record_crc(r);
}

// --- After the restart ---

void crash_init(void)
{
	const crash_record_t *s = hal_crash_store();
	have_last = crash_valid(s);
	if (have_last)
		last = *s;

	memset(&reset, 0, sizeof reset);
	uint8_t chip = hal_crash_reset_flags(&reset.flags);
	boot = hal_crash_boot();
	bool kept = boot != NULL && boot->magic == CRASH_BOOT_MAGIC;
	reset.cause = crash_reset_cause(chip, kept, kept && boot->soft == CRASH_BOOT_SOFT);
	if (kept) {
		reset.uptime_ms = boot->uptime_ms;
		reset.warm = boot->warm + 1u;
	}
	if (boot != NULL) {
		boot->soft = 0;
		boot->uptime_ms = 0;
		boot->warm = reset.warm;
		boot->magic = CRASH_BOOT_MAGIC;
	}
}

// The chip's own flags first where they say more than the memory can; a chip without them (or one that says only
// "the pin", which every internal reset also drives) leaves it to the kept words
uint8_t crash_reset_cause(uint8_t chip, bool kept, bool soft)
{
	if (chip == CRASH_RESET_WATCHDOG || chip == CRASH_RESET_BROWNOUT || chip == CRASH_RESET_POWER ||
	    chip == CRASH_RESET_SOFTWARE)
		return chip;
	if (soft)
		return CRASH_RESET_SOFTWARE;
	if (!kept)
		return CRASH_RESET_POWER;
	return chip == CRASH_RESET_PIN ? CRASH_RESET_PIN : CRASH_RESET_WARM;
}

const char *crash_reset_name(uint8_t cause)
{
	static const char *const names[] = { "unknown", "power", "software", "watchdog", "brownout", "pin", "warm" };
	return cause < sizeof names / sizeof names[0] ? names[cause] : "unknown";
}

const crash_reset_t *crash_reset(void)
{
	return &reset;
}

void crash_alive(uint32_t now_ms)
{
	if (boot != NULL)
		boot->uptime_ms = now_ms;
}

const crash_record_t *crash_last(void)
{
	return have_last ? &last : NULL;
}

void crash_mark(uint8_t flags)
{
	if (!have_last)
		return;
	last.flags |= flags;
	crash_reseal(&last);
	crash_record_t *s = hal_crash_store();
	if (s != NULL)
		*s = last;	// still the same one: nothing else writes the store while the radio runs
}

void crash_clear(void)
{
	crash_record_t *s = hal_crash_store();
	if (s != NULL)
		s->magic = 0;
	have_last = false;
}

// --- The report ---

static uint8_t *put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
	return p + 4;
}

size_t crash_pack(const crash_record_t *r, uint8_t *out, size_t max, bool (*is_code)(uint32_t addr))
{
	if (max < CRASH_PACK_FIXED)
		return 0;
	uint8_t *p = out;
	*p++ = 'O';
	*p++ = 'C';
	*p++ = CRASH_PACK_VERSION;
	*p++ = r->arch;
	*p++ = r->kind;
	*p++ = r->flags;
	*p++ = r->count;
	uint8_t *nret = p++;
	p = put32(p, r->build);
	p = put32(p, r->uptime_ms);
	p = put32(p, r->cause);
	p = put32(p, r->pc);
	p = put32(p, r->lr);
	p = put32(p, r->sp);
	p = put32(p, r->psr);
	for (int k = 0; k < 4; k++)
		p = put32(p, r->status[k]);
	memcpy(p, r->task, CRASH_TASK_LEN);
	p += CRASH_TASK_LEN;
	for (int k = 0; k < 4; k++)
		p = put32(p, r->r[k]);	// the arguments: often the bad pointer
	// The stack words that look like return addresses, in stack order: enough for a backtrace by hand
	uint8_t n = 0;
	if ((r->flags & CRASH_F_STACK) && is_code != NULL) {
		for (int k = 0; k < CRASH_STACK_WORDS && n < CRASH_PACK_RET_MAX; k++) {
			if ((size_t)(p - out) + 4u > max)
				break;
			if (is_code(r->stack[k] & ~1u)) {	// Thumb return addresses have bit 0 set
				p = put32(p, r->stack[k]);
				n++;
			}
		}
	}
	*nret = n;
	return (size_t)(p - out);
}

// --- Text ---

const char *crash_kind_name(uint8_t kind)
{
	switch (kind) {
	case CRASH_KIND_FAULT: return "Fault";
	case CRASH_KIND_STACK: return "Stack overflow";
	case CRASH_KIND_HEAP: return "Out of memory";
	case CRASH_KIND_ASSERT: return "Assert";
	case CRASH_KIND_HANG: return "Hang";
	default: return "Crash";
	}
}

void crash_summary(const crash_record_t *r, char *out, size_t len)
{
	// The debug link's printf subset, not formatting helpers of our own. Each piece is cut to the room left
	size_t n = dl_fmt(out, len, "%s", crash_kind_name(r->kind));
	if (r->kind == CRASH_KIND_FAULT || r->kind == CRASH_KIND_ASSERT)
		n += dl_fmt(out + n, len - n, r->kind == CRASH_KIND_FAULT ? " %u" : " line %u", (unsigned)r->cause);
	if (r->kind != CRASH_KIND_STACK && r->kind != CRASH_KIND_HEAP)
		n += dl_fmt(out + n, len - n, " at %08X", (unsigned)r->pc);
	if (r->kind == CRASH_KIND_HANG)
		n += dl_fmt(out + n, len - n, " for %u ms", (unsigned)r->cause);
	if (r->flags & CRASH_F_ISR) {
		dl_fmt(out + n, len - n, " in an interrupt");
	} else if (r->task[0] != 0) {
		char t[CRASH_TASK_LEN + 1];
		memcpy(t, r->task, CRASH_TASK_LEN);
		t[CRASH_TASK_LEN] = 0;
		dl_fmt(out + n, len - n, " in %s", t);
	}
}
