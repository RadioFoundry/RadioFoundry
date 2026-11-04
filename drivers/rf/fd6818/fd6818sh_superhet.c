// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The FD6818 as a superhet's main transceiver or LO (rf/superhet/superhet.h): chip = an fd6818sh_t
#include "fd6818sh.h"
#include "fd6818sh_superhet.h"

static fd6818sh_tone_t tone(superhet_tone_t t)
{
	switch (t) {
	case SUPERHET_TONE_CTCSS: return FD6818SH_TONE_CTCSS;
	case SUPERHET_TONE_DCS: return FD6818SH_TONE_DCS;
	case SUPERHET_TONE_DCS_INV: return FD6818SH_TONE_DCS_INV;
	default: return FD6818SH_TONE_NONE;
	}
}

static void idle(void *c) { fd6818sh_set_state(c, FD6818SH_STATE_IDLE); }
static void rx(void *c) { fd6818sh_set_state(c, FD6818SH_STATE_RX); }

static void to_if(void *c, uint32_t if_hz)
{
	fd6818sh_set_state(c, FD6818SH_STATE_IDLE);
	fd6818sh_tune(c, if_hz);
}

static void bandwidth(void *c, bool wide) { fd6818sh_set_bandwidth(c, wide); }
static void rx_tone(void *c, superhet_tone_t t, uint16_t v) { fd6818sh_set_rx_tone(c, tone(t), v); }

static void af(void *c, superhet_af_t a)
{
	fd6818sh_set_af(c, a == SUPERHET_AF_FM ? FD6818SH_AF_RX : a == SUPERHET_AF_DMR ? FD6818SH_AF_DMR : FD6818SH_AF_MUTE);
}

static void af_gain(void *c, uint8_t g) { fd6818sh_set_af_gain(c, g); }

static void tx_prepare(void *c, superhet_tone_t t, uint16_t v, uint32_t hz, uint8_t pa_gain)
{
	fd6818sh_tx_prepare(c, tone(t), v, hz, pa_gain);
}

static void tx_tune(void *c, uint32_t hz) { fd6818sh_tx_tune(c, hz); }
static void tx_end(void *c) { fd6818sh_tx_end(c); }
static bool tail(void *c, bool on) { return fd6818sh_tx_tail(c, on); }
static void tone_start(void *c, uint16_t hz, uint8_t gain) { fd6818sh_tone_start(c, hz, gain); }
static bool tone_on(void *c) { return ((fd6818sh_t *)c)->tone_hz != 0; }
static void tone_key(void *c, bool down) { fd6818sh_tone_key(c, down); }
static void dtmf(void *c, uint16_t lo, uint16_t hi) { fd6818sh_dtmf_start(c, lo, hi); }

// The digital set writes the stock's own 0x53 for the modem, so g isn't used: passing it to fd6818sh_set_af_gain() would
// replace the volume the analog set restores
static void dmr_enter(void *c, uint8_t g)
{
	(void)g;
	fd6818sh_dmr_mode(c);
}

static void dmr_leave(void *c) { fd6818sh_analog_mode(c); }
static void dmr_tx_prepare(void *c, uint8_t pa_gain) { fd6818sh_dmr_tx_prepare(c, pa_gain); }
static void burst(void *c, uint32_t hz, bool tx) { fd6818sh_burst_tune(c, hz, tx); }
static void gpio(void *c, uint8_t pin, bool high) { fd6818sh_gpio(c, pin, high); }

const superhet_main_ops_t fd6818sh_superhet_main = {
	.idle = idle, .rx = rx, .to_if = to_if, .bandwidth = bandwidth, .rx_tone = rx_tone, .af = af, .af_gain = af_gain,
	.tx_prepare = tx_prepare, .tx_tune = tx_tune, .tx_end = tx_end, .tail = tail, .tone = tone_start,
	.tone_on = tone_on, .tone_key = tone_key, .dtmf = dtmf, .dmr_enter = dmr_enter, .dmr_leave = dmr_leave,
	.dmr_tx_prepare = dmr_tx_prepare, .burst = burst, .gpio = gpio,
};

static void lo_tune(void *c, uint32_t hz) { fd6818sh_lo_tune(c, hz); }
static void lo_off(void *c) { fd6818sh_lo_off(c); }
static void lo_resume(void *c) { fd6818sh_lo_resume(c); }

const superhet_lo_ops_t fd6818sh_superhet_lo = {
	.tune = lo_tune, .off = lo_off, .resume = lo_resume, .gpio = gpio,
};
