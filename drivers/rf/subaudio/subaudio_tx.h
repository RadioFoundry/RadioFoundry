// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// CTCSS and DCS generators for radios that send the sub-audio as a PWM or DAC signal into the modulator (the MD-380:
// docs/spec/hardware-md380.md 6.4). One call per output sample from a timer interrupt; the result is a duty value
// that an analog low-pass filter turns into the tone. No hardware, no RTOS.
#ifndef SUBAUDIO_TX_H
#define SUBAUDIO_TX_H

#include <stdint.h>
#include <stdbool.h>
#include "subaudio_rx.h"	// subaudio_kind_t

typedef struct {
	uint32_t phase, step;		// CTCSS: sine phase. DCS: bit phase (a bit is 2^32)
	uint32_t word;			// DCS: the 23 bits to send, bit 0 first
	uint8_t bit;			// DCS: index of the bit being sent
	subaudio_kind_t kind;
	int32_t full, mid, peak;	// duty scale: 0..full, centered on mid, +-peak at the largest swing
	int32_t gain;			// start-up ramp, Q15, 0..32768
	int32_t gain_step;
	int32_t lp;			// DCS: smoothed level, Q15 of peak
	uint32_t rate;
	bool tail;			// DCS: the turn-off tail runs (subaudio_tx_tail)
} subaudio_tx_t;

// rate: samples per second the caller will take. full: duty value at 100 % (the PWM period minus one). percent: swing
// of the signal as a share of half the duty range, 1..100
void subaudio_tx_init(subaudio_tx_t *t, uint32_t rate, uint16_t full, uint8_t percent);
// Start the tone or code: kind and value as for subaudio_rx_set; invert flips a DCS waveform (a path that inverts). The
// output ramps up over 20 ms. NONE gives a steady middle duty
void subaudio_tx_start(subaudio_tx_t *t, subaudio_kind_t kind, uint16_t value, bool invert);
// The DCS turn-off tail: on sends a 134.4 Hz sine (the bit rate, a turn-off code receivers close on) in place of the
// code, at the same swing; off goes back to the code. False when the generator sends no DCS
bool subaudio_tx_tail(subaudio_tx_t *t, bool on);
// The next duty value, 0..full
uint16_t subaudio_tx_next(subaudio_tx_t *t);

#endif
