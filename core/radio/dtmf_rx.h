// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DTMF receive (docs/spec/radio-features.md 3.7): digits from FM receive audio. No hardware, no RTOS, no floating
// point (some of the radios' cores have no FPU): feed PCM, get digits.
//
// Eight Goertzel filters, one on each ITU-T Q.23 frequency (hal_dtmf.h), over Hann-windowed blocks of 25.6 ms (205
// samples at 8 kHz). The window keeps a tone 1.5 % off within about 1 dB, where a plain block loses most of it at the
// top of the band. Two banks run half a window apart, a result every 12.8 ms. A window holds a digit when:
//
//   - the strongest low and high tones are both above DTMF_RX_MIN_AMP;
//   - each beats every other tone of its group by 6 dB;
//   - the two are within 8 dB of each other (twist, either way: a receiver's de-emphasis tilts them);
//   - together they carry DTMF_RX_SHARE_PCT of the window's energy, which turns speech and noise away.
//
// A digit counts when DTMF_RX_ON_RESULTS results in a row hold it, and is over when DTMF_RX_OFF_RESULTS in a row
// don't: 40 ms tones with 40 ms pauses decode, 20 ms tones don't.
#ifndef RFOUNDRY_DTMF_RX_H
#define RFOUNDRY_DTMF_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DTMF_RX_TONES		8
#define DTMF_RX_WINDOW_US	25600u	// window length (205 samples at 8 kHz)
#define DTMF_RX_MIN_AMP		300	// smallest tone amplitude accepted, in sample counts (about -41 dBFS)
#define DTMF_RX_SHARE_PCT	45	// the two tones' least share of a window's energy, percent
#define DTMF_RX_ON_RESULTS	3	// results in a row that hold a digit before it counts
#define DTMF_RX_OFF_RESULTS	2	// results in a row without it before it is over

typedef struct {
	int32_t coef[DTMF_RX_TONES];	// 2 cos(2 pi f / fs), Q14
	uint16_t len;			// window length, samples
	int64_t min_pow;		// a tone's Goertzel power at DTMF_RX_MIN_AMP
	int64_t share_k;		// a tone's power to its energy in the window, Q24
	int64_t hann_k;			// 2 cos(2 pi / len), Q30: the window's cosine recurrence
	int32_t dc;			// DC tracker, Q8 (the receive path may carry an offset)
	struct {
		int32_t s1[DTMF_RX_TONES], s2[DTMF_RX_TONES];
		int64_t energy;
		int64_t c, c_prev;	// cos(2 pi n / len) and the one before, Q30
		uint16_t n;
	} win[2];
	char seen;			// the digit the last result held, 0 none
	uint8_t same;			// results in a row that held it (or none)
	char on;			// the accepted digit still sounding, 0 none
	uint32_t digits;		// digits accepted since init
} dtmf_rx_t;

// fs_hz: the sample rate (7200 Hz or more: the highest tone is 1633 Hz)
void dtmf_rx_init(dtmf_rx_t *d, uint32_t fs_hz);
// Forget a digit in progress (a retune, the squelch closing, our own transmission)
void dtmf_rx_reset(dtmf_rx_t *d);
// n samples; the digits accepted ('0'-'9', '*', '#', 'A'-'D') go into out, up to max. Returns how many
size_t dtmf_rx_feed(dtmf_rx_t *d, const int16_t *s, size_t n, char *out, size_t max);

#endif
