// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Software CTCSS and DCS detection on a low-rate ADC stream, for radios whose RF chip has no sub-audio decoder (the
// MD-380: docs/spec/hardware-md380.md 6.4). No hardware, no RTOS and no blocking: feed one sample per call from a DMA
// interrupt or a task and read the verdict.
//
// CTCSS: three Goertzel filters (the tone and a guard bin 3 Hz either side) over overlapping 0.2 s windows, one result
// every 0.1 s. A tone is heard when it holds a good share of the window's AC power and beats both guard bins, which
// tells neighboring standard tones (2.3 Hz apart at the bottom of the list) apart. It takes two good results to open
// and three bad ones in a row to close, so a fade or the phase reversal at the end of a transmission (the "reverse
// burst") does not chop the audio.
//
// DCS: a slicer with peak-tracked threshold, a clock recovery loop locked to the edges of the 134.4 baud stream, and a
// 23-bit shift register compared to the code's Golay word (up to 2 bit errors). Two matches 23 bits apart open, none
// for three words closes.
#ifndef SUBAUDIO_RX_H
#define SUBAUDIO_RX_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { SUBAUDIO_NONE, SUBAUDIO_CTCSS, SUBAUDIO_DCS, SUBAUDIO_DCS_INV } subaudio_kind_t;

typedef struct {
	subaudio_kind_t kind;
	float fs;			// sample rate, Hz
	float min_amp;			// smallest tone amplitude (or DCS swing / 2) accepted, in sample counts
	bool invert;			// the signal path inverts the waveform (matters for DCS only)

	float dc;			// high-pass state for the tone path

	// CTCSS: two staggered windows, each with the tone bin and two guard bins
	uint16_t win_len;
	uint16_t delay_b;		// window B starts this many samples after A
	float coef[3];
	struct {
		float s1[3], s2[3], sumsq;
		uint16_t n;
	} win[2];
	uint8_t good, bad;
	bool tone_on;

	// DCS
	float hi, lo, bit_phase, bit_step;
	bool primed, last_bit, bit_sampled;
	uint32_t reg, target;
	uint16_t since_match;
	uint8_t words;
	bool dcs_on;
} subaudio_rx_t;

// Sample rate in Hz; min_amp 0 takes the default (8 counts of a 12-bit converter)
void subaudio_rx_init(subaudio_rx_t *d, uint32_t fs_hz, float min_amp);
// Detect kind at value: CTCSS in Hz x 10, DCS the code as written in octal (as hal_radio.h's subtone_t). Resets the
// state. invert: see the field. SUBAUDIO_NONE matches always
void subaudio_rx_set(subaudio_rx_t *d, subaudio_kind_t kind, uint16_t value, bool invert);
// One raw sample (any DC offset)
void subaudio_rx_feed(subaudio_rx_t *d, int16_t sample);
// The tone or code is present now
bool subaudio_rx_match(const subaudio_rx_t *d);

#endif
