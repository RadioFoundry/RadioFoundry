// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion coding (ysf_fec.h)
#include <string.h>
#include "ysf/ysf_fec.h"

uint16_t ysf_crc(const uint8_t *buf, uint32_t nbits)
{
	uint16_t crc = 0;
	for (uint32_t i = 0; i < nbits; i++) {
		bool fb = ((crc >> 15) & 1u) != ysf_bit(buf, i);
		crc = (uint16_t)(crc << 1);
		if (fb) {
			crc ^= 0x1021u;
		}
	}
	return (uint16_t)~crc;
}

// Appendix A's generator matrix, the parity half: row d11 first, each row p11..p0. Not the symmetric form some
// Golay texts print, so the decoder searches the error patterns instead of using that form's shortcut
static const uint16_t golay_p[12] = {
	0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB,
};

static uint16_t golay_parity(uint16_t data)
{
	uint16_t p = 0;
	for (int i = 0; i < 12; i++) {
		if (data & (0x800u >> i)) {
			p ^= golay_p[i];
		}
	}
	return p;
}

uint32_t ysf_golay_encode(uint16_t data)
{
	data &= 0xFFFu;
	return ((uint32_t)data << 12) | golay_parity(data);
}

int ysf_golay_decode(uint32_t cw, uint16_t *data)
{
	uint16_t d = (uint16_t)((cw >> 12) & 0xFFFu);
	uint16_t s = (uint16_t)(golay_parity(d) ^ (cw & 0xFFFu));	// the syndrome: zero for a code word
	*data = d;
	// The code's distance is 8, so at most one pattern of three errors or fewer explains a syndrome. Try the data
	// errors from none to three; whatever the syndrome then leaves must be three or fewer parity errors in all
	int w = __builtin_popcount(s);
	if (w <= 3) {
		return w;
	}
	for (int i = 0; i < 12; i++) {
		uint16_t si = s ^ golay_p[i];
		if (__builtin_popcount(si) <= 2) {
			*data = (uint16_t)(d ^ (0x800u >> i));
			return 1 + __builtin_popcount(si);
		}
		for (int j = i + 1; j < 12; j++) {
			uint16_t sj = si ^ golay_p[j];
			if (__builtin_popcount(sj) <= 1) {
				*data = (uint16_t)(d ^ (0x800u >> i) ^ (0x800u >> j));
				return 2 + __builtin_popcount(sj);
			}
			for (int k = j + 1; k < 12; k++) {
				if ((sj ^ golay_p[k]) == 0) {
					*data = (uint16_t)(d ^ (0x800u >> i) ^ (0x800u >> j) ^ (0x800u >> k));
					return 3;
				}
			}
		}
	}
	return -1;
}

// The encoder's state: bit 3 the previous input, bit 0 the one four inputs back
static inline uint8_t conv_g1(uint8_t s, uint8_t u)
{
	return (uint8_t)((u ^ (s >> 1) ^ s) & 1u);	// x^4 + x^3 + 1: the input, three back, four back
}

static inline uint8_t conv_g2(uint8_t s, uint8_t u)
{
	return (uint8_t)((u ^ (s >> 3) ^ (s >> 2) ^ s) & 1u);	// x^4 + x^2 + x + 1: the input, one, two and four back
}

void ysf_conv_encode(const uint8_t *in, uint32_t nbits, uint8_t *out)
{
	uint8_t s = 0;
	for (uint32_t k = 0; k < nbits; k++) {
		uint8_t u = ysf_bit(in, k) ? 1u : 0u;
		ysf_bit_put(out, 2u * k, conv_g1(s, u) != 0);
		ysf_bit_put(out, 2u * k + 1u, conv_g2(s, u) != 0);
		s = (uint8_t)((u << 3) | (s >> 1));
	}
}

uint16_t ysf_viterbi(ysf_soft_get_t get, const void *ctx, uint32_t nbits, uint8_t *out)
{
	if (nbits > YSF_VITERBI_MAX) {
		nbits = YSF_VITERBI_MAX;
	}
	// Path metrics are correlations (larger is better); a state not yet reachable starts far down. The survivors
	// keep one bit per state and step: which of the two predecessors won
	int32_t pm[16], nm[16];
	uint16_t surv[YSF_VITERBI_MAX];
	for (int s = 0; s < 16; s++) {
		pm[s] = s == 0 ? 0 : -100000;
	}
	for (uint32_t k = 0; k < nbits; k++) {
		int32_t a = get(ctx, 2u * k), b = get(ctx, 2u * k + 1u);
		uint16_t dec = 0;
		for (uint8_t ns = 0; ns < 16; ns++) {
			uint8_t u = ns >> 3;
			int32_t best = 0;
			for (uint8_t x = 0; x < 2; x++) {
				uint8_t s = (uint8_t)(((ns & 7u) << 1) | x);
				int32_t m = pm[s] + (conv_g1(s, u) ? a : -a) + (conv_g2(s, u) ? b : -b);
				if (x == 0 || m > best) {
					best = m;
					if (x) {
						dec |= (uint16_t)(1u << ns);
					}
				}
			}
			nm[ns] = best;
		}
		memcpy(pm, nm, sizeof(pm));
		surv[k] = dec;
	}
	// The tail brings the encoder back to state 0: trace back from there
	uint8_t s = 0;
	for (uint32_t k = nbits; k-- > 0;) {
		ysf_bit_put(out, k, (s >> 3) != 0);
		s = (uint8_t)(((s & 7u) << 1) | ((surv[k] >> s) & 1u));
	}
	// Re-encode the path to count the coded bits it disagrees with
	uint16_t corrected = 0;
	s = 0;
	for (uint32_t k = 0; k < nbits; k++) {
		uint8_t u = ysf_bit(out, k) ? 1u : 0u;
		int8_t a = get(ctx, 2u * k), b = get(ctx, 2u * k + 1u);
		corrected += (uint16_t)((a != 0 && (a > 0) != (conv_g1(s, u) != 0)) + (b != 0 && (b > 0) != (conv_g2(s, u) != 0)));
		s = (uint8_t)((u << 3) | (s >> 1));
	}
	return corrected;
}

// Figure 4-28: nine stages shifting toward S00, whose bit is the output; the feedback S00 XOR S04 enters S08. Bit 0
// of the state below is S00
static uint8_t pn_next(uint16_t *st)
{
	uint8_t out = (uint8_t)(*st & 1u);
	uint16_t fb = (uint16_t)((*st ^ (*st >> 4)) & 1u);
	*st = (uint16_t)((*st >> 1) | (fb << 8));
	return out;
}

#define PN_INIT		0x1C9u	// S08..S00 = 1 1 1 0 0 1 0 0 1

bool ysf_pn_bit(uint32_t i)
{
	uint16_t st = PN_INIT;
	uint8_t b = 0;
	for (uint32_t k = 0; k <= i % 511u; k++) {
		b = pn_next(&st);
	}
	return b != 0;
}

void ysf_whiten(uint8_t *buf, uint32_t nbits)
{
	uint16_t st = PN_INIT;
	for (uint32_t i = 0; i < nbits; i++) {
		if (pn_next(&st)) {
			buf[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u));
		}
	}
}

// --- DCH units ---

#define UNIT_MAX_BYTES	(YSF_DCH_BYTES_LONG + 3u)	// data, CRC, the tail (and room)

static uint32_t dch_width(uint32_t n)
{
	return n == YSF_DCH_BYTES_SHORT ? 5u : 9u;	// dibits a row: figure 4-31, figure 4-30
}

// Coded dibit d sits at air dibit k = (d % width) x 20 + d / width (the inverse of ysf_dibit_il)
static inline uint32_t il_air_dibit(uint32_t d, uint32_t width)
{
	return (d % width) * 20u + d / width;
}

void ysf_dch_encode(const uint8_t *data, uint32_t n, ysf_whiten_t order, uint8_t *air)
{
	uint8_t u[UNIT_MAX_BYTES];
	uint8_t coded[YSF_DCH_BITS_LONG / 8u];
	uint32_t w = dch_width(n);
	uint32_t ibits = n * 8u + 16u + 4u, cbits = 2u * ibits;

	memset(u, 0, sizeof(u));
	memcpy(u, data, n);
	if (order == YSF_WHITEN_DATA) {
		ysf_whiten(u, n * 8u);
	}
	uint16_t crc = ysf_crc(u, n * 8u);
	u[n] = (uint8_t)(crc >> 8);
	u[n + 1u] = (uint8_t)crc;
	if (order == YSF_WHITEN_ALL) {
		ysf_whiten(u, n * 8u + 16u);
	}
	memset(coded, 0, sizeof(coded));
	ysf_conv_encode(u, ibits, coded);
	for (uint32_t d = 0; d < cbits / 2u; d++) {
		uint32_t k = il_air_dibit(d, w);
		ysf_bit_put(air, 2u * k, ysf_bit(coded, 2u * d));
		ysf_bit_put(air, 2u * k + 1u, ysf_bit(coded, 2u * d + 1u));
	}
}

typedef struct {
	const uint8_t *soft;
	uint32_t width;
} il_soft_t;

static int8_t il_get(const void *ctx, uint32_t j)
{
	const il_soft_t *c = ctx;
	return ysf_soft_get(c->soft, 2u * il_air_dibit(j >> 1, c->width) + (j & 1u));
}

ysf_unit_result_t ysf_dch_decode(const uint8_t *soft, uint32_t n, uint8_t *data)
{
	ysf_unit_result_t r = { 0 };
	uint8_t u[UNIT_MAX_BYTES];
	il_soft_t c = { soft, dch_width(n) };

	memset(u, 0, sizeof(u));
	r.corrected = ysf_viterbi(il_get, &c, n * 8u + 20u, u);
	uint16_t got = (uint16_t)((u[n] << 8) | u[n + 1u]);
	if (ysf_crc(u, n * 8u) == got) {
		// Figure 4-27's order: the CRC over the whitened data
		r.crc_ok = true;
		r.order = YSF_WHITEN_DATA;
		ysf_whiten(u, n * 8u);
	} else {
		ysf_whiten(u, n * 8u + 16u);
		got = (uint16_t)((u[n] << 8) | u[n + 1u]);
		if (ysf_crc(u, n * 8u) == got) {
			r.crc_ok = true;
			r.order = YSF_WHITEN_ALL;
		} else {
			// Neither: hand back the data as figure 4-27 would have it
			ysf_whiten(u, n * 8u + 16u);
			ysf_whiten(u, n * 8u);
		}
	}
	memcpy(data, u, n);
	return r;
}

// --- FICH ---

void ysf_fich_encode(uint32_t fich, uint8_t air[YSF_FICH_BITS / 8u])
{
	uint8_t u[13];		// 32 data, 16 CRC -> four Golay words of 24: 96 bits, then the four tail bits
	uint8_t d[6] = { (uint8_t)(fich >> 24), (uint8_t)(fich >> 16), (uint8_t)(fich >> 8), (uint8_t)fich, 0, 0 };
	uint8_t coded[YSF_FICH_BITS / 8u];

	uint16_t crc = ysf_crc(d, 32u);
	d[4] = (uint8_t)(crc >> 8);
	d[5] = (uint8_t)crc;
	memset(u, 0, sizeof(u));
	for (uint32_t w = 0; w < 4u; w++) {
		// Twelve bits at a time from the top (4.5 (1) (c))
		uint32_t bits = w * 12u;
		uint16_t v = 0;
		for (uint32_t i = 0; i < 12u; i++) {
			v = (uint16_t)((v << 1) | (ysf_bit(d, bits + i) ? 1u : 0u));
		}
		uint32_t cw = ysf_golay_encode(v);
		for (uint32_t i = 0; i < 24u; i++) {
			ysf_bit_put(u, w * 24u + i, ((cw >> (23u - i)) & 1u) != 0);
		}
	}
	memset(coded, 0, sizeof(coded));
	ysf_conv_encode(u, 100u, coded);
	for (uint32_t dd = 0; dd < YSF_FICH_BITS / 2u; dd++) {
		uint32_t k = il_air_dibit(dd, 5u);
		ysf_bit_put(air, 2u * k, ysf_bit(coded, 2u * dd));
		ysf_bit_put(air, 2u * k + 1u, ysf_bit(coded, 2u * dd + 1u));
	}
}

ysf_fich_result_t ysf_fich_decode(const uint8_t *soft, uint32_t *fich)
{
	ysf_fich_result_t r = { 0 };
	uint8_t u[13], d[6];
	il_soft_t c = { soft, 5u };

	memset(u, 0, sizeof(u));
	memset(d, 0, sizeof(d));
	r.corrected = ysf_viterbi(il_get, &c, 100u, u);
	for (uint32_t w = 0; w < 4u; w++) {
		uint32_t cw = 0;
		for (uint32_t i = 0; i < 24u; i++) {
			cw = (cw << 1) | (ysf_bit(u, w * 24u + i) ? 1u : 0u);
		}
		uint16_t v;
		int e = ysf_golay_decode(cw, &v);
		if (e < 0) {
			r.golay = 0xFFu;
		} else if (r.golay != 0xFFu) {
			r.golay = (uint8_t)(r.golay + e);
		}
		for (uint32_t i = 0; i < 12u; i++) {
			ysf_bit_put(d, w * 12u + i, ((v >> (11u - i)) & 1u) != 0);
		}
	}
	*fich = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
	r.crc_ok = ysf_crc(d, 32u) == (uint16_t)((d[4] << 8) | d[5]);
	return r;
}

// --- V/D mode 2 voice ---

void ysf_vd2_voice_encode(const uint8_t bits[YSF_VOICE_BITS], uint8_t air[YSF_VD2_VOICE_BITS / 8u])
{
	uint8_t s[YSF_VD2_VOICE_BITS / 8u];
	memset(s, 0, sizeof(s));
	for (uint32_t i = 0; i < 27u; i++) {
		for (uint32_t r = 0; r < 3u; r++) {
			ysf_bit_put(s, 3u * i + r, bits[i] != 0);
		}
	}
	for (uint32_t i = 27u; i < YSF_VOICE_BITS; i++) {
		ysf_bit_put(s, 81u + (i - 27u), bits[i] != 0);
	}
	// Bit 103 stays the zero the standard adds; the whitening covers all 104
	ysf_whiten(s, YSF_VD2_VOICE_BITS);
	for (uint32_t k = 0; k < YSF_VD2_VOICE_BITS; k++) {
		ysf_bit_put(air, k, ysf_bit(s, ysf_vch_il(k)));
	}
}

// Air bit of S(j): the inverse of ysf_vch_il (j = r x 26 + c sits at k = c x 4 + r)
static inline uint32_t vch_air_bit(uint32_t j)
{
	return (j % 26u) * 4u + j / 26u;
}

uint8_t ysf_vd2_voice_decode(const uint8_t *soft, uint8_t bits[YSF_VOICE_BITS])
{
	uint16_t st = PN_INIT;
	int8_t v[YSF_VD2_VOICE_BITS];
	for (uint32_t j = 0; j < YSF_VD2_VOICE_BITS; j++) {
		int8_t x = ysf_soft_get(soft, vch_air_bit(j));
		v[j] = pn_next(&st) ? (int8_t)-x : x;	// a whitening 1 flipped the bit: flip its confidence
	}
	uint8_t quality = 0;
	for (uint32_t i = 0; i < 27u; i++) {
		int32_t sum = v[3u * i] + v[3u * i + 1u] + v[3u * i + 2u];
		bits[i] = sum > 0 ? 1u : 0u;
		for (uint32_t r = 0; r < 3u; r++) {
			quality += (uint8_t)(v[3u * i + r] != 0 && (v[3u * i + r] > 0) != (sum > 0));
		}
	}
	for (uint32_t i = 27u; i < YSF_VOICE_BITS; i++) {
		int8_t x = v[81u + (i - 27u)];
		bits[i] = x > 0 ? 1u : 0u;
		quality += (uint8_t)(x == 0);
	}
	return quality;
}
