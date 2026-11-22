// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN frames and calls (core/nxdn): the scrambler against its register, the sync patterns against the standard's
// symbol lists, the LICH, SACCH and FACCH1 codecs (round trips, corrected errors, CRC), the VCH's place and the
// standard's FEC vectors for it, the VCALL and TX_REL messages, and whole calls built by the transmit side through
// the framer into the receive side: voice in order, late entry, RAN squelch, encryption, coasting, 9600 bit/s.
// Voice frames are made-up 72-bit patterns.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxdn/nxdn.h"
#include "air/air_syms.h"
#include "dmr/ambe/ambeFec.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x5EED1;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void fill(uint8_t *p, size_t n)
{
	while (n--)
		*p++ = (uint8_t)rng();
}

static uint8_t bget(const uint8_t *p, unsigned i) { return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1); }

// Flip symbol k's sign (an error the receiver reads as the wrong high bit)
static void flip_sign(uint8_t *frame, uint16_t k)
{
	air_sym_put(frame, k, (uint8_t)(air_sym_get(frame, k) ^ 2));
}

static void testScrambler(void)
{
	// 4.6: X^9 + X^4 + 1, S8..S0 = 0 1 1 1 0 0 1 0 0, output S0, feedback S0 ^ S4 into S8
	unsigned s = 0x0E4, ones = 0;

	for (uint16_t k = 0; k < NXDN_FSW_SYMS; k++)
		CHECK(nxdn_pn(k) == 0, "FSW symbol %u scrambled", k);
	for (uint16_t k = NXDN_FSW_SYMS; k < NXDN_FRAME_SYMS; k++) {
		unsigned out = s & 1, fb = (s ^ (s >> 4)) & 1;

		CHECK(nxdn_pn(k) == out, "PN symbol %u", k);
		ones += out;
		s = (s >> 1) | (fb << 8);
	}
	printf("  scrambler: 182 symbols, %u inverted\n", ones);
}

static void testSyncs(void)
{
	static const int8_t fsw[10] = { -3, 1, -3, 3, -3, -3, 3, 3, -1, 3 };				// 4.4.4
	static const int8_t pre[12] = { 3, 3, 3, -3, 3, -3, 3, 3, -3, -3, -3, 3 };			// table 4.4-1
	const air_profile_t *p = &nxdn_air_mode.profile;
	uint8_t frame[NXDN_FRAME_BYTES] = { 0 };

	nxdn_frame_begin(frame, 0x56);
	for (uint8_t i = 0; i < 10; i++) {
		CHECK(air_sym_level(air_sym_get(frame, i)) == fsw[i], "FSW %u", i);
		CHECK(air_sym_level(air_sym_get(p->sync[0].syms, i)) == fsw[i], "profile FSW %u", i);
		CHECK(air_sym_level(air_sym_get(p->sync[1].syms, (uint16_t)(9 + i))) == fsw[i], "profile long FSW %u", i);
	}
	for (uint8_t i = 0; i < 9; i++)
		CHECK(air_sym_level(air_sym_get(p->sync[1].syms, i)) == pre[3 + i], "profile preamble tail %u", i);
	for (uint8_t i = 0; i < 12; i++) {
		CHECK(air_sym_level(air_sym_get(nxdn_preamble, (uint16_t)(12 + i))) == pre[i], "preamble %u", i);
		CHECK(air_sym_level(air_sym_get(nxdn_preamble, i)) == ((i & 2) ? -3 : 3), "deviation pattern %u", i);
	}
	CHECK(p->sync[1].frame_at == 9 && p->sync[1].nsyms == 19 && (p->sync[1].flags & AIR_SYNC_START_ONLY), "long sync");
	CHECK(nxdn_air_mode.profile.sym_rate == 2400 && nxdn_air_mode_9600.profile.sym_rate == 4800, "rates");
	CHECK(nxdn_air_mode.profile.dev_outer_hz == 1050 && nxdn_air_mode_9600.profile.dev_outer_hz == 2400, "deviation");
}

static void testLich(void)
{
	unsigned caught = 0;

	for (unsigned l = 0; l < 128; l++) {
		uint8_t frame[NXDN_FRAME_BYTES];

		fill(frame, sizeof frame);
		nxdn_frame_begin(frame, (uint8_t)l);
		CHECK(nxdn_lich_get(frame, NULL) == (int)l, "LICH %02x", l);
		// Every LICH symbol is an outer one on air (after descrambling the sign)
		for (uint8_t i = 0; i < 8; i++)
			CHECK(air_sym_get(frame, (uint16_t)(NXDN_LICH_SYM + i)) & 1, "LICH %02x symbol %u inner", l, i);
		// A wrong sign in the parity's range (bits 6-3 and the parity bit) is caught
		for (uint8_t i = 0; i < 8; i++) {
			flip_sign(frame, (uint16_t)(NXDN_LICH_SYM + i));
			if (i < 4 || i == 7) {
				CHECK(nxdn_lich_get(frame, NULL) == -1, "LICH %02x bit %u", l, i);
				caught++;
			}
			flip_sign(frame, (uint16_t)(NXDN_LICH_SYM + i));
		}
	}
	// Examples of TS 1-B figure 4.1-7: RDCH superframe four VCH inbound, and the first frame
	CHECK(NXDN_LICH(NXDN_RF_RDCH, NXDN_USC_SACCH_SF, NXDN_STEAL_NONE, 0) == 0x56, "RDCH SF lich");
	CHECK(NXDN_LICH(NXDN_RF_RDCH, NXDN_USC_SACCH_NS, NXDN_STEAL_BOTH, 0) == 0x40, "RDCH NS lich");
	printf("  LICH: 128 values, %u single sign errors caught\n", caught);
}

static nxdn_fec_work_t work;

static void testSacchFacch(void)
{
	unsigned corrected = 0, crc_caught = 0, runs = 0;

	for (unsigned rep = 0; rep < 2000; rep++) {
		uint8_t frame[NXDN_FRAME_BYTES], before[NXDN_FRAME_BYTES], in[NXDN_SACCH_BYTES], out[NXDN_SACCH_BYTES];
		uint8_t f1[NXDN_FACCH1_BYTES], f2[NXDN_FACCH1_BYTES], g[NXDN_FACCH1_BYTES];
		int8_t soft[NXDN_FRAME_SYMS];
		unsigned nerr = rep % 4;

		fill(frame, sizeof frame);
		fill(in, sizeof in);
		in[3] &= 0xC0;		// 26 bits
		fill(f1, sizeof f1);
		fill(f2, sizeof f2);
		nxdn_frame_begin(frame, 0x40);
		memcpy(before, frame, sizeof frame);
		nxdn_sacch_put(frame, in);
		// It writes symbols 18-47 only
		CHECK(!memcmp(frame, before, 4) && !memcmp(frame + 12, before + 12, sizeof frame - 12), "SACCH place");
		nxdn_facch1_put(frame, 0, f1);
		nxdn_facch1_put(frame, 1, f2);
		CHECK(nxdn_sacch_get(frame, NULL, &work, out) == 0 && !memcmp(out, in, sizeof in), "SACCH clean");
		CHECK(nxdn_facch1_get(frame, NULL, 0, &work, g) == 0 && !memcmp(g, f1, sizeof g), "FACCH1 first clean");
		CHECK(nxdn_facch1_get(frame, NULL, 1, &work, g) == 0 && !memcmp(g, f2, sizeof g), "FACCH1 second clean");
		CHECK(nxdn_lich_get(frame, NULL) == 0x40, "LICH kept");
		// Spaced sign errors: corrected
		for (unsigned k = 0; k < nerr; k++) {
			flip_sign(frame, (uint16_t)(NXDN_SACCH_SYM + 2 + 10 * k));
			flip_sign(frame, (uint16_t)(NXDN_VCH_SYM(0) + 5 + 25 * k));
		}
		CHECK(nxdn_sacch_get(frame, NULL, &work, out) == (int)nerr && !memcmp(out, in, sizeof in), "SACCH %u errors", nerr);
		{ int e = nxdn_facch1_get(frame, NULL, 0, &work, g); CHECK(e == (int)nerr && !memcmp(g, f1, sizeof g), "FACCH1 %u errors: %d, data %s", nerr, e, memcmp(g, f1, sizeof g) ? "wrong" : "right"); }
		corrected += 2 * nerr;
		// Soft values: an error of low confidence costs less than the rest
		for (uint16_t k = 0; k < NXDN_FRAME_SYMS; k++)
			soft[k] = (int8_t)(air_sym_level(air_sym_get(frame, k)) * 32);
		// (inner symbols only: a weak sign there still reads as inner)
		for (unsigned k = 0, at = NXDN_SACCH_SYM; k < 6 && at < NXDN_SACCH_SYM + 30; at++) {
			if (soft[at] == 32 || soft[at] == -32) {
				soft[at] = (int8_t)(soft[at] > 0 ? -8 : 8);
				k++;
			}
		}
		CHECK(nerr || (nxdn_sacch_get(frame, soft, &work, out) >= 0 && !memcmp(out, in, sizeof in)), "SACCH soft");
		// Garbage: the CRC says so (6 bits: one in 64 passes)
		fill(frame + 4, 8);
		crc_caught += nxdn_sacch_get(frame, NULL, &work, out) < 0;
		runs++;
	}
	printf("  SACCH/FACCH1: %u round trips, %u errors corrected, CRC caught %u of %u garbage SACCH\n", runs, corrected,
	       crc_caught, runs);
	CHECK(crc_caught > runs * 9 / 10, "CRC6 catches %u", crc_caught);
}

static void testVch(void)
{
	// 7.2.1: the standard's two voice frames, 49 bits to 72 with the frame's FEC; the DMR side's voice frame
	// FEC gives the same 72 bits (the parameter bits here are only test data for the FEC)
	static const struct { uint8_t b49[7]; uint8_t b72[9]; } v[2] = {
		{ { 0xFE, 0xE2, 0x12, 0x12, 0x12, 0x10, 0x00 }, { 0xCE, 0xA8, 0xFE, 0x83, 0xAC, 0xC4, 0x58, 0x20, 0x0A } },
		{ { 0xF8, 0x01, 0xA9, 0x9F, 0x8C, 0xE0, 0x80 }, { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B } },
	};
	uint8_t frame[NXDN_FRAME_BYTES] = { 0 }, ehr[4][NXDN_EHR_BYTES], got[NXDN_EHR_BYTES];

	for (uint8_t i = 0; i < 2; i++) {
		uint8_t bits[AMBE_PARAM_BITS], out[AMBE_FRAME_BYTES];

		for (uint8_t k = 0; k < AMBE_PARAM_BITS; k++)
			bits[k] = bget(v[i].b49, k);
		ambeFecEncode(bits, out);
		CHECK(!memcmp(out, v[i].b72, 9), "7.2.1 vector %u", i);
	}
	// Four made-up patterns in their places, scrambled on air, read back
	for (uint8_t n = 0; n < 4; n++) {
		fill(ehr[n], NXDN_EHR_BYTES);
		nxdn_vch_put(frame, n, ehr[n]);
	}
	for (uint8_t n = 0; n < 4; n++) {
		unsigned diff = 0;

		nxdn_vch_get(frame, n, got);
		CHECK(!memcmp(got, ehr[n], NXDN_EHR_BYTES), "VCH %u", n);
		for (uint8_t k = 0; k < 72; k++)
			diff += bget(ehr[n], k) != ((air_sym_get(frame, (uint16_t)(NXDN_VCH_SYM(n) + k / 2)) >> (1 - (k & 1))) & 1);
		CHECK(diff > 5, "VCH %u is scrambled on air (%u bits differ)", n, diff);
	}
}

static void testMessages(void)
{
	nxdn_vcall_t v = { NXDN_MSG_VCALL, NXDN_CC_EMERGENCY, NXDN_CALL_GROUP, NXDN_VOICE_4800_EHR, 1234, 77, NXDN_CIPHER_AES, 5 }, w;
	uint8_t m[NXDN_FACCH1_BYTES];

	nxdn_vcall_build(&v, m, sizeof m);
	// Figure 6.4-1: type, CC option, call type and voice option, source, destination, cipher and key
	CHECK(m[0] == 0x01 && m[1] == 0x80 && m[2] == 0x20 && m[3] == 0x04 && m[4] == 0xD2 && m[5] == 0 && m[6] == 77 &&
	      m[7] == 0xC5 && m[8] == 0 && m[9] == 0, "VCALL octets");
	CHECK(nxdn_vcall_parse(m, &w) && !memcmp(&v, &w, sizeof v), "VCALL parse");
	v.msg = NXDN_MSG_TX_REL;
	nxdn_vcall_build(&v, m, 9);
	CHECK(m[0] == 0x08 && m[2] == 0x20 && m[7] == 0, "TX_REL octets");
	m[0] = NXDN_MSG_IDLE;
	CHECK(!nxdn_vcall_parse(m, &w), "IDLE is not a VCALL");
	CHECK(NXDN_SR(0, 5) == 0xC5 && NXDN_SR(3, 5) == 0x05 && NXDN_SR_PART(0x85) == 1, "SR");
}

// ── Whole calls: the transmit side's symbols through the framer into the receive side ──
static uint8_t tx_air[40000];		// one dibit per byte
static uint32_t tx_n;

static struct {
	uint32_t starts, updates, ends, voice;
	uint8_t frames[64][NXDN_EHR_BYTES];
	nxdn_call_t call;
} got;

static void s_start(const nxdn_call_t *c, void *ctx) { (void)ctx; got.starts++; got.call = *c; }
static void s_update(const nxdn_call_t *c, void *ctx) { (void)ctx; got.updates++; got.call = *c; }
static void s_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	(void)ctx;
	if (got.voice < 64)
		memcpy(got.frames[got.voice], ehr, NXDN_EHR_BYTES);
	got.voice++;
}
static void s_end(void *ctx) { (void)ctx; got.ends++; }
static const nxdn_call_sink_t sink = { s_start, s_update, s_voice, s_end };

static uint8_t voice[64][NXDN_EHR_BYTES];

// A call of nvoice voice frames: its symbols to tx_air
static void make_call_with_alias(const nxdn_vcall_t *v, uint8_t ran, bool rate9600, uint16_t nvoice, const char *alias)
{
	static const uint8_t silence[NXDN_EHR_BYTES] = { 0 };
	nxdn_call_tx_t t;
	uint16_t sent_voice = 0;
	bool ended = false;

	nxdn_call_tx_begin(&t, v, ran, NXDN_DIR_INBOUND, rate9600);
#if CONFIG_NXDN_OTA_ALIAS
	nxdn_call_tx_alias(&t, alias);
#else
	(void)alias;
#endif
	while (!nxdn_call_tx_done(&t)) {
		uint16_t first, n = nxdn_call_tx_ready(&t, &first);

		// The operator lets go after the last voice frame (as the engine's tx_stop: before what is ready goes)
		if (sent_voice == nvoice && !ended) {
			ended = true;
			nxdn_call_tx_end(&t, silence);
			continue;
		}
		if (!n) {
			nxdn_call_tx_voice(&t, voice[sent_voice++ % (sizeof voice / sizeof voice[0])]);
			continue;
		}
		// Released a few symbols at a time, as a provider's chunks take them
		if (n > 7)
			n = 7;
		for (uint16_t i = 0; i < n; i++)
			tx_air[tx_n++] = air_sym_get(nxdn_call_tx_syms(&t), (uint16_t)(first + i));
		nxdn_call_tx_sent(&t, n);
	}
}

static void make_call(const nxdn_vcall_t *v, uint8_t ran, bool rate9600, uint16_t nvoice)
{
	make_call_with_alias(v, ran, rate9600, nvoice, "");
}

static void noise_syms(uint16_t n)
{
	while (n--)
		tx_air[tx_n++] = (uint8_t)(rng() & 3);
}

// tx_air[from..] into a receiver in 48-symbol chunks; the call layer's stats back
static nxdn_call_stats_t receive(const air_mode_t *mode, uint8_t ran, uint32_t from, bool soft)
{
	static uint8_t fbuf[NXDN_FRAME_BYTES];
	static int8_t fsoft[NXDN_FRAME_SYMS];
	static nxdn_call_rx_t c;
	air_framer_t fr;

	memset(&got, 0, sizeof got);
	nxdn_call_rx_init(&c, ran, &sink, NULL);
	air_framer_init(&fr, mode, &c.rx, fbuf, fsoft, NXDN_FRAME_SYMS);
	for (uint32_t at = from; at < tx_n; at += 48) {
		uint8_t hard[12] = { 0 };
		int8_t sv[48];
		uint16_t n = (uint16_t)(tx_n - at < 48 ? tx_n - at : 48);
		air_chunk_t ch = { .t_us = at * 417u, .nsyms = n, .sync_at = -1, .hard = hard, .soft = soft ? sv : NULL };

		for (uint16_t i = 0; i < n; i++) {
			air_sym_put(hard, i, tx_air[at + i]);
			sv[i] = (int8_t)(air_sym_level(tx_air[at + i]) * 32);
		}
		air_framer_rx(&fr, &ch);
	}
	return c.stats;
}

static bool voice_in_order(uint16_t first, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++)
		if (memcmp(got.frames[i], voice[first + i], NXDN_EHR_BYTES))
			return false;
	return true;
}

static void testCalls(void)
{
	nxdn_vcall_t v = { NXDN_MSG_VCALL, 0, NXDN_CALL_GROUP, NXDN_VOICE_4800_EHR, 1001, 20, NXDN_CIPHER_NONE, 0 };
	nxdn_call_stats_t st;
	uint32_t call_at;

	for (uint8_t i = 0; i < 64; i++)
		fill(voice[i], NXDN_EHR_BYTES);
	// A clear group call of 30 voice frames, between noise. 30 is 7.5 frames: the last one is filled with silence
	tx_n = 0;
	noise_syms(500);
	call_at = tx_n;
	make_call(&v, 7, false, 30);
	CHECK(tx_n - call_at == NXDN_PREAMBLE_SYMS + 10 * NXDN_FRAME_SYMS, "call length %u", tx_n - call_at);
	noise_syms(1000);
	for (uint8_t soft = 0; soft < 2; soft++) {
		st = receive(&nxdn_air_mode, 7, 0, soft);
		CHECK(got.starts == 1 && got.ends == 1 && !got.call.late && got.call.have_vcall && got.call.vcall.src == 1001 &&
		      got.call.vcall.dst == 20 && got.call.ran == 7, "clear call (soft %u): starts %u ends %u", soft, got.starts, got.ends);
		CHECK(got.voice == 32 && voice_in_order(0, 30), "clear call: %u voice frames", got.voice);
		CHECK(st.sacch_fail == 0 && st.facch_fail == 0 && st.voice_held == 0, "clear call: fails %u %u", st.sacch_fail, st.facch_fail);
	}
	// RAN squelch: another RAN is never heard; RAN 0 hears every one
	st = receive(&nxdn_air_mode, 8, 0, true);
	CHECK(got.starts == 0 && got.voice == 0, "RAN 8 heard RAN 7: %u starts", got.starts);
	st = receive(&nxdn_air_mode, NXDN_RAN_ANY, 0, true);
	CHECK(got.starts == 1 && got.voice == 32, "RAN 0: %u starts", got.starts);
	// Late entry at the first superframe's first frame (frame 1): its VCALL is whole with frame 4, whose voice
	// (frames 12-15) is the first played
	st = receive(&nxdn_air_mode, 7, call_at + NXDN_PREAMBLE_SYMS + NXDN_FRAME_SYMS, true);
	CHECK(got.starts == 1 && got.call.late && got.call.have_vcall && got.call.vcall.src == 1001, "late entry");
	CHECK(got.voice == 20 && voice_in_order(12, 18) && st.voice_held == 12, "late entry: %u voice frames, %u held",
	      got.voice, st.voice_held);
	// ... and at its second frame: the next superframe (frames 5-8) brings it
	st = receive(&nxdn_air_mode, 7, call_at + NXDN_PREAMBLE_SYMS + 2 * NXDN_FRAME_SYMS, true);
	CHECK(got.starts == 1 && got.call.late && got.voice == 4 && voice_in_order(28, 2), "late entry mid-superframe: %u voice",
	      got.voice);
	// A LICH lost in a fade: the frame is placed by counting, its voice still read
	for (uint8_t k = 0; k < 8; k++)
		tx_air[call_at + NXDN_PREAMBLE_SYMS + 3 * NXDN_FRAME_SYMS + NXDN_LICH_SYM + k] ^= 2;
	st = receive(&nxdn_air_mode, 7, 0, true);
	CHECK(st.lich_fail == 1 && got.voice == 32 && voice_in_order(0, 30) && got.ends == 1, "coast: lich fails %u voice %u",
	      st.lich_fail, got.voice);
	// Symbol errors over the call behind the header's FSW: the FEC'd channels survive, voice bits are the voice path's
	for (uint32_t k = call_at + NXDN_PREAMBLE_SYMS + 30; k < call_at + 10 * NXDN_FRAME_SYMS; k += 97)
		tx_air[k] ^= 2;
	st = receive(&nxdn_air_mode, 7, 0, true);
	printf("  call with a symbol error every 97: starts %u, voice %u, LICH fails %u, SACCH fails %u, FACCH1 fails %u, "
	       "%u bits corrected\n", got.starts, got.voice, st.lich_fail, st.sacch_fail, st.facch_fail, st.fec_bits);
	CHECK(got.starts == 1 && got.voice == 32 && got.ends == 1 && st.fec_bits > 0, "errors: %u voice, %u ends, %u frames, held %u",
	      got.voice, got.ends, st.frames, st.voice_held);
	// A wrong sign in the header's FSW: the framer's fit of a 10-symbol pattern fails, so the header is missed and
	// the call is joined late, at the first superframe whose SACCH is whole
	tx_air[call_at + NXDN_PREAMBLE_SYMS + 6] ^= 2;
	st = receive(&nxdn_air_mode, 7, 0, true);
	printf("  header FSW with one error: starts %u, late %u, voice %u, held %u\n", got.starts, got.call.late, got.voice,
	       st.voice_held);
	CHECK(got.starts == 1 && got.ends == 1 && got.voice >= 16, "header FSW error: %u voice", got.voice);

	// Encrypted (AES, with VCALL_IV in place of every second superframe's VCALL in a real call; here the VCALL
	// says so): recognized, voice not handed on
	v.cipher = NXDN_CIPHER_AES;
	v.key_id = 3;
	tx_n = 0;
	noise_syms(300);
	make_call(&v, 7, false, 16);
	noise_syms(1000);
	st = receive(&nxdn_air_mode, 7, 0, true);
	CHECK(got.starts == 1 && got.call.vcall.cipher == NXDN_CIPHER_AES && got.voice == 0 && st.voice_held == 16,
	      "encrypted: voice %u held %u", got.voice, st.voice_held);

	// 9600 bit/s, EHR: every second frame is FACCH1; voice the same
	v.cipher = NXDN_CIPHER_NONE;
	v.voice_opt = NXDN_VOICE_9600_EHR;
	tx_n = 0;
	noise_syms(300);
	call_at = tx_n;
	make_call(&v, 7, true, 16);
	// Header, voice, FACCH1, voice, FACCH1, voice, FACCH1, voice, release
	CHECK(tx_n - call_at == NXDN_PREAMBLE_SYMS + 9 * NXDN_FRAME_SYMS, "9600 call length %u", tx_n - call_at);
	noise_syms(1000);
	st = receive(&nxdn_air_mode_9600, 7, 0, true);
	CHECK(got.starts == 1 && got.ends == 1 && got.voice == 16 && voice_in_order(0, 16) && st.facch_fail == 0,
	      "9600: starts %u voice %u facch fails %u", got.starts, got.voice, st.facch_fail);
}

#if CONFIG_NXDN_OTA_ALIAS
static void testAlias(void)
{
	// Full-width synthetic alias, with an independent fixture for the observed layout and sum.
	static const uint8_t expected[4][NXDN_FACCH1_BYTES] = {
		{ 0x3F, 0x68, 0x82, 0x04, 0x14, 'A', 'B', 'C', 'D', 0 },
		{ 0x3F, 0x68, 0x82, 0x04, 0x24, 'E', 'F', 'G', 'H', 0 },
		{ 0x3F, 0x68, 0x82, 0x04, 0x34, 'I', 'J', 'K', 'L', 0 },
		{ 0x3F, 0x68, 0x82, 0x04, 0x44, 'M', 'N', 0x03, 0xE9, 0 },
	};
	const uint32_t first = NXDN_PREAMBLE_SYMS + NXDN_FRAME_SYMS;
	nxdn_vcall_t v = { NXDN_MSG_VCALL, 0, NXDN_CALL_GROUP, NXDN_VOICE_4800_EHR, 1001, 20, NXDN_CIPHER_NONE, 0 };

	for (int rate9600 = 0; rate9600 <= 1; rate9600++) {
		const air_mode_t *mode = rate9600 ? &nxdn_air_mode_9600 : &nxdn_air_mode;

		v.voice_opt = rate9600 ? NXDN_VOICE_9600_EHR : NXDN_VOICE_4800_EHR;
		tx_n = 0;
		make_call_with_alias(&v, 7, rate9600, 16, "ABCDEFGHIJKLMN");
		uint32_t first_call_end = tx_n;
		for (uint8_t i = 0; i < 2; i++) {
			uint8_t frame[NXDN_FRAME_BYTES] = { 0 };
			nxdn_fec_work_t alias_work;
			uint8_t msg[NXDN_FACCH1_BYTES];

			for (uint16_t j = 0; j < NXDN_FRAME_SYMS; j++)
				air_sym_put(frame, j, tx_air[first + i * NXDN_FRAME_SYMS + j]);
			for (uint8_t h = 0; h < 2; h++)
				CHECK(nxdn_facch1_get(frame, NULL, h, &alias_work, msg) == 0 &&
				      memcmp(msg, expected[2 * i + h], sizeof msg) == 0, "alias frame %u/%u", i, h);
		}
		receive(mode, 7, 0, false);
		CHECK(strcmp(got.call.alias, "ABCDEFGHIJKLMN") == 0 && got.updates == 2, "full alias at rate %d", rate9600);
		CHECK(got.voice == 16 && voice_in_order(0, 16), "alias did not consume prepared voice");
		receive(mode, 7, first, false);
		CHECK(got.call.late && got.call.have_vcall && strcmp(got.call.alias, "ABCDEFGHIJKLMN") == 0, "late alias");

		// Keep the FACCH1 CRC valid while corrupting the independent checksum.
		uint32_t second = first + NXDN_FRAME_SYMS;
		uint8_t frame[NXDN_FRAME_BYTES] = { 0 }, original[NXDN_FRAME_BYTES], bad[NXDN_FACCH1_BYTES];
		for (uint16_t j = 0; j < NXDN_FRAME_SYMS; j++)
			air_sym_put(frame, j, tx_air[second + j]);
		memcpy(original, frame, sizeof original);
		memcpy(bad, expected[3], sizeof bad);
		bad[8]++;
		nxdn_facch1_put(frame, 1, bad);
		for (uint16_t j = 0; j < NXDN_FRAME_SYMS; j++)
			tx_air[second + j] = air_sym_get(frame, j);
		nxdn_call_stats_t st = receive(mode, 7, 0, false);
		CHECK(st.facch_fail == 0 && got.call.alias[0] == '\0', "alias checksum rejection");

		// Substitute a valid duplicate in place of fragment four.
		nxdn_facch1_put(frame, 1, expected[2]);
		for (uint16_t j = 0; j < NXDN_FRAME_SYMS; j++)
			tx_air[second + j] = air_sym_get(frame, j);
		receive(mode, 7, 0, false);
		CHECK(got.call.alias[0] == '\0', "duplicate cannot fill a missing fragment");

		for (uint16_t j = 0; j < NXDN_FRAME_SYMS; j++)
			tx_air[second + j] = air_sym_get(original, j);
		tx_n = first_call_end - NXDN_FRAME_SYMS;	// lost release, then the same caller with the alias off
		make_call(&v, 7, rate9600, 8);
		receive(mode, 7, 0, false);
		CHECK(got.starts == 2 && got.call.alias[0] == '\0', "new call clears old alias");

		tx_n = 0;
		make_call_with_alias(&v, 7, rate9600, 8, "TEST 1");
		receive(mode, 7, 0, false);
		CHECK(strcmp(got.call.alias, "TEST 1") == 0, "short padded alias");

		// Check two full rotations independently of the receiver. Each alias superframe replaces only SACCH.
		tx_n = 0;
		make_call_with_alias(&v, 7, rate9600, 256, "ABCDEFGHIJKLMN");
		for (uint8_t sf = 0; sf < 16; sf++) {
			uint8_t msg[NXDN_SACCH_MSG_BYTES] = { 0 };
			nxdn_fec_work_t alias_work;

			for (uint8_t part = 0; part < 4; part++) {
				uint8_t sacch_frame[NXDN_FRAME_BYTES] = { 0 }, sc[NXDN_SACCH_BYTES];
				uint32_t start = first + (4u * sf + part) * NXDN_FRAME_SYMS;

				for (uint16_t j = 0; j < NXDN_FRAME_SYMS; j++)
					air_sym_put(sacch_frame, j, tx_air[start + j]);
				CHECK(nxdn_sacch_get(sacch_frame, NULL, &alias_work, sc) == 0 && NXDN_SR_PART(sc[0]) == part,
				      "valid SACCH superframe %u part %u", sf, part);
				for (uint8_t j = 0; j < 18; j++) {
					uint16_t bit = (uint16_t)(18 * part + j);
					msg[bit >> 3] |= (uint8_t)(bget(sc, (uint16_t)(8 + j)) << (7 - (bit & 7)));
				}
				if (sf & 1) {
					int lich = nxdn_lich_get(sacch_frame, NULL);
					CHECK(lich >= 0 && NXDN_LICH_OPT(lich) ==
					      (rate9600 && (part & 1) ? NXDN_STEAL_BOTH : NXDN_STEAL_NONE),
					      "SACCH alias adds no voice slot stealing");
				}
			}
			if (sf & 1)
				CHECK(memcmp(msg, expected[(sf / 2) % 4], sizeof msg) == 0, "SACCH alias fragment %u", sf);
			else {
				nxdn_vcall_t decoded;
				CHECK(nxdn_vcall_parse(msg, &decoded) && memcmp(&v, &decoded, sizeof v) == 0,
				      "SACCH VCALL between alias fragments");
			}
		}
		receive(mode, 7, first + 4 * NXDN_FRAME_SYMS, false);
		CHECK(got.call.late && got.call.have_vcall && strcmp(got.call.alias, "ABCDEFGHIJKLMN") == 0,
		      "late entry gets the SACCH alias without opening FACCH1");
		CHECK(got.updates == 2, "repeat SACCH aliases do not trigger duplicate call updates");
	}
}
#endif

int main(void)
{
	testScrambler();
	testSyncs();
	testLich();
	testSacchFacch();
	testVch();
	testMessages();
	testCalls();
#if CONFIG_NXDN_OTA_ALIAS
	testAlias();
#endif
	printf("nxdnTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
