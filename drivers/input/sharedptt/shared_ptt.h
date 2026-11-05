// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a pin the programming cable's serial line shares (docs/spec/hardware-dm32.md 6.2). Every 0 bit the host sends
// pulls the pin low, so a pin read low while bytes are arriving is serial data, not a press. A real press holds the line
// low, which the UART sees as a break, never as a good byte. A low pin counts as pressed only once no good byte has
// arrived for SHARED_PTT_QUIET_MS, and it has read low (and not gated) for SHARED_PTT_HOLD_MS. The hold is time, not
// scans: a key scan that ran late catches up with scans back to back, and two of them in the same millisecond can land
// on the low bits of a host byte that starts a burst after a quiet gap, before the UART has reported it (seen during
// PPP, hardware-dm32.md 6.2).
//
// The radio's own transmission can get into the cable (a coiled cable near the antenna): the line then reads low and
// the UART receives garbage, flagged characters, none of them good. Gated as above, that is a press, and a press keeps
// the radio transmitting: a latch that held a radio keyed for a minute with nothing touched. So, while the radio
// transmits, the line is not trusted:
// - a press can't start while the radio transmits (one held when it started stays held);
// - a held press ends at a flagged character after its own break: a finger on the key grounds the line, and a grounded
//   line receives nothing more, so a character means the key is up and the line is picking up noise. The pin then
//   counts as released until it reads high.
// RF that holds a released line steadily low, with no character, still reads as held: the TX time-out ends that.
#ifndef SHARED_PTT_H
#define SHARED_PTT_H

#include <stdbool.h>
#include <stdint.h>

// Longer than the gap between the debug link's requests while a host polls, short against a press
#define SHARED_PTT_QUIET_MS	50u
// Longer than a host byte (87 us at 115200) plus a late scan's catch-up, short against a press
#define SHARED_PTT_HOLD_MS	20u
// For this long after a press is reported, flagged characters still count as its own (the break and the key's bounce)
#define SHARED_PTT_SETTLE_MS	50u

// What the serial line has received, and whether the radio transmits, as of the scan
typedef struct {
	bool rx_seen;		// a good byte has ever arrived
	uint32_t rx_ms;		// when the last one did (the same millisecond clock as now_ms, wrapping)
	uint32_t flagged;	// characters the UART flagged (break, framing, parity, overrun), a running count
	bool keyed;		// the radio is transmitting
} shared_ptt_line_t;

enum { SHARED_PTT_UP, SHARED_PTT_LOW, SHARED_PTT_PRESSED, SHARED_PTT_NOISE };

typedef struct {
	uint32_t ignored;	// scans with the pin low inside the quiet time or while the radio transmitted (diagnostics)
	uint32_t low_ms;	// when the pin was first read low outside the quiet time; valid in LOW and PRESSED
	uint16_t flagged;	// the line's flagged count (its low half) at the press, moved on while it settles
	uint8_t state;		// UP; LOW (held, not yet SHARED_PTT_HOLD_MS); PRESSED; NOISE (a held press ended by noise:
				// released until the pin reads high)
	uint8_t noise_releases;	// held presses ended by a flagged character, up to 255 (diagnostics)
} shared_ptt_t;

// pin_low: the PTT pin as read now
bool shared_ptt_pressed(shared_ptt_t *s, bool pin_low, const shared_ptt_line_t *line, uint32_t now_ms);

#endif
