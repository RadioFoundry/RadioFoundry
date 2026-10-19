// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The standalone SFR repeater's CW ID schedule (radio_cwid.h, docs/spec/sfr.md 4, CW ID)
#include "config.h"
#if CONFIG_SFR_CWID	// the whole file
#include "radio/radio_cwid.h"

uint32_t radio_cwid_interval_ms(uint8_t code)
{
	static const uint8_t minutes[RADIO_CWID_CODES] = { 0, 5, 10, 15, 30 };
	return code < RADIO_CWID_CODES ? minutes[code] * 60000u : 0;
}

bool radio_cwid_due(radio_cwid_t *c, uint32_t interval_ms, bool busy, uint32_t now_ms)
{
	if (busy) {
		c->pending = true;
		c->last_busy_ms = now_ms;
		return false;
	}
	if (!c->pending || interval_ms == 0 || (now_ms - c->last_busy_ms) < RADIO_CWID_QUIET_MS) {
		return false;
	}
	return !c->ever || (now_ms - c->last_id_ms) >= interval_ms;
}

void radio_cwid_sent(radio_cwid_t *c, uint32_t now_ms)
{
	c->pending = false;
	c->ever = true;
	c->last_id_ms = now_ms;
}

uint8_t radio_cwid_callsign(const char *name, char *out, uint8_t cap)
{
	uint8_t n = 0;
	if (cap == 0) {
		return 0;
	}
	while (*name == ' ') {
		name++;
	}
	while (name[n] != '\0' && name[n] != ' ' && name[n] != '-' && n < cap - 1u) {
		char ch = name[n];
		out[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
	}
	out[n] = '\0';
	return n;
}

#endif	// CONFIG_SFR_CWID
