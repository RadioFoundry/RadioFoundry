// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GMSK for D-STAR DV (docs/spec/dstar-modem.md 2): 4800 bps, BT = 0.5, modulation index 0.5 (+-1200 Hz), sent as
// audio into an FM modulator with a flat path and received as the FM discriminator's output, the way a radio without
// an I/Q path carries it. 1 is a positive deviation (JARL D-STAR standard, appendix 1.5); the board says whether its
// paths invert.
//   - the modulator turns air bits into Gaussian-shaped baseband at any rate above 9.6 kHz (8 kHz on the radios'
//     PCM paths works too: the pulse is smooth enough that 1.67 samples a bit carry it)
//   - the demodulator takes discriminator samples at the same rates and hands soft bits to the deframer: the bit
//     clock from the zero crossings (a phase and frequency loop, so the transmitter's clock may be a few hundred ppm
//     off), the decisions sampled between samples by interpolation, the DC (a frequency offset) and the level tracked
//     from the decisions
//   - a low-frequency restorer for a path with a high-pass in it (a DC-blocking capacitor, a voice filter that won't
//     turn off): a shelf that undoes a first-order high-pass down to a floor, on transmit before the path or on
//     receive after it
// Integer only (the HR_C7000's CK803S has no FPU). No RTOS, no board.
#ifndef RFOUNDRY_DSTAR_GMSK_H
#define RFOUNDRY_DSTAR_GMSK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DSTAR_BAUD		4800u
#define DSTAR_DEV_HZ		1200u	// the deviation of a long run of one value
#define DSTAR_RATE_MIN		8000u
#define DSTAR_RATE_MAX		96000u

// --- low-frequency restorer: H(s) = (s + wc) / (s + wf), unity above fc, a gain of fc / ff at DC ---

typedef struct {
	int32_t kc, kf;		// 2 pi (fc - ff) / rate and 2 pi ff / rate, Q16
	int64_t v;		// the leaky integrator, Q16 samples
} dstar_lfr_t;

// fc_hz = 0 makes it a straight wire. ff_hz is the floor: the gain at DC is fc / ff, the headroom it costs
void dstar_lfr_init(dstar_lfr_t *f, uint32_t rate, uint16_t fc_hz, uint16_t ff_hz);
int16_t dstar_lfr(dstar_lfr_t *f, int32_t x);

// --- modulator ---

// The next air bit: 0 or 1, -1 when there are no more (dstar_tx_bit fits)
typedef int (*dstar_bit_pull_t)(void *ctx);

typedef struct {
	uint32_t step;		// bit time per sample, 2^32 = one bit
	uint32_t phase;		// within the current bit
	int8_t sym[3];		// the previous, current and next bit as +1 / -1, 0 once they run out
	uint8_t tail;		// bits since the source ran out
	int16_t amp;		// sample value of a full deviation
	dstar_bit_pull_t pull;
	void *ctx;
	dstar_lfr_t lfr;
	uint32_t bits, samples;
} dstar_gmsk_mod_t;

// amp: the sample value that gives +1200 Hz on the board's path (hal_audio_tap_t.fm_tx_nominal_peak scaled from its
// nominal deviation). False when the rate is out of range
bool dstar_gmsk_mod_init(dstar_gmsk_mod_t *m, uint32_t rate, int16_t amp, dstar_bit_pull_t pull, void *ctx);
// Shift the bit clock by ppm (tests: a transmitter's clock error)
void dstar_gmsk_mod_trim(dstar_gmsk_mod_t *m, int32_t ppm);
// Up to max samples. Returns the number written; fewer than max (0 at last) once the bits ran out and the last
// pulse has decayed
size_t dstar_gmsk_mod(dstar_gmsk_mod_t *m, int16_t *out, size_t max);

// --- demodulator ---

// One bit: soft positive for 1, negative for 0, unit the magnitude of a clean bit (dstar_rx_bit fits)
typedef void (*dstar_soft_push_t)(void *ctx, int16_t soft, int16_t unit);

#define DSTAR_BOX_MAX		10u	// the pre-filter's length limit: half a bit at 96 kHz

typedef struct {
	uint32_t step;		// nominal bit time per sample
	int32_t adj;		// the frequency loop's correction to step
	int64_t phase;		// 0 = a bit boundary, 2^31 = the middle of a bit, 2^32 the next boundary
	bool decided;		// this bit's middle is past and its decision made
	int32_t prev;		// the previous sample less the DC
	int32_t dc, lev;	// DC and level, Q8
	int32_t kdc;		// the searching DC average's weight, Q16
	bool locked;		// slow loops (the deframer found a sync)
	bool invert;		// the board's discriminator is inverted
	uint8_t box_n, box_pos;
	int32_t box[DSTAR_BOX_MAX], box_sum;
	dstar_lfr_t lfr;
	dstar_soft_push_t push;
	void *ctx;
	uint32_t bits, crossings;
} dstar_gmsk_demod_t;

bool dstar_gmsk_demod_init(dstar_gmsk_demod_t *d, uint32_t rate, dstar_soft_push_t push, void *ctx);
void dstar_gmsk_demod(dstar_gmsk_demod_t *d, const int16_t *s, size_t n);
// The loops' speed: fast while looking for a transmission, slow once one is found (the deframer's lock)
static inline void dstar_gmsk_demod_lock(dstar_gmsk_demod_t *d, bool locked)
{
	d->locked = locked;
}
// The tracked clock error in ppm (positive: the transmitter's bits come faster than nominal)
int32_t dstar_gmsk_demod_ppm(const dstar_gmsk_demod_t *d);

#endif
