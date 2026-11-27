// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem (docs/spec/air-interface.md 6): discriminator samples from a board's tap (hal_baseband.h) to the
// air interface's symbol chunks, for a provider that receives through a tap. It resamples to five samples per
// symbol, applies a receive filter, finds the profile's sync patterns in the filtered samples (a least squares fit
// of each pattern's levels at every sample) to set its symbol timing, level and offset, and hands out symbols
// sampled there, with soft values. Between syncs it holds what the last one gave: a mode whose syncs recur gets
// every frame on fresh timing, and the clocks of two crystals do not move a fifth of a symbol in a second.
//
// It holds its output back by the length of a sync pattern, so a sync found is already sampled on the timing it
// gave: the framer (air_framer.h) sees every sync it could have seen. No RTOS or board call: the provider owns the
// tap, calls air_softmodem_rx with what it read, and passes the chunks on as AIR_EV_RX events.
#ifndef RFOUNDRY_AIR_SOFTMODEM_H
#define RFOUNDRY_AIR_SOFTMODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../hal/hal_air.h"
#include "../../hal/hal_baseband.h"

#define AIR_SOFTMODEM_SPS	5			// filtered samples per symbol
#define AIR_SOFTMODEM_CHUNK	48			// symbols per chunk handed out, at most
#define AIR_SOFTMODEM_LINE	((AIR_SYNC_SYMS_MAX + 4) * AIR_SOFTMODEM_SPS)	// the delay line: a sync and some
#define AIR_SOFTMODEM_TAPS	8			// interpolator taps per phase (input samples of history)

typedef void (*air_softmodem_out_t)(const air_chunk_t *c, void *ctx);

// The receiver's state: the caller's memory (a provider with no RAM to spare lays it over buffers its idle DMR
// engine does not use during a session). About 380 bytes
typedef struct {
	const air_profile_t *p;
	air_softmodem_out_t out;
	void *ctx;
	uint32_t rate_hz;			// of the tap
	uint32_t t_next_us;			// time of the next input sample, to see a hole in them
	// Input: offset, level, the interpolator
	int32_t dc;				// slow average of the input, x 1024: the tap's own offset
	int32_t env;				// slow average of its magnitude, x 1024 (a tap of unknown scale only)
	int32_t env_fast;			// the same over 2^TRACK_SHIFT_MIN samples: a step in the level
	uint32_t gain;				// summed input counts to line units, x 65536 (GAIN_MAX at most)
	uint16_t track_n;			// samples until the averages slow down a step
	uint8_t track_shift;			// their time constant: 2^shift samples
	bool gain_known;			// the tap gave counts_per_khz: the level is not tracked, only fitted on syncs
	bool running;				// samples have come since the open or the last reset
	bool locked;				// a sync set the timing and levels not long ago: the averages hold
	int16_t in[2 * AIR_SOFTMODEM_TAPS];	// the newest input samples, each written twice, TAPS apart: the last TAPS
	uint8_t in_at;				// are always one run, in[in_at + 1] the oldest
	uint16_t step, den;			// input samples per filtered sample, as a fraction
	uint16_t pos;				// the next filtered sample's place between two input samples, in 1/den
	uint16_t ph_r;				// pos x phases / den, kept without a divide: its remainder ...
	uint8_t ph;				// ... and its whole part, the interpolator phase
	// The receive filter (an integrate and dump over box samples) and its output, the line
	int16_t sum[AIR_SOFTMODEM_SPS];
	int32_t acc;
	uint8_t sum_at, box;
	int8_t line[AIR_SOFTMODEM_LINE];	// line units (16 per symbol level at nominal), a ring: the newest at line_at - 1
	uint8_t line_at;
	uint8_t line_n;				// valid samples in it (saturates)
	uint8_t phase;				// line_at modulo SPS: the sample phase, steps' index
	bool fitted;				// the sample before this one was fitted: last_q is its error
	// The sync search's gate: per sample phase, one bit per symbol back (bit 0 the newest): that sample was above
	// the one a symbol before it. A pattern is fitted only where the large steps of its last 17 symbols are all
	// there but one
	uint16_t steps[AIR_SOFTMODEM_SPS];
	uint16_t gate_up[AIR_SYNCS_MAX];	// per pattern: the bits of its steps up ...
	uint16_t gate_mask[AIR_SYNCS_MAX];	// ... and which bits are steps of four levels or more (0: no gate)
	uint8_t valid;				// bit i: pattern i can be fitted (its length, more than one level)
	// The sync search: the best fit so far of the peak being climbed
	uint16_t cand_q, cand_prev_q, last_q;	// fit errors (0xFFFF none): the candidate's, the sample's before it
	int16_t cand_lvl, cand_off;		// x 256
	uint8_t cand_sync;
	uint8_t hold;				// samples before a new candidate may start (the far side of a peak)
	// Symbols out
	uint8_t due;				// how far back in the line the next symbol's center is, in samples
	int8_t sub;				// ... and the rest of it, in 1/16 sample (positive: later)
	uint16_t scale;				// line units to soft values, x 256 (from the last sync's level)
	int16_t off;				// the last sync's offset, line units x 256
	uint16_t since_sync;			// symbols since a sync set the timing (saturates)
	uint8_t sync_wait;			// symbols until a found sync's first one goes out, + 1; 0 none
	uint8_t sync_wait_id;
	bool gap;				// the next chunk follows a hole in the samples
	uint8_t n;				// symbols in the chunk being filled
	int16_t sync_at;
	uint8_t sync_id;
	uint8_t flags;
	uint32_t t_chunk_us;
	int8_t soft[AIR_SOFTMODEM_CHUNK];	// (the hard symbols are sliced from these when the chunk goes out)
} air_softmodem_t;

// What receive through this tap gives a profile: AIR_FIT_RX | AIR_FIT_SOFT, or 0 when the tap's band or rate can't
// carry it. (No AIR_FIT_NEEDS_SYNC: without a sync it hands out symbols on the timing it has, and short syncs and
// late entry are the framer's.)
uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb);
// Start receiving profile p from a tap of rate_hz; counts_per_khz as the tap gives it (0 unknown: the level follows
// the input's average magnitude until a sync gives it, so a first sync straight out of noise can be missed). False
// when the rate can't be resampled
bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   air_softmodem_out_t out, void *ctx);
// Forget the signal (a retune, the end of a transmission): timing and levels start over, the next chunk says GAP
void air_softmodem_rx_reset(air_softmodem_t *m);
// n samples of one channel, the first taken at t_us on the provider's clock. Calls out for each chunk that fills
// and for what is left at the end of the call (a chunk never waits for the next call)
void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us);

// --- The transmitter (air_softmodem_tx.c, docs/spec/air-interface.md 6.1) ---
//
// Symbols to samples for a transmit tap (hal_baseband_t tx_write): each symbol's level times the profile's frequency
// pulse at the tap's rate, any rate from 8 to 96 kHz (a smooth pulse carries at 1.67 samples a symbol). The pulse
// written is the Gaussian at BT 0.5, the one D-STAR's GMSK modulator runs on. Integer only; no RTOS or board call:
// the caller pulls samples from wherever its tap wants them (an interrupt, a task) and writes them to the tap.

#define AIR_SOFTMODEM_TX_RATE_MIN	8000u
#define AIR_SOFTMODEM_TX_RATE_MAX	96000u
#define AIR_SOFTMODEM_NO_SYM		(-128)	// what a symbol source returns when it has no more

// The next symbol in level units: +1 / -1 for two levels (+ is a positive deviation), +3, +1, -1, -3 for four;
// AIR_SOFTMODEM_NO_SYM when there are no more. A mode maps its bits or dibits to levels (air_syms.h for dibits)
typedef int (*air_softmodem_sym_t)(void *ctx);

typedef struct {
	const int16_t *pulse;		// the profile's, 64 points a symbol over three symbols
	uint32_t step;			// symbol time per sample, 2^32 = one symbol
	uint32_t phase;			// within the current symbol
	int8_t sym[3];			// the previous, current and next symbol's level, 0 once they run out
	uint8_t tail;			// symbols since the source ran out
	int16_t amp;			// sample value of one level unit (the outer level on two levels, a third of it on four)
	air_softmodem_sym_t pull;
	void *ctx;
	uint32_t syms, samples;
} air_softmodem_tx_t;

// What transmit through this tap gives a profile: AIR_FIT_TX, or 0 when the pulse isn't written, the tap has no
// transmit half, its scale (tx_counts_per_khz) is unknown or its band or rate can't carry the profile
uint32_t air_softmodem_tx_fit(const air_profile_t *p, const hal_baseband_t *bb);
// Start a transmission of profile p on tap bb: the outer level at the profile's deviation by the tap's scale and
// polarity. False when it doesn't fit
bool air_softmodem_tx_open(air_softmodem_tx_t *m, const air_profile_t *p, const hal_baseband_t *bb,
			   air_softmodem_sym_t pull, void *ctx);
// The same with the sample rate and the outer level's sample value given (amp negative inverts): for a caller whose
// path is not a hal_baseband_t (D-STAR's flat FM path, dstar_gmsk.h)
bool air_softmodem_tx_init(air_softmodem_tx_t *m, const air_profile_t *p, uint32_t rate_hz, int16_t amp,
			   air_softmodem_sym_t pull, void *ctx);
// Shift the symbol clock by ppm (tests: a transmitter's clock error)
void air_softmodem_tx_trim(air_softmodem_tx_t *m, int32_t ppm);
// One sample, not saturated (for a caller that filters it further); false once the symbols ran out and the last
// pulse has decayed
bool air_softmodem_tx_sample(air_softmodem_tx_t *m, int32_t *out);
// Up to max samples, saturated to 16 bits. Returns the number written; fewer than max (0 at last) at the end
size_t air_softmodem_tx(air_softmodem_tx_t *m, int16_t *out, size_t max);

#endif
