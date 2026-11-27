// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-32UV flat FM paths for a data modem with its own waveform (hal_audio_tap.h, docs/spec/hardware-dm32.md 9.3): the
// board's hal_audio_tap_t, the paths' calibration and the bench's settings. The route switches are in radio_dm32.c,
// the HR_C7000's TX RAM writer in platform/hrc7000/fm_flat_tx.c and its receive ring in fm_rx_audio.c.
//
// Every register choice here comes from the datasheets or from other projects' notes; none is measured on this radio
// yet (9.3 lists them). So each is a setting the bench can change at run time through the debug link's knobs, not a
// constant: a wrong guess costs a bench session, not a rebuild.
#include "config.h"
#if CONFIG_FM_FLAT
#include "dm32.h"
#include "fm_rx_audio.h"
#include "fm_flat_tx.h"
#include "modem/hrc7000/hrc7000.h"

// FD6818 0x2B on the flat paths, by the BK4819 layout (dstar-modem.md 1.2). Transmit: FM's 0xC000 with the three
// transmit audio filter bypasses (bits 2:0). Receive: the receive high-pass, low-pass and de-emphasis bypasses (bits
// 10:8), the value the stock firmware writes for DMR
#define RF_TX_2B_FLAT	0xC007u
#define RF_RX_2B_FLAT	0x0700u

dm32_flat_bench_t dm32_flat_bench = {
	.rf_tx_2b = RF_TX_2B_FLAT,
	.rf_rx_2b = RF_RX_2B_FLAT,
	.tx_bw_clr = HRC7000_FM_FLAT_TX_CLR,
	.rx_bw_clr = HRC7000_FM_FLAT_RX_CLR,
	.dev_coef = 0,		// FM transmit's: whether its TX coefficient and limiter act on the TX RAM route is unknown
	.fm_play = false,	// the speaker carries the decoded voice alone
	.tx_other_half = false,	// FM_ADDR_SW names the half to fill (the datasheet's "asks for a half")
};

// The board's best values (hal_audio_tap.h). tx_1khz is a guess from another route: through the prompt FIFO at 25 kHz
// a PCM amplitude of 2750 gave 2.2 kHz on the 1200 Hz tone (9.1), about 1250 for 1 kHz, but that route kept
// pre-emphasis in front of the modulator and the TX RAM's scale is unmeasured. Polarity unknown both ways (the
// receive copy may follow RF_MODE bit 15, which the two RF set-ups set differently), so not inverted. No high-pass to
// compensate until the bench measures one: the 290 Hz another project found was the prompt FIFO's, not this route's.
// Latency: the half playing and the half filled, 64 ms at 8 kHz (fm_flat_tx.c), the modulator's own unmeasured
static const audio_tap_flat_cal_t cal_default = {
	.tx_1khz = 1250,
	.tx_invert = false,
	.rx_invert = false,
	.tx_hpf_hz = 0,
	.rx_hpf_hz = 0,
	.tx_latency_ms = 2u * HRC7000_FM_TX_SAMPLES * 1000u / HRC7000_FM_RX_RATE,
};
static audio_tap_flat_cal_t cal = cal_default;

static const audio_tap_flat_cal_t *flat_cal(void)
{
	return &cal;
}

static void set_flat_cal(const audio_tap_flat_cal_t *c)
{
	cal = c != NULL ? *c : cal_default;
}

// Voice injection isn't written for this board: the TX RAM route is the flat one, and FM voice keeps the microphone
static bool fm_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t mode)
{
	if (pull != NULL && mode != AUDIO_TAP_FLAT) {
		return false;
	}
	return dm32_flat_tx(pull);
}

// Only while flat receive owns the ring: APRS shares it and must not lose samples to a stray read
static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	return dm32_flat_rx_on() ? hrc_fm_rx_read(buf, max) : 0u;
}

// The bench knobs (hal_audio_tap_t.flat_knob): settings, then read-only counters. A setting takes a value up to max;
// with mask, only bits of the mask (the FM_BANDWIDTH ones: the route sets back what it cleared, so it may clear only
// bits FM's own set-up has)
enum { K_RF_TX_2B, K_RF_RX_2B, K_TX_BW_CLR, K_RX_BW_CLR, K_DEV_COEF, K_FM_PLAY, K_TX_HALF,
       K_TX_HALVES, K_TX_MISSES, K_RX_HALVES, K_RX_DROPPED, K_COUNT };
#define K_FIRST_COUNTER	K_TX_HALVES

static const struct {
	const char *name;
	uint32_t max;
	uint8_t mask;
} knobs[K_COUNT] = {
	[K_RF_TX_2B] = { "rf_tx_2b", 0xFFFFu, 0 },
	[K_RF_RX_2B] = { "rf_rx_2b", 0xFFFFu, 0 },
	[K_TX_BW_CLR] = { "tx_bw_clr", 0xFFu, HRC7000_FM_FLAT_TX_CLR },
	[K_RX_BW_CLR] = { "rx_bw_clr", 0xFFu, HRC7000_FM_FLAT_RX_CLR },
	[K_DEV_COEF] = { "dev_coef", 0xFFFFFFu, 0 },	// FM_DEV_COEF b23:0; 0 = FM transmit's
	[K_FM_PLAY] = { "fm_play", 1u, 0 },
	[K_TX_HALF] = { "tx_half", 1u, 0 },		// 1 = fill the half FM_ADDR_SW doesn't name
	[K_TX_HALVES] = { "tx_halves", 0, 0 },
	[K_TX_MISSES] = { "tx_misses", 0, 0 },		// FM TX interrupts that named neither half
	[K_RX_HALVES] = { "rx_halves", 0, 0 },
	[K_RX_DROPPED] = { "rx_dropped", 0, 0 },	// receive halves lost to a full ring
};

static int32_t knob_get(uint8_t i)
{
	const dm32_flat_bench_t *b = &dm32_flat_bench;
	uint32_t c[4];
	hrc_fm_flat_tx_stats(&c[0], &c[1]);
	hrc_fm_rx_stats(&c[2], &c[3]);
	switch (i) {
	case K_RF_TX_2B: return b->rf_tx_2b;
	case K_RF_RX_2B: return b->rf_rx_2b;
	case K_TX_BW_CLR: return b->tx_bw_clr;
	case K_RX_BW_CLR: return b->rx_bw_clr;
	case K_DEV_COEF: return (int32_t)b->dev_coef;
	case K_FM_PLAY: return b->fm_play;
	case K_TX_HALF: return b->tx_other_half;
	default: return (int32_t)(c[i - K_FIRST_COUNTER] & 0x7FFFFFFFu);
	}
}

static void knob_set(uint8_t i, uint32_t v)
{
	dm32_flat_bench_t *b = &dm32_flat_bench;
	switch (i) {
	case K_RF_TX_2B: b->rf_tx_2b = (uint16_t)v; break;
	case K_RF_RX_2B: b->rf_rx_2b = (uint16_t)v; break;
	case K_TX_BW_CLR: b->tx_bw_clr = (uint8_t)v; break;
	case K_RX_BW_CLR: b->rx_bw_clr = (uint8_t)v; break;
	case K_DEV_COEF: b->dev_coef = v; break;
	case K_FM_PLAY: b->fm_play = v != 0u; break;
	default: b->tx_other_half = v != 0u; break;
	}
}

// Any task (the debug link's): each setting is one field, read by the radio task at its next use
static bool flat_knob(uint8_t i, const char **name, int32_t *value, const int32_t *set)
{
	if (i >= K_COUNT) {
		return false;
	}
	*name = knobs[i].name;
	if (set != NULL) {
		uint32_t v = (uint32_t)*set;
		if (i >= K_FIRST_COUNTER || *set < 0 || v > knobs[i].max ||
		    (knobs[i].mask != 0u && (v & ~(uint32_t)knobs[i].mask) != 0u)) {
			return false;
		}
		knob_set(i, v);
	}
	*value = knob_get(i);
	return true;
}

const hal_audio_tap_t dm32_audio_tap = {
	.caps = AUDIO_TAP_FM_RX_RAW | AUDIO_TAP_FM_TX_FLAT,
	.fm_tx_inject = fm_tx_inject,
	.fm_rx_flat_rate_hz = HRC7000_FM_RX_RATE,
	.fm_rx_flat_start = dm32_flat_rx_start,
	.fm_rx_flat_read = rx_read,
	.fm_rx_flat_stop = dm32_flat_rx_stop,
	.flat_cal = flat_cal,
	.set_flat_cal = set_flat_cal,
	.flat_knob_count = K_COUNT,
	.flat_knob = flat_knob,
};
#endif
