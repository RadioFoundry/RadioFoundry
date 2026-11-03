// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Step lines: inputs that change level once per step, one line per direction (a navigation ring or a trackball whose
// lines toggle instead of pulsing), independent of the board's pins. The board samples every line on each key scan
// and passes the levels in, one bit per line.
//
// A level counts once two scans in a row agree, so a bounce never counts. Each counted change, either edge, is one
// step. A step comes out as a press one scan long with at least one scan up after it, so the core sees a key press
// per step; steps that arrive faster wait, up to a backlog, and a fast roll beyond it is dropped rather than run on.
#ifndef STEPLINES_H
#define STEPLINES_H

#include <stdint.h>

#define STEPLINES_MAX		8
#define STEPLINES_BACKLOG	4	// steps kept per line while presses are paced out

typedef struct {
	uint8_t lines;			// lines in use, up to STEPLINES_MAX
	uint8_t last;			// the last sample
	uint8_t level;			// the last level counted
	uint8_t out;			// the presses reported by the last scan
	uint8_t pending[STEPLINES_MAX];	// steps not yet reported
} steplines_t;

// lines = how many; levels = the lines now, which count as at rest
void steplines_init(steplines_t *s, uint8_t lines, uint8_t levels);
// One key scan; returns the lines pressed for this scan, one bit per line
uint8_t steplines_scan(steplines_t *s, uint8_t levels);

#endif
