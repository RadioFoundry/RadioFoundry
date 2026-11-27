// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RFOUNDRY_SPECTRUM_H
#define RFOUNDRY_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#define SPECTRUM_BINS 129
#define SPECTRUM_EMPTY UINT8_MAX
#define SPECTRUM_SETTLE_MS 10u
#define SPECTRUM_DWELL_MAX_MS 1000u
#define SPECTRUM_LISTEN_BINS 8u
#define SPECTRUM_RELEASE_MS 300u
#define SPECTRUM_PEAK_BUCKETS 5u
#define SPECTRUM_PEAK_BUCKET_MS 1000u

enum { SPECTRUM_STOP, SPECTRUM_START, SPECTRUM_TUNER, SPECTRUM_CENTER, SPECTRUM_STEP, SPECTRUM_SQUELCH,
	SPECTRUM_BANDWIDTH, SPECTRUM_DWELL, SPECTRUM_AUTO_PEAK, SPECTRUM_SCALE, SPECTRUM_FILTER, SPECTRUM_CENTER_TUNER,
	SPECTRUM_HASH_KEY, SPECTRUM_STAR_KEY };
enum { SPECTRUM_AUTO_OFF, SPECTRUM_AUTO_SWEEP, SPECTRUM_AUTO_IMMEDIATE, SPECTRUM_AUTO_COUNT };
// Stored key action IDs; keep their order when adding UI options.
enum { SPECTRUM_KEY_BANDWIDTH, SPECTRUM_KEY_AUTO_PEAK, SPECTRUM_KEY_FILTER, SPECTRUM_KEY_CENTER_TUNER,
	SPECTRUM_KEY_COUNT };

// One narrowband RSSI reading per bin, not an FFT. Owned only by the radio task.
typedef struct {
	bool active, listening, audio_open;
	uint8_t auto_peak;	// SPECTRUM_AUTO_*
	bool narrow, center_tuner;
	uint8_t index, tuner, since_listen, bins, peak_slot;
	int8_t scale_db;
	uint16_t dwell_ms;
	int16_t squelch_dbm;
	uint32_t center_hz, step_hz, lo_hz, hi_hz, due_ms, release_ms, sweeps;
	uint32_t peak_ms;
	uint8_t level[SPECTRUM_BINS], peak[SPECTRUM_PEAK_BUCKETS][SPECTRUM_BINS]; // dBm + 160; 255 means unread
	uint8_t hash_key, star_key;	// use the former tail padding; SPECTRUM_KEY_*
} spectrum_t;

// Read-only snapshot: keep the five history buckets in the radio task, not in every UI/host view copy.
typedef struct {
	bool active, listening, audio_open;
	uint8_t auto_peak;
	bool narrow, center_tuner;
	uint8_t index, tuner, bins;
	int8_t scale_db;
	uint16_t dwell_ms;
	int16_t squelch_dbm;
	uint32_t center_hz, step_hz, lo_hz, hi_hz, sweeps;
	uint8_t level[SPECTRUM_BINS], peak[SPECTRUM_BINS];
	uint8_t hash_key, star_key;
} spectrum_view_t;

_Static_assert(SPECTRUM_BINS <= UINT8_MAX && SPECTRUM_BINS % 2 == 1, "spectrum bin indices fit and have a center");
_Static_assert(SPECTRUM_KEY_COUNT == 4, "settings key actions occupy two bits each");

static inline int16_t spectrum_level_dbm(uint8_t level)
{
	return level == SPECTRUM_EMPTY ? INT16_MIN : (int16_t)((int)level - 160);
}

bool spectrum_start(spectrum_t *s, uint32_t center, uint32_t lo, uint32_t hi, uint32_t step, unsigned bins, uint32_t now);
bool spectrum_recenter(spectrum_t *s, uint32_t center, uint32_t lo, uint32_t hi, uint32_t now);
uint32_t spectrum_frequency(const spectrum_t *s, unsigned bin);
uint32_t spectrum_receiver_hz(const spectrum_t *s);
int16_t spectrum_threshold_dbm(const spectrum_t *s);
bool spectrum_sample(spectrum_t *s, int16_t dbm, uint32_t now);
bool spectrum_adjust(spectrum_t *s, uint8_t cmd, int32_t value, uint32_t now);
unsigned spectrum_strongest(const spectrum_t *s);
uint8_t spectrum_peak_level(const spectrum_t *s, unsigned bin);

void spectrum_read(const spectrum_t *s, spectrum_view_t *v);
uint32_t spectrum_view_frequency(const spectrum_view_t *v, unsigned bin);
unsigned spectrum_view_strongest(const spectrum_view_t *v);
static inline uint8_t spectrum_view_peak_level(const spectrum_view_t *v, unsigned bin)
{
	return bin < v->bins ? v->peak[bin] : SPECTRUM_EMPTY;
}

#endif
