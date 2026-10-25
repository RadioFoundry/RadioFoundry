// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Vibration alerts (docs/spec/radio-features.md 4.1): short pulse patterns on the board's vibration motor
// (hal_power_t.vibrate), for the alerts the UI already shows. The board's vibrate(ms) runs the motor for one pulse and
// returns at once; this module times the pulses of a pattern from the radio task's poll, so nothing waits.
//
// Threading: vibe_play and vibe_service in the radio task only.
#ifndef RADIO_VIBE_H
#define RADIO_VIBE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
	VIBE_NONE,
	VIBE_MESSAGE,		// a text message arrived: two short pulses
	VIBE_CALL,		// a call alert or a private call to us: one long pulse
	VIBE_EMERGENCY,		// someone's emergency heard: four long pulses
	VIBE_COUNT
} vibe_t;

typedef struct {
	uint16_t on_ms;		// the motor runs this long
	uint16_t off_ms;	// then rests this long before the next pulse
} vibe_pulse_t;

#define VIBE_PULSES_MAX	4

// Start a pattern (VIBE_NONE or out of range: nothing). Replaces a pattern still running; the first pulse goes at the
// next vibe_service
void vibe_play(vibe_t v);
// Every radio poll: start each pulse when it is due. motor NULL (the radio has no motor): the pattern is dropped
void vibe_service(uint32_t now_ms, void (*motor)(uint16_t ms));
// A pattern is pending or between its pulses
bool vibe_busy(void);
// The pattern's pulses (for tests and docs); count returned
uint8_t vibe_pattern(vibe_t v, const vibe_pulse_t **pulses);
// Patterns started on a motor (for the debug link view)
uint32_t vibe_played(void);
void vibe_reset(void);

#endif
