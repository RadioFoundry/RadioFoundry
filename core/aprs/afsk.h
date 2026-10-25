// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bell 202 AFSK, 1200 baud, mark 1200 Hz / space 2200 Hz (docs/spec/aprs.md 4), on PCM samples:
//   - the modulator turns a tone sequence (hdlc.h) into phase continuous audio at any sample rate; boards that send
//     PCM to their modulator use it, and the host tests use it to make test signals and WAV files
//   - the demodulator takes received FM audio (a board's hal_afsk_t rx samples, 8 kHz or more) and decides mark or
//     space per bit: two sliding one-bit correlators (quadrature, so the phase doesn't matter) whose magnitudes are
//     each normalized by their own slowly decaying peak (this undoes the tilt pre-emphasis / de-emphasis mismatch
//     puts between the tones), the difference sampled by a digital PLL that locks to the data transitions. Two such
//     paths run side by side, one on the audio as received and one after a high pass that removes audio under the
//     tones; a frame either finds is delivered once
// Integer only (radios without an FPU run it at 8 kHz: the CK803S of the HR_C7000). No RTOS, no board.
#ifndef RFOUNDRY_AFSK_H
#define RFOUNDRY_AFSK_H

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

#define AFSK_PATHS		2	// the audio as received, and high passed

// One decision path: its correlators, clock and HDLC decoder
typedef struct {
	int32_t hp_x[2], hp_y[2];	// the input high pass's two stages (the high passed path)
	int32_t ring[4][AFSK_WIN_MAX];	// x cos / x sin for mark, then space
	int32_t sum[4];
	uint32_t peak_mark, peak_space;
	int32_t pll;
	bool last_mark;
	hdlc_dec_t hdlc;
} afsk_path_t;

typedef struct {
	uint32_t rate;
	uint16_t win;			// correlator length (one bit)
	uint16_t pos;
	uint32_t ph_mark, ph_space, inc_mark, inc_space;
	uint8_t decay_shift;
	int32_t pll_step;
	int32_t hp_a;			// the high pass stages: y = a (y + x - x[-1]), a in Q15
	afsk_path_t path[AFSK_PATHS];
	uint8_t last_fcs[2];		// the last frame delivered, so the other path's copy of it is dropped
	size_t last_len;
	uint32_t last_at;		// sample count when it was delivered
	afsk_frame_fn fn;
	void *ctx;
	uint32_t samples, bits;		// counters: bits are the received path's
	uint32_t frames, dups;		// frames delivered, and copies dropped
} afsk_demod_t;

// False when the rate can't be handled (below 7200 Hz or a correlator over AFSK_WIN_MAX)
bool afsk_demod_init(afsk_demod_t *d, uint32_t rate, afsk_frame_fn fn, void *ctx);
void afsk_demod(afsk_demod_t *d, const int16_t *s, size_t n);
// The HDLC decoders' frames with a bad FCS and aborts, summed over the paths
uint32_t afsk_demod_fcs_errors(const afsk_demod_t *d);
uint32_t afsk_demod_aborts(const afsk_demod_t *d);

#endif
