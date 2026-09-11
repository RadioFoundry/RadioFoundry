// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Bell 202 AFSK, 1200 baud, mark 1200 Hz / space 2200 Hz (docs/spec/aprs.md 4), on PCM samples:
//   - the modulator turns a tone sequence (hdlc.h) into phase continuous audio at any sample rate; boards that send
//     PCM to their modulator use it, and the host tests use it to make test signals and WAV files
//   - the demodulator takes received FM audio (a board's hal_afsk_t rx samples, 8 kHz or more) and decides mark or
//     space per bit: two sliding one-bit correlators (quadrature, so the phase doesn't matter) whose magnitudes are
//     each normalized by their own slowly decaying peak (this undoes the tilt pre-emphasis / de-emphasis mismatch
//     puts between the tones), the difference sampled by a digital PLL that locks to the data transitions
// Floating point (single precision; the STM32F4 has the FPU). No RTOS, no board.
#ifndef ODR_AFSK_H
#define ODR_AFSK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "aprs/hdlc.h"

#define AFSK_BAUD		1200u
#define AFSK_MARK_HZ		1200u
#define AFSK_SPACE_HZ		2200u
#define AFSK_WIN_MAX		48u	// correlator length limit: one bit at up to 57.6 kHz

typedef struct {
	uint32_t rate;
	uint32_t phase;		// oscillator phase, 2^32 = one turn
	uint32_t sample;	// samples produced
	uint32_t bit;		// tone bits done
} afsk_mod_t;

void afsk_mod_init(afsk_mod_t *m, uint32_t rate);
// Audio for bits [m->bit, nbits) of tones, up to max samples at amplitude amp. Returns the samples written; call
// again until it returns 0
size_t afsk_mod(afsk_mod_t *m, const uint8_t *tones, uint32_t nbits, int16_t amp, int16_t *out, size_t max);

// A frame the demodulator found (FCS good): bytes without the FCS
typedef void (*afsk_frame_fn)(const uint8_t *frame, size_t n, void *ctx);

typedef struct {
	uint32_t rate;
	uint16_t win;			// correlator length (one bit)
	uint16_t pos;
	float ring[4][AFSK_WIN_MAX];	// x cos / x sin for mark, then space
	float sum[4];
	uint32_t ph_mark, ph_space, inc_mark, inc_space;
	float peak_mark, peak_space, decay;
	int32_t pll;
	int32_t pll_step;
	bool last_mark;
	hdlc_dec_t hdlc;
	afsk_frame_fn fn;
	void *ctx;
	uint32_t samples, bits;		// counters
} afsk_demod_t;

// False when the rate can't be handled (below 7200 Hz or a correlator over AFSK_WIN_MAX)
bool afsk_demod_init(afsk_demod_t *d, uint32_t rate, afsk_frame_fn fn, void *ctx);
void afsk_demod(afsk_demod_t *d, const int16_t *s, size_t n);

#endif
