// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR voice call stack (core/dmr/call) against a simulated modem (sim/dmr_sim_modem.c), plus its link control
// coding checked against references written here from the ETSI TS 102 361-1 definitions and against the older
// clean-tree checkers (sfr.c's RS(12,9) / CRC check, dmrData.c's CSBK builder)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/sfr.h"
#include "dmr/dmrData.h"
#include "sim/dmr_sim_modem.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define OWN_ID	3112345u
#define TG_TALK	91u
#define TG_LIST	3100u

// ---------------------------------------------------------------------------------------------------------------
// References, written from the definitions in a different form than core/dmr/call/dmr_lc.c

static uint8_t ref_gf_mul(uint8_t a, uint8_t b)
{
	unsigned r = 0;

	for (int i = 0; i < 8; i++)
		if (b & (1 << i))
			r ^= (unsigned)a << i;
	for (int i = 15; i >= 8; i--)	// reduce by x^8 + x^4 + x^3 + x^2 + 1
		if (r & (1u << i))
			r ^= 0x11Du << (i - 8);
	return (uint8_t)r;
}

static uint8_t ref_gf_pow2(int n)
{
	uint8_t r = 1;

	while (n-- > 0)
		r = ref_gf_mul(r, 2);
	return r;
}

// Codeword c(x) = sum c[k] x^(11-k); evaluate at a^j
static uint8_t ref_rs_eval(const uint8_t c[12], int j)
{
	uint8_t acc = 0;

	for (int k = 0; k < 12; k++)
		acc ^= ref_gf_mul(c[k], ref_gf_pow2(j * (11 - k)));
	return acc;
}

// Parity by long division by g(x) = (x + a)(x + a^2)(x + a^3), g built from its roots
static void ref_rs_parity(const uint8_t d[9], uint8_t p[3])
{
	uint8_t g[4] = { 1, 0, 0, 0 };	// g[i] coefficient of x^i

	for (int r = 1; r <= 3; r++) {
		uint8_t root = ref_gf_pow2(r);

		for (int i = 3; i > 0; i--)
			g[i] = g[i - 1] ^ ref_gf_mul(g[i], root);
		g[0] = ref_gf_mul(g[0], root);
	}
	uint8_t w[12] = { 0 };
	memcpy(w, d, 9);
	for (int i = 0; i < 9; i++) {
		uint8_t q = w[i];

		for (int j = 0; j <= 3; j++)
			w[i + j] ^= ref_gf_mul(q, g[3 - j]);
	}
	memcpy(p, &w[9], 3);
}

static int lcbit(const uint8_t *b, int i)
{
	return (b[i / 8] >> (7 - i % 8)) & 1;
}

// Embedded LC: matrix M[8][16], Hamming(16,11,4) parity equations written out, column parity, read column by column
static void ref_emb_encode(const uint8_t lc[9], uint8_t air[128])
{
	uint8_t M[8][16];
	unsigned sum = 0;
	int n = 0;

	memset(M, 0, sizeof(M));
	for (int i = 0; i < 9; i++)
		sum += lc[i];
	uint8_t cs = (uint8_t)(sum % 31);

	for (int r = 0; r < 7; r++) {
		for (int c = 0; c < (r < 2 ? 11 : 10); c++)
			M[r][c] = (uint8_t)lcbit(lc, n++);
		if (r >= 2)
			M[r][10] = (cs >> (4 - (r - 2))) & 1;
		uint8_t *d = M[r];
		M[r][11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
		M[r][12] = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
		M[r][13] = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
		M[r][14] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
		M[r][15] = d[0] ^ d[2] ^ d[5] ^ d[6] ^ d[8] ^ d[9] ^ d[10];
	}
	for (int c = 0; c < 16; c++)
		for (int r = 0; r < 7; r++)
			M[7][c] ^= M[r][c];
	n = 0;
	for (int c = 0; c < 16; c++)
		for (int r = 0; r < 8; r++)
			air[n++] = M[r][c];
}

// Decode without error correction: true if every check holds
static bool ref_emb_decode(const uint8_t bits[16], uint8_t lc[9])
{
	uint8_t air[128], M[8][16], again[128];
	int n = 0;

	for (int i = 0; i < 128; i++)
		air[i] = (uint8_t)lcbit(bits, i);
	for (int c = 0; c < 16; c++)
		for (int r = 0; r < 8; r++)
			M[r][c] = air[n++];
	memset(lc, 0, 9);
	n = 0;
	for (int r = 0; r < 7; r++)
		for (int c = 0; c < (r < 2 ? 11 : 10); c++, n++)
			if (M[r][c])
				lc[n / 8] |= (uint8_t)(0x80 >> (n % 8));
	ref_emb_encode(lc, again);
	return memcmp(air, again, 128) == 0;
}

static void pack_bits(const uint8_t air[128], uint8_t out[16])
{
	memset(out, 0, 16);
	for (int i = 0; i < 128; i++)
		if (air[i])
			out[i / 8] |= (uint8_t)(0x80 >> (i % 8));
}

static void mk_lc(uint8_t lc[9], uint8_t flco, uint32_t dst, uint32_t src)
{
	dmr_lc_t l = { .flco = flco, .dst = dst, .src = src };

	dmr_lc_pack(&l, lc);
}

// ---------------------------------------------------------------------------------------------------------------
// Coding tests

static void test_lc_pack(void)
{
	dmr_lc_t l = { .pf = true, .flco = 3, .fid = 0x10, .svc = 0xC1, .dst = 0x123456, .src = 0xABCDEF }, u;
	uint8_t b[9];

	dmr_lc_pack(&l, b);
	CHECK(b[0] == 0x83 && b[1] == 0x10 && b[2] == 0xC1 && b[3] == 0x12 && b[5] == 0x56 && b[6] == 0xAB && b[8] == 0xEF);
	dmr_lc_unpack(b, &u);
	CHECK(u.pf && u.flco == 3 && u.fid == 0x10 && u.svc == 0xC1 && u.dst == 0x123456 && u.src == 0xABCDEF);
	CHECK(dmr_lc_is_voice(b));
	b[0] = 0x04;
	CHECK(!dmr_lc_is_voice(b));
}

static void test_rs129(void)
{
	uint8_t zero[9] = { 0 }, full[12], lc[9];

	// Known values: the all-zero LC has zero parity, so the header / terminator carry the bare masks
	dmr_lc_encode_full(zero, DMR_RS_MASK_VOICE_HEADER, full);
	CHECK(full[9] == 0x96 && full[10] == 0x96 && full[11] == 0x96);
	dmr_lc_encode_full(zero, DMR_RS_MASK_TERMINATOR, full);
	CHECK(full[9] == 0x99 && full[10] == 0x99 && full[11] == 0x99);

	// Group voice TG 9 from 2345678 (the example LC of sfrTest), and random LCs: parity equal to the long-division
	// reference, syndromes zero, accepted by sfr.c's independent checker with the right mask only
	srand(12345);
	for (int n = 0; n < 300; n++) {
		uint8_t d[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x23, 0xCA, 0xCE }, p[3], q[3];

		if (n)
			for (int i = 0; i < 9; i++)
				d[i] = (uint8_t)rand();
		dmr_rs129_parity(d, p);
		ref_rs_parity(d, q);
		CHECK(memcmp(p, q, 3) == 0);

		uint8_t cw[12];
		memcpy(cw, d, 9);
		memcpy(&cw[9], p, 3);
		CHECK(ref_rs_eval(cw, 1) == 0 && ref_rs_eval(cw, 2) == 0 && ref_rs_eval(cw, 3) == 0);

		dmr_lc_encode_full(d, DMR_RS_MASK_VOICE_HEADER, full);
		CHECK(sfrDataBurstCrcOk(DT_VOICE_LC_HEADER, full));
		CHECK(dmr_lc_decode_full(full, DMR_RS_MASK_VOICE_HEADER, lc) == 0 && memcmp(lc, d, 9) == 0);
		CHECK(dmr_lc_decode_full(full, DMR_RS_MASK_TERMINATOR, lc) != 0);	// header isn't a terminator

		// Any one octet wrong (data or parity) is corrected
		int pos = n % 12;
		uint8_t bad[12];
		memcpy(bad, full, 12);
		bad[pos] ^= (uint8_t)(1 + rand() % 255);
		CHECK(dmr_lc_decode_full(bad, DMR_RS_MASK_VOICE_HEADER, lc) == 1 && memcmp(lc, d, 9) == 0);
		// Two octets wrong are detected, never miscorrected (distance 4)
		bad[(pos + 5) % 12] ^= (uint8_t)(1 + rand() % 255);
		CHECK(dmr_lc_decode_full(bad, DMR_RS_MASK_VOICE_HEADER, lc) == -1);

		dmr_lc_encode_full(d, DMR_RS_MASK_TERMINATOR, full);
		CHECK(sfrDataBurstCrcOk(DT_TERMINATOR_WITH_LC, full));
		CHECK(dmr_lc_decode_full(full, DMR_RS_MASK_TERMINATOR, lc) == 0);
	}
}

static void test_csbk(void)
{
	uint8_t mine[12], ten[10] = { 0xB8, 0, 0, 0, 0, 0, 0, 0x2F, 0x7D, 0x99 };	// from 3112345 = 0x2F7D99
	dmrBurst_t ref;

	dmr_csbk_bs_activation(OWN_ID, mine);
	CHECK(memcmp(mine, ten, 10) == 0);
	CHECK(dmrDataBuildCSBK(ten, &ref) > 0);
	CHECK(memcmp(mine, ref.payload, 12) == 0);	// same CRC as the data service's builder
	CHECK(sfrDataBurstCrcOk(DT_CSBK, mine));
}

static void test_embedded(void)
{
	uint8_t lc[9], bits[16], air[128], refbits[16], out[9], frag[4][4];

	// Checksum: sum of octets mod 31
	mk_lc(lc, 0, 9, 1);
	CHECK(dmr_emb_lc_checksum(lc) == 10);
	memset(lc, 0xFF, 9);
	CHECK(dmr_emb_lc_checksum(lc) == (9 * 255) % 31);
	// The all-zero LC is the all-zero embedded signalling (linear code, zero checksum)
	memset(lc, 0, 9);
	dmr_emb_lc_encode_bits(lc, bits);
	for (int i = 0; i < 16; i++)
		CHECK(bits[i] == 0);

	srand(777);
	for (int n = 0; n < 300; n++) {
		for (int i = 0; i < 9; i++)
			lc[i] = (uint8_t)rand();
		dmr_emb_lc_encode_bits(lc, bits);
		ref_emb_encode(lc, air);
		pack_bits(air, refbits);
		CHECK(memcmp(bits, refbits, 16) == 0);
		CHECK(ref_emb_decode(bits, out) && memcmp(out, lc, 9) == 0);
		CHECK(dmr_emb_lc_decode_bits(bits, out) && memcmp(out, lc, 9) == 0);

		// One bit error anywhere is corrected
		uint8_t bad[16];
		int e = n % 128;
		memcpy(bad, bits, 16);
		bad[e / 8] ^= (uint8_t)(0x80 >> (e % 8));
		CHECK(dmr_emb_lc_decode_bits(bad, out) && memcmp(out, lc, 9) == 0);
		// Two errors in one row (air bits 8 apart are one row, adjacent columns) are refused
		int e2 = (e + 8) % 128;
		if (e % 8 != 7 && e2 != 127 && e != 127) {
			bad[e2 / 8] ^= (uint8_t)(0x80 >> (e2 % 8));
			CHECK(!dmr_emb_lc_decode_bits(bad, out));
		}

		// Fragments B-E are the 4 x 32 bits in order; the reassembler wants first, cont, cont, last
		dmr_emb_lc_encode(lc, frag);
		CHECK(memcmp(frag[0], bits, 4) == 0 && memcmp(frag[3], &bits[12], 4) == 0);
		dmr_emb_rx_t rx;
		dmr_emb_rx_reset(&rx);
		CHECK(!dmr_emb_rx_push(&rx, frag[0], DMR_LCSS_FIRST, out));
		CHECK(!dmr_emb_rx_push(&rx, frag[1], DMR_LCSS_CONTINUATION, out));
		CHECK(!dmr_emb_rx_push(&rx, frag[2], DMR_LCSS_CONTINUATION, out));
		memset(out, 0, 9);
		CHECK(dmr_emb_rx_push(&rx, frag[3], DMR_LCSS_LAST, out) && memcmp(out, lc, 9) == 0);
		// A missing continuation spoils it
		CHECK(!dmr_emb_rx_push(&rx, frag[0], DMR_LCSS_FIRST, out));
		CHECK(!dmr_emb_rx_push(&rx, frag[1], DMR_LCSS_CONTINUATION, out));
		CHECK(!dmr_emb_rx_push(&rx, frag[3], DMR_LCSS_LAST, out));
	}
	CHECK(dmr_emb_lcss(0) == 0 && dmr_emb_lcss(1) == 1 && dmr_emb_lcss(2) == 3 && dmr_emb_lcss(3) == 3 &&
	      dmr_emb_lcss(4) == 2 && dmr_emb_lcss(5) == 0);
}

// Talker alias LCs for a string: 8-bit format (header 6 characters, blocks 7) or 7-bit (7, 8)
static int mk_alias(const char *text, int format, uint8_t lcs[4][9])
{
	int len = (int)strlen(text), width = format == DMR_TA_7BIT ? 7 : 8, start = format == DMR_TA_7BIT ? 0 : 1;
	uint8_t stream[28] = { 0 };

	for (int i = 0; i < len; i++)
		for (int b = 0; b < width; b++)
			if ((text[i] >> (width - 1 - b)) & 1) {
				int at = start + i * width + b;
				stream[at / 8] |= (uint8_t)(0x80 >> (at % 8));
			}
	int nbits = start + len * width, blocks = nbits <= 49 ? 0 : (nbits - 49 + 55) / 56;

	memset(lcs, 0, 4 * 9);
	lcs[0][0] = DMR_FLCO_TA_HEADER;
	lcs[0][2] = (uint8_t)((format << 6) | (len << 1) | lcbit(stream, 0));
	for (int i = 0; i < 48; i++)
		if (lcbit(stream, 1 + i))
			lcs[0][3 + i / 8] |= (uint8_t)(0x80 >> (i % 8));
	for (int bl = 1; bl <= blocks; bl++) {
		lcs[bl][0] = (uint8_t)(DMR_FLCO_TA_HEADER + bl);
		for (int i = 0; i < 56; i++)
			if (lcbit(stream, 49 + (bl - 1) * 56 + i))
				lcs[bl][2 + i / 8] |= (uint8_t)(0x80 >> (i % 8));
	}
	return 1 + blocks;
}

static void test_alias(void)
{
	uint8_t lcs[4][9];
	dmr_ta_t ta;
	int n = mk_alias("N0CALL Spencer", DMR_TA_ISO8, lcs);

	CHECK(n == 3);
	dmr_ta_reset(&ta);
	CHECK(dmr_ta_push(&ta, lcs[0]));
	CHECK(strcmp(ta.text, "N0CALL") == 0 && !ta.complete);
	CHECK(!dmr_ta_push(&ta, lcs[2]));	// block 2 before block 1: nothing new shows
	CHECK(strcmp(ta.text, "N0CALL") == 0);
	CHECK(dmr_ta_push(&ta, lcs[1]));
	CHECK(strcmp(ta.text, "N0CALL Spencer") == 0 && ta.complete);
	CHECK(!dmr_ta_push(&ta, lcs[1]));	// repeats change nothing

	n = mk_alias("KD9XYZ Bob Smith Radio", DMR_TA_7BIT, lcs);
	dmr_ta_reset(&ta);
	for (int i = 0; i < n; i++)
		dmr_ta_push(&ta, lcs[i]);
	CHECK(strcmp(ta.text, "KD9XYZ Bob Smith Radio") == 0 && ta.complete);
}

// ---------------------------------------------------------------------------------------------------------------
// Call stack harness

static uint32_t now;
static unsigned sub_ms;	// time into the current slot

static struct {
	unsigned begins, ends, frames, losts;
	dmr_caller_t caller;
	uint8_t first[9];
	uint8_t last[9];
	uint8_t seq[256];	// first octet of each frame played
} sink_log;

static void s_begin(const dmr_caller_t *c, void *ctx) { (void)ctx; sink_log.begins++; sink_log.caller = *c; }
static void s_frame(const uint8_t f[9], void *ctx)
{
	(void)ctx;
	if (sink_log.frames == 0)
		memcpy(sink_log.first, f, 9);
	memcpy(sink_log.last, f, 9);
	if (sink_log.frames < sizeof(sink_log.seq))
		sink_log.seq[sink_log.frames] = f[0];
	sink_log.frames++;
}
static void s_lost(void *ctx) { (void)ctx; sink_log.losts++; }
static void s_end(void *ctx) { (void)ctx; sink_log.ends++; }

static const dmr_voice_sink_t sink = { .begin = s_begin, .frame = s_frame, .lost = s_lost, .end = s_end };

// TX source: a frame every 20 ms of simulated time, numbered in octet 0 (and a fixed pattern after)
static struct {
	bool on;
	uint32_t start_ms;
	unsigned produced, taken, begins, ends;
	bool starve;
} src;

static void src_begin(void *ctx) { (void)ctx; src.on = true; src.start_ms = now; src.produced = src.taken = 0; src.begins++; }
static void src_end(void *ctx) { (void)ctx; src.on = false; src.ends++; }
static bool src_next(uint8_t f[9], void *ctx)
{
	(void)ctx;
	if (!src.on || src.starve)
		return false;
	unsigned avail = (now - src.start_ms) / 20 + 1;	// first frame 0 ms after begin
	if (src.taken >= avail)
		return false;
	f[0] = (uint8_t)(src.taken + 1);
	for (int i = 1; i < 9; i++)
		f[i] = (uint8_t)(0xA0 + i);
	src.taken++;
	return true;
}

static const dmr_voice_source_t source = { .begin = src_begin, .next = src_next, .end = src_end };

// 10 ms steps; a slot boundary every 30 ms; the stack polled every step
static void run_ms(unsigned ms)
{
	for (unsigned t = 0; t < ms; t += 10) {
		now += 10;
		sub_ms += 10;
		if (sub_ms >= 30) {
			sub_ms = 0;
			sim_slot();
		}
		dmr_call_poll(now);
	}
}

static dmr_call_config_t base_cfg(void)
{
	static const uint32_t list[] = { TG_LIST, 9 };
	dmr_call_config_t c;

	dmr_call_config_default(&c);
	c.own_id = OWN_ID;
	c.cc = 1;
	c.ts = 1;
	c.talk_target.id = TG_TALK;
	c.listen.groups = list;
	c.listen.group_count = 2;
	return c;
}

static void setup(const dmr_call_config_t *c)
{
	sim_reset();
	memset(&sink_log, 0, sizeof(sink_log));
	memset(&src, 0, sizeof(src));
	now = 1000;
	sub_ms = 0;
	dmr_call_init(&sim_modem);
	dmr_call_set_voice(&sink, &source);
	dmr_call_configure(c);
	CHECK(dmr_call_start());
	dmr_call_poll(now);
}

static const dmr_call_view_t *V(void) { return dmr_call_view(); }

// Received bursts as a driver reports them
static void rx_header(const uint8_t lc[9], uint8_t cc, int8_t ts, uint8_t sync, bool terminator)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = terminator ? DMR_BURST_TERMINATOR_LC : DMR_BURST_VOICE_LC_HEADER;
	b.sync = sync;
	b.cc = cc;
	b.ts = ts;
	b.crc_ok = true;
	b.in_step = true;
	b.len = 12;
	dmr_lc_encode_full(lc, terminator ? DMR_RS_MASK_TERMINATOR : DMR_RS_MASK_VOICE_HEADER, b.payload);
	sim_rx(&b);
}

// Voice burst; frames numbered from *frame; lc != NULL: the modem reports an assembled embedded LC with it
static void rx_voice(uint8_t index, uint8_t cc, int8_t ts, uint8_t sync, unsigned *frame, const uint8_t *lc, bool pi)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.sync = index == 0 ? sync : DMR_SYNC_NONE;
	b.cc = cc;
	b.ts = ts;
	b.crc_ok = true;
	b.pi = pi;
	b.in_step = true;
	b.rssi_dbm = -85;
	b.len = 27;
	for (int f = 0; f < 3; f++) {
		b.payload[f * 9] = (uint8_t)((*frame)++);
		memset(&b.payload[f * 9 + 1], 0x55, 8);
	}
	if (lc) {
		b.lc_valid = true;
		memcpy(b.lc, lc, 9);
	}
	sim_rx(&b);
}

// A call as heard: header (optional), superframes from burst `from`, embedded LC reported on each F, terminator
static void rx_call(const uint8_t lc[9], uint8_t cc, bool header, int from, int superframes, bool term, unsigned *frame)
{
	if (header) {
		for (int h = 0; h < 3; h++) {
			rx_header(lc, cc, 0, DMR_SYNC_MS, false);
			run_ms(60);
		}
	}
	for (int sf = 0; sf < superframes; sf++)
		for (int i = (sf == 0 ? from : 0); i < 6; i++) {
			rx_voice((uint8_t)i, cc, 0, DMR_SYNC_MS, frame, i == 5 ? lc : NULL, false);
			run_ms(60);
		}
	if (term) {
		rx_header(lc, cc, 0, DMR_SYNC_MS, true);
		run_ms(60);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// TX

static void check_tx_burst_sequence(unsigned first, uint8_t expect_lc[9], unsigned *voice_bursts)
{
	uint8_t full[12];
	unsigned i = first, v = 0;

	dmr_lc_encode_full(expect_lc, DMR_RS_MASK_VOICE_HEADER, full);
	for (int h = 0; h < DMR_CALL_TX_HEADERS; h++, i++) {
		CHECK(sim.air[i].kind == DMR_BURST_VOICE_LC_HEADER);
		CHECK(sim.air[i].len == 12 && sim.air[i].host_crc && memcmp(sim.air[i].payload, full, 12) == 0);
	}
	// Voice straight after the headers (gotcha 1.1), A-F in order, B-E carrying the LC's fragments
	dmr_emb_rx_t rx;
	dmr_emb_rx_reset(&rx);
	unsigned lcs = 0;
	while (i < sim.air_count && sim.air[i].kind == DMR_BURST_VOICE) {
		sim_air_t *a = &sim.air[i];
		uint8_t out[9];

		CHECK(a->voice_index == v % 6);
		CHECK(a->len == 27 && !a->host_crc);
		CHECK(a->lcss == dmr_emb_lcss(a->voice_index));
		if (a->voice_index >= 1 && a->voice_index <= 4 && dmr_emb_rx_push(&rx, a->emb, a->lcss, out)) {
			CHECK(memcmp(out, expect_lc, 9) == 0);
			lcs++;
		}
		if (a->voice_index == 4) {	// all four fragments of this superframe: the reference decoder agrees
			uint8_t bits[16], out2[9];

			for (int k = 0; k < 4; k++)
				memcpy(&bits[4 * k], sim.air[i - 3 + k].emb, 4);
			CHECK(ref_emb_decode(bits, out2) && memcmp(out2, expect_lc, 9) == 0);
		}
		i++;
		v++;
	}
	CHECK(v > 0 && v % 6 == 0);	// ends at a superframe boundary
	CHECK(lcs == v / 6);
	dmr_lc_encode_full(expect_lc, DMR_RS_MASK_TERMINATOR, full);
	CHECK(i < sim.air_count && sim.air[i].kind == DMR_BURST_TERMINATOR_LC);
	CHECK(sim.air[i].host_crc && memcmp(sim.air[i].payload, full, 12) == 0);
	CHECK(i + 1 == sim.air_count);
	*voice_bursts = v;
}

static void test_tx_call(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned v;

	setup(&c);
	CHECK(sim.started && sim.cc_filter == 1 && sim.locked_ts == 0);
	dmr_call_ptt(true);
	run_ms(10);
	CHECK(V()->state == DMR_CALL_STATE_TX && V()->phase == DMR_CALL_PHASE_TX_VOICE);
	CHECK(sim.tx_begins == 1 && sim.last_timing == DMR_TIMING_OWN && sim.last_ts == 1);
	mk_lc(lc, DMR_FLCO_GROUP_VOICE, TG_TALK, OWN_ID);
	CHECK(sim.emb_lc_set && memcmp(sim.emb_lc, lc, 9) == 0);
	CHECK(V()->caller.src == OWN_ID && V()->caller.dst == TG_TALK && V()->caller.group);
	CHECK(src.begins == 1);
	run_ms(1000);
	dmr_call_ptt(false);	// mid superframe
	run_ms(10);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_VOICE);	// finishing the superframe
	run_ms(600);
	CHECK(V()->state == DMR_CALL_STATE_IDLE && V()->phase == DMR_CALL_PHASE_IDLE);
	CHECK(sim.tx_ends == 1 && sim.tx_aborts == 0 && sim.underflows == 0);
	CHECK(src.ends == 1);
	check_tx_burst_sequence(0, lc, &v);
	CHECK(v >= 18);
	// Every encoded frame went out, in order, with silence only where the encoder had nothing yet
	unsigned silent = 0, expect = 1;
	for (unsigned i = DMR_CALL_TX_HEADERS; i < DMR_CALL_TX_HEADERS + v; i++)
		for (int f = 0; f < 3; f++) {
			const uint8_t *fr = &sim.air[i].payload[f * 9];
			if (memcmp(fr, dmr_ambe_silence, 9) == 0) {
				silent++;
			} else {
				CHECK(fr[0] == (uint8_t)expect);
				expect++;
			}
		}
	CHECK(silent == V()->tx_underruns);
	CHECK(silent <= 3);
	printf("# TX: %u headers, %u voice bursts, %u silence frames, 1 terminator\n", DMR_CALL_TX_HEADERS, v, silent);
	// The talk target was the channel's: no reply countdown
	CHECK(!V()->reply_active && V()->tx_target.id == TG_TALK);
}

static void test_tx_timeout_and_starve(void)
{
	dmr_call_config_t c = base_cfg();
	unsigned v;
	uint8_t lc[9];

	c.tot_s = 1;
	setup(&c);
	src.starve = true;	// encoder never ready: all silence, but the burst train keeps going
	dmr_call_ptt(true);
	run_ms(2000);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_DENIED && V()->tx_error == DMR_TX_ERR_TIMEOUT);
	CHECK(sim.underflows == 0);
	mk_lc(lc, 0, TG_TALK, OWN_ID);
	check_tx_burst_sequence(0, lc, &v);
	CHECK(v == 18);	// 1 s: headers 180 ms, then voice up to the superframe boundary after the time-out
	dmr_call_ptt(false);
	run_ms(20);
	CHECK(V()->phase == DMR_CALL_PHASE_IDLE);
}

static void test_tx_refused(void)
{
	dmr_call_config_t c = base_cfg();

	setup(&c);
	sim.refuse_tx = true;
	dmr_call_ptt(true);
	run_ms(30);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_DENIED && V()->tx_error == DMR_TX_ERR_REJECTED);
	dmr_call_ptt(false);
	run_ms(30);
	sim.refuse_tx = false;
	// The modem's polite refusal during the call
	dmr_call_ptt(true);
	run_ms(100);
	sim.reject_next = true;
	run_ms(100);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_DENIED && V()->tx_error == DMR_TX_ERR_REJECTED);
	CHECK(src.ends == 1);
	dmr_call_ptt(false);
	run_ms(30);
	CHECK(V()->phase == DMR_CALL_PHASE_IDLE);
}

static void test_rmo_wakeup(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t csbk[12], lc[9];
	unsigned v;

	c.rmo = true;
	c.ts = 2;
	setup(&c);
	CHECK(sim.locked_ts == 2 && sim.cfg.repeater);
	sim.wake_repeater_on_csbk = true;
	dmr_call_ptt(true);
	run_ms(10);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_WAKEUP && V()->state == DMR_CALL_STATE_TX);
	run_ms(400);
	dmr_csbk_bs_activation(OWN_ID, csbk);
	CHECK(sim.air_count >= 1 && sim.air[0].kind == DMR_BURST_CSBK && sim.air[0].host_crc &&
	      memcmp(sim.air[0].payload, csbk, 12) == 0);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_VOICE && V()->repeater_up);
	CHECK(sim.last_timing == DMR_TIMING_ALIGNED && sim.last_ts == 2 && sim.last_access == DMR_ACCESS_IMPOLITE);
	run_ms(700);
	dmr_call_ptt(false);
	run_ms(600);
	mk_lc(lc, 0, TG_TALK, OWN_ID);
	check_tx_burst_sequence(1, lc, &v);
	CHECK(V()->phase == DMR_CALL_PHASE_IDLE);

	// A second PTT while the repeater is still up: no wake-up
	unsigned before = sim.air_count;
	dmr_call_ptt(true);
	run_ms(100);
	CHECK(sim.air[before].kind == DMR_BURST_VOICE_LC_HEADER);
	dmr_call_ptt(false);
	run_ms(600);

	// No repeater: three tries 600 ms apart, then refused
	setup(&c);
	dmr_call_ptt(true);
	run_ms(3000);
	unsigned csbks = 0;
	for (unsigned i = 0; i < sim.air_count; i++)
		csbks += sim.air[i].kind == DMR_BURST_CSBK;
	CHECK(csbks == DMR_CALL_WAKEUP_ATTEMPTS && sim.air_count == csbks);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_DENIED && V()->tx_error == DMR_TX_ERR_NO_REPEATER);
	dmr_call_ptt(false);
	run_ms(20);
	CHECK(V()->phase == DMR_CALL_PHASE_IDLE);
}

// ---------------------------------------------------------------------------------------------------------------
// RX

static void test_rx_call(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned frame = 1;

	setup(&c);
	mk_lc(lc, DMR_FLCO_GROUP_VOICE, TG_LIST, 2345678);
	rx_header(lc, 1, 0, DMR_SYNC_MS, false);
	run_ms(20);
	CHECK(V()->state == DMR_CALL_STATE_RX && V()->phase == DMR_CALL_PHASE_RX_VOICE);
	CHECK(sink_log.begins == 1 && sink_log.caller.src == 2345678 && sink_log.caller.dst == TG_LIST &&
	      sink_log.caller.group && !sink_log.caller.late_entry);
	CHECK(V()->caller.src == 2345678 && V()->rssi_dbm == 0);
	run_ms(40);
	rx_call(lc, 1, true, 0, 2, false, &frame);	// more headers (same call), then two superframes
	CHECK(sink_log.begins == 1 && sink_log.frames == 36 && sink_log.losts == 0);
	for (unsigned i = 0; i < 36; i++)
		CHECK(sink_log.seq[i] == (uint8_t)(i + 1));
	CHECK(V()->rssi_dbm == -85);
	rx_header(lc, 1, 0, DMR_SYNC_MS, true);
	run_ms(20);
	CHECK(sink_log.ends == 1 && V()->state == DMR_CALL_STATE_IDLE);
	CHECK(V()->last_heard.src == 2345678 && V()->last_heard_ms == now - 10);
	// Reply window: PTT now goes to the TG just heard
	CHECK(V()->reply_active && V()->tx_target.id == TG_LIST && !V()->tx_target.private_call);
	CHECK(V()->reply_s == 10);
	dmr_call_ptt(true);
	run_ms(20);
	uint8_t want[9];
	mk_lc(want, 0, TG_LIST, OWN_ID);
	CHECK(memcmp(sim.emb_lc, want, 9) == 0);
	dmr_call_ptt(false);
	run_ms(600);
	CHECK(V()->reply_active && V()->tx_target.id == TG_LIST);	// renewed by our reply
	dmr_call_reply_cancel();
	run_ms(10);
	CHECK(!V()->reply_active && V()->tx_target.id == TG_TALK);

	// A burst missing in the middle is concealed; the fade timeout ends a call without a terminator
	setup(&c);
	frame = 1;
	rx_call(lc, 1, true, 0, 1, false, &frame);
	unsigned skip = frame;
	frame += 3;
	run_ms(60);	// burst A of the next superframe never heard
	for (int i = 1; i < 6; i++) {
		rx_voice((uint8_t)i, 1, 0, DMR_SYNC_MS, &frame, NULL, false);
		run_ms(60);
	}
	CHECK(sink_log.frames == 18 + 15 && sink_log.losts == 3);
	CHECK(sink_log.seq[18] == (uint8_t)(skip + 3));
	run_ms(DMR_CALL_FADE_MS - 60);
	CHECK(V()->state == DMR_CALL_STATE_RX);
	run_ms(60);
	CHECK(V()->state == DMR_CALL_STATE_IDLE && sink_log.ends == 1);
}

static void test_rx_filters(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned frame = 1;

	// Wrong color code: nothing at all
	setup(&c);
	mk_lc(lc, 0, TG_TALK, 2345678);
	rx_call(lc, 2, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 0 && sink_log.frames == 0 && V()->serial > 0 && V()->last_heard_ms == 0);

	// Talkgroup not listened to: the slot is busy but nothing is played
	mk_lc(lc, 0, 1234, 2345678);
	rx_call(lc, 1, true, 0, 1, false, &frame);
	CHECK(V()->phase == DMR_CALL_PHASE_RX_BUSY && V()->caller.dst == 1234);
	CHECK(sink_log.begins == 0 && sink_log.frames == 0);
	CHECK(!dmr_call_channel_clear(now));
	// ... and polite PTT is refused while it lasts
	dmr_call_ptt(true);
	run_ms(20);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_DENIED && V()->tx_error == DMR_TX_ERR_BUSY && sim.tx_begins == 0);
	dmr_call_ptt(false);
	rx_header(lc, 1, 0, DMR_SYNC_MS, true);
	run_ms(20);
	CHECK(V()->phase == DMR_CALL_PHASE_IDLE && !V()->reply_active);
	CHECK(!dmr_call_channel_clear(now));
	run_ms(DMR_CALL_LBT_DMO_MS);
	CHECK(dmr_call_channel_clear(now));

	// Hear all plays it; all call always plays
	c.listen.hear_all = true;
	setup(&c);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 1 && sink_log.frames == 18);
	c.listen.hear_all = false;
	setup(&c);
	mk_lc(lc, 0, DMR_ID_ALL_CALL, 2345678);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 1);

	// Private call to us: played, reply goes back privately
	setup(&c);
	mk_lc(lc, DMR_FLCO_UNIT_VOICE, OWN_ID, 2345678);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 1 && sink_log.caller.to_us && !sink_log.caller.group && sink_log.frames == 18);
	CHECK(V()->reply_active && V()->tx_target.id == 2345678 && V()->tx_target.private_call);
	dmr_call_ptt(true);
	run_ms(20);
	uint8_t want[9];
	mk_lc(want, DMR_FLCO_UNIT_VOICE, 2345678, OWN_ID);
	CHECK(memcmp(sim.emb_lc, want, 9) == 0);
	dmr_call_ptt(false);
	run_ms(600);

	// Private call to someone else: not played
	setup(&c);
	mk_lc(lc, DMR_FLCO_UNIT_VOICE, 7654321, 2345678);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 0);

	// Group list by callback
	setup(&c);
	mk_lc(lc, 0, 777, 2345678);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 0);

	// Our own call echoed by a repeater, with its terminators: never played, never ends someone else's call
	c.rmo = true;
	setup(&c);
	mk_lc(lc, 0, TG_TALK, OWN_ID);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 0);
	mk_lc(lc, 0, TG_TALK, 2345678);
	uint8_t own[9];
	mk_lc(own, 0, TG_TALK, OWN_ID);
	run_ms(500);
	rx_call(lc, 1, true, 0, 1, false, &frame);
	rx_header(own, 1, 0, DMR_SYNC_BS, true);	// stale echo of our terminator
	run_ms(20);
	CHECK(V()->phase == DMR_CALL_PHASE_RX_VOICE);
	// Through a repeater, a terminator that isn't this call's doesn't end it
	uint8_t stranger[9];
	mk_lc(stranger, 0, TG_TALK, 9999999);
	rx_header(stranger, 1, 0, DMR_SYNC_BS, true);
	run_ms(20);
	CHECK(V()->phase == DMR_CALL_PHASE_RX_VOICE && V()->caller.src == 2345678);
	// RMO: wrong timeslot is filtered
	mk_lc(lc, 0, TG_LIST, 1111111);
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE_LC_HEADER;
	b.cc = 1;
	b.ts = 2;
	b.sync = DMR_SYNC_BS;
	b.crc_ok = true;
	b.len = 12;
	dmr_lc_encode_full(lc, DMR_RS_MASK_VOICE_HEADER, b.payload);
	sim_rx(&b);
	run_ms(20);
	CHECK(V()->caller.src == 2345678);
}

static bool tg777(uint32_t tg, void *ctx) { (void)ctx; return tg == 777; }

static void test_rx_group_callback(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned frame = 1;

	c.listen.group_ok = tg777;
	setup(&c);
	mk_lc(lc, 0, 777, 2345678);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 1);
}

static void test_late_entry(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9], other[9];
	unsigned frame = 1;

	// Joined at burst C, no header: the call starts at F with the embedded LC, audio from F on
	setup(&c);
	mk_lc(lc, 0, TG_TALK, 2345678);
	rx_call(lc, 1, false, 2, 1, false, &frame);
	CHECK(sink_log.begins == 1 && sink_log.caller.late_entry && sink_log.caller.src == 2345678);
	CHECK(sink_log.frames == 3 && sink_log.seq[0] == 10);	// C, D, E were frames 1-9
	rx_call(lc, 1, false, 0, 1, true, &frame);
	CHECK(sink_log.frames == 21 && sink_log.losts == 0 && sink_log.ends == 1);

	// A lone burst with an LC (noise, a stray) starts nothing
	setup(&c);
	rx_voice(5, 1, 0, DMR_SYNC_MS, &frame, lc, false);
	run_ms(60);
	CHECK(sink_log.begins == 0 && V()->state == DMR_CALL_STATE_IDLE);
	run_ms(500);

	// The next call without a header and with the same LC as the last one (the modem's stale LC): only after a
	// second superframe confirms it
	setup(&c);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 1);
	run_ms(1000);
	rx_call(lc, 1, false, 0, 1, false, &frame);
	CHECK(sink_log.begins == 1 && V()->state == DMR_CALL_STATE_IDLE);
	rx_call(lc, 1, false, 0, 1, false, &frame);
	CHECK(sink_log.begins == 2 && V()->caller.late_entry);
	rx_header(lc, 1, 0, DMR_SYNC_MS, true);
	run_ms(500);

	// A different LC than the stale one is taken at the first superframe
	mk_lc(other, 0, TG_LIST, 3333333);
	rx_call(other, 1, false, 0, 1, false, &frame);
	CHECK(sink_log.begins == 3 && V()->caller.src == 3333333);

	// New talker on the same call without a header (repeater): caller follows the embedded LC
	mk_lc(lc, 0, TG_LIST, 4444444);
	rx_call(lc, 1, false, 0, 1, false, &frame);
	CHECK(sink_log.begins == 4 && V()->caller.src == 4444444 && V()->phase == DMR_CALL_PHASE_RX_VOICE);
	// ... and to a TG we don't listen to: playing stops
	mk_lc(lc, 0, 5555, 4444444);
	rx_call(lc, 1, false, 0, 1, false, &frame);
	CHECK(V()->phase == DMR_CALL_PHASE_RX_BUSY && sink_log.ends == 4);
}

static void test_alias_and_privacy(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9], ta[4][9];
	unsigned frame = 1;
	int n = mk_alias("N0CALL Spencer", DMR_TA_ISO8, ta);

	setup(&c);
	mk_lc(lc, 0, TG_TALK, 2345678);
	rx_call(lc, 1, true, 0, 1, false, &frame);
	// Talker alias in the following superframes' embedded LC (instead of the voice LC)
	for (int sf = 0; sf < n; sf++)
		for (int i = 0; i < 6; i++) {
			rx_voice((uint8_t)i, 1, 0, DMR_SYNC_MS, &frame, i == 5 ? ta[sf] : NULL, false);
			run_ms(60);
		}
	CHECK(strcmp(V()->caller.alias, "N0CALL Spencer") == 0 && V()->caller.alias_complete);
	CHECK(V()->caller.src == 2345678 && sink_log.begins == 1);
	rx_header(lc, 1, 0, DMR_SYNC_MS, true);
	run_ms(20);
	CHECK(strcmp(V()->last_heard.alias, "N0CALL Spencer") == 0);

	// PI set and no key: shown as encrypted, never played
	setup(&c);
	rx_header(lc, 1, 0, DMR_SYNC_MS, false);
	run_ms(60);
	for (int i = 0; i < 6; i++) {
		rx_voice((uint8_t)i, 1, 0, DMR_SYNC_MS, &frame, NULL, true);
		run_ms(60);
	}
	CHECK(sink_log.begins == 1 && sink_log.frames == 0 && sink_log.losts == 18 && V()->caller.encrypted);
}

static unsigned privacy_calls;
static void privacy_xor(uint8_t v[27], uint8_t index, bool tx, void *ctx)
{
	(void)ctx;
	(void)tx;
	privacy_calls++;
	for (int i = 0; i < 27; i++)
		v[i] ^= (uint8_t)(0x10 + index);
}

static void test_privacy_hook(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned frame = 1;

	c.privacy = privacy_xor;
	c.tx_fid = 0x10;
	c.tx_svc = DMR_SVC_PRIVACY;
	setup(&c);
	mk_lc(lc, 0, TG_TALK, 2345678);
	lc[2] = DMR_SVC_PRIVACY;
	rx_header(lc, 1, 0, DMR_SYNC_MS, false);
	run_ms(60);
	rx_voice(0, 1, 0, DMR_SYNC_MS, &frame, NULL, true);
	run_ms(60);
	CHECK(sink_log.frames == 3 && sink_log.first[0] == (1 ^ 0x10) && privacy_calls == 1);

	setup(&c);
	privacy_calls = 0;
	dmr_call_ptt(true);
	run_ms(500);
	dmr_call_ptt(false);
	run_ms(600);
	CHECK(sim.air[0].payload[0] == 0 && sim.air[0].payload[1] == 0x10 && sim.air[0].payload[2] == DMR_SVC_PRIVACY);
	CHECK(sim.air[3].kind == DMR_BURST_VOICE && sim.air[3].payload[9] == (2 ^ 0x10));
	CHECK(privacy_calls > 0);
}

static void test_impolite_over_call(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned frame = 1;

	c.access = DMR_ACCESS_IMPOLITE;
	setup(&c);
	mk_lc(lc, 0, TG_LIST, 2345678);
	rx_call(lc, 1, true, 0, 1, false, &frame);
	CHECK(V()->phase == DMR_CALL_PHASE_RX_VOICE);
	dmr_call_ptt(true);
	run_ms(20);
	// Keyed over the call: it ends here, and the reply goes to its TG
	CHECK(V()->phase == DMR_CALL_PHASE_TX_VOICE && sink_log.ends == 1);
	CHECK(V()->caller.dst == TG_LIST && sim.last_timing == DMR_TIMING_ALIGNED);
	// Bursts heard while we transmit are ignored
	rx_voice(0, 1, 0, DMR_SYNC_MS, &frame, NULL, false);
	run_ms(20);
	CHECK(sink_log.frames == 18 && V()->state == DMR_CALL_STATE_TX);
	dmr_call_ptt(false);
	run_ms(600);
	CHECK(V()->state == DMR_CALL_STATE_IDLE);

	// Polite with a wait: keys once the call ends
	c.access = DMR_ACCESS_POLITE_CC;
	c.access_wait_ms = 2000;
	setup(&c);
	rx_call(lc, 1, true, 0, 1, false, &frame);
	dmr_call_ptt(true);
	run_ms(100);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_ACCESS && V()->state == DMR_CALL_STATE_RX && sim.tx_begins == 0);
	rx_header(lc, 1, 0, DMR_SYNC_MS, true);
	run_ms(30);
	CHECK(V()->phase == DMR_CALL_PHASE_TX_VOICE && sim.tx_begins == 1);
	dmr_call_ptt(false);
	run_ms(600);
}

static void test_reconfigure(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned frame = 1;

	setup(&c);
	mk_lc(lc, 0, TG_TALK, 2345678);
	rx_call(lc, 1, true, 0, 1, false, &frame);
	CHECK(V()->state == DMR_CALL_STATE_RX);
	c.cc = 3;
	dmr_call_configure(&c);
	run_ms(10);
	CHECK(V()->state == DMR_CALL_STATE_IDLE && sink_log.ends == 1 && sim.cc_filter == 3 && V()->cc == 3);
	c.listen.any_cc = true;
	dmr_call_configure(&c);
	CHECK(sim.cc_filter == -1);
	rx_call(lc, 7, true, 0, 1, false, &frame);
	CHECK(V()->state == DMR_CALL_STATE_RX && V()->cc == 7);

	dmr_call_stop();
	CHECK(!sim.started);
	dmr_call_ptt(true);
	run_ms(20);
	CHECK(V()->tx_error == DMR_TX_ERR_NO_MODEM && V()->phase == DMR_CALL_PHASE_TX_DENIED);
	dmr_call_ptt(false);
	run_ms(20);
}

int main(void)
{
	test_lc_pack();
	test_rs129();
	test_csbk();
	test_embedded();
	test_alias();
	test_tx_call();
	test_tx_timeout_and_starve();
	test_tx_refused();
	test_rmo_wakeup();
	test_rx_call();
	test_rx_filters();
	test_rx_group_callback();
	test_late_entry();
	test_alias_and_privacy();
	test_privacy_hook();
	test_impolite_over_call();
	test_reconfigure();
	printf("# dmrCallTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
