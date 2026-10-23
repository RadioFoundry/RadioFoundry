// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The VOX mic level (drivers/audio/miclevel): the log scale the STM32 boards' analog taps use, the peak of a PCM
// block from its mean, the envelope, and where tones of known level land against the core's VOX threshold
// (100 - 8 x sensitivity, radio-features.md 3.4)
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "audio/miclevel/miclevel.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define HALF	80	// the HR_C7000's 10 ms mic half at 8 kHz
#define DECAY	((int)MICLEVEL_DECAY)

// The analog-tap math two STM32 boards each kept a copy of before they called miclevel_log(x, 12)
// (in their power code), kept here so the shared scale can't drift from what they computed
static uint8_t board_scale(uint32_t x)
{
	if (x == 0) {
		return 0;
	}
	uint32_t msb = 31u - (uint32_t)__builtin_clz(x);
	uint32_t log2_q8 = msb * 256u + ((x << 8) >> msb) - 256u;
	uint32_t level = log2_q8 * 100u / (12u * 256u);
	return (uint8_t)(level > 100u ? 100u : level);
}

static void sine(int16_t *b, int n, double amp, double hz, int16_t dc, int phase0)
{
	for (int i = 0; i < n; i++) {
		double v = amp * sin(2.0 * M_PI * hz * (double)(i + phase0) / 8000.0) + dc;
		b[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

static uint8_t threshold(uint8_t sensitivity)
{
	return (uint8_t)(100u - 8u * sensitivity);
}

static void test_log_scale(void)
{
	// The same scale as the boards' analog taps, over the whole 12-bit range their ADC average can read
	for (uint32_t x = 0; x < 4096; x++) {
		CHECK(miclevel_log(x, 12) == board_scale(x));
	}
	// Values the boards computed, edge cases included (0: silence, 4095: full scale)
	static const struct { uint16_t x; uint8_t level; } known[] = {
		{ 0, 0 }, { 1, 0 }, { 2, 8 }, { 3, 12 }, { 16, 33 }, { 100, 54 }, { 256, 66 }, { 1000, 82 },
		{ 2048, 91 }, { 3000, 95 }, { 4094, 99 }, { 4095, 99 },
	};
	for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
		CHECK(miclevel_log(known[i].x, 12) == known[i].level);
	}
	CHECK(miclevel_log(0xFFFFFFFFu, 12) == 100);	// clamped
	CHECK(miclevel_log(5, 0) == 0);
	// Monotonic, and one bit (6 dB) is 100 / 12 steps
	for (uint32_t x = 1; x < 32768; x++) {
		CHECK(miclevel_log(x + 1, 12) >= miclevel_log(x, 12));
	}
	CHECK(miclevel_log(2048, 12) - miclevel_log(1024, 12) == 8 || miclevel_log(2048, 12) - miclevel_log(1024, 12) == 9);
	// Full-scale PCM peak is the top of the scale; 8 counts and below is silence
	CHECK(miclevel_of_peak(32767) == 99);
	CHECK(miclevel_of_peak(7) == 0);
	CHECK(miclevel_of_peak(0) == 0);
}

static void test_peak(void)
{
	int16_t b[HALF] = { 0 };
	CHECK(miclevel_peak(b, 0) == 0);
	for (int i = 0; i < HALF; i++) {
		b[i] = 1234;
	}
	CHECK(miclevel_peak(b, HALF) == 0);	// DC alone is no sound
	sine(b, HALF, 1000.0, 1000.0, 0, 0);
	CHECK(miclevel_peak(b, HALF) >= 990 && miclevel_peak(b, HALF) <= 1001);
	sine(b, HALF, 1000.0, 1000.0, -6000, 0);	// an ADC offset: the same peak
	CHECK(miclevel_peak(b, HALF) >= 990 && miclevel_peak(b, HALF) <= 1001);
	b[0] = INT16_MIN;	// one full-scale negative spike against a mean near the offset
	CHECK(miclevel_peak(b, HALF) > 26000);
	for (int i = 0; i < HALF; i++) {
		b[i] = (int16_t)(i & 1 ? 32767 : -32768);	// the widest swing: clamped to 32767
	}
	CHECK(miclevel_peak(b, HALF) == 32767);
}

static void test_envelope(void)
{
	miclevel_t m = { 0 };
	int16_t loud[HALF], quiet[HALF] = { 0 };
	sine(loud, HALF, 8000.0, 700.0, 0, 0);
	uint8_t loud_level = miclevel_of_peak(miclevel_peak(loud, HALF));

	miclevel_block(&m, loud, HALF);
	CHECK(m.level == loud_level);	// attack: at once
	CHECK(m.blocks == 1);
	CHECK(m.peak == miclevel_peak(loud, HALF));
	miclevel_block(&m, quiet, HALF);
	CHECK(m.level == loud_level - DECAY);	// release: MICLEVEL_DECAY per block
	CHECK(m.peak == 0);
	for (int i = 0; i < 40; i++) {
		miclevel_block(&m, quiet, HALF);
	}
	CHECK(m.level == 0);	// down to silence, never below
	CHECK(m.blocks == 42);
	// Falling onto a level a little below: it stops there, doesn't undershoot
	miclevel_block(&m, loud, HALF);
	int16_t mid[HALF];
	sine(mid, HALF, 7000.0, 700.0, 0, 0);
	uint8_t mid_level = miclevel_of_peak(miclevel_peak(mid, HALF));
	CHECK(mid_level < loud_level && loud_level - mid_level <= DECAY);
	miclevel_block(&m, mid, HALF);
	CHECK(m.level == mid_level);
	miclevel_reset(&m);
	CHECK(m.level == 0 && m.peak == 0 && m.blocks == 44);	// the count keeps running for diagnostics
}

// Tones of known level against the core's threshold. The scale puts 0 dBFS at 100 and 0.72 dB per step, so
// sensitivity 5 (threshold 60) needs a peak about 29 dB below full scale, sensitivity 10 (20) about 58 dB below
static void test_vox_thresholds(void)
{
	struct { double dbfs; uint8_t sens; int keys; } cases[] = {
		{ -4.0, 1, 1 },		// level 1 (92) needs a voice near full scale
		{ -7.0, 1, 0 },
		{ -20.0, 5, 1 },	// the default sensitivity: ordinary speech into a mic set for it
		{ -40.0, 5, 0 },	// room noise
		{ -50.0, 10, 1 },	// the most sensitive setting
		{ -66.0, 10, 0 },
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		int16_t b[HALF];
		sine(b, HALF, 32767.0 * pow(10.0, cases[i].dbfs / 20.0), 1000.0, 300, 0);
		miclevel_t m = { 0 };
		miclevel_block(&m, b, HALF);
		int keys = m.level >= threshold(cases[i].sens);
		if (keys != cases[i].keys) {
			printf("  %.0f dBFS: level %u, threshold %u\n", cases[i].dbfs, m.level, threshold(cases[i].sens));
		}
		CHECK(keys == cases[i].keys);
	}
	// A 25 ms poll between 10 ms halves: speech that stopped 20 ms ago still reads within 8 steps (about 6 dB) of it,
	// so two polls in a row see the syllable
	int16_t v[HALF], q[HALF] = { 0 };
	sine(v, HALF, 32767.0 * pow(10.0, -20.0 / 20.0), 300.0, 0, 0);
	miclevel_t m = { 0 };
	miclevel_block(&m, v, HALF);
	uint8_t at_voice = m.level;
	miclevel_block(&m, q, HALF);
	miclevel_block(&m, q, HALF);
	CHECK(at_voice - m.level == 2 * DECAY);
}

int main(void)
{
	test_log_scale();
	test_peak();
	test_envelope();
	test_vox_thresholds();
	if (failures) {
		printf("miclevelTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("miclevelTest: all passed\n");
	return 0;
}
