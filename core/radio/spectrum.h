// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Sandro Kalatozishvili
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
	SPECTRUM_BANDWIDTH, SPECTRUM_DWELL, SPECTRUM_AUTO_PEAK, SPECTRUM_SCALE, SPECTRUM_FILTER };
enum { SPECTRUM_AUTO_OFF, SPECTRUM_AUTO_SWEEP, SPECTRUM_AUTO_IMMEDIATE, SPECTRUM_AUTO_COUNT };

// One narrowband RSSI reading per bin, not an FFT. The radio task owns this; other tasks read its view copy.
typedef struct {
	bool active, listening, audio_open;
	uint8_t auto_peak;	// SPECTRUM_AUTO_*
	bool narrow;
	uint8_t index, tuner, since_listen, bins, peak_slot;
	int8_t scale_db;
	uint16_t dwell_ms;
	int16_t squelch_dbm;
	uint32_t center_hz, step_hz, lo_hz, hi_hz, due_ms, release_ms, sweeps;
	uint32_t peak_ms;
	uint8_t level[SPECTRUM_BINS], peak[SPECTRUM_PEAK_BUCKETS][SPECTRUM_BINS]; // dBm + 160; 255 means unread
} spectrum_t;

_Static_assert(SPECTRUM_BINS <= UINT8_MAX && SPECTRUM_BINS % 2 == 1, "spectrum bin indices fit and have a center");

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

#endif
