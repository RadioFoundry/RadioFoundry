// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The scan engine (scan.h)
#include <string.h>
#include "radio/scan.h"
#include "config.h"

#if CONFIG_SCAN

static void phase(scan_t *s, uint8_t p, uint32_t now_ms)
{
	s->phase = p;
	s->phase_ms = now_ms;
}

void scan_begin(scan_t *s, uint8_t kind, const scan_cfg_t *cfg, uint32_t now_ms)
{
	uint16_t origin_pos = s->origin_pos;
	uint32_t origin_hz = s->origin_hz, lo = s->lo_hz, hi = s->hi_hz;
	bool origin_vfo = s->origin_vfo;
	memset(s, 0, sizeof(*s));
	// The caller sets the origin and the VFO range before scan_begin
	s->origin_pos = origin_pos;
	s->origin_hz = origin_hz;
	s->origin_vfo = origin_vfo;
	s->lo_hz = lo;
	s->hi_hz = hi;
	s->sweep_pos = origin_pos;
	s->kind = kind;
	s->cfg = *cfg;
	s->tuned_ms = now_ms;
	phase(s, SCAN_PH_LISTEN, now_ms);
}

void scan_end(scan_t *s)
{
	s->kind = SCAN_OFF;
	s->phase = SCAN_PH_LISTEN;
	s->skipped = 0;
	memset(s->skip, 0, sizeof(s->skip));
	memset(s->skip_hz, 0, sizeof(s->skip_hz));
}

void scan_tuned(scan_t *s, uint32_t now_ms)
{
	s->tuned_ms = now_ms;
	s->heard = false;
	s->synced = false;
	s->steps++;
	phase(s, SCAN_PH_LISTEN, now_ms);
}

void scan_pause(scan_t *s, bool paused, uint32_t now_ms)
{
	if (paused && s->phase != SCAN_PH_PAUSED) {
		phase(s, SCAN_PH_PAUSED, now_ms);
	} else if (!paused && s->phase == SCAN_PH_PAUSED) {
		s->tuned_ms = now_ms;
		s->heard = false;
		s->synced = false;
		phase(s, SCAN_PH_LISTEN, now_ms);
	}
}

void scan_talked(scan_t *s, uint32_t now_ms)
{
	s->tuned_ms = now_ms;	// the settle again: the receiver comes back from transmitting
	s->heard = false;
	s->synced = false;
	phase(s, SCAN_PH_RESUME, now_ms);
}

static scan_act_t hold(scan_t *s, uint32_t now_ms)
{
	s->holds++;
	phase(s, SCAN_PH_HOLD, now_ms);
	return s->cfg.resume == SCAN_RESUME_STOP ? SCAN_ACT_STOP : SCAN_ACT_NONE;
}

scan_act_t scan_poll(scan_t *s, uint32_t now_ms, bool active, bool carrier, bool sync)
{
	if (s->kind == SCAN_OFF || s->phase == SCAN_PH_PAUSED) {
		return SCAN_ACT_NONE;
	}
	uint32_t since_tune = now_ms - s->tuned_ms;
	uint32_t in_phase = now_ms - s->phase_ms;
	if (since_tune < SCAN_SETTLE_MS) {
		return SCAN_ACT_NONE;
	}
	if (carrier || sync) {
		s->heard_ms = now_ms;
		s->heard = true;
	}
	if (sync && !s->synced) {
		s->synced = true;
		s->stats_synced++;
	}
	// A carrier now, or (carrier_hold_ms) a moment ago: a TDMA signal is off the air half the time
	bool heard = s->heard && now_ms - s->heard_ms <= s->cfg.carrier_hold_ms;
	switch (s->phase) {
	case SCAN_PH_LISTEN:
		if (active) {
			return hold(s, now_ms);
		}
		if (since_tune >= s->cfg.dwell_ms) {
			if (heard && s->cfg.probe_ms != 0) {
				s->probes++;
				phase(s, SCAN_PH_PROBE, now_ms);
				return SCAN_ACT_NONE;
			}
			return SCAN_ACT_NEXT;
		}
		return SCAN_ACT_NONE;
	case SCAN_PH_PROBE: {
		if (active) {
			return hold(s, now_ms);
		}
		// Sync: a call of the channel's kind is on, give its late entry the time it needs
		uint32_t limit = s->synced && s->cfg.sync_ms > s->cfg.probe_ms ? s->cfg.sync_ms : s->cfg.probe_ms;
		return (!heard || in_phase >= limit) ? SCAN_ACT_NEXT : SCAN_ACT_NONE;
	}
	case SCAN_PH_HOLD:
		if (!active) {
			phase(s, SCAN_PH_RESUME, now_ms);
			return SCAN_ACT_NONE;
		}
		if (s->cfg.resume == SCAN_RESUME_TIME && s->cfg.hold_ms != 0 && in_phase >= s->cfg.hold_ms) {
			return SCAN_ACT_NEXT;
		}
		return SCAN_ACT_NONE;
	case SCAN_PH_RESUME:
		if (active) {
			phase(s, SCAN_PH_HOLD, now_ms);	// the same signal again (a pause between overs)
			return SCAN_ACT_NONE;
		}
		return in_phase >= s->cfg.delay_ms ? SCAN_ACT_NEXT : SCAN_ACT_NONE;
	default:
		return SCAN_ACT_NONE;
	}
}

bool scan_pos_skipped(const scan_t *s, uint16_t pos)
{
	return pos < CODEPLUG_ZONE_MAX && (s->skip[pos / 8] & (1u << (pos % 8))) != 0;
}

bool scan_nuisance_pos(scan_t *s, uint16_t pos)
{
	if (pos >= CODEPLUG_ZONE_MAX || scan_pos_skipped(s, pos)) {
		return false;
	}
	s->skip[pos / 8] |= (uint8_t)(1u << (pos % 8));
	s->skipped++;
	return true;
}

bool scan_nuisance_hz(scan_t *s, uint32_t hz)
{
	for (uint8_t i = 0; i < SCAN_VFO_NUISANCE_MAX; i++) {
		if (s->skip_hz[i] == hz) {
			return false;
		}
		if (s->skip_hz[i] == 0) {
			s->skip_hz[i] = hz;
			s->skipped++;
			return true;
		}
	}
	return false;
}

static bool pos_ok(const scan_t *s, uint16_t pos, scan_usable_fn usable, void *ctx)
{
	return !scan_pos_skipped(s, pos) && usable(pos, ctx);
}

int scan_next_pos(scan_t *s, uint16_t count, scan_usable_fn usable, void *ctx)
{
	if (count == 0) {
		return -1;
	}
	bool origin_ok = s->origin_pos < count && pos_ok(s, s->origin_pos, usable, ctx);
	if (s->cfg.priority && origin_ok && s->since_priority >= SCAN_PRIORITY_EVERY) {
		s->since_priority = 0;
		return s->origin_pos;
	}
	for (uint16_t i = 1; i <= count; i++) {
		uint16_t p = (uint16_t)((s->sweep_pos + i) % count);
		if (s->cfg.priority && p == s->origin_pos && count > 1) {
			continue;	// visited by the priority revisits
		}
		if (pos_ok(s, p, usable, ctx)) {
			s->sweep_pos = p;
			if (s->cfg.priority) {
				s->since_priority++;
			}
			return p;
		}
	}
	if (origin_ok) {
		s->since_priority = 0;
		return s->origin_pos;	// priority on and the start channel is the only one left
	}
	return -1;
}

static bool hz_skipped(const scan_t *s, uint32_t hz, uint32_t step)
{
	for (uint8_t i = 0; i < SCAN_VFO_NUISANCE_MAX && s->skip_hz[i] != 0; i++) {
		uint32_t d = hz > s->skip_hz[i] ? hz - s->skip_hz[i] : s->skip_hz[i] - hz;
		if (d < step / 2 + 1) {
			return true;
		}
	}
	return false;
}

uint32_t scan_next_hz(const scan_t *s, uint32_t hz, uint32_t step)
{
	if (step == 0 || s->hi_hz < s->lo_hz) {
		return 0;
	}
	uint32_t n = (s->hi_hz - s->lo_hz) / step + 1;
	for (uint32_t i = 0; i < n; i++) {
		if (hz < s->lo_hz || hz > s->hi_hz || s->hi_hz - hz < step) {
			hz = s->lo_hz;
		} else {
			hz += step;
		}
		if (!hz_skipped(s, hz, step)) {
			return hz;
		}
	}
	return 0;
}

#endif
