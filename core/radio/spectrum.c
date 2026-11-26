// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "config.h"
#if CONFIG_SPECTRUM
#include <string.h>
#include "spectrum.h"

bool spectrum_start(spectrum_t *s, uint32_t center, uint32_t lo, uint32_t hi, uint32_t step, unsigned bins, uint32_t now)
{
	uint64_t span = (uint64_t)step * (bins - 1);
	if ((bins != 65 && bins != 97 && bins != 129) || step < 1000 || step > 100000 || step % 10 ||
	    hi < lo || span > (uint64_t)hi - lo)
		return false;
	uint32_t half = (uint32_t)(span / 2);
	if (center < lo + half)
		center = lo + half;
	if (center > hi - half)
		center = hi - half;
	memset(s, 0, sizeof *s);
	s->active = s->listening = true;
	s->auto_peak = SPECTRUM_AUTO_SWEEP;
	s->dwell_ms = SPECTRUM_SETTLE_MS;
	s->bins = (uint8_t)bins;
	s->tuner = s->bins / 2;
	s->squelch_dbm = -80;
	s->center_hz = center;
	s->lo_hz = lo;
	s->hi_hz = hi;
	s->step_hz = step;
	s->due_ms = now + SPECTRUM_SETTLE_MS;
	s->release_ms = now;
	s->peak_ms = now;
	memset(s->level, SPECTRUM_EMPTY, sizeof s->level);
	memset(s->peak, SPECTRUM_EMPTY, sizeof s->peak);
	return true;
}

static bool restart(spectrum_t *s, uint32_t center, uint32_t lo, uint32_t hi, uint32_t step, unsigned bins, uint32_t now)
{
	int16_t squelch_dbm = s->squelch_dbm;
	uint16_t dwell_ms = s->dwell_ms;
	uint8_t auto_peak = s->auto_peak;
	bool narrow = s->narrow, center_tuner = s->center_tuner;
	int8_t scale_db = s->scale_db;
	if (!spectrum_start(s, center, lo, hi, step, bins, now))
		return false;
	s->squelch_dbm = squelch_dbm;
	s->dwell_ms = dwell_ms;
	s->auto_peak = auto_peak;
	s->narrow = narrow;
	s->center_tuner = center_tuner;
	s->scale_db = scale_db;
	return true;
}

bool spectrum_recenter(spectrum_t *s, uint32_t center, uint32_t lo, uint32_t hi, uint32_t now)
{
	return s->active && center >= lo && center <= hi && restart(s, center, lo, hi, s->step_hz, s->bins, now);
}

uint32_t spectrum_frequency(const spectrum_t *s, unsigned bin)
{
	return s->center_hz - s->step_hz * (s->bins / 2) + s->step_hz * bin;
}

uint32_t spectrum_receiver_hz(const spectrum_t *s)
{
	return spectrum_frequency(s, s->listening ? s->tuner : s->index);
}

int16_t spectrum_threshold_dbm(const spectrum_t *s)
{
	return s->squelch_dbm;
}

static uint32_t clamp_center(const spectrum_t *s, int64_t hz)
{
	uint32_t half = s->step_hz * (s->bins / 2u);
	return hz < s->lo_hz + half ? s->lo_hz + half : hz > s->hi_hz - half ? s->hi_hz - half : (uint32_t)hz;
}

static void move_window(spectrum_t *s, uint32_t center, uint8_t tuner)
{
	int64_t delta = (int64_t)center - s->center_hz;
	int64_t bins = delta / s->step_hz;
	unsigned moved = (unsigned)(bins < 0 ? -bins : bins);
	// Keep each reading attached to its frequency; a newly exposed bin has no sample yet.
	for (unsigned row = 0; row <= SPECTRUM_PEAK_BUCKETS; row++) {
		uint8_t *levels = row == 0 ? s->level : s->peak[row - 1];
		if (delta % s->step_hz || moved >= s->bins) {
			memset(levels, SPECTRUM_EMPTY, s->bins);
		} else if (bins > 0) {
			memmove(levels, levels + moved, s->bins - moved);
			memset(levels + s->bins - moved, SPECTRUM_EMPTY, moved);
		} else if (bins < 0) {
			memmove(levels + moved, levels, s->bins - moved);
			memset(levels, SPECTRUM_EMPTY, moved);
		}
	}
	s->center_hz = center;
	s->tuner = tuner;
	s->index = s->since_listen = 0;
}

static unsigned strongest_current(const spectrum_t *s)
{
	unsigned best = SPECTRUM_EMPTY;
	for (unsigned i = 0; i < s->bins; i++)
		if (s->level[i] != SPECTRUM_EMPTY && (best == SPECTRUM_EMPTY || s->level[i] > s->level[best]))
			best = i;
	return best;
}

bool spectrum_sample(spectrum_t *s, int16_t dbm, uint32_t now)
{
	if (!s->active || (int32_t)(now - s->due_ms) < 0)
		return false;
	unsigned elapsed = (now - s->peak_ms) / SPECTRUM_PEAK_BUCKET_MS;
	if (elapsed) {
		// Rotate by wall time, including while listening: an old carrier must age out without another sweep.
		if (elapsed >= SPECTRUM_PEAK_BUCKETS)
			memset(s->peak, SPECTRUM_EMPTY, sizeof s->peak);
		else for (unsigned i = 1; i <= elapsed; i++)
			memset(s->peak[(s->peak_slot + i) % SPECTRUM_PEAK_BUCKETS], SPECTRUM_EMPTY, sizeof s->peak[0]);
		s->peak_slot = (uint8_t)((s->peak_slot + elapsed) % SPECTRUM_PEAK_BUCKETS);
		s->peak_ms += elapsed * SPECTRUM_PEAK_BUCKET_MS;
	}
	if (dbm < -160) dbm = -160;
	if (dbm > 0) dbm = 0;
	uint8_t level = (uint8_t)(dbm + 160);
	unsigned bin = s->listening ? s->tuner : s->index;
	s->level[bin] = level;
	uint8_t *peak = &s->peak[s->peak_slot][bin];
	if (*peak == SPECTRUM_EMPTY || level > *peak)
		*peak = level;
	if (s->listening) {
		// A single receiver checks the selected frequency between short batches of bins.
		// Once it hears a carrier it stays there, without chopping the received audio.
		int threshold = spectrum_threshold_dbm(s) - (s->audio_open ? 3 : 0);
		s->audio_open = dbm >= threshold;
		if (s->audio_open)
			s->release_ms = now + SPECTRUM_RELEASE_MS;
		else if ((int32_t)(now - s->release_ms) >= 0) {
			// A captured peak borrows the tuner; check the fixed center before resuming the sweep.
			if (s->center_tuner && s->tuner != s->bins / 2u)
				s->tuner = s->bins / 2u;
			else
				s->listening = false;
			s->since_listen = 0;
		}
	} else {
		if (s->auto_peak == SPECTRUM_AUTO_IMMEDIATE && dbm >= spectrum_threshold_dbm(s)) {
			// This bin is already tuned and settled; keep its audio without a second retune.
			s->tuner = s->index;
			s->listening = s->audio_open = true;
			s->release_ms = now + SPECTRUM_RELEASE_MS;
		}
		if (++s->index == s->bins) {
			s->index = 0;
			s->sweeps++;
			s->since_listen = 0;
			if (s->auto_peak == SPECTRUM_AUTO_SWEEP) {
				// Pick the strongest reading of this sweep, not an old peak from the display's history.
				unsigned best = strongest_current(s);
				if (best != SPECTRUM_EMPTY && spectrum_level_dbm(s->level[best]) >= spectrum_threshold_dbm(s)) {
					s->tuner = (uint8_t)best;
					s->listening = true; // confirm at the tuned frequency before opening audio
				}
			} else {
				s->listening = true;
			}
		}
		if (s->auto_peak != SPECTRUM_AUTO_SWEEP && ++s->since_listen >= SPECTRUM_LISTEN_BINS)
			s->listening = true;
		if (s->listening && !s->audio_open)
			s->release_ms = now;
	}
	s->due_ms = now + (s->listening ? SPECTRUM_SETTLE_MS : s->dwell_ms);
	return true;
}

bool spectrum_adjust(spectrum_t *s, uint8_t cmd, int32_t value, uint32_t now)
{
	if (!s->active)
		return false;
	if (cmd == SPECTRUM_CENTER_TUNER) {
		if (value != 0 && value != 1)
			return false;
		s->center_tuner = value != 0;
		if (!s->center_tuner || s->tuner == s->bins / 2u)
			return true;
		uint32_t tuner = spectrum_frequency(s, s->tuner), center = clamp_center(s, tuner);
		move_window(s, center, s->bins / 2u);
		if (center == tuner && s->listening)
			return true;
	}
	if (cmd == SPECTRUM_FILTER) {
		if (value != 0 && value != 1)
			return false;
		uint8_t tuner = s->tuner;
		if (!restart(s, s->center_hz, s->lo_hz, s->hi_hz, s->step_hz, s->bins, now))
			return false;
		s->tuner = tuner;
		s->narrow = value != 0;
		return true;
	}
	if (cmd == SPECTRUM_DWELL) {
		if (value < (int32_t)SPECTRUM_SETTLE_MS || value > (int32_t)SPECTRUM_DWELL_MAX_MS)
			return false;
		s->dwell_ms = (uint16_t)value;
		s->due_ms = now + (s->listening ? SPECTRUM_SETTLE_MS : s->dwell_ms);
		return true;
	}
	if (cmd == SPECTRUM_AUTO_PEAK) {
		if (value < 0 || value >= SPECTRUM_AUTO_COUNT)
			return false;
		s->auto_peak = (uint8_t)value;
		return true;
	}
	if (cmd == SPECTRUM_SCALE) {
		if (value < -40 || value > 40 || value % 20)
			return false;
		s->scale_db = (int8_t)value;
		return true;
	}
	if (cmd == SPECTRUM_TUNER) {
		if (value == 0)
			return false;
		if (s->center_tuner) {
			uint32_t center = clamp_center(s, (int64_t)s->center_hz + (int64_t)value * s->step_hz);
			if (center == s->center_hz && s->tuner == s->bins / 2u)
				return false;
			move_window(s, center, s->bins / 2u);
		} else {
			int dir = value < 0 ? -1 : 1;
			int bin = s->tuner + dir;
			if (bin < 0 || bin >= s->bins) {
				uint32_t center = clamp_center(s, (int64_t)s->center_hz + (int64_t)dir * s->step_hz);
				if (center == s->center_hz)
					return false;
				move_window(s, center, s->tuner);
			} else {
				s->tuner = (uint8_t)bin;
			}
		}
	} else if (cmd == SPECTRUM_SQUELCH) {
		if (value < -160 || value > 0)
			return false;
		s->squelch_dbm = (int16_t)value;
	} else if (cmd == SPECTRUM_CENTER || cmd == SPECTRUM_STEP || cmd == SPECTRUM_BANDWIDTH) {
		uint32_t center = cmd == SPECTRUM_CENTER ? (uint32_t)value : s->center_hz;
		uint32_t step = cmd == SPECTRUM_STEP ? (uint32_t)value : s->step_hz;
		unsigned bins = cmd == SPECTRUM_BANDWIDTH ? (unsigned)value : s->bins;
		if (center < s->lo_hz || center > s->hi_hz)
			return false;
		uint32_t tuner = spectrum_frequency(s, s->tuner);
		if (!restart(s, center, s->lo_hz, s->hi_hz, step, bins, now))
			return false;
		if (cmd != SPECTRUM_CENTER && !s->center_tuner) {
			int64_t bin = ((int64_t)tuner - spectrum_frequency(s, 0) + step / 2) / step;
			s->tuner = bin < 0 ? 0 : bin >= s->bins ? s->bins - 1 : (uint8_t)bin;
		}
		return true;
	} else if (cmd != SPECTRUM_CENTER_TUNER) {
		return false;
	}
	s->listening = true;
	s->audio_open = false;
	s->release_ms = now;
	s->due_ms = now + SPECTRUM_SETTLE_MS;
	return true;
}

uint8_t spectrum_peak_level(const spectrum_t *s, unsigned bin)
{
	if (bin >= s->bins)
		return SPECTRUM_EMPTY;
	uint8_t best = SPECTRUM_EMPTY;
	for (unsigned i = 0; i < SPECTRUM_PEAK_BUCKETS; i++) {
		uint8_t level = s->peak[i][bin];
		if (level != SPECTRUM_EMPTY && (best == SPECTRUM_EMPTY || level > best))
			best = level;
	}
	return best;
}

unsigned spectrum_strongest(const spectrum_t *s)
{
	unsigned best = 0;
	uint8_t level = spectrum_peak_level(s, 0);
	for (unsigned i = 1; i < s->bins; i++) {
		uint8_t next = spectrum_peak_level(s, i);
		if (next != SPECTRUM_EMPTY && (level == SPECTRUM_EMPTY || next > level)) {
			best = i;
			level = next;
		}
	}
	return best;
}
#endif
