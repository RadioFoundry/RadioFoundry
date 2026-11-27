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
// transmits, no new press starts on the line (one held when it keyed stays held). And a held press ends, keyed or
// not, at a good byte or at a flagged character after its own break and bounce: a finger on the key grounds the line,
// and a grounded line receives nothing more, so a character means the key is up and the line is picking up noise or
// data. After noise the pin counts as released until it reads high.
// RF that holds a released line steadily low, with no character, still reads as held: the TX time-out ends that.
//
// The line can also latch low: the radio starting to send while the host still sends clamps it until a power cycle, and
// a chip reset doesn't release it (docs/spec/gotchas.md 16.5). It reads just like a held key, but it starts differently:
// a key goes down on a line that has idled high, a latch at the end of the traffic. So a new press needs the pin to
// have idled high since start-up and to have read high since the last exchange (a good byte and a send of the radio's
// own, both after the last high read), and not to have gone low right after the radio's own serial send. A good byte
// alone doesn't count: a key's bounce as it closes can frame one (a short low pulse is a start bit followed by ones),
// and a hand mic can send one as its key goes down. Taken for a latch, such a press never keyed the radio and flashed
// the stuck LEDs 2 s later (hardware-db25d.md 3.3). A line that stays low that way is stuck (shared_ptt_stuck): no
// press, until the pin reads high. A key held from power-on reads the same way, and counts once it has been released.
//
// Idled high since start-up is the arm of drivers/input/plugptt (PLUG_PTT_ARM_MS high, after PLUG_PTT_SETTLE_MS): the
// line is often on a connector, and with a cable or a mic in it can read high while the pads and what is plugged in
// come up, then rest low. A press is a fall from idle high, never a level found at start-up (docs/spec/gotchas.md
// 16.3).
#ifndef SHARED_PTT_H
#define SHARED_PTT_H

#include <stdbool.h>
#include <stdint.h>

#include "input/plugptt/plug_ptt.h"

// Longer than the gap between the debug link's requests while a host polls, short against a press
#define SHARED_PTT_QUIET_MS	50u
// Longer than a host byte (87 us at 115200) plus a late scan's catch-up, short against a press
#define SHARED_PTT_HOLD_MS	20u
// For this long after a press is reported, flagged characters still count as its own (the break and the key's bounce)
#define SHARED_PTT_SETTLE_MS	50u
// A low read this soon after the radio's own serial send may be the clamp: longer than a key scan plus the clamp's few ms
#define SHARED_PTT_TX_GUARD_MS	20u
// Low this long without being able to count as a press: the line is stuck. Longer than a key held across a power-on
#define SHARED_PTT_STUCK_MS	2000u

// What the serial line has received, and whether the radio transmits, as of the scan
typedef struct {
	bool rx_seen;		// a good byte has ever arrived
	uint32_t rx_ms;		// when the last one did (the same millisecond clock as now_ms, wrapping)
	uint32_t flagged;	// characters the UART flagged (break, framing, parity, overrun), a running count
	bool keyed;		// the radio is transmitting
	bool tx_seen;		// the radio has sent on the line
	uint32_t tx_ms;		// when it last did (same clock): now while a send is under way
} shared_ptt_line_t;

enum { SHARED_PTT_UP, SHARED_PTT_LOW, SHARED_PTT_PRESSED, SHARED_PTT_NOISE };

typedef struct {
	plug_ptt_t arm;		// armed: the pin has idled high since start-up
	uint32_t ignored;	// scans with the pin low inside the quiet time or while the radio transmitted (diagnostics)
	uint32_t low_ms;	// LOW: when the pin was first read low outside the quiet time; PRESSED: when it was reported
	uint32_t high_rx_ms;	// the line's rx_ms at the last scan that read the pin high
	uint32_t high_tx_ms;	// the line's tx_ms at the same scan
	uint32_t stuck_ms;	// when the pin was first read low unable to count as a press (stuck_timing)
	uint16_t flagged;	// the line's flagged count (its low half) at the press, moved on while it settles
	uint8_t state;		// UP; LOW (held, not yet SHARED_PTT_HOLD_MS); PRESSED; NOISE (a held press ended by noise:
				// released until the pin reads high)
	uint8_t noise_releases;	// held presses ended by a flagged character, up to 255 (diagnostics)
	bool high_rx_seen;	// the line's rx_seen at the last scan that read the pin high
	bool high_tx_seen;	// the line's tx_seen at the same scan
	bool after_send;	// went low right after the radio's own serial send: no press until it reads high
	bool stuck_timing;	// stuck_ms is running
} shared_ptt_t;

// pin_low: the PTT pin as read now
bool shared_ptt_pressed(shared_ptt_t *s, bool pin_low, const shared_ptt_line_t *line, uint32_t now_ms);
// The line has stayed low for SHARED_PTT_STUCK_MS without being able to count as a press, as of the last scan
bool shared_ptt_stuck(const shared_ptt_t *s, uint32_t now_ms);

#endif
