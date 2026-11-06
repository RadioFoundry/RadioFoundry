// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT-D578UV's PB9 samples (tone_synth.h, hardware-d578uv.md 5.3): APRS tones made at the sample interrupt's rate
// decode through the core's demodulator, keep the baud rate though a symbol isn't a whole number of samples, stay
// within the PWM's range and change tone without a phase step
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tone_synth.h"
#include "aprs/afsk.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The board's figures: 120 MHz, a sample every 4592 clocks (26,132 Hz)
#define CLOCK_HZ	120000000u
#define SAMPLE_CLOCKS	4592u
#define RATE_HZ		(CLOCK_HZ / SAMPLE_CLOCKS)

static uint8_t want[64];
static size_t want_len;
static int frames;

static void on_frame(const uint8_t *frame, size_t n, void *ctx)
{
	(void)ctx;
	if (n == want_len && memcmp(frame, want, n) == 0) {
		frames++;
	}
}

// A frame (any bytes: the demodulator checks the FCS) with a preamble and a tail, as tones
static uint16_t make_tones(uint8_t *tones, size_t bytes)
{
	for (size_t i = 0; i < sizeof(want); i++) {
		want[i] = (uint8_t)(i * 37u + 11u);
	}
	want_len = sizeof(want);
	hdlc_enc_t e;
	hdlc_enc_init(&e, tones, bytes);
	CHECK(hdlc_enc_flags(&e, 20) && hdlc_enc_frame(&e, want, want_len) && hdlc_enc_flags(&e, 3));
	return (uint16_t)e.nbits;
}

// Every sample of a transmission as signed PCM; the count
static size_t synth(const uint8_t *tones, uint16_t nbits, uint8_t amp, int16_t *pcm, size_t max)
{
	afsk_tx_t t;
	afsk_tx_init(&t, tones, nbits, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ, amp, SAMPLE_CLOCKS, CLOCK_HZ);
	size_t n = 0;
	int32_t d;
	while ((d = afsk_tx_next(&t)) >= 0 && n < max) {
		CHECK(d >= 1 && d <= 255);
		pcm[n++] = (int16_t)((d - 128) * 256);
	}
	return n;
}

static void test_round_trip(void)
{
	static uint8_t tones[256];
	static int16_t pcm[60000];
	uint16_t nbits = make_tones(tones, sizeof(tones));
	static const uint8_t amps[] = { 127, 90, 30, 8 };
	for (size_t a = 0; a < sizeof(amps); a++) {
		size_t n = synth(tones, nbits, amps[a], pcm, sizeof(pcm) / sizeof(pcm[0]));
		// The symbols take nbits / 1200 s within one sample
		uint64_t clocks = (uint64_t)nbits * (CLOCK_HZ / AFSK_BAUD);
		size_t expect = (size_t)((clocks + SAMPLE_CLOCKS - 1u) / SAMPLE_CLOCKS);
		CHECK(n + 1u >= expect && n <= expect + 1u);
		afsk_demod_t d;
		CHECK(afsk_demod_init(&d, RATE_HZ, on_frame, NULL));
		frames = 0;
		afsk_demod(&d, pcm, n);
		static const int16_t quiet[400];
		afsk_demod(&d, quiet, sizeof(quiet) / sizeof(quiet[0]));
		CHECK(frames == 1);
	}
}

// A tone change keeps the phase: no step between samples beyond what the higher tone moves in one
static void test_phase(void)
{
	static uint8_t tones[256];
	uint16_t nbits = make_tones(tones, sizeof(tones));
	afsk_tx_t t;
	afsk_tx_init(&t, tones, nbits, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ, 127, SAMPLE_CLOCKS, CLOCK_HZ);
	// 2 pi x 2200 / 26132 of 127, and a count for the table's rounding
	int32_t limit = (int32_t)(2.0 * 3.14159265 * AFSK_SPACE_HZ / RATE_HZ * 127.0) + 3;
	int32_t prev = afsk_tx_next(&t), d;
	while ((d = afsk_tx_next(&t)) >= 0) {
		CHECK(abs(d - prev) <= limit);
		prev = d;
	}
}

static void test_edges(void)
{
	uint8_t one = 0xFF;
	afsk_tx_t t;
	afsk_tx_init(&t, &one, 0, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ, 200, SAMPLE_CLOCKS, CLOCK_HZ);
	CHECK(afsk_tx_next(&t) == -1);		// nothing to send
	CHECK(t.amp == 127);			// clamped to the PWM's range
	CHECK(tone_step(1200, SAMPLE_CLOCKS, CLOCK_HZ) == 3009);	// 1200 x 65536 / 26132.4
	CHECK(tone_sine(0x4000, 255) == 255 && tone_sine(0xC000, 255) == -255 && tone_sine(0, 255) == 0);
}

int main(void)
{
	test_round_trip();
	test_phase();
	test_edges();
	if (failures) {
		printf("d578uvToneSynthTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("d578uvToneSynthTest: all passed\n");
	return 0;
}
