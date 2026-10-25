// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 FM receive audio (fm_rx_audio.h, modem-hrc7000.md 8.5). In FM the modem writes its demodulated audio into
// two 256-sample halves of its RX RAM in turn and raises system interrupt bit 14 as each fills (every 32 ms). The DMR
// glue owns that interrupt and calls half_ready for the bit; it copies the half into the ring. The radio task polls
// every 25 ms, so the ring holds four halves (128 ms) for a late poll. The ring comes from the RTOS heap only while
// APRS listens: static RAM is short on these radios.
#include "config.h"
#if CONFIG_APRS_RX
#include "fm_rx_audio.h"
#include "dmr_glue.h"
#include "platform.h"
#include "FreeRTOS.h"

#define RING_HALVES	4u
#define RING		(RING_HALVES * HRC7000_FM_RX_SAMPLES)	// samples, a power of two

static hrc7000_t *modem;
static int16_t *ring;
static volatile uint16_t head, tail;	// free running, in samples: the interrupt moves head, the task tail
static volatile uint32_t halves, dropped;

static HRC_RAMFUNC void half_ready(void)
{
	if ((uint16_t)(head - tail) > RING - HRC7000_FM_RX_SAMPLES) {
		dropped++;	// the radio task hasn't read: keep what is there, lose this half
		return;
	}
	if (hrc7000_fm_rx_audio(modem, &ring[head & (RING - 1u)]) != 0) {
		head = (uint16_t)(head + HRC7000_FM_RX_SAMPLES);
		halves++;
	}
}

void hrc_fm_rx_attach(hrc7000_t *m)
{
	modem = m;
}

bool hrc_fm_rx_start(void)
{
	if (modem == NULL || ring != NULL) {
		return false;
	}
	ring = pvPortMalloc(RING * sizeof(int16_t));
	if (ring == NULL) {
		return false;
	}
	head = tail = 0;
	if (!hrc_dmr_fm_rx_hook(half_ready)) {
		vPortFree(ring);
		ring = NULL;
		return false;
	}
	hrc7000_fm_rx_audio_irq(modem, true);
	return true;
}

void hrc_fm_rx_stop(void)
{
	if (ring == NULL) {
		return;
	}
	hrc7000_fm_rx_audio_irq(modem, false);
	hrc_dmr_fm_rx_hook(NULL);	// after this the interrupt no longer touches the ring
	vPortFree(ring);
	ring = NULL;
}

uint16_t hrc_fm_rx_read(int16_t *buf, uint16_t max)
{
	if (ring == NULL) {
		return 0;
	}
	uint16_t t = tail, n = (uint16_t)(head - t);
	if (n > max) {
		n = max;
	}
	for (uint16_t i = 0; i < n; i++) {
		buf[i] = ring[(uint16_t)(t + i) & (RING - 1u)];
	}
	hrc_barrier();
	tail = (uint16_t)(t + n);
	return n;
}

void hrc_fm_rx_stats(uint32_t *h, uint32_t *d)
{
	*h = halves;
	*d = dropped;
}
#endif
