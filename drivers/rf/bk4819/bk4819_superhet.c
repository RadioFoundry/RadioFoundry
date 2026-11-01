// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The BK4819 as a superhet's main transceiver or LO (rf/superhet/superhet.h): chip = a bk4819_t
#include "bk4819.h"
#include "bk4819_superhet.h"

static bk4819_tone_t tone(superhet_tone_t t)
{
	switch (t) {
	case SUPERHET_TONE_CTCSS: return BK4819_TONE_CTCSS;
	case SUPERHET_TONE_DCS: return BK4819_TONE_DCS;
	case SUPERHET_TONE_DCS_INV: return BK4819_TONE_DCS_INV;
	default: return BK4819_TONE_NONE;
	}
}

static void idle(void *c) { bk4819_set_state(c, BK4819_STATE_IDLE); }
static void rx(void *c) { bk4819_set_state(c, BK4819_STATE_RX); }
static void to_if(void *c, uint32_t if_hz) { bk4819_set_freq(c, if_hz); }	// idles first
static void bandwidth(void *c, bool wide) { bk4819_set_bandwidth(c, wide ? BK4819_BW_25K : BK4819_BW_12K5); }
static void rx_tone(void *c, superhet_tone_t t, uint16_t v) { bk4819_set_rx_tone(c, tone(t), v); }

static void af(void *c, superhet_af_t a)
{
	bk4819_set_af(c, a == SUPERHET_AF_FM ? BK4819_AF_FM : a == SUPERHET_AF_DMR ? BK4819_AF_DMR : BK4819_AF_MUTE);
}

static void af_gain(void *c, uint8_t g) { bk4819_set_af_gain(c, g); }

// The stock deviation and sub-audio words; the PA gain register stays at the value set at start-up
static void tx_prepare(void *c, superhet_tone_t t, uint16_t v, uint32_t hz, uint8_t pa_gain)
{
	(void)hz;
	(void)pa_gain;
	bk4819_tx_prepare(c, tone(t), v);
}

static void tx_tune(void *c, uint32_t hz) { bk4819_tx_tune(c, hz); }
static void tx_end(void *c) { bk4819_tx_end(c); }

static bool tail(void *c, bool on)
{
	if (on) {
		return bk4819_tx_tail_start(c);
	}
	bk4819_tx_tail_end(c);
	return true;
}

static void tone_start(void *c, uint16_t hz, uint8_t gain) { bk4819_tone_start(c, hz, gain); }
static bool tone_on(void *c) { return ((bk4819_t *)c)->tone_hz != 0; }
static void tone_key(void *c, bool down) { bk4819_tone_key(c, down); }
static void dtmf(void *c, uint16_t lo, uint16_t hi) { bk4819_dtmf_start(c, lo, hi); }

static void dmr_enter(void *c, uint8_t g)
{
	bk4819_dmr_rx_enter(c);
	bk4819_set_af_gain(c, g);
}

static void dmr_leave(void *c) { bk4819_dmr_rx_end(c); }

static void dmr_tx_prepare(void *c, uint8_t pa_gain)
{
	(void)pa_gain;
	bk4819_dmr_tx_prepare(c);
}

static void burst(void *c, uint32_t hz, bool tx) { bk4819_burst_tune(c, hz, tx ? BK4819_STATE_TX_MAIN : BK4819_STATE_RX); }
static void gpio(void *c, uint8_t pin, bool high) { bk4819_gpio(c, pin, high); }

const superhet_main_ops_t bk4819_superhet_main = {
	.idle = idle, .rx = rx, .to_if = to_if, .bandwidth = bandwidth, .rx_tone = rx_tone, .af = af, .af_gain = af_gain,
	.tx_prepare = tx_prepare, .tx_tune = tx_tune, .tx_end = tx_end, .tail = tail, .tone = tone_start,
	.tone_on = tone_on, .tone_key = tone_key, .dtmf = dtmf, .dmr_enter = dmr_enter, .dmr_leave = dmr_leave,
	.dmr_tx_prepare = dmr_tx_prepare, .burst = burst, .gpio = gpio,
};

static void lo_tune(void *c, uint32_t hz) { bk4819_lo_tune(c, hz); }
static void lo_off(void *c) { bk4819_lo_off(c); }
static void lo_resume(void *c) { bk4819_lo_resume(c); }

const superhet_lo_ops_t bk4819_superhet_lo = {
	.tune = lo_tune, .off = lo_off, .resume = lo_resume, .gpio = gpio,
};
