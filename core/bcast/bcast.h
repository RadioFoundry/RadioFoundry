// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The broadcast FM receiver (CONFIG_BCAST_FM; docs/spec/airband-and-broadcast.md 4): listening on the board's second
// receiver (hal_bcast.h) while the channel receiver keeps monitoring the channel. Tune, step, seek, volume and six
// presets; the broadcast pauses (is muted) while the channel has traffic and comes back BCAST_RESUME_MS after it ends.
//
// No RTOS and no storage here: bcast_task.c runs it in the radio task and keeps bcast_saved_t in the settings store;
// the host tests and the UI simulator drive it directly against a fake receiver.
#ifndef BCAST_H
#define BCAST_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_bcast.h"
#include "radio/radio.h"

#define BCAST_PRESETS		6
#define BCAST_STEP_KHZ		100	// Left / Right on the screen
#define BCAST_DEFAULT_KHZ	100000	// first use: 100.0 MHz (inside every receiver's range)
#define BCAST_VOLUME_DEFAULT	8
#define BCAST_RESUME_MS		2000	// after channel traffic ends (a reply window counts as traffic)
#define BCAST_POLL_MS		200	// receiver status reads while idle (every call while a tune or seek runs)

typedef struct {
	bool present;		// the radio has a broadcast receiver (it may still fail to answer when switched on)
	bool on;		// listening: the user's choice
	bool paused;		// on, but muted for channel traffic
	bool tuning;		// a tune or seek runs
	bool seeking;
	bool seek_failed;	// the last seek found nothing
	bool failed;		// the receiver didn't answer when switched on
	uint32_t khz;		// the frequency (follows a seek as it runs)
	uint8_t volume;		// 0..BCAST_VOLUME_MAX
	bool stereo, station;
	uint8_t rssi;		// receiver units (RDA5807M: 0..127)
	uint32_t preset_khz[BCAST_PRESETS];	// 0 = empty
	int8_t preset;		// the preset holding khz, -1 = none
	uint32_t lo_khz, hi_khz, grid_khz;
} bcast_view_t;

// What bcast_task.c keeps in the settings store (settings_t bc_*)
typedef struct {
	uint32_t khz;		// 0 = never set
	uint8_t volume;		// > BCAST_VOLUME_MAX = the default
	bool on;		// listening when the radio was switched off
	uint32_t preset_khz[BCAST_PRESETS];
} bcast_saved_t;

typedef enum {
	BCMD_POWER,		// arg: 0 off, 1 on, 2 toggle
	BCMD_TUNE,		// arg: kHz (rounded to the grid)
	BCMD_STEP,		// arg: +1 / -1 (BCAST_STEP_KHZ, wraps round the range)
	BCMD_SEEK,		// arg: +1 up / -1 down
	BCMD_VOLUME,		// arg: 0..BCAST_VOLUME_MAX
	BCMD_VOLUME_STEP,	// arg: +1 / -1
	BCMD_PRESET,		// arg: 0..BCAST_PRESETS-1: tune to it
	BCMD_PRESET_SAVE,	// arg: 0..BCAST_PRESETS-1: the frequency into it
} bcast_cmd_t;

typedef struct {
	const hal_bcast_t *hw;	// NULL: no receiver
	bcast_view_t view;
	uint32_t serial;	// bumped on every visible change
	bool dirty;		// bcast_saved_t changed: the owner saves it (bcast_take_saved)
	int8_t audio_sent;	// last hw->audio(), -1 = none yet
	uint32_t quiet_ms;	// the channel has been quiet since (0 = busy now)
	uint32_t poll_ms;	// last status read
} bcast_t;

// hw may be NULL. saved: the stored state (NULL = defaults). Switches the receiver on if it was on at power off
void bcast_init(bcast_t *b, const hal_bcast_t *hw, const bcast_saved_t *saved, uint32_t now_ms);
// A user request; false when refused (no receiver, off, out of range, empty preset)
bool bcast_request(bcast_t *b, uint8_t cmd, int32_t arg, uint32_t now_ms);
// Every radio poll: follow tunes and seeks, apply the traffic policy. channel_busy = bcast_channel_busy(view);
// hotspot: the host link owns the radio (the broadcast is switched off)
void bcast_poll(bcast_t *b, bool channel_busy, bool hotspot, uint32_t now_ms);
// The channel has traffic the user should hear now: a reception (squelch open, monitor), a transmission or refused
// PTT, a DMR call we play or our own, the DMR reply window
bool bcast_channel_busy(const radio_view_t *v);
// The state to store when dirty is set (clears dirty)
void bcast_take_saved(bcast_t *b, bcast_saved_t *out);

#endif
