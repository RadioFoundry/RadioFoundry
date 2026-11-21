// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PTT on a pin shared with the serial line (shared_ptt.h)
#include "input/sharedptt/shared_ptt.h"

// Traffic on the line, seen at since_ms: no press until it has read high and quiet for the arm time from then
static void disarm(shared_ptt_t *s, uint32_t since_ms)
{
	if (s->armed && s->disarms != 0xFFu) {
		s->disarms++;
	}
	s->armed = false;
	s->clean_ms = since_ms;
	s->lone = false;
}

// Characters at the edge just seen: the key's own, unless another edge had some too, too recently for a key
static bool key_chars(shared_ptt_t *s, uint32_t now_ms)
{
	if (s->edge_chars) {
		return true;	// the same edge: its bounce and its break
	}
	if (s->key_chars && (uint32_t)(now_ms - s->key_chars_ms) < SHARED_PTT_STREAM_MS) {
		return false;
	}
	s->key_chars = true;
	s->key_chars_ms = now_ms;
	s->edge_chars = true;
	return true;
}

bool shared_ptt_pressed(shared_ptt_t *s, bool pin_low, const shared_ptt_line_t *line, uint32_t now_ms)
{
	// What arrived since the last scan. The first scan only takes the counts: what came before it is power-on. While
	// the radio transmits, flagged characters are its RF on the cable, not traffic; RF makes no good byte
	uint16_t flagged_now = (uint16_t)line->flagged;
	bool good = s->started && line->rx_seen && (!s->seen_rx || line->rx_ms != s->seen_rx_ms);
	bool chars = good || (s->started && flagged_now != s->seen_flagged && !line->keyed);
	bool edge = s->started && pin_low != s->was_low;
	if (!s->started) {
		s->started = true;
		s->clean_ms = now_ms;
		s->edge_ms = now_ms - SHARED_PTT_BOUNCE_MS - 1u;	// no edge yet
	}
	s->seen_flagged = flagged_now;
	s->seen_rx = line->rx_seen;
	s->seen_rx_ms = line->rx_ms;
	s->was_low = pin_low;
	if (edge) {
		// A key's edge: the pin going low with the radio not transmitting (under TX a low is its RF), or going high
		// at the end of a low that was, or was becoming, a press
		s->edge_ms = now_ms;
		s->edge_chars = false;
		s->edge_key = pin_low ? !line->keyed : (s->state == SHARED_PTT_LOW || s->state == SHARED_PTT_PRESSED);
	}
	bool data = line->rx_seen && (uint32_t)(now_ms - line->rx_ms) < SHARED_PTT_QUIET_MS;
	bool armed_before = s->armed;

	// Characters before this scan that no low followed: traffic, not a bounce
	bool own = false;	// characters here are the key's: they belong to the press starting now
	if (s->lone) {
		if (pin_low && s->edge_key && (uint32_t)(now_ms - s->lone_ms) <= SHARED_PTT_BOUNCE_MS) {
			s->lone = false;
			if (key_chars(s, now_ms)) {
				own = true;
			} else {
				disarm(s, now_ms);
			}
		} else if ((uint32_t)(now_ms - s->lone_ms) > SHARED_PTT_BOUNCE_MS) {
			disarm(s, s->lone_ms);
		}
	}
	// Characters this scan. On a held press, the press judges them below. Otherwise they are the key's own only right
	// at an edge, or, on a high scan, if a low follows at once
	if (chars && !(pin_low && s->state == SHARED_PTT_PRESSED)) {
		if (s->edge_key && (uint32_t)(now_ms - s->edge_ms) <= SHARED_PTT_BOUNCE_MS && key_chars(s, now_ms)) {
			own = own || pin_low;
		} else if (!pin_low && !s->lone &&
			   !(s->key_chars && (uint32_t)(now_ms - s->key_chars_ms) < SHARED_PTT_STREAM_MS)) {
			s->lone = true;	// one scan of them may still be a press's bounce: a low must follow at once
			s->lone_ms = now_ms;
		} else {
			disarm(s, now_ms);
		}
	}
	// A press's own bounce can leave a good byte that no scan saw with the pin high: it counts as seen high, so the
	// latch check below doesn't take the press for a line clamped at the end of traffic
	if (own && pin_low && armed_before && s->armed) {
		s->high_rx_seen = line->rx_seen;
		s->high_rx_ms = line->rx_ms;
	}

	if (!pin_low) {
		if (!s->armed && !s->lone && (uint32_t)(now_ms - s->clean_ms) >= SHARED_PTT_ARM_MS) {
			s->armed = true;
		}
		s->state = SHARED_PTT_UP;
		s->high = true;
		s->high_rx_seen = line->rx_seen;
		s->high_rx_ms = line->rx_ms;
		s->after_send = false;
		s->stuck_timing = false;
		return false;
	}

	// The latch (shared_ptt.h): judged on every low scan that isn't a press under way, armed or not, so the stuck
	// diagnostic sees a line that latched while the gate was disarmed too. While the radio transmits, the low is its RF
	bool idle = s->state == SHARED_PTT_UP || s->state == SHARED_PTT_NOISE;
	if (idle && line->tx_seen && (uint32_t)(now_ms - line->tx_ms) < SHARED_PTT_TX_GUARD_MS) {
		s->after_send = true;
	}
	bool high_since = s->high && (!line->rx_seen || (s->high_rx_seen && s->high_rx_ms == line->rx_ms));
	bool latch = !high_since || s->after_send;
	if (data) {
		s->stuck_timing = false;	// good bytes arrive: the line isn't clamped
	} else if (latch && idle && !line->keyed && !s->stuck_timing) {
		s->stuck_timing = true;
		s->stuck_ms = now_ms;
	}

	if (!s->armed) {
		s->clean_ms = now_ms;	// low from power-on, or after traffic: it must read high for the whole arm time
		s->ignored++;
		s->state = SHARED_PTT_UP;
		return false;
	}
	// While the radio transmits, only a press already held stays trusted. A low that starts then stays untrusted
	// until the pin reads high: else a cable that dropped the line during a transmission keyed by the link (a port
	// closed with a break) would press once the link's own time-out ended it
	if (line->keyed && s->state != SHARED_PTT_PRESSED) {
		s->ignored++;	// the radio's own transmission on the line
		s->state = SHARED_PTT_NOISE;
		return false;
	}
	if (s->state == SHARED_PTT_NOISE) {
		return false;
	}
	// A good byte means the line carries data, whatever the state: a held key would ground it. Before a press, one
	// byte may be its own bounce: wait out the quiet time. During one, the key is up and a host is talking
	if (data) {
		s->ignored++;
		if (s->state == SHARED_PTT_PRESSED) {
			s->state = SHARED_PTT_NOISE;
			disarm(s, now_ms);
		} else {
			s->state = SHARED_PTT_UP;
		}
		return false;
	}
	if (s->state == SHARED_PTT_PRESSED) {
		if (flagged_now == s->flagged) {
			return true;
		}
		if ((uint32_t)(now_ms - s->low_ms) < SHARED_PTT_SETTLE_MS) {
			s->flagged = flagged_now;	// the press's own break or bounce
			return true;
		}
		// A character on a line the key should hold low: the key is up, the line is picking up noise. The radio's
		// RF or a cable's break look the same, so the gate wants the line quiet again either way
		s->state = SHARED_PTT_NOISE;
		if (s->noise_releases != 0xFFu) {
			s->noise_releases++;
		}
		disarm(s, now_ms);
		return false;
	}
	if (s->state == SHARED_PTT_UP) {
		if (latch) {
			return false;	// stuck_ms runs from above
		}
		s->state = SHARED_PTT_LOW;
		s->low_ms = now_ms;
	}
	if ((uint32_t)(now_ms - s->low_ms) < SHARED_PTT_HOLD_MS) {
		return false;
	}
	s->state = SHARED_PTT_PRESSED;
	s->low_ms = now_ms;	// the settle time runs from the report, however late the scan
	s->flagged = flagged_now;
	return true;
}

bool shared_ptt_stuck(const shared_ptt_t *s, uint32_t now_ms)
{
	return s->stuck_timing && (uint32_t)(now_ms - s->stuck_ms) >= SHARED_PTT_STUCK_MS;
}
