// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Input: the board reports which physical keys are down and how far the encoder moved; core/input turns that
// into logical actions with gestures through the board's key map (docs/ux/information-architecture.md section 2)
#ifndef HAL_INPUT_H
#define HAL_INPUT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
	ACT_NONE = 0,
	ACT_NAV_UP, ACT_NAV_DOWN, ACT_NAV_LEFT, ACT_NAV_RIGHT,
	ACT_SELECT, ACT_BACK, ACT_QUICK, ACT_CONTEXT,
	ACT_DIGIT_0, ACT_DIGIT_1, ACT_DIGIT_2, ACT_DIGIT_3, ACT_DIGIT_4,
	ACT_DIGIT_5, ACT_DIGIT_6, ACT_DIGIT_7, ACT_DIGIT_8, ACT_DIGIT_9,
	ACT_PTT, ACT_MONITOR, ACT_TALK_TARGET, ACT_EMERGENCY,
	ACT_ENC_STEP,		// value = +1 / -1 per detent
	ACT_USER_1, ACT_USER_2, ACT_USER_3, ACT_USER_4,
	// What a reassigned side key does (core/input/keyassign.h): the same on a press and a long press, on any screen
	ACT_POWER, ACT_ZONE_UP, ACT_ZONE_DOWN, ACT_SCAN,
	ACT_LOCK, ACT_CALL_BACK, ACT_HOME_ZONE,
	ACT_COUNT
} input_action_t;

typedef enum {
	GESTURE_PRESS,		// short, on release before the long-press threshold
	GESTURE_LONG,		// once, at the threshold
	GESTURE_REPEAT,		// navigation keys only, while held after LONG
	GESTURE_DOWN,		// raw edges, for PTT and MONITOR (and ENC_STEP uses DOWN)
	GESTURE_UP,
} input_gesture_t;

typedef struct {
	uint8_t action;		// input_action_t
	uint8_t gesture;	// input_gesture_t
	int8_t value;		// encoder steps
} input_event_t;

// One physical key: its bit in the scan mask and its short / long logical actions.
// raw_edges: report DOWN / UP instead of PRESS / LONG (PTT, monitor)
typedef struct {
	uint8_t bit;
	uint8_t short_action;
	uint8_t long_action;
	bool raw_edges;
	bool repeat;
} keymap_entry_t;

// The encoder decoder's own counts, for the debug link (core/input, input_encoder_stats)
typedef struct {
	int32_t detents;	// detents decoded since start (before the UI takes them)
	uint32_t transitions;	// valid quadrature steps seen (2 or 4 per detent, depending on the encoder)
	uint32_t illegal;	// samples with both lines changed (a missed sample; ignored)
	uint32_t reversals;	// steps opposite to the one before (contact bounce, or a turn back)
	uint32_t steps_per_detent;
} hal_encoder_stats_t;

typedef struct {
	void (*init)(void);
	// Bit mask of physical keys held now (bits defined by the board, matching its key map)
	uint32_t (*scan)(void);
	// Encoder detents since the last call (0 if the board has no encoder). Positive = "forward": the next channel on
	// Home, down a list (core/ui). What doesn't fit in the int8_t stays for the next call
	int8_t (*encoder)(void);
	// The decoder's counts (NULL: none)
	void (*encoder_stats)(hal_encoder_stats_t *st);
} hal_input_t;

#endif
