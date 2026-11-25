// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 data units (core/p25): the NID against the standard's parity table, status symbol places, every field's round
// trip clean and through bit errors, the voice code word, and a whole call through a noisy channel, the framer
// (core/air) and the receive hooks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p25/p25.h"
#include "air/air_syms.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0xC0FFEE;
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

// Flip nerr information bits of a unit in [from, to)
static void hit(uint8_t *unit, uint16_t from, uint16_t to, uint16_t nerr)
{
	while (nerr--) {
		uint16_t i = (uint16_t)(from + rng() % (to - from));

		p25_bits_put(unit, i, 1, p25_bits_get(unit, i, 1) ^ 1);
	}
}

static void testNid(void)
{
	// Table 8-4: the 64th bit for each data unit ID, whatever the NAC
	static const uint8_t duid[] = { 0x0, 0x3, 0x5, 0xA, 0xC, 0xF }, parity[] = { 0, 0, 1, 1, 0, 0 };
	uint8_t unit[P25_UNIT_BYTES_MAX];

	for (uint8_t d = 0; d < 6; d++) {
		for (uint16_t k = 0; k < 200; k++) {
			uint16_t nac = (uint16_t)(k < 3 ? (k == 0 ? 0x293 : k == 1 ? 0 : 0xFFF) : rng() & 0xFFF), got_nac;
			uint8_t got_duid;

			p25_unit_begin(unit, P25_TDU_SYMS, nac, duid[d], P25_SS_UNKNOWN);
			CHECK(p25_bits_get(unit, 111, 1) == parity[d], "duid %x nac %03x: parity %u", duid[d], nac, (unsigned)p25_bits_get(unit, 111, 1));
			CHECK(p25_bits_get(unit, 0, 24) == 0x5575F5 && p25_bits_get(unit, 24, 24) == 0xFF77FF, "frame sync");
			CHECK(p25_bits_get(unit, 48, 16) == (uint32_t)((nac << 4) | duid[d]), "NID is systematic");
			CHECK(p25_nid_get(unit, &got_nac, &got_duid) == 0 && got_nac == nac && got_duid == duid[d], "clean NID");
			hit(unit, 48, 111, (uint16_t)(rng() % 12));
			CHECK(p25_nid_get(unit, &got_nac, &got_duid) >= 0 && got_nac == nac && got_duid == duid[d], "NID with errors");
		}
	}
	// The first row of the standard's generator matrix (8.5.2): NAC bit 11 alone gives 6331 1413 6723 5452 octal
	p25_unit_begin(unit, P25_TDU_SYMS, 0x800, 0, 0);
	CHECK(p25_bits_get(unit, 64, 24) == 0xCD930B && p25_bits_get(unit, 88, 24) == 0xDD3B2A, "matrix row 1: %06x %06x",
	      (unsigned)p25_bits_get(unit, 64, 24), (unsigned)p25_bits_get(unit, 88, 24));
}

static void testStatus(void)
{
	static const uint16_t len[] = { P25_HDU_SYMS, P25_LDU_SYMS, P25_TDU_SYMS, P25_TDULC_SYMS };
	static const uint8_t count[] = { 11, 24, 2, 6 };	// 8.2.1-8.2.3
	uint8_t unit[P25_UNIT_BYTES_MAX];

	for (uint8_t k = 0; k < 4; k++) {
		uint16_t n = 0;

		p25_unit_begin(unit, len[k], 0x293, P25_DUID_TDU, P25_SS_IDLE);
		// Everything behind the NID is null but the status symbols
		for (uint16_t a = P25_HEAD_SYMS; a < len[k]; a++)
			if (air_sym_get(unit, a)) {
				CHECK(air_sym_get(unit, a) == P25_SS_IDLE && a % 36 == 35, "symbol %u", a);
				n++;
			}
		CHECK(air_sym_get(unit, 35) == P25_SS_IDLE, "the status symbol inside the NID");
		CHECK(n + 1 == count[k], "unit of %u: %u status symbols", len[k], n + 1);
		for (uint8_t i = 0; i < count[k]; i++)
			CHECK(p25_status_get(unit, i) == P25_SS_IDLE, "status %u", i);
		// The last information bit sits right before the last status symbol: 2 bits after every 70
		CHECK(len[k] % 36 == 0, "length %u", len[k]);
	}
}

static void testFields(void)
{
	uint8_t unit[P25_UNIT_BYTES_MAX], in[16], out[16];

	// 5.6's example: "A" = $41 encodes to $41 1E
	p25_unit_begin(unit, P25_LDU_SYMS, 0x293, P25_DUID_LDU1, P25_SS_UNKNOWN);
	in[0] = 0x41;
	in[1] = 0x00;
	p25_ldu_lsd_put(unit, in);
	CHECK(p25_bits_get(unit, 1504, 16) == 0x411E, "LSD code word %04x", (unsigned)p25_bits_get(unit, 1504, 16));
	for (uint16_t round = 0; round < 300; round++) {
		int r;

		// LDU1: link control through 0-6 wrecked Hamming words (RS(24,12,13) corrects 6 symbols)
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, P25_DUID_LDU1, P25_SS_UNKNOWN);
		fill(in, sizeof(in));
		p25_ldu_lc_put(unit, in);
		p25_ldu_lsd_put(unit, in + 9);
		for (uint8_t k = (uint8_t)(rng() % 7); k; k--) {
			uint16_t at = (uint16_t)(400 + 184 * (rng() % 6) + 10 * (rng() % 4));

			hit(unit, at, (uint16_t)(at + 10), (uint16_t)(1 + rng() % 3));
		}
		hit(unit, 1504, 1520, 2);
		hit(unit, 1520, 1536, 1);
		memset(out, 0, sizeof(out));
		r = p25_ldu_lc_get(unit, out);
		CHECK(r >= 0 && !memcmp(in, out, P25_LC_BYTES), "LC round %u: %d", round, r);
		CHECK(p25_ldu_lsd_get(unit, out) >= 0 && out[0] == in[9] && out[1] == in[10], "LSD round %u", round);

		// LDU2: encryption sync, RS(24,16,9) corrects 4
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, P25_DUID_LDU2, P25_SS_UNKNOWN);
		p25_ldu_es_put(unit, in);
		for (uint8_t k = (uint8_t)(rng() % 5); k; k--) {
			uint16_t at = (uint16_t)(400 + 184 * (rng() % 6) + 10 * (rng() % 4));

			hit(unit, at, (uint16_t)(at + 10), (uint16_t)(1 + rng() % 3));
		}
		r = p25_ldu_es_get(unit, out);
		CHECK(r >= 0 && !memcmp(in, out, P25_ES_BYTES), "ES round %u: %d", round, r);

		// HDU: 36 Golay words, 3 errors each corrected; beyond that RS(36,20,17) takes 8 symbols
		p25_unit_begin(unit, P25_HDU_SYMS, 0x293, P25_DUID_HDU, P25_SS_UNKNOWN);
		p25_hdu_put(unit, in);
		for (uint8_t w = 0; w < 36; w++)
			hit(unit, (uint16_t)(112 + 18 * w), (uint16_t)(112 + 18 * w + 18), (uint16_t)(rng() % 3));
		for (uint8_t k = (uint8_t)(rng() % 7); k; k--) {
			uint16_t at = (uint16_t)(112 + 18 * (rng() % 36));

			hit(unit, at, (uint16_t)(at + 18), 6);
		}
		r = p25_hdu_get(unit, out);
		CHECK(r >= 0 && !memcmp(in, out, P25_HDR_BYTES), "HDU round %u: %d", round, r);
		CHECK(p25_bits_get(unit, 112 + 648, 10) == 0, "HDU nulls");

		// TDULC: 12 extended Golay words
		p25_unit_begin(unit, P25_TDULC_SYMS, 0x293, P25_DUID_TDULC, P25_SS_UNKNOWN);
		p25_tdulc_put(unit, in);
		for (uint8_t w = 0; w < 12; w++)
			hit(unit, (uint16_t)(112 + 24 * w), (uint16_t)(112 + 24 * w + 24), (uint16_t)(rng() % 4));
		r = p25_tdulc_get(unit, out);
		CHECK(r >= 0 && !memcmp(in, out, P25_LC_BYTES), "TDULC round %u: %d", round, r);
	}
	// Past what the codes hold: refused, never a wrong word passed as good (random content)
	{
		uint16_t refused = 0;

		for (uint16_t round = 0; round < 300; round++) {
			fill(unit, sizeof(unit));
			if (p25_ldu_lc_get(unit, out) < 0)
				refused++;
		}
		CHECK(refused >= 295, "random LDUs: %u of 300 refused", refused);
	}
}

static void testLc(void)
{
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .emergency = true, .tgid = 0xBEEF, .src = 0x123456 }, got;
	uint8_t raw[P25_LC_BYTES];

	p25_lc_build(&lc, raw);
	CHECK(raw[0] == 0 && raw[1] == 0 && raw[2] == 0x80 && raw[4] == 0xBE && raw[5] == 0xEF && raw[6] == 0x12 && raw[8] == 0x56, "group LC");
	CHECK(p25_lc_parse(raw, &got) && got.emergency && got.tgid == 0xBEEF && got.src == 0x123456, "group LC back");
	lc.lcf = P25_LCF_UNIT;
	lc.dst = 0xABCDEF;
	p25_lc_build(&lc, raw);
	CHECK(raw[0] == 3 && raw[3] == 0xAB && raw[5] == 0xEF, "unit LC");
	CHECK(p25_lc_parse(raw, &got) && got.dst == 0xABCDEF && got.src == 0x123456, "unit LC back");
	raw[1] = 0x90;
	CHECK(!p25_lc_parse(raw, &got) && got.mfid == 0x90, "a manufacturer's LC");

	// A blocked system in link control (TIA-102.AABF-D 7.3.16, 7.3.17, 7.3.28), in an LDU1 and a TDULC
	{
		static uint8_t unit[P25_UNIT_BYTES_MAX];
		p25_rx_unit_t u = { .air = unit };

		// LCO 36, network status: reserved octet, WACN 20 bits, system 12, channel, service class
		memset(raw, 0, sizeof raw);
		raw[0] = 0x40 | 36;
		raw[2] = 0xBE; raw[3] = 0xE0; raw[4] = 0x02; raw[5] = 0xF3;
		p25_unit_begin(unit, P25_TDULC_SYMS, 0x293, P25_DUID_TDULC, P25_SS_IDLE);
		p25_tdulc_put(unit, raw);
		u.duid = P25_DUID_TDULC;
		CHECK(p25_unit_sys_no_tx(&u), "TDULC, LCO 36, WACN BEE00");
		raw[2] = 0xA4; raw[3] = 0xC2; raw[4] = 0x12;
		p25_tdulc_put(unit, raw);
		CHECK(!p25_unit_sys_no_tx(&u), "TDULC, LCO 36, WACN A4C21 system 2F3");
		raw[4] = 0x10; raw[5] = 0x0A;
		p25_tdulc_put(unit, raw);
		CHECK(p25_unit_sys_no_tx(&u), "TDULC, LCO 36, system 00A");
		// LCO 41, explicit network status: WACN 20 bits right after the opcode, system 12
		memset(raw, 0, sizeof raw);
		raw[0] = 0x40 | 41;
		raw[1] = 0xA4; raw[2] = 0xC2; raw[3] = 0x12; raw[4] = 0xF3;
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, P25_DUID_LDU1, P25_SS_IDLE);
		p25_ldu_lc_put(unit, raw);
		u.duid = P25_DUID_LDU1;
		CHECK(!p25_unit_sys_no_tx(&u), "LDU1, LCO 41, WACN A4C21 system 2F3");
		raw[1] = 0xBE; raw[2] = 0xEF; raw[3] = 0xF2;
		p25_ldu_lc_put(unit, raw);
		CHECK(p25_unit_sys_no_tx(&u), "LDU1, LCO 41, WACN BEEFF");
		raw[1] = 0xA4; raw[2] = 0xC2; raw[3] = 0x10; raw[4] = 0x0A;
		p25_ldu_lc_put(unit, raw);
		CHECK(p25_unit_sys_no_tx(&u), "LDU1, LCO 41, system 00A");
		raw[4] = 0x0B;
		p25_ldu_lc_put(unit, raw);
		CHECK(!p25_unit_sys_no_tx(&u), "LDU1, LCO 41, system 00B");
		// LCO 35, RFSS status: LRA, reserved nibble, system 12 (no WACN)
		memset(raw, 0, sizeof raw);
		raw[0] = 0x40 | 35;
		raw[1] = 0x01; raw[2] = 0xF0; raw[3] = 0x0A;
		p25_ldu_lc_put(unit, raw);
		CHECK(p25_unit_sys_no_tx(&u), "LDU1, LCO 35, system 00A (reserved bits set)");
		raw[3] = 0x09;
		p25_ldu_lc_put(unit, raw);
		CHECK(!p25_unit_sys_no_tx(&u), "LDU1, LCO 35, system 009");
		// Not counted: protected, the manufacturer's format bit clear, another opcode (an adjacent site's status
		// names another site), an LDU2
		raw[3] = 0x0A;
		raw[0] = 0xC0 | 35;
		p25_ldu_lc_put(unit, raw);
		CHECK(!p25_unit_sys_no_tx(&u), "protected LC");
		raw[0] = 35;
		p25_ldu_lc_put(unit, raw);
		CHECK(!p25_unit_sys_no_tx(&u), "SF clear");
		raw[0] = 0x40 | 34;
		raw[2] = 0x00;
		p25_ldu_lc_put(unit, raw);
		CHECK(!p25_unit_sys_no_tx(&u), "adjacent site, system 00A");
		raw[0] = 0x40 | 35;
		p25_ldu_lc_put(unit, raw);
		u.duid = P25_DUID_LDU2;
		CHECK(!p25_unit_sys_no_tx(&u), "an LDU2 has no link control");
	}
}

static void testVoice(void)
{
	uint8_t unit[P25_UNIT_BYTES_MAX], in[P25_VOICE_FRAMES][P25_IMBE_BYTES], out[P25_IMBE_BYTES];

	for (uint16_t round = 0; round < 200; round++) {
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, P25_DUID_LDU1, P25_SS_UNKNOWN);
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			fill(in[n], P25_IMBE_BYTES);
			p25_ldu_voice_put(unit, n, in[n]);
		}
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			CHECK(p25_ldu_voice_get(unit, n, out) == 0 && !memcmp(in[n], out, P25_IMBE_BYTES), "voice %u clean", n);
		}
		// One error anywhere in a voice code word but its 7 unprotected bits is corrected
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			static const uint16_t at[] = { 112, 256, 440, 624, 808, 992, 1176, 1360, 1536 };
			// Symbols 53, 56, 59, 62, 65, 68, 71 carry u_7 (table 5-1): bits 106, 113, 118, 125, 130, 137, 142
			uint16_t b;

			do
				b = (uint16_t)(rng() % 144);
			while (b == 106 || b == 113 || b == 118 || b == 125 || b == 130 || b == 137 || b == 142);
			p25_bits_put(unit, (uint16_t)(at[n] + b), 1, p25_bits_get(unit, (uint16_t)(at[n] + b), 1) ^ 1);
			CHECK(P25_VOICE_ERRS_TOTAL(p25_ldu_voice_get(unit, n, out)) == 1 && !memcmp(in[n], out, P25_IMBE_BYTES), "voice %u, bit %u wrong", n, b);
		}
	}
	// The whole LDU is used exactly once: voice, the 24 words, low speed data
	memset(unit, 0, sizeof(unit));
	p25_unit_begin(unit, P25_LDU_SYMS, 0, 0, 0);
	{
		uint8_t ones[16];
		uint16_t set = 0;

		memset(ones, 0xFF, sizeof(ones));
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			uint8_t before[P25_UNIT_BYTES_MAX], after_n = 0;

			memcpy(before, unit, sizeof(before));
			// A frame of ones: every bit of its code word is written, ones or not; mark its 144 bits by hand
			for (uint16_t i = 0; i < 144; i++) {
				static const uint16_t at[] = { 112, 256, 440, 624, 808, 992, 1176, 1360, 1536 };

				CHECK(p25_bits_get(unit, (uint16_t)(at[n] + i), 1) == 0, "voice %u bit %u already taken", n, i);
				p25_bits_put(unit, (uint16_t)(at[n] + i), 1, 1);
				set++;
			}
			(void)after_n;
		}
		for (uint8_t w = 0; w < 6; w++)
			for (uint8_t i = 0; i < 40; i++) {
				uint16_t at = (uint16_t)(400 + 184 * w + i);

				CHECK(p25_bits_get(unit, at, 1) == 0, "word bit %u taken", at);
				p25_bits_put(unit, at, 1, 1);
				set++;
			}
		for (uint8_t i = 0; i < 32; i++) {
			CHECK(p25_bits_get(unit, (uint16_t)(1504 + i), 1) == 0, "LSD bit taken");
			p25_bits_put(unit, (uint16_t)(1504 + i), 1, 1);
			set++;
		}
		CHECK(set == 1680 - 112, "%u bits behind the NID", set);
		for (uint16_t i = 112; i < 1680; i++)
			if (!p25_bits_get(unit, i, 1)) {
				CHECK(false, "information bit %u belongs to no field", i);
				break;
			}
	}
}

// ── A call through the framer ──
#define AIR_MAX		8000
static uint8_t air[AIR_MAX];		// one dibit per byte
static uint32_t air_n;
static p25_rx_unit_t got[40];
static uint8_t got_air[40][P25_UNIT_BYTES_MAX];
static uint8_t ngot, nlost;

static void on_unit(const p25_rx_unit_t *u, void *ctx)
{
	if (ngot < 40) {
		got[ngot] = *u;
		memcpy(got_air[ngot], u->air, (size_t)(u->nsyms + 3) / 4);
		ngot++;
	}
}

static void on_lost(void *ctx) { nlost++; }

static void air_add(const uint8_t *unit, uint16_t nsyms)
{
	for (uint16_t i = 0; i < nsyms; i++)
		air[air_n++] = air_sym_get(unit, i);
}

static void air_noise(uint16_t n)
{
	while (n--)
		air[air_n++] = (uint8_t)(rng() & 3);
}

// The air through a discriminator: gain_pct of nominal, an offset, noise (peak, soft counts), in chunks of any size
static void run(air_framer_t *fr, int gain_pct, int off, int noise)
{
	uint8_t hard[AIR_CHUNK_SYMS_MAX / 4];
	int8_t soft[AIR_CHUNK_SYMS_MAX];

	for (uint32_t from = 0; from < air_n;) {
		air_chunk_t c;
		uint16_t n = (uint16_t)(1 + rng() % AIR_CHUNK_SYMS_MAX);

		if (n > air_n - from)
			n = (uint16_t)(air_n - from);
		memset(&c, 0, sizeof(c));
		for (uint16_t i = 0; i < n; i++) {
			int v = off + (32 * air_sym_level(air[from + i]) * gain_pct) / 100 + (noise ? (int)(rng() % (2 * noise + 1)) - noise : 0);

			v = v > 127 ? 127 : v < -127 ? -127 : v;
			soft[i] = (int8_t)v;
			air_sym_put(hard, i, v >= 64 ? 1 : v >= 0 ? 0 : v > -64 ? 2 : 3);
		}
		c.t_us = (uint32_t)(((uint64_t)from * 1000000) / 4800);
		c.nsyms = n;
		c.sync_at = -1;
		c.hard = hard;
		c.soft = soft;
		air_framer_rx(fr, &c);
		from += n;
	}
}

static void testCall(void)
{
	static uint8_t fbuf[P25_UNIT_BYTES_MAX];
	uint8_t unit[P25_UNIT_BYTES_MAX], hdr[P25_HDR_BYTES], lcraw[P25_LC_BYTES], es[P25_ES_BYTES], out[16];
	uint8_t voice[4][P25_VOICE_FRAMES][P25_IMBE_BYTES];
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = 1001, .src = 3101276 }, lcgot;
	air_framer_t fr;
	p25_rx_t rx;
	uint32_t at[8];

	fill(hdr, sizeof(hdr));
	fill(es, sizeof(es));
	p25_lc_build(&lc, lcraw);
	air_n = 0;
	air_noise(333);
	// HDU, LDU1, LDU2, LDU1, LDU2, TDULC, then another system's TDU, then noise
	at[0] = air_n;
	p25_unit_begin(unit, P25_HDU_SYMS, 0x293, P25_DUID_HDU, P25_SS_UNKNOWN);
	p25_hdu_put(unit, hdr);
	air_add(unit, P25_HDU_SYMS);
	for (uint8_t k = 0; k < 4; k++) {
		at[1 + k] = air_n;
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, k & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1, P25_SS_UNKNOWN);
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			fill(voice[k][n], P25_IMBE_BYTES);
			p25_ldu_voice_put(unit, n, voice[k][n]);
		}
		if (k & 1)
			p25_ldu_es_put(unit, es);
		else
			p25_ldu_lc_put(unit, lcraw);
		// The third LDU's frame sync and NID are lost in a fade: it is placed by the call's order
		if (k == 2)
			for (uint16_t i = 0; i < P25_HEAD_SYMS; i++)
				air_sym_put(unit, i, (uint8_t)(rng() & 3));
		air_add(unit, P25_LDU_SYMS);
	}
	at[5] = air_n;
	p25_unit_begin(unit, P25_TDULC_SYMS, 0x293, P25_DUID_TDULC, P25_SS_UNKNOWN);
	p25_tdulc_put(unit, lcraw);
	air_add(unit, P25_TDULC_SYMS);
	air_noise(500);
	p25_unit_begin(unit, P25_TDU_SYMS, 0x555, P25_DUID_TDU, P25_SS_UNKNOWN);
	air_add(unit, P25_TDU_SYMS);
	air_noise(400);

	for (uint8_t pass = 0; pass < 3; pass++) {
		// Nominal; 0.93 of nominal (1800 Hz into a receiver scaled for 1944) with an offset and noise; hot
		static const int gain[] = { 100, 93, 125 }, off[] = { 0, 14, -20 }, noise[] = { 0, 9, 6 };
		uint8_t bad_voice = 0;

		ngot = nlost = 0;
		p25_rx_init(&rx, 0x293, on_unit, on_lost, NULL);
		air_framer_init(&fr, &p25_air_mode, &rx, fbuf, NULL, P25_UNIT_SYMS_MAX);
		run(&fr, gain[pass], off[pass], noise[pass]);
		CHECK(ngot == 6, "pass %u: %u units", pass, ngot);
		if (ngot != 6)
			continue;
		CHECK(got[0].duid == P25_DUID_HDU && got[0].nsyms == P25_HDU_SYMS && got[0].nac == 0x293 && got[0].nid_errs == 0, "HDU");
		CHECK(p25_hdu_get(got_air[0], out) == 0 && !memcmp(out, hdr, P25_HDR_BYTES), "HDU content");
		for (uint8_t k = 0; k < 4; k++) {
			const p25_rx_unit_t *u = &got[1 + k];

			CHECK(u->duid == (k & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1) && u->nsyms == P25_LDU_SYMS, "pass %u LDU %u: duid %x", pass, k, u->duid);
			// To a few microseconds: the chunk times and the symbol period are whole microseconds
			CHECK(abs((int)(u->t_us - (uint32_t)(((uint64_t)at[1 + k] * 1000000) / 4800))) <= 3, "LDU %u time", k);
			CHECK((u->nid_errs < 0) == (k == 2) && !(u->flags & AIR_FRAME_NO_SYNC) == (k != 2), "LDU %u placed: errs %d flags %x", k, u->nid_errs, u->flags);
			if (k & 1)
				CHECK(p25_ldu_es_get(got_air[1 + k], out) >= 0 && !memcmp(out, es, P25_ES_BYTES), "ES %u", k);
			else
				CHECK(p25_ldu_lc_get(got_air[1 + k], out) >= 0 && p25_lc_parse(out, &lcgot) && lcgot.tgid == 1001 && lcgot.src == 3101276, "LC %u", k);
			for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++)
				if (p25_ldu_voice_get(got_air[1 + k], n, out) < 0 || memcmp(out, voice[k][n], P25_IMBE_BYTES))
					bad_voice++;
		}
		CHECK(bad_voice == 0, "pass %u: %u voice frames wrong", pass, bad_voice);
		CHECK(got[5].duid == P25_DUID_TDULC && p25_tdulc_get(got_air[5], out) >= 0 && !memcmp(out, lcraw, P25_LC_BYTES), "TDULC");
		// The other system's terminator was refused by its NAC; the end of the call was reported once
		CHECK(nlost == 1 && fr.stats.rejects >= 1, "pass %u: lost %u rejects %u", pass, nlost, fr.stats.rejects);
	}
	// Any NAC: the other system's unit comes through too
	ngot = nlost = 0;
	p25_rx_init(&rx, P25_NAC_ANY, on_unit, on_lost, NULL);
	air_framer_init(&fr, &p25_air_mode, &rx, fbuf, NULL, P25_UNIT_SYMS_MAX);
	run(&fr, 100, 0, 0);
	CHECK(ngot == 7 && got[6].nac == 0x555 && got[6].duid == P25_DUID_TDU && got[6].nsyms == P25_TDU_SYMS, "any NAC: %u units", ngot);
	(void)at;
}

// ── The call layer: a transmitter's units through the framer to a receiver's call ──
static struct {
	uint8_t starts, updates, ends;
	p25_call_t call;
	uint8_t voice[64][P25_IMBE_BYTES];
	uint8_t nvoice, worst;
} cs;

static void cs_start(const p25_call_t *c, void *ctx) { cs.starts++; cs.call = *c; }
static void cs_update(const p25_call_t *c, void *ctx) { cs.updates++; cs.call = *c; }
static void cs_end(void *ctx) { cs.ends++; }
static void cs_voice(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx)
{
	if (cs.nvoice < 64)
		memcpy(cs.voice[cs.nvoice++], imbe, P25_IMBE_BYTES);
	if (errs > cs.worst)
		cs.worst = errs;
}
static const p25_call_sink_t cs_sink = { cs_start, cs_update, cs_voice, cs_end };

// A transmission of nframes voice frames onto air[]; the symbols go out in pulls of any size, a voice frame comes
// only when nothing else can go out (so a symbol sent before its frame was in would show as a wrong frame)
static uint8_t tx_voice[64][P25_IMBE_BYTES];
static const uint8_t silence[P25_IMBE_BYTES] = { 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A };
static uint32_t tx_at[8];	// where each unit starts on air
static uint8_t tx_units;

static void transmit(p25_call_tx_t *t, const p25_lc_t *lc, uint8_t nframes)
{
	uint8_t given = 0;
	uint16_t first, n;

	p25_call_tx_begin(t, 0x293, lc, P25_SS_UNKNOWN);
	tx_units = 0;
	if (!nframes)
		p25_call_tx_end(t, silence);	// PTT let go while the header is still going out
	for (uint32_t guard = 0; guard < 100000 && !p25_call_tx_done(t); guard++) {
		n = p25_call_tx_ready(t, &first);
		if (n) {
			uint16_t take = (uint16_t)(1 + rng() % 97);

			if (take > n)
				take = n;
			if (first == 0 && tx_units < 8)
				tx_at[tx_units++] = air_n;
			for (uint16_t i = 0; i < take; i++)
				air[air_n++] = air_sym_get(t->unit, (uint16_t)(first + i));
			p25_call_tx_sent(t, take);
		} else if (given < nframes) {
			CHECK(p25_call_tx_wants_voice(t), "nothing to send and no voice wanted");
			fill(tx_voice[given], P25_IMBE_BYTES);
			p25_call_tx_voice(t, tx_voice[given++]);
		} else {
			p25_call_tx_end(t, silence);
		}
	}
	CHECK(p25_call_tx_done(t), "the transmission ended");
}

static void testCallLayer(void)
{
	static uint8_t fbuf[P25_UNIT_BYTES_MAX];
	static p25_call_tx_t t;
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = 4321, .src = 7654321 };
	p25_call_rx_t c;
	air_framer_t fr;
	uint8_t bad = 0;

	// 23 voice frames: header, LDU1, LDU2, an LDU1 filled up with 4 silence frames, terminator
	air_n = 0;
	air_noise(100);
	transmit(&t, &lc, 23);
	air_noise(300);
	CHECK(tx_units == 5 && tx_at[1] - tx_at[0] == P25_HDU_SYMS && tx_at[2] - tx_at[1] == P25_LDU_SYMS && tx_at[4] - tx_at[3] == P25_LDU_SYMS, "units on air: %u", tx_units);
	CHECK(air_n == 100 + P25_HDU_SYMS + 3 * P25_LDU_SYMS + P25_TDU_SYMS + 300, "symbols on air: %u", air_n);
	memset(&cs, 0, sizeof(cs));
	p25_call_rx_init(&c, 0x293, &cs_sink, &c);
	air_framer_init(&fr, &p25_air_mode, &c.rx, fbuf, NULL, P25_UNIT_SYMS_MAX);
	run(&fr, 100, 0, 0);
	CHECK(cs.starts == 1 && cs.ends == 1 && cs.updates == 1, "starts %u ends %u updates %u", cs.starts, cs.ends, cs.updates);
	CHECK(!cs.call.late && cs.call.nac == 0x293 && cs.call.have_lc && cs.call.lc.tgid == 4321 && cs.call.lc.src == 7654321 && cs.call.have_alg && cs.call.algid == P25_ALGID_CLEAR, "call fields");
	CHECK(cs.nvoice == 27 && cs.worst == 0, "voice frames %u, worst %u", cs.nvoice, cs.worst);
	for (uint8_t i = 0; i < 27 && i < cs.nvoice; i++)
		if (memcmp(cs.voice[i], i < 23 ? tx_voice[i] : silence, P25_IMBE_BYTES))
			bad++;
	CHECK(bad == 0, "%u voice frames differ", bad);
	CHECK(c.stats.units == 5 && c.stats.voice == 27 && c.stats.calls == 1 && !c.active, "stats: units %u voice %u", c.stats.units, c.stats.voice);

	// Late entry: the receiver comes on the channel inside the first LDU. The call starts with the LDU2, the link
	// control arrives with the next LDU1. The voice ends on an LDU's last frame: the terminator follows it, no LDU of
	// silence
	air_n = 0;
	transmit(&t, &lc, 36);
	air_noise(200);
	CHECK(tx_units == 6 && air_n == P25_HDU_SYMS + 4 * P25_LDU_SYMS + P25_TDU_SYMS + 200, "36 frames: %u units, %u symbols", tx_units, air_n);
	memset(&cs, 0, sizeof(cs));
	p25_call_rx_init(&c, 0x293, &cs_sink, &c);
	air_framer_init(&fr, &p25_air_mode, &c.rx, fbuf, NULL, P25_UNIT_SYMS_MAX);
	memmove(air, air + tx_at[1] + 200, air_n - tx_at[1] - 200);
	air_n -= tx_at[1] + 200;
	run(&fr, 100, 0, 0);
	CHECK(cs.starts == 1 && cs.ends == 1 && cs.call.late && cs.call.have_lc && cs.call.lc.src == 7654321, "late entry: starts %u ends %u late %d lc %d", cs.starts, cs.ends, cs.call.late, cs.call.have_lc);
	CHECK(cs.nvoice == 27 && !memcmp(cs.voice[0], tx_voice[9], P25_IMBE_BYTES) && !memcmp(cs.voice[26], tx_voice[35], P25_IMBE_BYTES), "late entry: %u voice frames", cs.nvoice);

	// PTT let go during the header: no LDU is sent
	air_n = 0;
	transmit(&t, &lc, 0);
	CHECK(tx_units == 2 && air_n == P25_HDU_SYMS + P25_TDU_SYMS, "header and terminator only: %u units, %u symbols", tx_units, air_n);

	// A unit to unit call, and an encrypted call from another radio: its fields are shown, its voice is not played
	lc.lcf = P25_LCF_UNIT;
	lc.dst = 1234;
	air_n = 0;
	transmit(&t, &lc, 18);
	{
		uint8_t unit[P25_UNIT_BYTES_MAX], es[P25_ES_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xAA, 0x12, 0x34 };

		// Its LDU2 again with an encryption sync, then one more LDU1 as it is
		for (uint16_t i = 0; i < P25_LDU_SYMS; i++)
			air_sym_put(unit, i, air[tx_at[2] + i]);
		p25_ldu_es_put(unit, es);
		for (uint16_t i = 0; i < P25_LDU_SYMS; i++)
			air[tx_at[2] + i] = air_sym_get(unit, i);
		memmove(air + tx_at[3] + P25_LDU_SYMS, air + tx_at[3], air_n - tx_at[3]);
		memcpy(air + tx_at[3], air + tx_at[1], P25_LDU_SYMS);
		air_n += P25_LDU_SYMS;
	}
	air_noise(200);
	memset(&cs, 0, sizeof(cs));
	p25_call_rx_init(&c, P25_NAC_ANY, &cs_sink, &c);
	air_framer_init(&fr, &p25_air_mode, &c.rx, fbuf, NULL, P25_UNIT_SYMS_MAX);
	run(&fr, 100, 0, 0);
	CHECK(cs.starts == 1 && cs.ends == 1 && cs.call.lc.lcf == P25_LCF_UNIT && cs.call.lc.dst == 1234 && cs.call.lc.src == 7654321, "unit call fields");
	CHECK(cs.call.algid == 0xAA && cs.call.kid == 0x1234 && cs.updates == 2, "encryption seen: algid %02x kid %04x updates %u", cs.call.algid, cs.call.kid, cs.updates);
	CHECK(cs.nvoice == 9, "only the LDU before the encryption sync was played: %u frames", cs.nvoice);
}

int main(void)
{
	testNid();
	testStatus();
	testFields();
	testLc();
	testVoice();
	testCall();
	testCallLayer();
	printf("p25Test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
