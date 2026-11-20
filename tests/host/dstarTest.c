// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV (core/dstar): the header's CRC and FEC, the slow data, and calls from the transmitter's symbols through the
// framer to the receiver's call layer, from the start and joined late, with noise and errors.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/dstar/dstar.h"
#include "../../core/air/air_syms.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 0x13579BDu;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void make_header(dstar_header_t *h, const char *my, const char *ur)
{
	memset(h, 0, sizeof *h);
	dstar_call_set(h->rpt2, DSTAR_CALL_LEN, "DIRECT");
	dstar_call_set(h->rpt1, DSTAR_CALL_LEN, "DIRECT");
	dstar_call_set(h->ur, DSTAR_CALL_LEN, ur);
	dstar_call_set(h->my, DSTAR_CALL_LEN, my);
	dstar_call_set(h->sfx, DSTAR_SUFFIX_LEN, "RF");
}

// CRC-CCITT as X.25 sends it: "123456789" gives 0x906E
static void test_crc(void)
{
	CHECK(dstar_crc((const uint8_t *)"123456789", 9) == 0x906E, "crc %04x", dstar_crc((const uint8_t *)"123456789", 9));
}

// The slow data's scrambler bytes are the first 24 bits of the header's scrambler sequence
static void test_scrambler(void)
{
	uint8_t d[3] = { 0, 0, 0 }, ok = 1;

	dstar_data_scramble(d);
	for (int i = 0; i < 24; i++)
		ok &= ((d[i >> 3] >> (i & 7)) & 1) == dstar_pn((uint16_t)i);
	CHECK(ok, "slow data scrambler: %02x %02x %02x is not the sequence's start", d[0], d[1], d[2]);
	// period 127
	ok = 1;
	for (uint16_t i = 0; i < 300; i++)
		ok &= dstar_pn(i) == dstar_pn((uint16_t)(i + 127));
	CHECK(ok, "scrambler period");
}

// Headers through the FEC: clean, and with errors spread over the 660 symbols
static void test_header_fec(void)
{
	static uint8_t syms[(DSTAR_HDR_SYMS + 3) / 4];
	static int8_t soft[DSTAR_HDR_SYMS];
	static dstar_fec_work_t w;
	int wrong[4] = { 0, 0, 0, 0 };
	const int errs_per[4] = { 0, 10, 25, 40 };

	for (int e = 0; e < 4; e++) {
		for (int n = 0; n < 200; n++) {
			dstar_header_t h, back;
			uint8_t raw[DSTAR_HDR_BYTES], got[DSTAR_HDR_BYTES];
			char call[9];

			for (int i = 0; i < 6; i++)
				call[i] = (char)('A' + rnd() % 26u);
			call[6] = '\0';
			make_header(&h, call, "CQCQCQ");
			h.flag[0] = (uint8_t)(rnd() & 0x48);
			dstar_header_pack(&h, raw);
			dstar_header_encode(raw, syms, 0);
			for (int i = 0; i < DSTAR_HDR_SYMS; i++)
				soft[i] = (int8_t)((air_sym_get(syms, (uint16_t)i) & 2) ? -96 : 96);
			for (int k = 0; k < errs_per[e]; k++) {
				int i = (int)(rnd() % DSTAR_HDR_SYMS);

				soft[i] = (int8_t)-soft[i] / 2;	// a weak wrong value
			}
			if (dstar_header_decode(syms, soft, 0, &w, got) < 0 || !dstar_header_unpack(got, &back) ||
			    memcmp(&h, &back, sizeof h) != 0)
				wrong[e]++;
		}
	}
	printf("# header FEC: wrong of 200 with 0/10/25/40 weak errors: %d %d %d %d\n", wrong[0], wrong[1], wrong[2], wrong[3]);
	CHECK(wrong[0] == 0 && wrong[1] == 0, "header FEC: %d, %d of 200 headers with few errors wrong", wrong[0], wrong[1]);
	CHECK(wrong[2] <= 2, "header FEC: %d of 200 headers with 25 errors wrong", wrong[2]);
	// hard decisions only
	{
		dstar_header_t h, back;
		uint8_t raw[DSTAR_HDR_BYTES], got[DSTAR_HDR_BYTES];

		make_header(&h, "JA1RL", "CQCQCQ");
		dstar_header_pack(&h, raw);
		dstar_header_encode(raw, syms, 0);
		for (int k = 0; k < 8; k++) {
			uint16_t i = (uint16_t)(k * 77 + 5);

			air_sym_put(syms, i, air_sym_get(syms, i) == 1 ? 3 : 1);
		}
		CHECK(dstar_header_decode(syms, NULL, 0, &w, got) >= 0 && dstar_header_unpack(got, &back) &&
		      memcmp(&h, &back, sizeof h) == 0, "header FEC: hard decisions with 8 errors");
	}
}

// Slow data: a message and the header sent again, through the transmitter's schedule and the receiver's assembler
static void test_slow(void)
{
	dstar_slow_tx_t tx;
	dstar_slow_rx_t rx;
	dstar_header_t h;
	uint8_t raw[DSTAR_HDR_BYTES], got = 0;

	make_header(&h, "K1ABC", "CQCQCQ");
	dstar_header_pack(&h, raw);
	dstar_slow_tx_init(&tx, raw, "Hello from the DM-32");
	dstar_slow_rx_reset(&rx);
	for (int sf = 0; sf < 2; sf++)
		for (uint8_t n = 1; n <= 20; n++) {
			uint8_t d[3];

			dstar_slow_tx_frame(&tx, n, d);
			got |= dstar_slow_rx_frame(&rx, d, n);
		}
	CHECK(got & DSTAR_SLOW_MSG, "slow data: no message");
	CHECK(memcmp(rx.msg, "Hello from the DM-32", 20) == 0, "slow data: message %.20s", rx.msg);
	CHECK(got & DSTAR_SLOW_HEADER, "slow data: no header");
	CHECK(memcmp(rx.hdr, raw, DSTAR_HDR_BYTES) == 0, "slow data: header differs");
	// simple data: a line in pieces
	dstar_slow_rx_reset(&rx);
	{
		const char *line = "$$CRC1234,K1ABC>API51:!4200.00N/07100.00W>\r";
		uint8_t blk[6], n = 1;
		size_t at = 0, len = strlen(line);

		got = 0;
		while (at < len) {
			uint8_t k = (uint8_t)(len - at < 5 ? len - at : 5);

			memset(blk, 0x66, 6);
			blk[0] = (uint8_t)(DSTAR_MINI_DATA | k);
			memcpy(blk + 1, line + at, k);
			at += k;
			got |= dstar_slow_rx_frame(&rx, blk, n);
			got |= dstar_slow_rx_frame(&rx, blk + 3, (uint8_t)(n + 1));
			n = (uint8_t)(n + 2 > 19 ? 1 : n + 2);
		}
		CHECK(got & DSTAR_SLOW_TEXT, "slow data: no line of text");
		CHECK(strncmp(rx.text, line, len - 1) == 0, "slow data: text '%s'", rx.text);
	}
}

// ── Calls through the framer ──
#define FRAMES		63
static uint8_t voice[FRAMES][DSTAR_VOICE_BYTES];
static struct {
	int starts, updates, ends, nvoice;
	bool late;
	dstar_call_t last;
	uint8_t frames[FRAMES + 8][DSTAR_VOICE_BYTES];
} got;

static void s_start(const dstar_call_t *c, void *ctx)
{
	(void)ctx;
	got.starts++;
	got.late = c->late;
	got.last = *c;
}
static void s_update(const dstar_call_t *c, void *ctx)
{
	(void)ctx;
	got.updates++;
	got.last = *c;
}
static void s_voice(const uint8_t v[DSTAR_VOICE_BYTES], void *ctx)
{
	(void)ctx;
	if (got.nvoice < FRAMES + 8)
		memcpy(got.frames[got.nvoice], v, DSTAR_VOICE_BYTES);
	got.nvoice++;
}
static void s_end(void *ctx)
{
	(void)ctx;
	got.ends++;
}
static const dstar_call_sink_t sink = { s_start, s_update, s_voice, s_end };

// The transmitter's symbols for a call of FRAMES voice frames, as dibits one a byte
static uint16_t tx_call(uint8_t *out, uint16_t cap, const char *msg)
{
	static dstar_call_tx_t t;
	dstar_header_t h;
	uint16_t n = 0, first;

	make_header(&h, "K1ABC", "CQCQCQ");
	dstar_call_tx_begin(&t, &h, msg);
	for (int f = 0; f < FRAMES; f++)
		for (int i = 0; i < DSTAR_VOICE_BYTES; i++)
			voice[f][i] = (uint8_t)rnd();
	int f = 0;

	while (!dstar_call_tx_done(&t) && n < cap) {
		uint16_t r = dstar_call_tx_ready(&t, &first);

		if (r == 0) {
			if (f < FRAMES)
				CHECK(dstar_call_tx_voice(&t, voice[f++]), "tx: voice frame %d not taken", f);
			else
				dstar_call_tx_end(&t);
			continue;
		}
		for (uint16_t i = 0; i < r && n < cap; i++)
			out[n++] = air_sym_get(dstar_call_tx_syms(&t), (uint16_t)(first + i));
		dstar_call_tx_sent(&t, r);
	}
	CHECK(n == DSTAR_START_SYMS + FRAMES * DSTAR_FRAME_SYMS + DSTAR_END_SYMS, "tx: %u symbols", n);
	return n;
}

// Feed symbols [from, to) to a framer in chunks, with 500 symbols of noise before and tail after them; noise: the soft
// values' noise, out of 96
static dstar_call_stats_t rx_run(const uint8_t *air, uint16_t from, uint16_t to, int noise, uint32_t tail)
{
	static dstar_call_rx_t c;
	static air_framer_t fr;
	static uint8_t buf[(24 + DSTAR_HDR_SYMS + 3) / 4];
	static int8_t soft[24 + DSTAR_HDR_SYMS];
	uint8_t hard[AIR_CHUNK_SYMS_MAX / 4];
	int8_t sv[AIR_CHUNK_SYMS_MAX];
	uint32_t total = (uint32_t)(to - from) + 500 + tail, at = 0;

	memset(&got, 0, sizeof got);
	dstar_call_rx_init(&c, &sink, NULL);
	air_framer_init(&fr, &dstar_air_mode, &c, buf, soft, 24 + DSTAR_HDR_SYMS);
	while (at < total) {
		uint16_t n = (uint16_t)(total - at < 100 ? total - at : 100);
		air_chunk_t ch = { .t_us = at * 208u, .nsyms = n, .sync_at = -1, .hard = hard, .soft = sv };

		for (uint16_t i = 0; i < n; i++) {
			uint32_t k = at + i;
			int32_t v;

			if (k >= 500 && k - 500 < (uint32_t)(to - from))
				v = air_sym_level(air[from + k - 500]) * 32;
			else
				v = (rnd() & 1) ? 96 : -96;	// noise: a random level
			if (noise)
				v += (int32_t)(rnd() % (uint32_t)(2 * noise + 1)) - noise;
			v = v > 127 ? 127 : v < -127 ? -127 : v;
			sv[i] = (int8_t)v;
			air_sym_put(hard, i, v >= 0 ? 1 : 3);
		}
		air_framer_rx(&fr, &ch);
		at += n;
	}
	return c.stats;
}

static void test_call(void)
{
	static uint8_t air[DSTAR_START_SYMS + FRAMES * DSTAR_FRAME_SYMS + DSTAR_END_SYMS + 16];
	uint16_t n = tx_call(air, sizeof air, "Hello from the DM-32");
	dstar_call_stats_t st;

	// the whole call
	st = rx_run(air, 0, n, 0, 500);
	CHECK(got.starts == 1 && !got.late, "call: %d starts, late %d", got.starts, got.late);
	CHECK(got.last.have_header && memcmp(got.last.hdr.my, "K1ABC   ", 8) == 0 && memcmp(got.last.hdr.sfx, "RF  ", 4) == 0,
	      "call: header %d my '%.8s'", got.last.have_header, got.last.hdr.my);
	CHECK(got.nvoice == FRAMES, "call: %d voice frames, sent %d", got.nvoice, FRAMES);
	CHECK(memcmp(got.frames, voice, sizeof voice) == 0, "call: voice frames differ");
	CHECK(got.last.have_msg && strcmp(got.last.msg, "Hello from the DM-32") == 0, "call: message '%s'", got.last.msg);
	CHECK(got.ends == 1 && st.ends == 1, "call: ended %d times (%u by its last frame)", got.ends, (unsigned)st.ends);
	CHECK(st.headers == 1 && st.syncs == 3, "call: %u headers, %u data syncs", (unsigned)st.headers, (unsigned)st.syncs);

	// with noise on the soft values (uniform, a fifth of the level each way)
	st = rx_run(air, 0, n, 20, 500);
	CHECK(got.starts == 1 && got.nvoice == FRAMES && got.ends == 1 && got.last.have_header,
	      "noisy call: %d starts, %d voice, %d ends", got.starts, got.nvoice, got.ends);

	// joined late, after the header: from the 5th voice frame. The voice from the next data sync on (frame 21), the
	// header from the slow data
	st = rx_run(air, DSTAR_START_SYMS + 4 * DSTAR_FRAME_SYMS, n, 0, 500);
	CHECK(got.starts == 1 && got.late, "late: %d starts, late %d", got.starts, got.late);
	CHECK(got.nvoice == FRAMES - 21, "late: %d voice frames, want %d", got.nvoice, FRAMES - 21);
	CHECK(got.nvoice > 0 && memcmp(got.frames[0], voice[21], DSTAR_VOICE_BYTES) == 0, "late: the first frame is not frame 21");
	CHECK(got.last.have_header && memcmp(got.last.hdr.my, "K1ABC   ", 8) == 0, "late: no header from the slow data");
	CHECK(got.ends == 1, "late: ended %d times", got.ends);

	// cut off without its last frame: the call ends when two data syncs in a row are missed
	st = rx_run(air, 0, (uint16_t)(n - DSTAR_END_SYMS - 10 * DSTAR_FRAME_SYMS), 0, 50 * DSTAR_FRAME_SYMS);
	CHECK(got.ends == 1 && st.lost == 1, "cut off: %d ends, %u lost", got.ends, (unsigned)st.lost);
}

int main(void)
{
	test_crc();
	test_scrambler();
	test_header_fec();
	test_slow();
	test_call();
	printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ok", checks - failures, checks);
	return failures != 0;
}
