// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// USB / network audio building blocks (core/usbaudio, docs/spec/usb-audio.md): the USRP framing and G.711 mu-law,
// the link's PTT watchdogs and sequence accounting, and the PCM jitter buffer between two drifting 8 kHz clocks
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "usbaudio/usrp.h"
#include "usbaudio/pcm_slip.h"

static int failures;
static int checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- G.711 ---

static void test_ulaw(void)
{
	// Known points of the G.711 mu-law table: 0 -> 0xFF, the largest positive -> 0x80, the largest negative -> 0x00
	CHECK(usrp_ulaw_encode(0) == 0xFF);
	CHECK(usrp_ulaw_encode(32767) == 0x80);
	CHECK(usrp_ulaw_encode(-32768) == 0x00);
	CHECK(usrp_ulaw_decode(0xFF) == 0);
	CHECK(usrp_ulaw_decode(0x7F) == 0);	// "negative zero"
	CHECK(usrp_ulaw_decode(0x80) == 32124);
	CHECK(usrp_ulaw_decode(0x00) == -32124);
	CHECK(usrp_ulaw_encode(-1) == 0x7F);

	// Every code decodes and re-encodes to itself (except negative zero, which becomes positive zero)
	int bad = 0;
	for (int c = 0; c < 256; c++) {
		uint8_t back = usrp_ulaw_encode(usrp_ulaw_decode((uint8_t)c));
		if (back != c && !(c == 0x7F && back == 0xFF)) {
			bad++;
		}
	}
	CHECK(bad == 0);

	// Monotonic, and the quantization error stays within half a step of its segment (about 3 % of the value)
	int prev = -40000;
	int worst_pct = 0;
	for (int x = -32768; x <= 32767; x += 7) {
		int y = usrp_ulaw_decode(usrp_ulaw_encode((int16_t)x));
		if (y < prev) {
			bad++;
		}
		prev = y;
		int err = abs(y - x);
		if (abs(x) > 256 && abs(x) < 32000) {
			int pct = err * 100 / abs(x);
			worst_pct = pct > worst_pct ? pct : worst_pct;
		}
	}
	CHECK(bad == 0);
	CHECK(worst_pct <= 3);
}

// --- framing ---

static void test_framing(void)
{
	int16_t pcm[USRP_FRAME_SAMPLES], back[USRP_FRAME_SAMPLES];
	for (int i = 0; i < USRP_FRAME_SAMPLES; i++) {
		pcm[i] = (int16_t)(i * 300 - 24000);
	}
	uint8_t buf[USRP_MAX_LEN + 8];
	usrp_header_t h = { .seq = 0x01020304u, .keyup = true, .talkgroup = 9, .type = USRP_TYPE_VOICE };
	size_t n = usrp_pack(buf, sizeof buf, &h, pcm);
	CHECK(n == 352);
	CHECK(memcmp(buf, "USRP", 4) == 0);
	// Header words big endian
	CHECK(buf[4] == 1 && buf[5] == 2 && buf[6] == 3 && buf[7] == 4);
	CHECK(buf[12] == 0 && buf[15] == 1);	// keyup
	CHECK(buf[19] == 9);			// talkgroup
	CHECK(buf[23] == USRP_TYPE_VOICE);
	// Samples little endian
	CHECK(buf[32] == (uint8_t)(uint16_t)pcm[0] && buf[33] == (uint8_t)((uint16_t)pcm[0] >> 8));
	CHECK(usrp_pack(buf, 351, &h, pcm) == 0);

	usrp_header_t g;
	size_t samples;
	CHECK(usrp_parse(buf, n, &g, back, &samples) == USRP_PARSE_OK);
	CHECK(samples == USRP_FRAME_SAMPLES);
	CHECK(g.seq == h.seq && g.keyup && g.talkgroup == 9 && g.type == USRP_TYPE_VOICE);
	CHECK(memcmp(back, pcm, sizeof pcm) == 0);

	// Errors
	CHECK(usrp_parse(buf, 31, &g, back, &samples) == USRP_PARSE_SHORT);
	CHECK(usrp_parse(buf, n - 2, &g, back, &samples) == USRP_PARSE_LENGTH);
	buf[0] = 'X';
	CHECK(usrp_parse(buf, n, &g, back, &samples) == USRP_PARSE_MAGIC);

	// A header-only voice frame (an unkey without samples) is fine
	h.keyup = false;
	n = usrp_pack(buf, sizeof buf, &h, NULL);
	CHECK(usrp_parse(buf, USRP_HEADER_LEN, &g, back, &samples) == USRP_PARSE_OK && samples == 0 && !g.keyup);

	// mu-law form
	h.type = USRP_TYPE_VOICE_ULAW;
	n = usrp_pack(buf, sizeof buf, &h, pcm);
	CHECK(n == 192);
	CHECK(usrp_parse(buf, n, &g, back, &samples) == USRP_PARSE_OK && samples == USRP_FRAME_SAMPLES);
	int worst = 0;
	for (int i = 0; i < USRP_FRAME_SAMPLES; i++) {
		int e = abs(back[i] - pcm[i]);
		worst = e > worst ? e : worst;
	}
	CHECK(worst <= 1024);	// top segment step 1024

	// Ping: header only
	h.type = USRP_TYPE_PING;
	CHECK(usrp_pack(buf, sizeof buf, &h, NULL) == USRP_HEADER_LEN);
	CHECK(usrp_parse(buf, USRP_HEADER_LEN, &g, NULL, &samples) == USRP_PARSE_OK && samples == 0);
}

// --- link: PTT and its watchdogs ---

static size_t frame(uint8_t *buf, uint32_t seq, bool keyup)
{
	usrp_header_t h = { .seq = seq, .keyup = keyup, .type = USRP_TYPE_VOICE };
	return usrp_pack(buf, USRP_MAX_LEN, &h, NULL);
}

static void test_link(void)
{
	usrp_link_t l;
	uint8_t buf[USRP_MAX_LEN];
	int16_t pcm[USRP_FRAME_SAMPLES];
	bool voice;
	uint32_t t = 1000;

	usrp_link_init(&l, NULL);
	CHECK(l.cfg.keyup_timeout_ms == USRP_KEYUP_TIMEOUT_MS);

	// Key, carry on, unkey
	CHECK(usrp_link_rx(&l, buf, frame(buf, 10, true), t, pcm, &voice) == USRP_EV_KEY && voice && l.keyed);
	for (uint32_t s = 11; s < 60; s++) {
		t += 20;
		CHECK(usrp_link_rx(&l, buf, frame(buf, s, true), t, pcm, &voice) == USRP_EV_NONE);
		CHECK(usrp_link_tick(&l, t + 5) == USRP_EV_NONE);
	}
	t += 20;
	CHECK(usrp_link_rx(&l, buf, frame(buf, 60, false), t, pcm, &voice) == USRP_EV_UNKEY && !l.keyed);
	CHECK(l.stats.keys == 1 && l.stats.lost == 0 && l.stats.frames == 51);

	// The host vanishes while keyed: the watchdog unkeys after keyup_timeout_ms, not before
	t += 1000;
	CHECK(usrp_link_rx(&l, buf, frame(buf, 61, true), t, pcm, &voice) == USRP_EV_KEY);
	CHECK(usrp_link_tick(&l, t + USRP_KEYUP_TIMEOUT_MS - 1) == USRP_EV_NONE && l.keyed);
	CHECK(usrp_link_tick(&l, t + USRP_KEYUP_TIMEOUT_MS) == USRP_EV_TIMEOUT && !l.keyed);
	CHECK(l.stats.timeouts == 1);
	// It comes back still keyed: keys again
	t += 400;
	CHECK(usrp_link_rx(&l, buf, frame(buf, 62, true), t, pcm, &voice) == USRP_EV_KEY);
	t += 20;
	CHECK(usrp_link_rx(&l, buf, frame(buf, 63, false), t, pcm, &voice) == USRP_EV_UNKEY);

	// Sequence: gaps counted, late and repeated frames dropped (their keyup ignored), a restarted peer accepted
	t += 20;
	CHECK(usrp_link_rx(&l, buf, frame(buf, 67, false), t, pcm, &voice) == USRP_EV_NONE);
	CHECK(l.stats.lost == 3);
	CHECK(usrp_link_rx(&l, buf, frame(buf, 65, true), t, pcm, &voice) == USRP_EV_NONE && !l.keyed);
	CHECK(usrp_link_rx(&l, buf, frame(buf, 67, true), t, pcm, &voice) == USRP_EV_NONE && !l.keyed);
	CHECK(l.stats.out_of_order == 2);
	CHECK(usrp_link_rx(&l, buf, frame(buf, 0, true), t, pcm, &voice) == USRP_EV_KEY);	// restarted at 0
	t += 20;
	CHECK(usrp_link_rx(&l, buf, frame(buf, 1, false), t, pcm, &voice) == USRP_EV_UNKEY);

	// Garbage
	CHECK(usrp_link_rx(&l, (const uint8_t *)"hello", 5, t, pcm, &voice) == USRP_EV_NONE && !voice);
	CHECK(l.stats.bad == 1);

	// The TX limit: unkeys, stays unkeyed while the host holds keyup, released by keyup 0 or by a quiet gap
	usrp_link_cfg_t cfg = { .keyup_timeout_ms = 200, .tx_max_ms = 1000 };
	usrp_link_init(&l, &cfg);
	t = 50000;
	uint32_t s = 100;
	CHECK(usrp_link_rx(&l, buf, frame(buf, s++, true), t, pcm, &voice) == USRP_EV_KEY);
	int limit_at = -1;
	for (int i = 1; i <= 80; i++) {
		t += 20;
		usrp_event_t ev = usrp_link_rx(&l, buf, frame(buf, s++, true), t, pcm, &voice);
		CHECK(ev == USRP_EV_NONE);	// never re-keys while locked out
		if (usrp_link_tick(&l, t) == USRP_EV_TX_LIMIT) {
			limit_at = i;
		}
	}
	CHECK(limit_at == 50);	// 50 x 20 ms = the 1000 ms limit
	CHECK(!l.keyed && l.locked_out && l.stats.tx_limits == 1 && l.stats.keys == 1);
	t += 20;
	CHECK(usrp_link_rx(&l, buf, frame(buf, s++, false), t, pcm, &voice) == USRP_EV_NONE && !l.locked_out);
	t += 20;
	CHECK(usrp_link_rx(&l, buf, frame(buf, s++, true), t, pcm, &voice) == USRP_EV_KEY);
	// Locked out again, then the host just stops (no keyup 0): released after the quiet gap
	t += 1000;
	CHECK(usrp_link_rx(&l, buf, frame(buf, s++, true), t, pcm, &voice) == USRP_EV_NONE);
	CHECK(usrp_link_tick(&l, t) == USRP_EV_TX_LIMIT && l.locked_out);
	CHECK(usrp_link_tick(&l, t + 199) == USRP_EV_NONE && l.locked_out);
	CHECK(usrp_link_tick(&l, t + 200) == USRP_EV_NONE && !l.locked_out);

	// A zero keyup timeout can't switch the watchdog off
	cfg.keyup_timeout_ms = 0;
	usrp_link_init(&l, &cfg);
	CHECK(l.cfg.keyup_timeout_ms == USRP_KEYUP_TIMEOUT_MS);

	// Our frames toward the peer count their own sequence and carry COS
	usrp_link_init(&l, NULL);
	usrp_header_t g;
	size_t samples;
	CHECK(usrp_link_tx(&l, buf, sizeof buf, NULL, true, false) == USRP_VOICE_LEN);
	CHECK(usrp_link_tx(&l, buf, sizeof buf, NULL, false, true) == USRP_ULAW_LEN);
	CHECK(usrp_parse(buf, USRP_ULAW_LEN, &g, pcm, &samples) == USRP_PARSE_OK && g.seq == 1 && !g.keyup);
}

// --- jitter buffer between drifting clocks ---

// Producer at 8 kHz x (1 + ppm), 160-sample blocks with jitter; consumer at exactly 8 kHz, 160-sample pulls. A sine
// through it must come out continuous (no steps beyond what one slipped sample makes)
static void drift_run(double ppm, int seconds, int jitter_ms, uint32_t *drops, uint32_t *repeats, uint32_t *underruns,
		      uint32_t *overflow, uint32_t *fill_lo, uint32_t *fill_hi)
{
	static pcm_slip_t p;
	pcm_slip_init(&p, 480, 160);
	double phase_in = 0;
	const double w = 2 * M_PI * 400.0 / 8000.0;
	double t_prod = 0;		// next producer block, us
	double t_cons = 0;		// next consumer block, us
	const double prod_period = 20000.0 / (1.0 + ppm * 1e-6);
	unsigned rng = 12345;
	int16_t block[160], out[160];
	*fill_lo = 0xFFFFFFFFu;
	*fill_hi = 0;
	int primed_once = 0;
	while (t_cons < seconds * 1e6) {
		rng = rng * 1103515245u + 12345u;
		double jitter = jitter_ms ? (double)((rng >> 16) % (unsigned)(jitter_ms * 1000)) : 0;
		if (t_prod + jitter <= t_cons) {
			for (int i = 0; i < 160; i++) {
				block[i] = (int16_t)(10000 * sin(phase_in));
				phase_in += w;
			}
			pcm_slip_push(&p, block, 160);
			t_prod += prod_period;
		} else {
			bool ok = pcm_slip_pull(&p, out, 160);
			if (ok) {
				primed_once = 1;
				uint32_t f = pcm_slip_fill(&p);
				if (t_cons > 2e6) {
					*fill_lo = f < *fill_lo ? f : *fill_lo;
					*fill_hi = f > *fill_hi ? f : *fill_hi;
				}
			}
			t_cons += 20000.0;
		}
	}
	(void)primed_once;
	*drops = p.stats.drops;
	*repeats = p.stats.repeats;
	*underruns = p.stats.underruns;
	*overflow = p.stats.overflow;
}

static void test_slip(void)
{
	uint32_t drops, repeats, underruns, overflow, lo, hi;
	const int secs = 600;

	// Producer 200 ppm fast: 200e-6 x 8000 x 600 = 960 samples of drift; the first one to two blocks of it fill the
	// buffer up to the slip threshold, the rest is dropped one sample at a time. Nothing repeated, no underrun
	drift_run(200, secs, 5, &drops, &repeats, &underruns, &overflow, &lo, &hi);
	printf("  +200 ppm: drops %u repeats %u underruns %u overflow %u fill %u..%u\n", drops, repeats, underruns, overflow,
	       lo, hi);
	CHECK(underruns == 0 && overflow == 0);
	CHECK(drops > 960 - 2 * 160 && drops < 1000);
	CHECK(repeats < 20);
	CHECK(hi < 480 + 3 * 160);

	// 200 ppm slow
	drift_run(-200, secs, 5, &drops, &repeats, &underruns, &overflow, &lo, &hi);
	printf("  -200 ppm: drops %u repeats %u underruns %u overflow %u fill %u..%u\n", drops, repeats, underruns, overflow,
	       lo, hi);
	CHECK(underruns == 0 && overflow == 0);
	CHECK(repeats > 960 - 2 * 160 && repeats < 1000);
	CHECK(drops < 20);
	CHECK(lo >= 40);	// fill after the pull: never close to running dry

	// Same clock, 15 ms jitter: no slipping beyond a handful, no underrun
	drift_run(0, secs, 15, &drops, &repeats, &underruns, &overflow, &lo, &hi);
	printf("  0 ppm, 15 ms jitter: drops %u repeats %u underruns %u fill %u..%u\n", drops, repeats, underruns, lo, hi);
	CHECK(underruns == 0 && overflow == 0);
	CHECK(drops + repeats < 40);

	// Priming, underrun and reset
	static pcm_slip_t p;
	int16_t in[160], out[160];
	for (int i = 0; i < 160; i++) {
		in[i] = (int16_t)(i + 1);
	}
	pcm_slip_init(&p, 320, 160);
	pcm_slip_push(&p, in, 160);
	CHECK(!pcm_slip_pull(&p, out, 160) && out[0] == 0 && pcm_slip_fill(&p) == 160);	// not primed yet
	pcm_slip_push(&p, in, 160);
	CHECK(pcm_slip_pull(&p, out, 160) && out[0] == 1 && out[159] == 160 && p.stats.primes == 1);
	CHECK(pcm_slip_pull(&p, out, 160) && out[0] == 1);
	CHECK(!pcm_slip_pull(&p, out, 160) && p.stats.underruns == 1 && out[0] == 0);
	pcm_slip_push(&p, in, 100);
	pcm_slip_reset(&p);
	CHECK(pcm_slip_fill(&p) == 0 && !p.primed);
	// Overflow keeps the newest
	for (int k = 0; k < 8; k++) {
		pcm_slip_push(&p, in, 160);
	}
	CHECK(pcm_slip_fill(&p) == PCM_SLIP_CAP && p.stats.overflow == 8 * 160 - PCM_SLIP_CAP);
}

int main(void)
{
	test_ulaw();
	test_framing();
	test_link();
	test_slip();
	if (failures) {
		printf("usrpTest: %d of %d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("usrpTest: all %d checks passed\n", checks);
	return 0;
}
