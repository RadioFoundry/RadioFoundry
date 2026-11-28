// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ADC audio as PCM (drivers/audio/adc_pcm): the ring's order and overrun, the bias tracking, and a packet sampled the
// way a board feeds the APRS demodulator from a 12-bit ADC pin (biased at mid-scale, read in the radio task's chunks)
// decoding through the core's AFSK receiver
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "audio/adc_pcm/adc_pcm.h"
#include "aprs/afsk.h"
#include "aprs/ax25.h"
#include "aprs/hdlc.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define RATE		9600u	// the rate a board samples at for the demodulator
#define CHUNK		160u	// the core's read buffer (core/aprs/aprs.c rxbuf)

static void test_ring(void)
{
	uint16_t buf[8];
	adc_pcm_t p;
	int16_t out[16];
	adc_pcm_init(&p, buf, 8, 12, 2048);
	CHECK(adc_pcm_count(&p) == 0 && adc_pcm_read(&p, out, 16) == 0);
	// Seven fit (one slot stays empty), the eighth is dropped and counted
	for (uint16_t i = 0; i < 8; i++) {
		adc_pcm_put(&p, (uint16_t)(2048u + i));
	}
	CHECK(adc_pcm_count(&p) == 7 && p.dropped == 1);
	// In order: each a step of 16 above the last, less the little the bias tracker has followed them
	CHECK(adc_pcm_read(&p, out, 3) == 3 && out[0] == 0 && out[1] == 16 && out[2] == 32);
	CHECK(adc_pcm_read(&p, out, 16) == 4 && out[0] == 48 && out[3] >= 94 && out[3] <= 96);
	// Wrapping
	for (uint16_t i = 0; i < 5; i++) {
		adc_pcm_put(&p, 2048);
	}
	CHECK(adc_pcm_count(&p) == 5 && adc_pcm_read(&p, out, 16) == 5);
	adc_pcm_put(&p, 2048);
	adc_pcm_flush(&p);
	CHECK(adc_pcm_count(&p) == 0 && adc_pcm_read(&p, out, 16) == 0);
	// The full scale reaches the 16-bit ends without wrapping
	adc_pcm_init(&p, buf, 8, 12, 2048);
	adc_pcm_put(&p, 4095);
	adc_pcm_put(&p, 0);
	CHECK(adc_pcm_read(&p, out, 2) == 2 && out[0] > 32000 && out[1] < -32000);
}

// A bias away from mid-scale is tracked out: after a second the mean of a tone is near 0
static void test_bias(void)
{
	static uint16_t buf[1024];
	adc_pcm_t p;
	adc_pcm_init(&p, buf, 1024, 12, 2048);
	int16_t out[CHUNK];
	double sum = 0;
	int n = 0;
	for (uint32_t i = 0; i < RATE * 2u; i++) {
		double v = 1500.0 + 400.0 * sin(2.0 * M_PI * 1000.0 * i / RATE);
		adc_pcm_put(&p, (uint16_t)lround(v));
		if (adc_pcm_count(&p) >= CHUNK) {
			uint16_t k = adc_pcm_read(&p, out, CHUNK);
			for (uint16_t j = 0; j < k; j++) {
				if (i >= RATE) {
					sum += out[j];
					n++;
				}
			}
		}
	}
	CHECK(n > 0 && fabs(sum / n) < 64.0);	// 64 of 32768: under a third of a 12-bit step after the scale
	CHECK(p.dropped == 0);
}

static uint32_t frames;
static uint8_t got[AX25_FRAME_MAX];
static size_t got_len;

static void on_frame(const uint8_t *f, size_t n, void *ctx)
{
	(void)ctx;
	frames++;
	if (n <= sizeof(got)) {
		memcpy(got, f, n);
		got_len = n;
	}
}

// One APRS packet through a 12-bit ADC: the modulator's audio at a third of full scale around a bias off mid-scale,
// with a little noise, put one reading at a time and read in the core's chunks
static void test_packet(void)
{
	const char *tnc2 = "N0CALL-9>APRS,WIDE1-1:!4903.50N/07201.75W>adc to pcm";
	ax25_frame_t f;
	uint8_t frame[AX25_FRAME_MAX];
	CHECK(ax25_from_tnc2(tnc2, strlen(tnc2), &f));
	size_t n = ax25_encode(&f, frame, sizeof(frame));
	CHECK(n > 0);
	static uint8_t tones[512];
	hdlc_enc_t e;
	hdlc_enc_init(&e, tones, sizeof(tones));
	CHECK(hdlc_enc_flags(&e, 30) && hdlc_enc_frame(&e, frame, n) && hdlc_enc_flags(&e, 4));

	static afsk_mod_t m;
	static afsk_demod_t d;
	afsk_mod_init(&m, RATE);
	CHECK(afsk_demod_init(&d, RATE, on_frame, NULL));
	static uint16_t ring[1024];
	adc_pcm_t p;
	adc_pcm_init(&p, ring, 1024, 12, 2048);
	// Half a second of the bias alone first, as when the squelch opens on a carrier
	uint32_t seed = 1;
	int16_t pcm[64], out[CHUNK];
	size_t k;
	for (uint32_t i = 0; i < RATE / 2u; i++) {
		adc_pcm_put(&p, 1900);
		if (adc_pcm_count(&p) >= CHUNK) {
			k = adc_pcm_read(&p, out, CHUNK);
			afsk_demod(&d, out, k);
		}
	}
	while ((k = afsk_mod(&m, tones, e.nbits, 10000, pcm, 64)) != 0) {
		for (size_t i = 0; i < k; i++) {
			seed = seed * 1103515245u + 12345u;
			int32_t noise = (int32_t)((seed >> 16) & 31u) - 16;
			int32_t v = 1900 + pcm[i] / 16 + noise;	// 16-bit audio to 12-bit readings
			adc_pcm_put(&p, (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v));
		}
		if (adc_pcm_count(&p) >= CHUNK) {
			k = adc_pcm_read(&p, out, CHUNK);
			afsk_demod(&d, out, k);
		}
	}
	while ((k = adc_pcm_read(&p, out, CHUNK)) != 0) {
		afsk_demod(&d, out, k);
	}
	CHECK(p.dropped == 0);
	CHECK(frames == 1 && got_len == n && memcmp(got, frame, n) == 0);
}

int main(void)
{
	test_ring();
	test_bias();
	test_packet();
	if (failures) {
		printf("adcPcmTest: %d failures\n", failures);
		return 1;
	}
	printf("adcPcmTest: all passed\n");
	return 0;
}
