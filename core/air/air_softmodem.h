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
#include <stdint.h>

#include "config.h"
#include "../../hal/hal_air.h"
#include "../../hal/hal_baseband.h"

#define AIR_SOFTMODEM_SPS	5			// filtered samples per symbol
#define AIR_SOFTMODEM_CHUNK	48			// symbols per chunk handed out, at most
#define AIR_SOFTMODEM_LINE	((AIR_SYNC_SYMS_MAX + 4) * AIR_SOFTMODEM_SPS)	// the delay line: a sync and some
#define AIR_SOFTMODEM_TAPS	8			// interpolator taps per phase (input samples of history)

typedef void (*air_softmodem_out_t)(const air_chunk_t *c, void *ctx);

#if CONFIG_AIR_RX_ADAPT
// The adaptive receive options (air_profile_t.rx_opts, docs/spec/air-interface.md 6.1). Their state is memory the
// mode's engine lends for its session (air_softmodem_lend): about 1.2 KB, none for the modes that don't ask
#define AIR_ADAPT_EQ_TAPS	7			// a symbol apart, centered on the symbol
#define AIR_ADAPT_TONES		50			// the standard CTCSS tones
#define AIR_ADAPT_PERIOD_MAX	480			// a tone search's frame: the profile's sync spacing, up to this
#define AIR_ADAPT_DECIM		4			// symbols per tone filter step: 1200 Hz at 4800 symbols/s
#define AIR_ADAPT_DCS_BINS	184			// the DCS word's waveform: 8 bins a bit, 23 bits

typedef struct {
	uint8_t opts;				// the profile's options this session runs (AIR_RX_*)
	// The equalizer: taps x 16384, the oldest symbol first; the symbols before this one, the newest first
	int16_t eq_w[AIR_ADAPT_EQ_TAPS];
	int16_t eq_past[AIR_ADAPT_EQ_TAPS / 2];
	// The tone search: the slicing error of the symbols whose neighborhood differs from the same place a frame
	// before (what repeats every frame is a comb at the frame rate, not a tone), less its slow mean, through a
	// second-order CIC that keeps one in AIR_ADAPT_DECIM, into a Goertzel filter per tone over blocks of four frames
	uint16_t period, at, n;
	uint8_t hist[AIR_ADAPT_PERIOD_MAX];	// each place's neighborhood a frame before: 3 decided symbols | 0x40
	uint8_t ctx;				// the last 3 decided symbols, 2 bits each
	bool e_ok;				// e_prev is a fresh symbol's
	int8_t e_prev;				// the error of the symbol before (the middle of ctx)
	int32_t e_mean;				// the errors' slow mean, x 256
	uint32_t ci1, ci2, cc1, cc2;		// the CIC's integrators (each symbol) and combs (each output), modulo 2^32
	uint8_t dn;				// symbols since its last output
	int32_t s1[AIR_ADAPT_TONES], s2[AIR_ADAPT_TONES];
	int16_t coef[AIR_ADAPT_TONES];		// 2 cos(2 pi f / the summed rate) x 4096
	uint16_t gain[AIR_ADAPT_TONES];		// the CIC's power gain at the tone, x 4096
	uint8_t cand;				// the last block's winner, waiting for a second block (0xFF none)
	uint8_t sel;				// the tone notched (0xFF none)
	// The notch on the input samples: its gain x 2^14; 2 - 2 cos w, 2 + a1 and 1 - a2 x 2^20 (each a small
	// difference from the double zero and double pole at DC, so they keep their precision at the low tones); its state
	int32_t ng, ndc, nda, ne;
	int32_t x1, x2, y1, y2;
	// DCS: the input samples averaged by their place in the 23-bit word (it repeats every 171 ms, the 4FSK doesn't),
	// the place in bins x 65536 and its step a sample, the words seen, the lock, the words against it running
	int16_t dcs_avg[AIR_ADAPT_DCS_BINS];
	uint32_t dcs_pos, dcs_step;
	uint32_t dcs_w;				// the word read last, bit k from bin dcs_off + 8 k
	int16_t dcs_shape[8];			// the waveform off a 1 bit, by the place in the bit (a 0 bit's is minus it)
	uint8_t dcs_off;
	uint8_t dcs_words, dcs_bad;
	bool dcs_lock;
} air_softmodem_adapt_t;

// What they do now, for the debug link (any task reads it; the modem writes it)
typedef struct {
	uint8_t opts;				// running: AIR_RX_* of the session less the bench's
	uint16_t tone_dhz;			// the tone notched, Hz x 10, 0 none
	int16_t eq_center;			// the equalizer's center tap x 16384
	uint16_t eq_resets, tone_blocks, tone_picks;
	uint16_t dcs_code;			// the DCS code taken off (its 9 bits: octal digits), 0 none; 0x8000 inverted
	uint16_t dcs_amp;			// its amplitude in input counts
	uint16_t dcs_locks;
} air_softmodem_adapt_view_t;

// The memory a session's options run in, from the engine that opens it (NULL after it closes): lent before the
// provider opens the session, taken back after it closes
void air_softmodem_lend(air_softmodem_adapt_t *a);
// Bench A/B (debug link): AIR_RX_* turned off from the next symbol, whatever a profile asks
extern volatile uint8_t air_softmodem_adapt_off;
void air_softmodem_adapt_get(air_softmodem_adapt_view_t *out);
#endif

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
#if CONFIG_AIR_RX_ADAPT
	air_softmodem_adapt_t *ad;		// the options' state, NULL: the plain receiver
#endif
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

#endif
