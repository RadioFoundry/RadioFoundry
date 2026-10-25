// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Broadcast FM receiver: a second receiver chip beside the channel receiver (e.g. an RDA5807M) that plays FM broadcast
// (wideband FM) when a channel or the VFO is tuned into its range (docs/spec/airband-and-broadcast.md 4).
//
// The board composes the chip driver, its bus and the audio routing behind this. All calls come from the radio task
// (core/radio/radio_bcast.h: a channel or the VFO in the receiver's range plays on it); tune() and seek() only start
// the operation, status() says when it is done.
#ifndef HAL_BCAST_H
#define HAL_BCAST_H

#include <stdint.h>
#include <stdbool.h>

#define BCAST_VOLUME_MAX	15	// volume() levels 0..15 (0 = silent)

typedef struct {
	uint32_t khz;		// the frequency the receiver is on now (follows a seek as it runs)
	bool tuning;		// a tune or a seek still runs
	bool seek_failed;	// the last seek went round the band without finding a station
	bool stereo;		// stereo pilot received
	bool station;		// the receiver judges this a station (its own threshold)
	uint8_t rssi;		// signal strength in the chip's units (RDA5807M: 0..127, about dBuV)
} bcast_status_t;

typedef struct {
	uint32_t lo_khz, hi_khz;	// the range the receiver tunes on this radio
	uint16_t grid_khz;		// frequencies are multiples of this (50 kHz: every broadcast grid)
	// Power the receiver up (tuned to the last frequency, muted) or down. False: the chip didn't answer
	bool (*power)(bool on);
	// Start tuning to khz (on the grid, in range). False: out of range or the chip didn't answer
	bool (*tune)(uint32_t khz);
	// Start a seek up or down from the current frequency; it stops at a station or after one round of the band
	bool (*seek)(bool up);
	// Receiver volume 0..BCAST_VOLUME_MAX
	void (*volume)(uint8_t level);
	// The receiver's state. False: not powered or the chip didn't answer
	bool (*status)(bcast_status_t *st);
	// Broadcast audio to the speaker (true) or muted (false). The core keeps hal_radio's audio() off while this is on;
	// a board whose two paths share a switch may still let the channel win
	void (*audio)(bool on);
} hal_bcast_t;

#endif
