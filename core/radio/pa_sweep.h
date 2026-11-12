// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The PA drive sweep (docs/spec/pa-keyup.md 7): on a keyed FM carrier the drive goes to 0 for off_ms, then to each
// value of a grid for on_ms, point after point, so one SDR capture holds a radio's output against drive and the PA
// loop's rise and fall at every drive. A timing marker, a small step of the carrier's frequency, comes mark_ms after
// each drive write and mark_ms before each drop, so the capture shows when the drive was written. The steps alternate
// up and down from point to point, so a missed point shows too
#ifndef RFOUNDRY_PA_SWEEP_H
#define RFOUNDRY_PA_SWEEP_H

#include <stdbool.h>
#include <stdint.h>

#define PA_SWEEP_CEILING_MAX_PCT	115u	// the highest drive: % of the board's top power level's drive here
#define PA_SWEEP_OVER_MAX		8u	// points above the top level's drive per sweep
#define PA_SWEEP_OVER_ON_MAX_MS		30u	// how long one of those may last
#define PA_SWEEP_OVER_OFF_MIN_MS	40u	// and the drive at 0 before it
#define PA_SWEEP_MS_MAX			1000u	// one sweep's length (it holds the debug link): a longer grid goes in several

typedef struct {
	uint16_t start;		// the first point's drive (the board's units)
	int16_t step;		// added per point
	uint8_t count;		// points
	uint8_t on_ms;		// each point's length (1..255)
	uint8_t off_ms;		// the drive at 0 before each point
	uint8_t ceiling_pct;	// the highest drive, % of the top level's (at most PA_SWEEP_CEILING_MAX_PCT)
	uint8_t mark_ms;	// the marker's distance from the drive write and the drop; 0: no marker
} pa_sweep_req_t;

typedef struct {
	uint16_t drive;
	uint8_t on_ms, off_ms;
} pa_sweep_pt_t;

// Point i of r after the limits: the ceiling (and max, the board's hardware limit), the length and the gap of a point
// over top, at most PA_SWEEP_OVER_MAX of those (*over counts them; start it at 0) and PA_SWEEP_MS_MAX in all (*ms
// counts it; start it at 0). False: the sweep ends before point i
bool pa_sweep_point(const pa_sweep_req_t *r, uint16_t top, uint16_t max, uint8_t i, uint8_t *over, uint32_t *ms,
		    pa_sweep_pt_t *pt);

// What a sweep runs on: the board's (hal_radio_t.tx_drive_now, tx_mark; the mark may be NULL) and a delay
typedef struct {
	bool (*drive)(uint16_t drive);
	bool (*mark)(int8_t step);
	void (*delay_ms)(uint32_t ms);
} pa_sweep_io_t;

// Runs r against top (the top level's drive at the transmission's frequency) and max; returns the points run. The
// drive is 0 and the marker centered at the end, also when the board refuses a write (the transmission ended)
uint8_t pa_sweep_run(const pa_sweep_req_t *r, uint16_t top, uint16_t max, const pa_sweep_io_t *io);

#endif
