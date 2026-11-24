// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DTMF receive (core/radio/dtmf_rx.c): synthesized digits, noise and speech-like sound, at the receive paths' rates
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/dtmf_rx.h"
#include "hal_dtmf.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define MAX_SAMPLES	200000

static int16_t pcm[MAX_SAMPLES];
static size_t pcm_n;
static uint32_t fs;
static uint32_t rng = 12345;

static double noise(void)
{
	rng = rng * 1103515245u + 12345u;
	return ((double)((rng >> 8) & 0xFFFF) / 32768.0) - 1.0;	// -1 .. 1, flat
}

static void clear(uint32_t rate)
{
	fs = rate;
	pcm_n = 0;
}

static void put(double v)
{
	if (v > 32767.0) {
		v = 32767.0;
	} else if (v < -32768.0) {
		v = -32768.0;
	}
	if (pcm_n < MAX_SAMPLES) {
		pcm[pcm_n++] = (int16_t)lrint(v);
	}
}

// A tone pair: amplitudes a_lo and a_hi, the frequencies scaled by (1 + off_lo) and (1 + off_hi)
static void pair(char digit, double ms, double a_lo, double a_hi, double off_lo, double off_hi)
{
	uint16_t lo, hi;
	if (!dtmf_pair(digit, &lo, &hi)) {
		return;
	}
	size_t n = (size_t)(ms * fs / 1000.0);
	for (size_t k = 0; k < n; k++) {
		double t = (double)k / fs;
		put(a_lo * sin(2 * M_PI * lo * (1 + off_lo) * t) + a_hi * sin(2 * M_PI * hi * (1 + off_hi) * t + 0.7));
	}
}

static void digit(char c, double ms)
{
	pair(c, ms, 4000, 4000, 0, 0);
}

static void silence(double ms)
{
	size_t n = (size_t)(ms * fs / 1000.0);
	for (size_t k = 0; k < n; k++) {
		put(0);
	}
}

// Add noise of this amplitude over everything so far, plus a DC offset
static void add_noise(double amp, double dc)
{
	for (size_t k = 0; k < pcm_n; k++) {
		double v = pcm[k] + amp * noise() + dc;
		pcm[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

// Decode what was built, fed in chunks of chunk samples (0 = random sizes): the digits as a string
static const char *decode(size_t chunk)
{
	static char got[128];
	dtmf_rx_t d;
	dtmf_rx_init(&d, fs);
	size_t n = 0, at = 0;
	while (at < pcm_n) {
		size_t c = chunk != 0 ? chunk : 1 + (size_t)((noise() + 1.0) * 120);
		if (c > pcm_n - at) {
			c = pcm_n - at;
		}
		n += dtmf_rx_feed(&d, pcm + at, c, got + n, sizeof(got) - 1 - n);
		at += c;
	}
	got[n] = '\0';
	return got;
}

static const uint32_t rates[] = { 8000, 9146 };

// Every digit, as a radio sends them (100 ms tones, 60 ms gaps), whatever the chunk size
static void test_all_digits(void)
{
	static const char all[] = "123A456B789C*0#D";
	for (unsigned r = 0; r < 2; r++) {
		clear(rates[r]);
		silence(50);
		for (const char *p = all; *p; p++) {
			digit(*p, 100);
			silence(60);
		}
		CHECK(strcmp(decode(160), all) == 0, "%u Hz: '%s'", (unsigned)fs, decode(160));
		CHECK(strcmp(decode(1), all) == 0, "%u Hz, one sample at a time: '%s'", (unsigned)fs, decode(1));
		CHECK(strcmp(decode(0), all) == 0, "%u Hz, random chunks: '%s'", (unsigned)fs, decode(0));
	}
}

// Durations: 40 ms tones with 40 ms gaps decode (a repeated digit counts each time), 20 ms tones don't
static void test_timing(void)
{
	for (unsigned r = 0; r < 2; r++) {
		for (int shift = 0; shift < 26; shift += 5) {	// where the tones fall against the windows
			clear(rates[r]);
			silence(30 + shift);
			for (int i = 0; i < 4; i++) {
				digit('5', 40);
				silence(40);
			}
			digit('9', 40);
			silence(60);
			CHECK(strcmp(decode(160), "55559") == 0, "%u Hz, shift %d: '%s'", (unsigned)fs, shift, decode(160));

			clear(rates[r]);
			silence(30 + shift);
			for (int i = 0; i < 6; i++) {
				digit('7', 20);
				silence(60);
			}
			CHECK(strcmp(decode(160), "") == 0, "%u Hz, 20 ms tones: '%s'", (unsigned)fs, decode(160));
		}
		// A long press is one digit
		clear(rates[r]);
		digit('#', 1500);
		silence(100);
		CHECK(strcmp(decode(160), "#") == 0, "%u Hz, held: '%s'", (unsigned)fs, decode(160));
	}
}

// Level and twist
static void test_level_twist(void)
{
	clear(8000);
	pair('3', 100, 200, 200, 0, 0);		// under DTMF_RX_MIN_AMP
	silence(60);
	pair('4', 100, 600, 600, 0, 0);
	silence(60);
	pair('6', 100, 16000, 16000, 0, 0);	// near full scale
	silence(60);
	pair('8', 100, 2000, 4000, 0, 0);	// +6 dB twist
	silence(60);
	pair('2', 100, 4000, 2000, 0, 0);	// -6 dB
	silence(60);
	pair('1', 100, 1000, 4000, 0, 0);	// +12 dB: refused
	silence(60);
	pair('0', 100, 4000, 1000, 0, 0);	// -12 dB: refused
	silence(60);
	CHECK(strcmp(decode(160), "4682") == 0, "'%s'", decode(160));
}

// Frequency: 1.5 % off (Q.24's acceptance) still decodes, 5 % off doesn't. A sender makes both tones from one clock,
// so they are off the same way
static void test_frequency(void)
{
	for (unsigned r = 0; r < 2; r++) {
		clear(rates[r]);
		pair('1', 100, 4000, 4000, 0.015, 0.015);
		silence(60);
		pair('5', 100, 4000, 4000, -0.015, -0.015);
		silence(60);
		pair('9', 100, 4000, 4000, 0.015, 0.015);
		silence(60);
		pair('D', 100, 4000, 4000, -0.015, -0.015);
		silence(60);
		pair('2', 100, 4000, 4000, 0.05, 0.05);
		silence(60);
		pair('8', 100, 4000, 4000, -0.05, -0.05);
		silence(60);
		CHECK(strcmp(decode(160), "159D") == 0, "%u Hz: '%s'", (unsigned)fs, decode(160));
	}
}

// Noise, a DC offset, one tone alone
static void test_noise(void)
{
	for (unsigned r = 0; r < 2; r++) {
		clear(rates[r]);
		silence(5000);
		add_noise(8000, 0);
		CHECK(strcmp(decode(160), "") == 0, "%u Hz, noise: '%s'", (unsigned)fs, decode(160));

		// Digits in noise 10 dB under each tone, on a DC offset
		clear(rates[r]);
		const char *s = "*0472#";
		for (const char *p = s; *p; p++) {
			digit(*p, 80);
			silence(60);
		}
		add_noise(4000 / sqrt(10.0) * sqrt(3.0), 2500);	// flat noise of this peak has power peak^2 / 3
		CHECK(strcmp(decode(160), s) == 0, "%u Hz, in noise: '%s'", (unsigned)fs, decode(160));

		clear(rates[r]);
		for (int i = 0; i < 4; i++) {
			pair('5', 300, 6000, 0, 0, 0);	// the low tone only
			silence(60);
			pair('5', 300, 0, 6000, 0, 0);	// the high one only
			silence(60);
		}
		CHECK(strcmp(decode(160), "") == 0, "%u Hz, one tone: '%s'", (unsigned)fs, decode(160));
	}
}

// Speech-like sound, the usual source-filter model of a vowel: a gliding fundamental whose harmonics fall 6 dB an
// octave (the glottal source with the lips' radiation), shaped by three resonant formants that move about. Its
// harmonics cross the DTMF frequencies again and again
static double formant(double f, double center, double bw)
{
	double x = (f - center) / (bw / 2);
	return 1 / sqrt(1 + x * x);
}

static void test_speech(void)
{
	for (unsigned r = 0; r < 2; r++) {
		for (int voice = 0; voice < 3; voice++) {
			static const double base[3] = { 110, 160, 230 };	// low, middle, high voices
			clear(rates[r]);
			double ph = 0;
			size_t n = (size_t)(20.0 * fs);
			if (n > MAX_SAMPLES) {
				n = MAX_SAMPLES;
			}
			for (size_t k = 0; k < n; k++) {
				double t = (double)k / fs;
				double f0 = base[voice] * (1 + 0.25 * sin(2 * M_PI * 0.7 * t) + 0.1 * sin(2 * M_PI * 3.1 * t));
				double f1 = 650 + 250 * sin(2 * M_PI * 1.3 * t), f2 = 1500 + 500 * sin(2 * M_PI * 0.9 * t + 1);
				ph += 2 * M_PI * f0 / fs;
				double v = 0;
				for (int h = 1; h * f0 < 3400; h++) {
					double f = h * f0;
					double g = (formant(f, f1, 90) + 0.6 * formant(f, f2, 120) + 0.3 * formant(f, 2500, 150)) / h;
					v += g * sin(h * ph);
				}
				put(6000 * v);
			}
			add_noise(300, 0);
			CHECK(strcmp(decode(160), "") == 0, "%u Hz, speech-like voice %d: '%s'", (unsigned)fs, voice,
			      decode(160));
		}
	}
}

// A reset in the middle of a digit forgets it: the same digit after it is new
static void test_reset(void)
{
	clear(8000);
	digit('6', 200);
	dtmf_rx_t d;
	dtmf_rx_init(&d, fs);
	char out[8];
	size_t n = dtmf_rx_feed(&d, pcm, pcm_n / 2, out, sizeof(out));
	CHECK(n == 1 && out[0] == '6', "first half: %u", (unsigned)n);
	dtmf_rx_reset(&d);
	n = dtmf_rx_feed(&d, pcm + pcm_n / 2, pcm_n - pcm_n / 2, out, sizeof(out));
	CHECK(n == 1 && out[0] == '6', "after the reset: %u", (unsigned)n);
	CHECK(d.digits == 2, "%u", (unsigned)d.digits);
}

int main(void)
{
	test_all_digits();
	test_timing();
	test_level_twist();
	test_frequency();
	test_noise();
	test_speech();
	test_reset();
	if (failures) {
		printf("# dtmfRxTest: FAILED %d\n", failures);
		return 1;
	}
	printf("# dtmfRxTest: PASSED\n");
	return 0;
}
