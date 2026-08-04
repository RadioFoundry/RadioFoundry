// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The dlog() ring (see dlog.h). Lines are kept as text, "<ms> message\n", in a byte ring; overflow drops whole lines
// from the old end.
#include <stdarg.h>
#include <stdbool.h>
#include "debuglink/dlog.h"
#include "debuglink/dl_fmt.h"
#include "hal_time.h"

#if CONFIG_DEBUGLINK

#ifdef DLOG_NO_RTOS	// host tests
#define LOCK()
#define UNLOCK()
#else
#include "FreeRTOS.h"
#include "task.h"
#define LOCK()		taskENTER_CRITICAL()
#define UNLOCK()	taskEXIT_CRITICAL()
#endif

static char ring[DLOG_RING_BYTES];
static uint16_t head, tail, used;	// write at head, read at tail
static uint32_t dropped_lines;

static void drop_oldest_line(void)
{
	while (used > 0) {
		char c = ring[tail];
		tail = (uint16_t)((tail + 1u) % DLOG_RING_BYTES);
		used--;
		if (c == '\n') {
			break;
		}
	}
	dropped_lines++;
}

void dlog(const char *fmt, ...)
{
	char line[DLOG_LINE_MAX + 14];
	size_t n = dl_fmt(line, 12, "%u ", (unsigned)hal_millis());
	va_list ap;

	va_start(ap, fmt);
	n += dl_vfmt(line + n, DLOG_LINE_MAX + 1u, fmt, ap);
	va_end(ap);
	for (size_t i = 0; i < n; i++) {	// one line per call: embedded newlines become spaces
		if (line[i] == '\n' || line[i] == '\r') {
			line[i] = ' ';
		}
	}
	line[n++] = '\n';

	LOCK();
	while ((size_t)(DLOG_RING_BYTES - used) < n) {
		drop_oldest_line();
	}
	for (size_t i = 0; i < n; i++) {
		ring[head] = line[i];
		head = (uint16_t)((head + 1u) % DLOG_RING_BYTES);
	}
	used = (uint16_t)(used + n);
	UNLOCK();
}

size_t dlog_read(char *buf, size_t cap, uint8_t *more, uint32_t *dropped)
{
	size_t out = 0;

	LOCK();
	*dropped = dropped_lines;
	dropped_lines = 0;
	for (;;) {
		// length of the oldest line, '\n' included
		uint16_t len = 0;
		bool whole = false;
		while (len < used) {
			if (ring[(tail + len) % DLOG_RING_BYTES] == '\n') {
				len++;
				whole = true;
				break;
			}
			len++;
		}
		if (!whole || out + len > cap) {
			break;
		}
		for (uint16_t i = 0; i < len; i++) {
			buf[out++] = ring[tail];
			tail = (uint16_t)((tail + 1u) % DLOG_RING_BYTES);
		}
		used = (uint16_t)(used - len);
	}
	*more = used > 0;
	UNLOCK();
	return out;
}

#endif
