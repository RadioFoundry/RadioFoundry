// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 APRS AFSK (hrc_afsk.h, hal_afsk.h, docs/spec/aprs.md 5).
//
// Transmit: the tones as 8 kHz PCM (afsk_mod, phase continuous) through the prompt FIFO, which feeds the FM modulator
// while transmitting (modem-hrc7000.md 8.4); the board's tx_route mutes the microphone. The DTMF tone generator that
// makes the 1750 Hz burst would need no PCM, but its deviation field tops out near 1.5 kHz, short of APRS's 3 kHz.
// Timer 4 tops the FIFO up every 2 ms, in 32-sample chunks while it has room; after the last chunk it waits a few
// ticks for the FIFO to play out before tx_busy goes false. Speaker PCM stays off the FIFO meanwhile
// (hrc_audio_pcm_hold): its chunks would land between the tones.
//
// Receive: the HR_C7000 copies its demodulated FM audio to the CPU (fm_rx_audio.c); the board's listen hook keeps the
// receiver's audio on the HR_C7000's input. Needs the DMR glue's system interrupt (CONFIG_DMR).
#include "config.h"
#if CONFIG_APRS
#include "hrc_afsk.h"
#include "platform.h"
#include "audio_pcm.h"
#include "fm_rx_audio.h"
#include "aprs/afsk.h"

#define TICK_US		2000u	// FIFO top-up period: 16 samples' time, half a chunk
#define DRAIN_TICKS	8u	// after the last chunk: 16 ms for what the FIFO still holds

static hrc7000_t *modem;
static const hrc_afsk_board_t *hooks;
static afsk_mod_t mod;
static const uint8_t *tones;
static uint16_t nbits;
static int16_t amp;
static volatile bool busy;
static bool routed;
static bool done;
static uint8_t drain;
static bool irq_set;

// Generate and push chunks while the FIFO has room (interrupt context, or task level before the timer starts). A chunk
// the full FIFO refused waits in chunk[] for the next tick: the modulator's phase has moved past it
static int16_t chunk[C7_PROMPT_CHUNK];
static bool pending;

static void fill(void)
{
	for (;;) {
		if (!pending) {
			if (done) {
				return;
			}
			size_t n = afsk_mod(&mod, tones, nbits, amp, chunk, C7_PROMPT_CHUNK);
			if (n == 0) {
				done = true;
				return;
			}
			for (size_t i = n; i < C7_PROMPT_CHUNK; i++) {
				chunk[i] = 0;
			}
			pending = true;
		}
		if (!hrc7000_prompt_push(modem, chunk)) {
			return;
		}
		pending = false;
	}
}

static void tick(void)
{
	hrc_timer_ack(TIMER_AFSK);
	if (!busy) {
		return;
	}
	fill();
	if (done && !pending && ++drain >= DRAIN_TICKS) {
		hrc_timer_stop(TIMER_AFSK);
		busy = false;
	}
}

void hrc_afsk_attach(hrc7000_t *m, const hrc_afsk_board_t *b)
{
	modem = m;
	hooks = b;
}

// The FIFO to the modulator: speaker PCM held off it first, so no beep chunk lands between the tones
static bool route(bool on)
{
	if (on) {
		hrc_audio_pcm_hold(true);
		routed = hooks->tx_route(true);
		if (!routed) {
			hrc_audio_pcm_hold(false);
		}
		return routed;
	}
	hooks->tx_route(false);
	hrc_audio_pcm_hold(false);
	routed = false;
	return true;
}

static bool tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	if (modem == NULL || busy || n == 0 || baud != AFSK_BAUD || mark != AFSK_MARK_HZ || space != AFSK_SPACE_HZ) {
		return false;	// afsk_mod makes Bell 202 only
	}
	if (!route(true)) {
		return false;
	}
	tones = t;
	nbits = n;
	int32_t a = (int32_t)level * hooks->amp_per_level;
	amp = (int16_t)(a > 32000 ? 32000 : a);
	afsk_mod_init(&mod, 8000u);
	done = false;
	pending = false;
	drain = 0;
	if (!irq_set) {
		hrc_irq_register(IRQ_TIMER4, tick);
		hrc_irq_enable(IRQ_TIMER4, true);
		irq_set = true;
	}
	fill();
	busy = true;
	hrc_timer_periodic(TIMER_AFSK, TICK_US);
	return true;
}

static bool tx_busy(void)
{
	return busy;
}

static void tx_stop(void)
{
	uint32_t s = hrc_irq_save();
	hrc_timer_stop(TIMER_AFSK);
	busy = false;
	hrc_irq_restore(s);
	if (routed) {
		route(false);
	}
}

#if CONFIG_APRS_RX && CONFIG_DMR
static bool rx_start(void)
{
	if (hooks->listen != NULL) {
		hooks->listen(true);
	}
	if (!hrc_fm_rx_start()) {
		if (hooks->listen != NULL) {
			hooks->listen(false);
		}
		return false;
	}
	return true;
}

static void rx_stop(void)
{
	hrc_fm_rx_stop();
	if (hooks->listen != NULL) {
		hooks->listen(false);
	}
}
#endif

const hal_afsk_t hrc_afsk = {
	.tx_start = tx_start,
	.tx_busy = tx_busy,
	.tx_stop = tx_stop,
#if CONFIG_APRS_RX && CONFIG_DMR
	.rx_rate_hz = HRC7000_FM_RX_RATE,
	.rx_start = rx_start,
	.rx_read = hrc_fm_rx_read,
	.rx_stop = rx_stop,
#endif
};
#endif
