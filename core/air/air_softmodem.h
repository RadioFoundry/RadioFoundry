// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem (docs/spec/air-interface.md 6): discriminator samples from a board's tap (hal_baseband.h) to the
// air interface's symbol chunks, for a provider that receives through a tap. It resamples to five samples per
// symbol, applies a receive filter, finds the profile's sync patterns in the filtered samples (a least squares fit
// of each pattern's levels at every sample) to set its symbol timing, level and offset, and hands out symbols
// sampled there, with soft values. Between syncs it holds what the last one gave: a mode whose syncs recur gets
// every frame on fresh timing, and the clocks of two crystals do not move a fifth of a symbol in a second.
//
// A slope tap (hal_baseband_t.rx_slope_hz: its samples are the derivative of the deviation) is integrated first, with
// a leak at the tap's corner, and its symbols get a decision-directed baseline: what the leak's high-pass leaves of
// a run of one level is taken back out between syncs.
//
// It holds its output back by the length of a sync pattern, so a sync found is already sampled on the timing it
// gave: the framer (air_framer.h) sees every sync it could have seen. No RTOS or board call: the provider owns the
// tap, calls air_softmodem_rx with what it read, and passes the chunks on as AIR_EV_RX events.
#ifndef RFOUNDRY_AIR_SOFTMODEM_H
#define RFOUNDRY_AIR_SOFTMODEM_H

#include <stdbool.h>
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
	uint16_t rate_hz;			// of the tap (rx_open takes 48 kHz at most)
	uint16_t track_n;			// samples until the averages slow down a step
	uint32_t t_next_us;			// time of the next input sample, to see a hole in them
	// A slope tap's integrator: the tap's own offset (x 1024; its average's window, slope_shift and slope_n, below)
	// and the sum (its leak and the baseline below: their fields sit where the others leave room)
	int32_t slope_dc;
	int32_t integ;
	// Input: offset, level, the interpolator
	int32_t dc;				// slow average of the input, x 1024: the tap's own offset
	int32_t env;				// slow average of its magnitude, x 1024 (a tap of unknown scale only)
	int32_t env_fast;			// the same over 2^TRACK_SHIFT_MIN samples: a step in the level
	uint32_t gain;				// summed input counts to line units, x 65536 (GAIN_MAX at most)
	uint8_t track_shift;			// their time constant: 2^shift samples
	bool gain_known;			// the tap gave counts_per_khz: the level is not tracked, only fitted on syncs
	bool running;				// samples have come since the open or the last reset
	bool locked;				// a sync set the timing and levels not long ago: the averages hold
	int16_t in[2 * AIR_SOFTMODEM_TAPS];	// the newest input samples, each written twice, TAPS apart: the last TAPS
	uint8_t in_at;				// are always one run, in[in_at + 1] the oldest
	uint8_t slope_shift;			// the slope tap offset's window, 2^shift samples, from an open ...
	uint16_t step, den;			// input samples per filtered sample, as a fraction
	uint16_t pos;				// the next filtered sample's place between two input samples, in 1/den
	uint16_t ph_r;				// pos x phases / den, kept without a divide: its remainder ...
	uint8_t ph;				// ... and its whole part, the interpolator phase
	uint8_t slope_n;			// ... which doubles each time this wraps (256 samples), to 2^10
	// The receive filter (an integrate and dump over box samples) and its output, the line
	int16_t sum[AIR_SOFTMODEM_SPS];
	int16_t two_past;			// a slope tap's two-sided sum (TWO_K, air_softmodem.c): the next symbol's past half
						// less its line value, x8 line units
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
	uint8_t leak;				// a slope tap's: the sum loses 1 / 2^leak a sample (0: a flat tap)
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
	int16_t base;				// a slope tap's decision-directed baseline, soft units x 16
	uint32_t t_chunk_us;
	int8_t soft[AIR_SOFTMODEM_CHUNK];	// (the hard symbols are sliced from these when the chunk goes out)
} air_softmodem_t;

// What receive through this tap gives a profile: AIR_FIT_RX | AIR_FIT_SOFT, or 0 when the tap's band or rate can't
// carry it. (No AIR_FIT_NEEDS_SYNC: without a sync it hands out symbols on the timing it has, and short syncs and
// late entry are the framer's.)
uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb);
// Start receiving profile p from a tap of rate_hz; counts_per_khz as the tap gives it (0 unknown: the level follows
// the input's average magnitude until a sync gives it, so a first sync straight out of noise can be missed).
// slope_hz as the tap gives it (hal_baseband_t.rx_slope_hz): 0 a flat tap; else the samples are integrated with a
// leak at about that corner, and the level is always tracked. False when the rate can't be resampled
bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   uint16_t slope_hz, air_softmodem_out_t out, void *ctx);
// The integrator's leak for a slope tap at rate_hz: the sum loses 1 / 2^leak a sample, the power of two nearest the
// corner slope_hz asks for. *corner_hz (may be NULL) the corner that gives. 0: no slope, or none in range
uint8_t air_softmodem_leak(uint32_t rate_hz, uint16_t slope_hz, uint16_t *corner_hz);
// Forget the signal (a retune, the end of a transmission): timing and levels start over, the next chunk says GAP
void air_softmodem_rx_reset(air_softmodem_t *m);
// n samples of one channel, the first taken at t_us on the provider's clock. Calls out for each chunk that fills
// and for what is left at the end of the call (a chunk never waits for the next call)
void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us);

// ── Transmit (air_softmodem_tx.c) ──
// Symbols to samples for a transmit tap: each symbol's level, after the tap's low-end filter (hal_baseband_t.tx_lf),
// through one table, the profile's pulse convolved with the tap's equalizer (tx_eq) and scaled to its counts, at the
// tap's rate. The table holds AIR_SOFTMODEM_TX_COEFS samples of that response: 32 symbols at 5 samples a symbol, 16
// at 10, placed where it has the most energy; the output lags the input by the part of it that comes before the
// symbol and by the low-end filter's look-ahead (lead). Each symbol in gives one symbol's samples out, the one lead
// symbols back. No RTOS or board call. About 520 bytes: a board short of RAM lays it over memory its receive tap
// leaves while it transmits
#define AIR_SOFTMODEM_TX_COEFS	160
#define AIR_SOFTMODEM_TX_SPAN	32			// symbols of history at most (a power of two)
#define AIR_SOFTMODEM_TX_LF	128			// symbols the low-end filter spans at most (a power of two)

typedef struct {
	union {
		int16_t coef[AIR_SOFTMODEM_TX_COEFS];	// [phase][tap], tap i weighs the symbol i back from the
							// newest, in counts per symbol level
		uint32_t scratch[AIR_SOFTMODEM_TX_COEFS / 2];	// the open's, before the table
	};
	int16_t hist[AIR_SOFTMODEM_TX_SPAN];	// levels out of the low-end filter, x16 (+-1, +-3 without one; 0
						// none), a ring: the newest at hist_at
	int8_t lv[AIR_SOFTMODEM_TX_LF];		// the levels into it, a ring: the newest at lv_at
	const int16_t *lf;			// the tap's tx_lf (NULL: levels go straight to hist)
	uint8_t lf_n;				// its taps
	uint8_t lf_step;			// taps a symbol: 1, or 2 at half its rate (each pair summed)
	uint8_t lv_at;
	uint8_t hist_at;
	uint8_t sps;				// samples a symbol
	uint8_t span;				// symbols in the table (a power of two)
	uint8_t lead;				// symbols the output lags the input
} air_softmodem_tx_t;

// What transmit through this tap gives a profile: AIR_FIT_TX, or 0 (no transmit tap, its rate not 5 or 10 samples a
// symbol, a pulse it has no table for, a band that can't carry the profile, a low-end filter for another rate)
uint32_t air_softmodem_tx_fit(const air_profile_t *p, const hal_baseband_t *bb);
// Build the table for profile p on the tap; the history starts empty. False when the fit is 0
bool air_softmodem_tx_open(air_softmodem_tx_t *m, const air_profile_t *p, const hal_baseband_t *bb);
// One symbol in (a dibit; AIR_SOFTMODEM_TX_NONE: no symbol, the carrier's center), sps samples out to out[]: the
// symbol lead symbols back. Returns sps
#define AIR_SOFTMODEM_TX_NONE	0xFF
uint8_t air_softmodem_tx_sym(air_softmodem_tx_t *m, uint8_t dibit, int16_t *out);
// The symbols of nothing (AIR_SOFTMODEM_TX_NONE) to send after the last one for its response to be out
uint8_t air_softmodem_tx_tail(const air_softmodem_tx_t *m);

#endif
