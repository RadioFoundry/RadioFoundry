// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Vibration alerts (vibe.h)
#include <stddef.h>
#include "radio/vibe.h"
#include "config.h"

#if CONFIG_VIBRATE

// Told apart by feel: short pairs for a message, one long pulse for a call (as the stock firmware does on an incoming
// call), a long repeated buzz for an emergency
static const vibe_pulse_t p_message[] = { { 150, 150 }, { 150, 0 } };
static const vibe_pulse_t p_call[] = { { 500, 0 } };
static const vibe_pulse_t p_emergency[] = { { 400, 200 }, { 400, 200 }, { 400, 200 }, { 400, 0 } };

#define PAT(p)	{ p, sizeof(p) / sizeof(p[0]) }
static const struct {
	const vibe_pulse_t *pulses;
	uint8_t count;
} patterns[VIBE_COUNT] = {
	[VIBE_MESSAGE] = PAT(p_message),
	[VIBE_CALL] = PAT(p_call),
	[VIBE_EMERGENCY] = PAT(p_emergency),
};

static uint8_t cur;		// the pattern running, VIBE_NONE when idle
static uint8_t next;		// the pulse due next
static bool started;		// the first pulse went (due_ms is set)
static uint32_t due_ms;		// when the next pulse starts
static uint32_t played;

uint8_t vibe_pattern(vibe_t v, const vibe_pulse_t **pulses)
{
	if (v <= VIBE_NONE || v >= VIBE_COUNT) {
		*pulses = NULL;
		return 0;
	}
	*pulses = patterns[v].pulses;
	return patterns[v].count;
}

void vibe_play(vibe_t v)
{
	if (v <= VIBE_NONE || v >= VIBE_COUNT) {
		return;
	}
	cur = (uint8_t)v;
	next = 0;
	started = false;
}

void vibe_service(uint32_t now_ms, void (*motor)(uint16_t ms))
{
	if (cur == VIBE_NONE) {
		return;
	}
	if (motor == NULL) {
		cur = VIBE_NONE;
		return;
	}
	if (started && (int32_t)(now_ms - due_ms) < 0) {
		return;
	}
	if (!started) {
		played++;
	}
	// The board's motor stops itself after on_ms; the rest is timed from this pulse's start
	const vibe_pulse_t *p = &patterns[cur].pulses[next];
	motor(p->on_ms);
	started = true;
	due_ms = now_ms + p->on_ms + p->off_ms;
	if (++next >= patterns[cur].count) {
		cur = VIBE_NONE;
	}
}

bool vibe_busy(void)
{
	return cur != VIBE_NONE;
}

uint32_t vibe_played(void)
{
	return played;
}

void vibe_reset(void)
{
	cur = VIBE_NONE;
	next = 0;
	started = false;
	played = 0;
}

#endif
