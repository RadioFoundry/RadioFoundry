// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM for Yaesu System Fusion (docs/spec/ysf-modem.md 2): 4800 symbols/s, four levels at +-900 and +-2700 Hz
// (wide; narrow is half that), root raised cosine shaped (roll-off 0.2), carried as audio through an FM modulator with a flat path and received as
// the FM discriminator's output. This is the software modem for a board whose only way to the air is such a path; a
// board whose modem chip sends and receives 4FSK symbols itself skips it and hands symbols to the framing directly
// (ysf_engine.h).
//   - the modulator turns symbols into root raised cosine shaped baseband at any rate from 8 kHz
//   - the demodulator takes discriminator samples at the same rates and hands soft symbols to the deframer: the
//     matched filter evaluated at the symbol instants straight from the input samples (the pulse is band-limited to
//     2.88 kHz, so 8 kHz samples hold all of it and no separate interpolator is needed), the symbol clock from a
//     Gardner detector (a phase and frequency loop), the DC (a frequency offset) and the level tracked from the
//     decisions once the deframer has found a frame sync
// Symbols are the dibit values 0-3 as the framing sends them; ysf_c4fm_level maps them to +-1, +-3 (2.1).
// Integer only (the HR_C7000's CK803S has no FPU). No RTOS, no board.
#ifndef RFOUNDRY_YSF_C4FM_H
#define RFOUNDRY_YSF_C4FM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define YSF_BAUD		4800u
#define YSF_DEV_HZ		900u	// the deviation of a +1 symbol, wide (table 3-1); +3 is three times it
#define YSF_RATE_MIN		8000u
#define YSF_RATE_MAX		48000u
#define YSF_RRC_SPAN		6	// the pulse is kept to +-6 symbols (ysf_c4fm.c says why)

// The level of a dibit (b1 b0, b1 sent first): 01 = +3, 00 = +1, 10 = -1, 11 = -3 (2.1)
static inline int ysf_c4fm_level(uint8_t dibit)
{
	static const int8_t lv[4] = { 1, 3, -1, -3 };
	return lv[dibit & 3u];
}

// --- low-frequency restorer: H(s) = (s + wc) / (s + wf), unity above fc, a gain of fc / ff at DC ---

// After a first-order high-pass at fc (a coupling capacitor, a voice filter that won't turn off) the pair is a
// high-pass at the floor ff, so the levels stop sagging under a run of one sign. The input plus a leaky integrator of
// it: no tan() for the coefficients, exact enough while fc is far below the rate
typedef struct {
	int32_t kc, kf;		// 2 pi (fc - ff) / rate and 2 pi ff / rate, Q16
	int64_t v;		// the integrator, Q16 samples
} ysf_lfr_t;

// fc_hz = 0 makes it a straight wire; ff_hz = 0 takes YSF_LFR_FLOOR_HZ. The floor costs headroom (fc / ff at DC)
// and what it leaves the demodulator's DC loop follows: 30 Hz is what that loop takes with a small error floor
// (docs/spec/ysf-modem.md 2.5)
#define YSF_LFR_FLOOR_HZ	30u
void ysf_lfr_init(ysf_lfr_t *f, uint32_t rate, uint16_t fc_hz, uint16_t ff_hz);
int16_t ysf_lfr(ysf_lfr_t *f, int32_t x);

// --- modulator ---

// The next symbol: a dibit 0-3, -1 when there are no more
typedef int (*ysf_sym_pull_t)(void *ctx);

typedef struct {
	uint32_t step;		// symbol time per sample, 2^32 = one symbol
	uint32_t phase;		// within the current symbol
	int8_t lv[2 * YSF_RRC_SPAN];	// the levels whose pulses reach the current sample, oldest first; 0 once run out
	uint8_t tail;		// symbols since the source ran out
	int16_t amp;		// sample value of a +1 symbol
	ysf_sym_pull_t pull;
	void *ctx;
	uint32_t symbols, samples;
} ysf_c4fm_mod_t;

// amp: the sample value that gives a +1 symbol's deviation (900 Hz wide) on the board's path, at the matched filter's output: the shaped waveform
// itself peaks near 4.5 x amp, so amp up to about 7000 fits 16 bits. False when the rate is out of range
bool ysf_c4fm_mod_init(ysf_c4fm_mod_t *m, uint32_t rate, int16_t amp, ysf_sym_pull_t pull, void *ctx);
// Shift the symbol clock by ppm (tests: a transmitter's clock error)
void ysf_c4fm_mod_ppm(ysf_c4fm_mod_t *m, int32_t ppm);
// Fill n samples. Returns false once the source has run out and the last symbol's pulse has passed (the rest zeros)
bool ysf_c4fm_mod(ysf_c4fm_mod_t *m, int16_t *out, size_t n);

// --- demodulator ---

// One soft symbol: the matched filter's value at the symbol instant less the DC, and the tracked value of a +1
// symbol (positive; the polarity is the deframer's to find)
typedef void (*ysf_sym_push_t)(void *ctx, int16_t soft, int16_t unit);

#define YSF_DEMOD_RING		128u	// input samples kept: the pulse's span (12 symbols) at 48 kHz and two more (a power of two)

typedef struct {
	uint32_t rate;
	int32_t sps;		// samples per symbol, Q16
	int32_t tstep;		// pulse table points per sample, Q16 (YSF_RRC_PTS symbols per sample)
	int32_t gain;		// symbol time per sample (4800 / rate), Q16: the matched filter's sum to an integral
	int16_t ring[YSF_DEMOD_RING];
	uint32_t n;		// samples received
	int64_t t;		// the next symbol instant in samples, Q16
	int32_t freq;		// the symbol clock's correction, 1/256 of a Q16 sample a symbol (its ppm against sps)
	int32_t prev;		// the last symbol's matched filter value, less the DC
	int32_t prev_a;		// and its decision, +-1 or +-3
	int32_t dc;		// the matched filter's DC (a frequency offset), Q4
	int32_t unit;		// the value of a +1 symbol, Q4
	int32_t hi, lo;		// the envelope: the outer levels as the search sees them, Q4
	ysf_lfr_t lfr;		// the receive path's high-pass undone (ysf_lfr_init; a wire when not set)
	bool locked;		// the deframer has a sync: the loops slow and follow the decisions
	ysf_sym_push_t push;
	void *ctx;
	uint32_t symbols;
} ysf_c4fm_demod_t;

bool ysf_c4fm_demod_init(ysf_c4fm_demod_t *d, uint32_t rate, ysf_sym_push_t push, void *ctx);
void ysf_c4fm_demod(ysf_c4fm_demod_t *d, const int16_t *in, size_t n);
// The deframer found (true) or lost (false) the frame sync
void ysf_c4fm_demod_lock(ysf_c4fm_demod_t *d, bool locked);
// The symbol clock's offset from nominal as the loop has it, ppm (positive: the transmitter is fast)
int32_t ysf_c4fm_demod_ppm(const ysf_c4fm_demod_t *d);
// The DC the demodulator removes, as Hz of carrier offset when a +1 symbol is the wide 900 Hz
int32_t ysf_c4fm_demod_offset_hz(const ysf_c4fm_demod_t *d);

#endif
