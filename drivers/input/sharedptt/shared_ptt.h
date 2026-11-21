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
// have read high since start-up and since the last good byte, and not to have gone low right after the radio's own
// serial send. A line that stays low that way is stuck (shared_ptt_stuck): no press, until the pin reads high. A key
// held from power-on reads the same way, and counts once it has been released.
//
// A cable can also hold the line low by itself, and that is a press to everything above. A cable keyed a radio at
// power-on, nobody near the PTT, while a tool answered the boot ROM's greeting every 30 ms (2026-11-21, the radio's
// hardware page): a break, a TXD left idling low after a failed baud change or on close, or a rate gone wrong all read
// as a held key once the good bytes stop for 50 ms. A finger can't be told from a held line, but a cable gives itself
// away by what it did before. So, in front of the press and latch checks above, an arming gate:
// - It starts disarmed, and arms once the line has read high, with nothing received, for SHARED_PTT_ARM_MS. A line low
//   or busy from power-on never keys, however long it stays that way.
// - Characters (good or flagged) are traffic, and disarm it until the line is high and quiet for the arm time again,
//   unless they are a key's own: those arrive within SHARED_PTT_BOUNCE_MS of the pin going low or high (the contact's
//   bounce, the press's break) at most at one such edge in SHARED_PTT_STREAM_MS, or flagged ones while the radio
//   transmits (its own RF on the cable). A held press that a character ends disarms it too: RF and a cable's break
//   can't be told apart. So a cable that talks and then goes quiet and drops the line (a port closed with a break)
//   can't key, however soon or late within the arm time.
// - A low that starts while the radio transmits stays untrusted until the pin reads high, as after noise: a cable
//   that drops the line during a transmission the link keyed can't key the radio again when the link's time-out
//   ends it.
// The latch check and the stuck diagnostic run whether the gate is armed or not: a line that latches while disarmed
// (after traffic, or from power-on) still reads stuck after SHARED_PTT_STUCK_MS, and a good byte or the pin reading
// high still clears it. And a key's own bounce can leave a good byte between the last scan that read the pin high and the
// first that reads it low: when the gate is armed and that byte came at the press's edge, it is the press's, and
// counts as seen high, so the press keys once the quiet time has passed (before, the latch check refused every press
// whose bounce made a good byte, for as long as it was held).
// The cost: while a host talks on the cable, and for a second after, the PTT on the radio does nothing, and a stray
// character on an idle line costs the same second. A cable left idle high for longer than that and then pulled low is
// still a press: nothing on the line tells it from a finger. The TX time-out ends it.
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
// A low read this soon after the radio's own serial send may be the clamp: longer than a key scan plus the clamp's few ms
#define SHARED_PTT_TX_GUARD_MS	20u
// Low this long without being able to count as a press: the line is stuck. Longer than a key held across a power-on
#define SHARED_PTT_STUCK_MS	2000u
// The line must read high with nothing received for this long before a low can press: after power-on and after
// traffic. Longer than a tool takes between giving up and closing its port, short against reaching for the PTT
#define SHARED_PTT_ARM_MS	1000u
// Characters this close to the pin going low or high are the key's own (its bounce lasts a few ms, its break is one
// character): a scan late by one period still sees them inside it
#define SHARED_PTT_BOUNCE_MS	25u
// Two edges with characters closer together than this are traffic, not a key: far shorter than a person can press
// and release, longer than the boot ROM greeting's 30 ms repeat at any rate
#define SHARED_PTT_STREAM_MS	100u

// What the serial line has received, and whether the radio transmits, as of the scan
typedef struct {
	bool rx_seen;		// a good byte has ever arrived
	uint32_t rx_ms;		// when the last one did (the same millisecond clock as now_ms, wrapping)
	uint32_t flagged;	// characters the UART flagged (break, framing, parity, overrun), a running count
	bool keyed;		// the radio is transmitting
	bool tx_seen;		// the radio has sent on the line
	uint32_t tx_ms;		// when it last did (same clock)
} shared_ptt_line_t;

enum { SHARED_PTT_UP, SHARED_PTT_LOW, SHARED_PTT_PRESSED, SHARED_PTT_NOISE };

// All zero is the state at power-on: disarmed
typedef struct {
	uint32_t ignored;	// scans with the pin low inside the quiet time, while the radio transmitted, or before the
				// gate armed (diagnostics)
	uint32_t low_ms;	// LOW: when the pin was first read low outside the quiet time; PRESSED: when it was reported
	uint32_t high_rx_ms;	// the line's rx_ms at the last scan that read the pin high (or its press's own bounce)
	uint32_t stuck_ms;	// when the pin was first read low unable to count as a press (stuck_timing)
	uint32_t clean_ms;	// since when the line has read high with nothing received (arms after SHARED_PTT_ARM_MS)
	uint32_t edge_ms;	// the last scan that found the pin changed
	uint32_t key_chars_ms;	// the last edge whose characters were taken as the key's own
	uint32_t lone_ms;	// characters on a high scan away from an edge: a low must follow within the bounce time
	uint32_t seen_rx_ms;	// the line's rx_ms as of the last scan (a new good byte moves it)
	uint16_t flagged;	// the line's flagged count (its low half) at the press, moved on while it settles
	uint16_t seen_flagged;	// the line's flagged count (its low half) as of the last scan
	uint8_t state;		// UP; LOW (held, not yet SHARED_PTT_HOLD_MS); PRESSED; NOISE (a held press ended by noise:
				// released until the pin reads high)
	uint8_t noise_releases;	// held presses ended by a flagged character, up to 255 (diagnostics)
	uint8_t disarms;	// traffic that disarmed the gate, up to 255 (diagnostics)
	bool high;		// the pin has read high since start-up
	bool high_rx_seen;	// the line's rx_seen at the last scan that read the pin high
	bool after_send;	// went low right after the radio's own serial send: no press until it reads high
	bool stuck_timing;	// stuck_ms is running
	bool started;		// the first scan has run (seen_* and was_low are valid)
	bool was_low;		// the last scan read the pin low
	bool seen_rx;		// the line's rx_seen as of the last scan
	bool armed;		// the line has read high and quiet for SHARED_PTT_ARM_MS since power-on or the last traffic
	bool key_chars;		// key_chars_ms is valid
	bool edge_key;		// the edge at edge_ms can be a key's: a press starting, or one ending
	bool edge_chars;	// the characters of the edge at edge_ms were taken as the key's own
	bool lone;		// lone_ms is running
} shared_ptt_t;

// pin_low: the PTT pin as read now
bool shared_ptt_pressed(shared_ptt_t *s, bool pin_low, const shared_ptt_line_t *line, uint32_t now_ms);
// The line has stayed low for SHARED_PTT_STUCK_MS without being able to count as a press, as of the last scan
bool shared_ptt_stuck(const shared_ptt_t *s, uint32_t now_ms);

#endif
