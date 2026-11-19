// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Signaling tones (core/radio/sigtone.c): which outputs sound where, the presets, the length cap and the sample
// generator (length, silence, no samples after the end, the pitch)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "radio/sigtone.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_out(void)
{
	CHECK(!sig_out_local(SIG_OUT_DEFAULT) && !sig_out_local(SIG_OUT_OFF) && sig_out_local(SIG_OUT_LOCAL) &&
	      !sig_out_local(SIG_OUT_AIR) && sig_out_local(SIG_OUT_BOTH));
	CHECK(sig_out_air(SIG_OUT_AIR, SIG_EV_BEGIN) && sig_out_air(SIG_OUT_BOTH, SIG_EV_END) &&
	      !sig_out_air(SIG_OUT_LOCAL, SIG_EV_END) && !sig_out_air(SIG_OUT_DEFAULT, SIG_EV_BEGIN));
	// nothing is sent when a reception ends
	CHECK(!sig_out_air(SIG_OUT_AIR, SIG_EV_IDLE) && !sig_out_air(SIG_OUT_BOTH, SIG_EV_IDLE));
	CHECK(sig_out_clean(SIG_OUT_AIR, SIG_EV_IDLE) == SIG_OUT_DEFAULT && sig_out_clean(SIG_OUT_BOTH, SIG_EV_IDLE) == SIG_OUT_DEFAULT);
	CHECK(sig_out_clean(SIG_OUT_BOTH, SIG_EV_END) == SIG_OUT_BOTH && sig_out_clean(SIG_OUT_LOCAL, SIG_EV_IDLE) == SIG_OUT_LOCAL);
	CHECK(sig_out_clean(200, SIG_EV_BEGIN) == SIG_OUT_DEFAULT && sig_out_clean(SIG_OUT_COUNT, SIG_EV_END) == SIG_OUT_DEFAULT);
}

// What the radio core and the audio side take as the steps' count, length and "a chain is set"
static bool chain_set(const uint8_t ms10[SIG_STEPS])
{
	return ms10[0] != 0;
}

static uint32_t total_ms(const beep_seg_t *s, uint8_t n)
{
	uint32_t t = 0;
	for (uint8_t i = 0; i < n; i++) {
		t += s[i].ms;
	}
	return t;
}

static uint32_t gen_samples(const beep_seg_t *s, uint8_t n)
{
	return total_ms(s, n) * 8;
}

static bool gen_done(const beep_gen_t *g)
{
	return g->i >= g->n;
}

static void test_chain(void)
{
	beep_seg_t s[SIG_STEPS];
	uint16_t hz[SIG_STEPS] = { 0 };
	uint8_t ms[SIG_STEPS] = { 0 };

	// no step: the event's standard sound, whatever the frequencies say
	CHECK(!chain_set(ms));
	hz[0] = 1234;
	uint8_t n = sig_chain(hz, ms, SIG_EV_BEGIN, s);
	CHECK(n == 3 && s[0].hz == 1600 && s[1].hz == 0 && s[2].hz == 1600);
	n = sig_chain(hz, ms, SIG_EV_END, s);
	CHECK(n == 3 && s[0].hz == 1200 && s[2].hz == 900);
	n = sig_chain(hz, ms, SIG_EV_IDLE, s);
	CHECK(n == 3 && s[0].hz == 1200);

	// a CPS call tone: a pause, then four tones of 40 ms
	static const uint16_t c5h[SIG_STEPS] = { 0, 1570, 1050, 1570, 1320 };
	static const uint8_t c5m[SIG_STEPS] = { 4, 4, 4, 4, 4 };
	CHECK(chain_set(c5m));
	n = sig_chain(c5h, c5m, SIG_EV_BEGIN, s);
	CHECK(n == 5 && s[0].hz == 0 && s[1].hz == 1570 && s[2].hz == 1050 && s[3].hz == 1570 && s[4].hz == 1320);
	CHECK(s[0].ms == 40 && s[4].ms == 40 && total_ms(s, n) == 200);

	// a step of length 0 ends the chain: what follows it is ignored
	static const uint16_t eh[SIG_STEPS] = { 640, 950, 2000, 0, 0 };
	static const uint8_t em[SIG_STEPS] = { 10, 5, 0, 7, 7 };
	n = sig_chain(eh, em, SIG_EV_END, s);
	CHECK(n == 2 && s[0].hz == 640 && s[0].ms == 100 && s[1].hz == 950 && s[1].ms == 50);

	// a first step of length 0 is no chain at all, even with later steps
	static const uint8_t zm[SIG_STEPS] = { 0, 9, 9, 9, 9 };
	CHECK(!chain_set(zm));
	n = sig_chain(eh, zm, SIG_EV_END, s);
	CHECK(n == 3 && s[0].hz == 1200);

	// a frequency above the band is cut, and so is the total length (10 x 255 x 10 ms would be 25 s)
	static const uint16_t bh[SIG_STEPS] = { 9000, 1000, 1000, 1000, 1000 };
	static const uint8_t bm[SIG_STEPS] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
	n = sig_chain(bh, bm, SIG_EV_END, s);
	CHECK(n == 1 && s[0].hz == SIG_HZ_MAX && total_ms(s, n) == SIG_MAX_MS);
	static const uint8_t cm[SIG_STEPS] = { 30, 30, 30, 30, 30 };	// 300 ms each: the third fills the 900 ms
	n = sig_chain(bh, cm, SIG_EV_END, s);
	CHECK(n == 3 && s[0].ms == 300 && s[2].ms == 300 && total_ms(s, n) == SIG_MAX_MS);
	static const uint8_t dm[SIG_STEPS] = { 50, 30, 20, 10, 10 };	// 500, 300, then 100 of the next 200
	n = sig_chain(bh, dm, SIG_EV_END, s);
	CHECK(n == 3 && s[0].ms == 500 && s[1].ms == 300 && s[2].ms == 100);

	// an eight tone ZVEI key at 70 ms each fits, and ten steps of 90 ms are the whole 900
	static const uint16_t zh[SIG_STEPS] = { 1830, 2200, 1830, 1400, 1270, 1670, 1160, 1060, 0, 0 };
	static const uint8_t zm8[SIG_STEPS] = { 7, 7, 7, 7, 7, 7, 7, 7, 0, 0 };
	n = sig_chain(zh, zm8, SIG_EV_BEGIN, s);
	CHECK(n == 8 && s[7].hz == 1060 && total_ms(s, n) == 560);
	static const uint8_t tm[SIG_STEPS] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
	n = sig_chain(zh, tm, SIG_EV_BEGIN, s);
	CHECK(n == 10 && total_ms(s, n) == SIG_MAX_MS);
}

// Zero crossings of a block: about 2 * hz * seconds
static int crossings(const int16_t *p, int n)
{
	int c = 0;
	for (int i = 1; i < n; i++) {
		if ((p[i - 1] < 0) != (p[i] < 0)) {
			c++;
		}
	}
	return c;
}

// The settings area's record (docs/spec/codeplug-settings.md 3): outputs first, then the first five frequencies (u16 little endian)
// and five lengths, then the other five of each (the first version's layout is the front of this one). Short reads zero-filled, long from the front, what the setting can't take reads as absent
static void test_rec(void)
{
	sig_rec_t r = { SIG_OUT_BOTH, SIG_OUT_LOCAL, { 1600, 0, 800, 3400, 10, 1060, 0, 0, 0, 2200 }, { 5, 0, 7, 60, 1, 7, 0, 0, 0, 9 } }, d;
	uint8_t b[SIG_REC_LEN + 4] = { 0 };
	CHECK(SIG_REC_LEN == 32);
	sig_rec_encode(&r, b);
	CHECK(b[0] == SIG_OUT_BOTH && b[1] == SIG_OUT_LOCAL && b[2] == (1600 & 0xFF) && b[3] == (1600 >> 8) &&
	      b[2 + 4] == (800 & 0xFF) && b[2 + 10] == 5 && b[2 + 10 + 3] == 60);
	CHECK(sig_rec_decode(b, SIG_REC_LEN, SIG_EV_END, &d) && d.dmr == SIG_OUT_BOTH && d.fm == SIG_OUT_LOCAL &&
	      d.hz[0] == 1600 && d.hz[3] == 3400 && d.ms10[2] == 7 && d.ms10[3] == 60);
	CHECK(sig_rec_decode(b, SIG_REC_LEN + 4, SIG_EV_END, &d) && d.hz[4] == 10 && d.hz[5] == 1060 && d.hz[9] == 2200 &&
	      d.ms10[5] == 7 && d.ms10[9] == 9);	// a later version's longer record, and the steps 6 to 10
	// The first version's record (five steps, 17 bytes) reads as it did, with the other five empty
	CHECK(sig_rec_decode(b, 17, SIG_EV_END, &d) && d.hz[3] == 3400 && d.ms10[3] == 60 && d.hz[5] == 0 && d.ms10[9] == 0);
	CHECK(sig_rec_decode(b, 2, SIG_EV_END, &d) && d.dmr == SIG_OUT_BOTH && d.hz[0] == 0 && d.ms10[0] == 0);	// short
	CHECK(!sig_rec_decode(b, 0, SIG_EV_END, &d));
	uint8_t c[SIG_REC_LEN];
	memcpy(c, b, sizeof c);
	c[0] = SIG_OUT_COUNT;
	CHECK(!sig_rec_decode(c, SIG_REC_LEN, SIG_EV_END, &d));
	memcpy(c, b, sizeof c);
	c[0] = SIG_OUT_AIR;	// the end of a reception has no air copy
	CHECK(!sig_rec_decode(c, SIG_REC_LEN, SIG_EV_IDLE, &d) && sig_rec_decode(c, SIG_REC_LEN, SIG_EV_END, &d));
	memcpy(c, b, sizeof c);
	c[1] = SIG_OUT_BOTH;	// FM's start tone goes on air as its end tone does
	CHECK(sig_rec_decode(c, SIG_REC_LEN, SIG_EV_BEGIN, &d) && sig_rec_decode(c, SIG_REC_LEN, SIG_EV_END, &d));
	CHECK(!sig_rec_decode(c, SIG_REC_LEN, SIG_EV_IDLE, &d));
	memcpy(c, b, sizeof c);
	c[3] = 0x20;	// 8192 Hz
	CHECK(!sig_rec_decode(c, SIG_REC_LEN, SIG_EV_END, &d));
	memcpy(c, b, sizeof c);
	c[2] = 1573 & 0xFF;	// a frequency off the 10 Hz grid
	c[3] = 1573 >> 8;
	CHECK(!sig_rec_decode(c, SIG_REC_LEN, SIG_EV_END, &d));
	memcpy(c, b, sizeof c);
	c[2 + 10 + 1] = 91;	// a length over the limit
	CHECK(!sig_rec_decode(c, SIG_REC_LEN, SIG_EV_END, &d));
}

static void test_gen(void)
{
	static int16_t pcm[8000];
	beep_seg_t one[] = { { 1000, 100 } };
	beep_gen_t g;
	CHECK(gen_samples(one, 1) == 800);
	beep_gen_start(&g, one, 1, 16000);
	memset(pcm, 0x55, sizeof(pcm));
	// 160 samples: the tone's first 100 ms is 800 samples, so the sequence goes on
	CHECK(beep_gen_block(&g, pcm, 160) && !gen_done(&g));
	int c = crossings(pcm + 16, 144);
	CHECK(c >= 2 * 1000 * 144 / 8000 - 2 && c <= 2 * 1000 * 144 / 8000 + 2);
	// the fade in: the first sample is silent, the peak is near the level
	int peak = 0;
	for (int i = 0; i < 160; i++) {
		peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
	}
	CHECK(pcm[0] == 0 && peak > 14000 && peak <= 16000);
	// the rest: 640 samples of the tone end the sequence; the unplayed part of the block is zero
	CHECK(!beep_gen_block(&g, pcm, 800 - 160 + 40));
	CHECK(gen_done(&g));
	for (int i = 800 - 160; i < 800 - 160 + 40; i++) {
		CHECK(pcm[i] == 0);
	}
	CHECK(pcm[800 - 160 - 1] == 0);	// fades out

	// a pause is silent, the next step goes on in phase (no jump to a full level at its first sample)
	beep_seg_t seq[] = { { 0, 40 }, { 1570, 40 }, { 1050, 40 } };
	beep_gen_start(&g, seq, 3, 20000);
	CHECK(gen_samples(seq, 3) == 960);
	CHECK(beep_gen_block(&g, pcm, 320));
	for (int i = 0; i < 320; i++) {
		CHECK(pcm[i] == 0);
	}
	CHECK(!beep_gen_block(&g, pcm, 640));
	c = crossings(pcm + 16, 320 - 32);
	CHECK(c >= 2 * 1570 * 288 / 8000 - 2 && c <= 2 * 1570 * 288 / 8000 + 2);
	c = crossings(pcm + 320 + 16, 320 - 32);
	CHECK(c >= 2 * 1050 * 288 / 8000 - 2 && c <= 2 * 1050 * 288 / 8000 + 2);
	CHECK(gen_done(&g));
	// after the end: zeros, and no longer "more"
	memset(pcm, 0x55, 320);
	CHECK(!beep_gen_block(&g, pcm, 160) && pcm[0] == 0 && pcm[159] == 0);

	// an empty sequence is done at once
	beep_gen_start(&g, one, 0, 1000);
	CHECK(gen_done(&g) && !beep_gen_block(&g, pcm, 160));
	// a level above full scale is held at full scale
	beep_gen_start(&g, one, 1, 99999);
	CHECK(g.amp == 32767);
}

int main(void)
{
	test_out();
	test_chain();
	test_rec();
	test_gen();
	printf("%d checks, %d failed\n", checks, failures);
	return failures != 0;
}
