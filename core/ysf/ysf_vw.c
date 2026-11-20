// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion Voice FR (ysf_vw.h)
#include <string.h>
#include "config.h"
#include "ysf/ysf_vw.h"

#if CONFIG_YSF_VW

// --- the full-rate frame (TIA-102.BAAA 5.3, as P25 Phase 1 sends it) ---

// Golay (23,12) is the extended (24,12) code without its last parity bit, and TIA-102.BAAA table 5-3 prints the same
// generator matrix as Yaesu's appendix A, so the FICH's extended code (ysf_fec.h) serves: the 23 bits with the dropped
// bit put back both ways, the one decoded with fewer corrections in the 23 bits kept. Within three errors the right
// guess decodes and the wrong one either decodes to the same word or fails; past three this perfect code still finds
// its nearest word, as a (23,12) decoder would
static int golay23_decode(uint32_t cw, uint16_t *data)
{
	int best = 4;
	for (uint32_t p = 0; p < 2u; p++) {
		uint16_t d;
		if (ysf_golay_decode((cw << 1) | p, &d) < 0) {
			continue;
		}
		int e = __builtin_popcount((ysf_golay_encode(d) >> 1) ^ cw);
		if (e < best) {
			best = e;
			*data = d;
		}
	}
	return best;
}

static uint32_t golay23_encode(uint16_t data)
{
	return ysf_golay_encode(data) >> 1;
}

// Hamming (15,11): the parity of each data bit, the top one first (TIA-102.BAAA table 5-4)
static const uint8_t ham_p[11] = { 0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x7, 0x6, 0x5, 0x3 };

static uint16_t ham_parity(uint16_t data)
{
	uint16_t p = 0;
	for (int i = 0; i < 11; i++) {
		if (data & (0x400u >> i)) {
			p ^= ham_p[i];
		}
	}
	return p;
}

static uint16_t ham_encode(uint16_t data)
{
	return (uint16_t)(((data & 0x7FFu) << 4) | ham_parity(data & 0x7FFu));
}

// The 11 data bits; *fixed 1 when a bit was corrected. Every syndrome is a single error's (the code is perfect)
static uint16_t ham_decode(uint16_t cw, uint8_t *fixed)
{
	uint16_t d = (uint16_t)((cw >> 4) & 0x7FFu);
	uint16_t s = (uint16_t)(ham_parity(d) ^ (cw & 0xFu));
	*fixed = s != 0;
	for (int i = 0; s != 0 && i < 11; i++) {
		if (ham_p[i] == s) {
			d ^= (uint16_t)(0x400u >> i);	// a data bit; a syndrome of one bit was a parity bit, the data stands
			break;
		}
	}
	return d;
}

// The scrambling of c1..c6 (5.3): p = 173 p + 13849 mod 65536 from p = 16 u0, bit 15 of each value in turn, 23 bits
// for each Golay word and 15 for each Hamming word
static void masks(uint16_t u0, uint32_t m[7])
{
	uint16_t p = (uint16_t)(16u * u0);
	m[0] = 0;
	for (int c = 1; c < 7; c++) {
		int n = c < 4 ? 23 : 15;
		m[c] = 0;
		for (int i = 0; i < n; i++) {
			p = (uint16_t)(173u * p + 13849u);
			m[c] = (m[c] << 1) | (uint32_t)(p >> 15);
		}
	}
}

// The voice interleave (table 5-1): air bit i is bit (entry & 31) of code word (entry >> 5), bit 0 a word's last
#define V(c, i)		(uint8_t)(((c) << 5) | (i))
static const uint8_t il[YSF_VW_FRAME_BITS] = {
	V(0, 22), V(1, 21), V(2, 20), V(3, 19), V(4, 10), V(5, 1), V(1, 20), V(0, 21), V(3, 18), V(2, 19), V(5, 0), V(4, 9),
	V(0, 20), V(1, 19), V(2, 18), V(3, 17), V(4, 8), V(6, 14), V(1, 18), V(0, 19), V(3, 16), V(2, 17), V(6, 13), V(4, 7),
	V(0, 18), V(1, 17), V(2, 16), V(3, 15), V(4, 6), V(6, 12), V(1, 16), V(0, 17), V(3, 14), V(2, 15), V(6, 11), V(4, 5),
	V(0, 16), V(1, 15), V(2, 14), V(3, 13), V(4, 4), V(6, 10), V(1, 14), V(0, 15), V(3, 12), V(2, 13), V(6, 9), V(4, 3),
	V(0, 14), V(1, 13), V(2, 12), V(3, 11), V(4, 2), V(6, 8), V(1, 12), V(0, 13), V(3, 10), V(2, 11), V(6, 7), V(4, 1),
	V(0, 12), V(1, 11), V(2, 10), V(3, 9), V(4, 0), V(6, 6), V(1, 10), V(0, 11), V(3, 8), V(2, 9), V(6, 5), V(5, 14),
	V(0, 10), V(1, 9), V(2, 8), V(3, 7), V(5, 13), V(6, 4), V(1, 8), V(0, 9), V(3, 6), V(2, 7), V(6, 3), V(5, 12),
	V(0, 8), V(1, 7), V(2, 6), V(3, 5), V(5, 11), V(6, 2), V(1, 6), V(0, 7), V(3, 4), V(2, 5), V(6, 1), V(5, 10),
	V(0, 6), V(1, 5), V(2, 4), V(3, 3), V(5, 9), V(6, 0), V(1, 4), V(0, 5), V(3, 2), V(2, 3), V(7, 6), V(5, 8),
	V(0, 4), V(1, 3), V(2, 2), V(3, 1), V(5, 7), V(7, 5), V(1, 2), V(0, 3), V(3, 0), V(2, 1), V(7, 4), V(5, 6),
	V(0, 2), V(1, 1), V(2, 0), V(4, 14), V(5, 5), V(7, 3), V(1, 0), V(0, 1), V(4, 13), V(3, 22), V(7, 2), V(5, 4),
	V(0, 0), V(2, 22), V(3, 21), V(4, 12), V(5, 3), V(7, 1), V(2, 21), V(1, 22), V(4, 11), V(3, 20), V(7, 0), V(5, 2),
};
#undef V

static uint8_t word_len(int k)
{
	return (uint8_t)(k < 4 ? 12 : k < 7 ? 11 : 7);
}

void ysf_vw_frame_encode(const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t *buf, uint32_t at)
{
	uint32_t c[8], m[7];
	uint16_t u[8];
	uint32_t o = 0;

	for (int k = 0; k < 8; k++) {
		u[k] = 0;
		for (int b = 0; b < word_len(k); b++) {
			u[k] = (uint16_t)((u[k] << 1) | (ysf_bit(imbe, o++) ? 1u : 0u));
		}
	}
	masks(u[0], m);
	c[0] = golay23_encode(u[0]);
	for (int k = 1; k < 4; k++) {
		c[k] = golay23_encode(u[k]) ^ m[k];
	}
	for (int k = 4; k < 7; k++) {
		c[k] = ham_encode(u[k]) ^ m[k];
	}
	c[7] = u[7];
	for (uint32_t i = 0; i < YSF_VW_FRAME_BITS; i++) {
		ysf_bit_put(buf, at + i, ((c[il[i] >> 5] >> (il[i] & 31u)) & 1u) != 0);
	}
}

uint8_t ysf_vw_frame_decode(const uint8_t *buf, uint32_t at, uint8_t imbe[YSF_VW_IMBE_BYTES])
{
	uint32_t c[8] = { 0 }, m[7];
	uint16_t u[8];
	uint32_t o = 0;

	for (uint32_t i = 0; i < YSF_VW_FRAME_BITS; i++) {
		if (ysf_bit(buf, at + i)) {
			c[il[i] >> 5] |= 1u << (il[i] & 31u);
		}
	}
	// u0 first: the masks of the other words come from it
	int e0 = golay23_decode(c[0], &u[0]);
	int total = e0;
	uint8_t e4 = 0;
	masks(u[0], m);
	for (int k = 1; k < 4; k++) {
		total += golay23_decode(c[k] ^ m[k], &u[k]);
	}
	for (int k = 4; k < 7; k++) {
		uint8_t f;
		u[k] = ham_decode((uint16_t)(c[k] ^ m[k]), &f);
		total += f;
		if (k == 4) {
			e4 = f;
		}
	}
	u[7] = (uint16_t)c[7];
	memset(imbe, 0, YSF_VW_IMBE_BYTES);
	for (int k = 0; k < 8; k++) {
		for (int b = word_len(k) - 1; b >= 0; b--) {
			ysf_bit_put(imbe, o++, ((u[k] >> b) & 1u) != 0);
		}
	}
	return YSF_VW_ERRS((uint8_t)total, (uint8_t)e0, e4);
}

// --- receive ---

// The soft buffer (ysf_rx_t.soft) holds, in the first frame, the CSD3 unit's soft bits in air order (payload bits
// 0..359); the subframe being received goes in as hard bits after the unit's 180 bytes, as V/D mode 1's 72-bit frame
// does
#define VOICE_AT	(YSF_DCH_BITS_LONG / 2u)

void ysf_vw_rx_bit(ysf_rx_t *r, uint32_t b, int8_t s)
{
	uint32_t sub = b / YSF_SUB_BITS, off = b % YSF_SUB_BITS;

	if (ysf_vw_has_csd3(&r->call.fich) && sub < YSF_VW_CSD3_SUBS) {
		if (b < YSF_DCH_BITS_LONG) {
			ysf_soft_put(r->soft, b, s);
		}
		return;		// the pad's 72 bits: nothing in them
	}
	uint8_t *v = &r->soft[VOICE_AT];
	ysf_bit_put(v, off, s > 0);
	if (off == YSF_SUB_BITS - 1u) {
		uint8_t imbe[YSF_VW_IMBE_BYTES];
		uint8_t errs = ysf_vw_frame_decode(v, 0, imbe);
		r->voice_frs++;
		r->voice_fr_errors += YSF_VW_ERRS_TOTAL(errs);
		if (r->voice_fr != NULL) {
			r->voice_fr(r->cb.ctx, imbe, errs);
		}
	}
}

// CSD3: the four 5-character radio IDs (table 4-2), as V/D mode 1's FN 2 carries them. rx_payload_done tells cb.call
// when the callsign data grew
void ysf_vw_rx_done(ysf_rx_t *r)
{
	if (!ysf_vw_has_csd3(&r->call.fich)) {
		return;
	}
	uint8_t d[YSF_DCH_BYTES_LONG];
	ysf_unit_result_t u = ysf_dch_decode(r->soft, YSF_DCH_BYTES_LONG, d);
	if (u.crc_ok) {
		r->csd_good++;
		if (u.order == YSF_WHITEN_ALL) {
			r->order_all++;
		}
		memcpy(r->call.csd.rem, d, sizeof(r->call.csd.rem));
		r->call.have |= YSF_HAVE_REM12 | YSF_HAVE_REM34;	// the caller tells cb.call
	} else {
		r->csd_bad++;
	}
}

// --- transmit ---

#define PAYLOAD_AT	240u	// bits: after the sync and the FICH

// The sync and FICH are the caller's (tx_comm builds them for every frame type); here the payload's data part. The
// voice goes in subframe by subframe as each is reached (ysf_vw_tx_voice), so the encoder's latest frame goes out
void ysf_vw_tx_comm(ysf_tx_t *t)
{
	if (t->fn != 0u) {
		return;		// all voice
	}
	uint8_t air[YSF_DCH_BITS_LONG / 8u];
	uint8_t d[YSF_DCH_BYTES_LONG];
	memcpy(d, t->cfg.csd.rem, sizeof(d));
	ysf_dch_encode(d, YSF_DCH_BYTES_LONG, t->cfg.order, air);
	for (uint32_t i = 0; i < YSF_DCH_BITS_LONG; i++) {
		ysf_bit_put(t->frame, PAYLOAD_AT + i, ysf_bit(air, i));
	}
	// The pad stays as the frame was cleared: zeros, what the radio this was read from sends
}

void ysf_vw_tx_voice(ysf_tx_t *t, uint32_t sub)
{
	uint8_t imbe[YSF_VW_IMBE_BYTES];
	if (t->cfg.pull_fr == NULL || !t->cfg.pull_fr(t->cfg.ctx, imbe)) {
		memcpy(imbe, t->cfg.silence_fr, sizeof(imbe));
		t->silences++;
	}
	t->voice_frames++;
	ysf_vw_frame_encode(imbe, t->frame, PAYLOAD_AT + ysf_sub_bit(sub, 0));
}

#endif
