// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Hand mic key frames on a shared UART line (serialmic.h)
#include <string.h>
#include "serialmic.h"

void serialmic_init(serialmic_t *m)
{
	memset(m, 0, sizeof(*m));
	m->cand = SERIALMIC_NONE;
	m->held = SERIALMIC_NONE;
}

static uint8_t frame_code(const serialmic_t *m)
{
	const uint8_t *f = m->frame;
	if (m->len != 3 || m->error || f[0] != 'K' || f[1] < '0' || f[1] > '9' || f[2] < '0' || f[2] > '9') {
		return SERIALMIC_NONE;
	}
	return (uint8_t)((f[1] - '0') * 10 + (f[2] - '0'));
}

// A frame is over: a gap followed it
static void frame_end(serialmic_t *m)
{
	uint8_t code = frame_code(m);
	uint32_t end = m->last_us;
	m->len = 0;
	m->error = false;
	if (code == SERIALMIC_NONE || (m->host_seen && end - m->host_us < SERIALMIC_HOST_QUIET_US)) {
		m->cand = SERIALMIC_NONE;
		return;
	}
	if (code == m->held || (code == m->cand && end - m->cand_us < SERIALMIC_REPEAT_US)) {
		m->held = code;
		m->held_us = end;
	}
	m->cand = code;
	m->cand_us = end;
}

void serialmic_byte(serialmic_t *m, uint8_t b, bool error, uint32_t now_us)
{
	if (m->len != 0 && now_us - m->last_us > SERIALMIC_GAP_US) {
		frame_end(m);
	}
	if (m->prev == 0xA5u && b == 0x5Au) {
		m->host_seen = true;
		m->host_us = now_us;
		m->held = SERIALMIC_NONE;
	}
	m->prev = b;
	if (m->len < sizeof(m->frame)) {
		m->frame[m->len] = b;
	}
	if (m->len < UINT8_MAX) {
		m->len++;
	}
	m->error |= error;
	m->last_us = now_us;
}

uint8_t serialmic_key(serialmic_t *m, uint32_t now_us)
{
	if (m->len != 0 && now_us - m->last_us > SERIALMIC_GAP_US) {
		frame_end(m);
	}
	if (m->held != SERIALMIC_NONE && now_us - m->held_us > SERIALMIC_HELD_US) {
		m->held = SERIALMIC_NONE;
	}
	return m->held;
}
