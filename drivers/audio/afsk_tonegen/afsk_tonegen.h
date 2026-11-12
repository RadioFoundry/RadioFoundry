// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// APRS AFSK transmit through an RF chip's FM TX tone generator (hal_afsk.h, docs/spec/aprs.md 5.2), for a radio whose
// microphone goes to the RF chip rather than through a modem it could play audio into. The chip sends one tone in place
// of the microphone (as for the tone burst); a periodic interrupt at the baud rate retunes it at each symbol boundary
// with one register write. Chip and platform come from the board's hooks; this module only steps the symbols.
#ifndef AFSK_TONEGEN_H
#define AFSK_TONEGEN_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_afsk.h"

typedef struct {
	// The first symbol's tone at level (0-100, 50 nominal), task level. False unless the radio is transmitting FM.
	// From here to end the symbol interrupt owns the chip's bus: the board keeps its own reads off it
	bool (*begin)(uint16_t hz, uint8_t level);
	// Retune to hz, from the symbol interrupt
	void (*tone)(uint16_t hz);
	// The microphone back, task level
	void (*end)(void);
	// A periodic interrupt every period_us that calls tick (the platform's timer; the board acknowledges it), and its
	// stop
	void (*timer_start)(uint32_t period_us, void (*tick)(void));
	void (*timer_stop)(void);
} afsk_tonegen_board_t;

// Once, from the radio init; then board.afsk = &afsk_tonegen
void afsk_tonegen_attach(const afsk_tonegen_board_t *board);
extern const hal_afsk_t afsk_tonegen;

#endif
