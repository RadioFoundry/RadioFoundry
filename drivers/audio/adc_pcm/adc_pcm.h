// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Audio sampled by the MCU's own ADC, as signed 16-bit PCM: an interrupt puts each raw reading into a ring, and a task
// reads them back as PCM with the bias removed. For receive audio that reaches an ADC pin biased at mid-scale (the
// APRS demodulator's input, hal_afsk.h rx_read).
//
// One writer (the sampling interrupt) and one reader (a task): the writer owns head, the reader tail, so neither
// needs a lock. A full ring drops the new reading and counts it, so the reader always gets an unbroken run up to the
// overrun.
//
// The bias is tracked, not assumed: a one-pole low pass of the readings (time constant ADC_PCM_DC_SHIFT bits of
// samples, about 0.1 s at 9.6 kHz, so its corner is near 1.5 Hz) is subtracted, so a divider or a part whose mid-scale
// isn't the ADC's still reads as centered audio. A 12-bit reading spans the 16-bit range (times 16).
//
// Header only and allocation free, so adc_pcm_put() can run in the interrupt.
#ifndef ADC_PCM_H
#define ADC_PCM_H

#include <stdint.h>
#include <stdbool.h>

#define ADC_PCM_DC_SHIFT	10u	// the bias tracker's time constant: 1024 samples
#define ADC_PCM_FRAC		8u	// the bias tracker's fraction bits

typedef struct {
	volatile uint16_t *buf;		// len readings, len a power of two
	uint16_t mask;
	volatile uint16_t head;		// the next slot the writer fills
	volatile uint16_t tail;		// the next slot the reader takes
	volatile uint32_t dropped;	// readings lost to a full ring
	int32_t dc;			// the bias, in readings << ADC_PCM_FRAC
	uint8_t shift;			// readings << shift is 16-bit PCM
} adc_pcm_t;

// buf holds len readings (a power of two, 2..32768) of bits each (8..16); mid is the bias to start from (mid-scale)
static inline void adc_pcm_init(adc_pcm_t *p, uint16_t *buf, uint16_t len, uint8_t bits, uint16_t mid)
{
	p->buf = buf;
	p->mask = (uint16_t)(len - 1u);
	p->head = 0;
	p->tail = 0;
	p->dropped = 0;
	p->dc = (int32_t)mid << ADC_PCM_FRAC;
	p->shift = (uint8_t)(16u - bits);
}

// Forget what is buffered (the reader's side: call it from the reading task, not the interrupt)
static inline void adc_pcm_flush(adc_pcm_t *p)
{
	p->tail = p->head;
}

// One reading (the interrupt)
static inline void adc_pcm_put(adc_pcm_t *p, uint16_t raw)
{
	uint16_t h = p->head;
	uint16_t next = (uint16_t)((h + 1u) & p->mask);
	if (next == p->tail) {
		p->dropped++;
		return;
	}
	p->buf[h] = raw;
	p->head = next;		// after the store: the reader sees the slot filled once head moves past it
}

// Readings waiting
static inline uint16_t adc_pcm_count(const adc_pcm_t *p)
{
	return (uint16_t)((p->head - p->tail) & p->mask);
}

// Up to max readings as PCM with the bias taken out; returns how many (0 when none)
static inline uint16_t adc_pcm_read(adc_pcm_t *p, int16_t *out, uint16_t max)
{
	uint16_t t = p->tail;
	uint16_t h = p->head;
	uint16_t n = 0;
	while (t != h && n < max) {
		int32_t x = (int32_t)p->buf[t] << ADC_PCM_FRAC;
		p->dc += (x - p->dc) >> ADC_PCM_DC_SHIFT;
		int32_t v = ((x - p->dc) * (int32_t)(1u << p->shift)) >> ADC_PCM_FRAC;
		out[n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		t = (uint16_t)((t + 1u) & p->mask);
	}
	p->tail = t;
	return n;
}

#endif
