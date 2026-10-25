// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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

// The end of the name's first word: a space, an SSID ('-') or a suffix ('/')
static bool word_end(char ch)
{
	return ch == '\0' || ch == ' ' || ch == '-' || ch == '/';
}

uint8_t radio_cwid_callsign(const char *name, char *out, uint8_t cap)
{
	uint8_t n = 0;
	bool letter = false, digit = false;
	if (cap == 0) {
		return 0;
	}
	out[0] = '\0';
	while (*name == ' ') {
		name++;
	}
	for (; !word_end(name[n]); n++) {
		char ch = name[n];
		if (ch >= 'a' && ch <= 'z') {
			ch = (char)(ch - 'a' + 'A');
		}
		if (ch >= 'A' && ch <= 'Z') {
			letter = true;
		} else if (ch >= '0' && ch <= '9') {
			digit = true;
		} else {
			break;
		}
		if (n >= RADIO_CWID_CALL_MAX || n + 1u >= cap) {
			break;
		}
		out[n] = ch;
	}
	if (!word_end(name[n]) || n < RADIO_CWID_CALL_MIN || !letter || !digit) {
		out[0] = '\0';
		return 0;	// a name, not a callsign (or too long, or no room): never key it as the station's ID
	}
	out[n] = '\0';
	return n;
}

#endif	// CONFIG_SFR_CWID
