// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The test TX limit (docs/spec/radio-features.md 3.1): a limit a test harness puts on the transmitter over the debug
// link (RADIO op 35), so a test can't key the radio where the rig may not transmit. Off by default. Block: every
// transmission refused. A list: only those frequencies. The radio core checks it wherever it keys the transmitter
// (radio_sm.c), and PTT is denied as by the TX inhibit (TXDENY_INHIBIT).
//
// RAM only, never saved: a restart clears it, so a test run that dies leaves nothing behind (the TX inhibit setting
// is saved). Checked when the radio keys up: a transmission already on air when it is set goes on until it ends.
//
// Threading: the debug link task sets it, the radio task reads it. The writer raises block first and sets it last, so
// a reader never sees a half-written list without block (aligned word stores).
#ifndef RADIO_TX_LIMIT_H
#define RADIO_TX_LIMIT_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#define TX_LIMIT_MAX	10	// frequencies in the list (the HIL harness's transmit options)

typedef struct {
	bool on;		// a limit is set
	bool block;		// every transmission refused
	uint8_t n;		// frequencies in the list
	uint32_t refused;	// keyings refused by it since the start
	uint32_t last_hz;	// the last one refused
} tx_limit_info_t;

#if CONFIG_DEBUGLINK
// block, or n frequencies (Hz; n <= TX_LIMIT_MAX); !block and n 0: off. False (nothing changed): n too large
bool tx_limit_set(bool block, const uint32_t *hz, uint8_t n);
// The limit lets the radio key on hz (always true while off)
bool tx_limit_allows(uint32_t hz);
// A keying the limit refused: counted for the view
void tx_limit_refused(uint32_t hz);
// The state, and up to cap of the list's frequencies into hz (may be NULL)
void tx_limit_get(tx_limit_info_t *info, uint32_t *hz, uint8_t cap);
#else
static inline bool tx_limit_allows(uint32_t hz)
{
	(void)hz;
	return true;
}
static inline void tx_limit_refused(uint32_t hz)
{
	(void)hz;
}
#endif

#endif
