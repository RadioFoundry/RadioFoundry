// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV APRS AFSK (hal_afsk.h, docs/spec/aprs.md 5).
//
// Transmit: the tones as 8 kHz PCM (afsk_mod, phase continuous) through the HR_C7000's prompt FIFO, which feeds the
// FM modulator while transmitting (radio_dm32.c dm32_afsk_tx mutes the microphone). The DTMF tone generator that
// makes the 1750 Hz burst would need no PCM, but its deviation field tops out near 1.5 kHz, short of APRS's 3 kHz.
// Timer 4 tops the FIFO up every 2 ms, in 32-sample chunks while it has room; after the last chunk it waits a few
// ticks for the FIFO to play out before tx_busy goes false.
//
// Receive: the HR_C7000 copies its demodulated FM audio to the CPU (platform fm_rx_audio.c); while APRS listens the
// FD6818's audio stays on the HR_C7000's input and the speaker plays the HR_C7000's FM output instead (radio_dm32.c,
// hardware-dm32.md 9). Needs the DMR glue's system interrupt (CONFIG_DMR).
#include "config.h"
#if CONFIG_APRS
#include "dm32.h"
#include "platform.h"
#include "fm_rx_audio.h"
#include "aprs/afsk.h"

#define TICK_US		2000u	// FIFO top-up period: 16 samples' time, half a chunk
#define DRAIN_TICKS	8u	// after the last chunk: 16 ms for what the FIFO still holds
#define AMP_PER_LEVEL	55	// PCM amplitude per level step: 50 (nominal) = 2750, about 3 kHz peak. Bench, 25 kHz:
				// 2925 gave 3.3 kHz peak; 5850 and up are limited at 5.1 kHz peak, 4.5 kHz on the tones

static afsk_mod_t mod;
static const uint8_t *tones;
static uint16_t nbits;
static int16_t amp;
static volatile bool busy;
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
		if (!dm32_prompt_push(chunk)) {
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

static bool tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	if (busy || n == 0 || baud != AFSK_BAUD || mark != AFSK_MARK_HZ || space != AFSK_SPACE_HZ) {
		return false;	// afsk_mod makes Bell 202 only
	}
	if (!dm32_afsk_tx(true)) {
		return false;
	}
	tones = t;
	nbits = n;
	int32_t a = (int32_t)level * AMP_PER_LEVEL;
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
	dm32_afsk_tx(false);
}

#if CONFIG_APRS_RX && CONFIG_DMR
static bool rx_start(void)
{
	dm32_aprs_listen(true);
	if (!hrc_fm_rx_start()) {
		dm32_aprs_listen(false);
		return false;
	}
	return true;
}

static void rx_stop(void)
{
	hrc_fm_rx_stop();
	dm32_aprs_listen(false);
}
#endif

const hal_afsk_t dm32_afsk = {
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
