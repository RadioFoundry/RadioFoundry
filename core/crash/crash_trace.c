// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The fault trace line (docs/spec/crash-log.md 2.3): what a platform's fault handler prints on the host port before it
// restarts, framed so a tool finds it among whatever else the port carries (tools/odr_fault_trace.py). Pure: the
// handler passes its own polled writer, and nothing here waits or allocates.
#include "crash.h"

static uint16_t crc_add(uint16_t crc, const char *s)
{
	for (; *s != 0; s++) {
		crc ^= (uint16_t)((uint16_t)(uint8_t)*s << 8);
		for (int b = 0; b < 8; b++) {
			uint32_t c = (uint32_t)crc << 1;
			crc = (uint16_t)(((crc & 0x8000u) != 0u) ? (c ^ 0x1021u) : c);
		}
	}
	return crc;
}

uint16_t crash_trace_crc(const char *s)
{
	return crc_add(0xFFFFu, s);
}

static void hex(char *p, uint32_t v, int digits)
{
	for (int i = 0; i < digits; i++) {
		p[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15u];
	}
	p[digits] = 0;
}

typedef struct {
	void (*out)(const char *s);
	uint16_t crc;
} sink_t;

static void put(sink_t *k, const char *s)
{
	k->crc = crc_add(k->crc, s);
	k->out(s);
}

static void put_reg(sink_t *k, const char *name, uint32_t v)
{
	char h[9];
	hex(h, v, 8);
	put(k, name);
	put(k, "=");
	put(k, h);
	put(k, " ");
}

void crash_trace_line(const crash_trace_t *t, uint8_t n, void (*out)(const char *s))
{
	sink_t k = { out, 0xFFFFu };
	char h[9];

	out("\r\n");
	put(&k, CRASH_TRACE_SYNC " n=");
	hex(h, n, 2);
	put(&k, h);
	put(&k, " ");
	put_reg(&k, "v", t->vector);
	put_reg(&k, "pc", t->pc);
	put_reg(&k, "psr", t->psr);
	put_reg(&k, "lr", t->lr);
	put_reg(&k, "sp", t->sp);
	for (uint8_t i = 0; i < t->nr && i < 16u; i++) {
		char name[4] = { 'r', (char)('0' + i % 10u), 0, 0 };
		if (i >= 10u) {
			name[1] = '1';
			name[2] = (char)('0' + i - 10u);
		}
		put_reg(&k, name, t->r[i]);
	}
	hex(h, k.crc, 4);
	out("*");
	out(h);
	out("\r\n");
}
