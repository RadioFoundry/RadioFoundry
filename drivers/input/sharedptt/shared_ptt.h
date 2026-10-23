// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// PTT on a pin the programming cable's serial line shares (docs/spec/hardware-dm32.md 6.2). Every 0 bit the host sends
// pulls the pin low, so a pin read low while bytes are arriving is serial data, not a press. A real press holds the line
// low, which the UART sees as a break, never as a good byte. A low pin counts as pressed only once no good byte has
// arrived for SHARED_PTT_QUIET_MS, and it has read low (and not gated) for SHARED_PTT_HOLD_MS. The hold is time, not
// scans: a key scan that ran late catches up with scans back to back, and two of them in the same millisecond can land
// on the low bits of a host byte that starts a burst after a quiet gap, before the UART has reported it (seen during
// PPP, hardware-dm32.md 6.2).
#ifndef SHARED_PTT_H
#define SHARED_PTT_H

#include <stdbool.h>
#include <stdint.h>

// Longer than the gap between the debug link's requests while a host polls, short against a press
#define SHARED_PTT_QUIET_MS	50u
// Longer than a host byte (87 us at 115200) plus a late scan's catch-up, short against a press
#define SHARED_PTT_HOLD_MS	20u

typedef struct {
	uint32_t ignored;	// scans with the pin low inside the quiet time (diagnostics)
	uint32_t low_ms;	// when the pin was first read low outside the quiet time; valid while low
	bool low;
} shared_ptt_t;

// pin_low: the PTT pin as read now. rx_seen: a good byte has ever arrived; rx_ms: when the last one did (the same
// millisecond clock as now_ms, wrapping)
bool shared_ptt_pressed(shared_ptt_t *s, bool pin_low, bool rx_seen, uint32_t rx_ms, uint32_t now_ms);

#endif
