// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The standalone SFR repeater's CW ID (CONFIG_SFR_CWID, docs/spec/sfr.md 4, CW ID): when to key it and what it sends. No
// hardware here: radio_sm.c keys the Morse on the channel's frequency in FM (the hotspot's CW keyer).
#ifndef RADIO_CWID_H
#define RADIO_CWID_H

#include <stdint.h>
#include <stdbool.h>

#define RADIO_CWID_QUIET_MS	2000u	// the repeater idle this long before the ID is keyed
#define RADIO_CWID_CODES	5u	// interval codes 0..4
#define RADIO_CWID_CALL_MIN	4u	// a callsign: 4 to 7 letters and digits, at least one of each
#define RADIO_CWID_CALL_MAX	7u

typedef struct {
	bool pending;		// the repeater transmitted since the last ID
	bool ever;		// an ID was sent since the role started
	uint32_t last_id_ms;
	uint32_t last_busy_ms;
} radio_cwid_t;

// The interval for a settings code: 0 off, 1 5 min, 2 10 min, 3 15 min, 4 30 min (0 for an unknown code)
uint32_t radio_cwid_interval_ms(uint8_t code);

// Every tick while the repeater role runs. busy: it transmits now (a repeat or its beacon). True when the ID is due:
// the repeater transmitted since the last ID, has been idle RADIO_CWID_QUIET_MS, and the interval has passed since the
// last ID (the first one is due at once)
bool radio_cwid_due(radio_cwid_t *c, uint32_t interval_ms, bool busy, uint32_t now_ms);
// The ID was keyed (or given up): the next one waits for new traffic and the interval
void radio_cwid_sent(radio_cwid_t *c, uint32_t now_ms);

// The callsign from the radio name: its first word up to a space, '-' (an SSID) or '/', upper case, and only if it
// clearly is one: 4 to 7 letters and digits with at least one of each. Returns its length, 0 if the name has none
// (out is then empty); out gets a terminating NUL (cap includes it, so at least 8 for a 7-character callsign)
uint8_t radio_cwid_callsign(const char *name, char *out, uint8_t cap);

#endif
