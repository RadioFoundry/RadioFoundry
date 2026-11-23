// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR voice encryption with AES-256 (docs/spec/dmr-voice.md 5.1). The AES core is written from FIPS-197; the DMRA
 * keystream layout was worked out on the bench from on-air captures (no other implementation's source was used).
 */

#include <string.h>
#include "dmr/dmrAes.h"
#include "dmr/dmrPrivacy.h"
#include "dmr/ambe/ambeFec.h"
#include "dmr/call/dmr_call.h"

// --- AES-256, FIPS-197 ---

static const uint8_t SBOX[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

#define AES256_NK	8
#define AES256_NR	14

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

void aes256Init(aes256_ctx_t *ctx, const uint8_t key[DMR_AES_KEY_BYTES])
{
	uint8_t *w = ctx->rk;
	uint8_t rcon = 1;

	memcpy(w, key, DMR_AES_KEY_BYTES);
	for (int i = AES256_NK; i < 4 * (AES256_NR + 1); i++) {
		uint8_t t[4];

		memcpy(t, &w[(i - 1) * 4], 4);
		if (i % AES256_NK == 0) {
			uint8_t t0 = t[0];

			t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
			t[1] = SBOX[t[2]];
			t[2] = SBOX[t[3]];
			t[3] = SBOX[t0];
			rcon = xtime(rcon);
		} else if (i % AES256_NK == 4) {
			for (int k = 0; k < 4; k++)
				t[k] = SBOX[t[k]];
		}
		for (int k = 0; k < 4; k++)
			w[i * 4 + k] = (uint8_t)(w[(i - AES256_NK) * 4 + k] ^ t[k]);
	}
}

void aes256Encrypt(const aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];

	for (int i = 0; i < 16; i++)
		s[i] = (uint8_t)(in[i] ^ ctx->rk[i]);
	for (int r = 1; r <= AES256_NR; r++) {
		uint8_t t[16];

		// SubBytes and ShiftRows (the state is column-major: byte 4c + row)
		for (int c = 0; c < 4; c++)
			for (int row = 0; row < 4; row++)
				t[4 * c + row] = SBOX[s[4 * ((c + row) & 3) + row]];
		if (r != AES256_NR) {
			// MixColumns
			for (int c = 0; c < 4; c++) {
				uint8_t *col = &t[4 * c];
				uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
				uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

				col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
				col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
				col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
				col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
			}
		}
		for (int i = 0; i < 16; i++)
			s[i] = (uint8_t)(t[i] ^ ctx->rk[16 * r + i]);
	}
	memcpy(out, s, 16);
}

// --- The DMRA scheme ---

uint32_t dmrAesMiNext(uint32_t w)
{
	for (int i = 0; i < 32; i++) {
		uint32_t t = w & 0x80200003u;

		t ^= t >> 16;
		t ^= t >> 8;
		t ^= t >> 4;
		t ^= t >> 2;
		t ^= t >> 1;
		w = (w << 1) | (t & 1u);
	}
	return w;
}

void dmrAesIv(uint32_t mi, uint32_t sf, uint8_t iv[16])
{
	uint32_t w = mi;

	for (uint32_t n = 0; n < sf; n++)
		w = dmrAesMiNext(w);
	for (int k = 0; k < 4; k++) {
		iv[4 * k] = (uint8_t)(w >> 24);
		iv[4 * k + 1] = (uint8_t)(w >> 16);
		iv[4 * k + 2] = (uint8_t)(w >> 8);
		iv[4 * k + 3] = (uint8_t)w;
		w = dmrAesMiNext(w);
	}
}

void dmrAesBurstKeystream(const aes256_ctx_t *ctx, const uint8_t iv[16], uint8_t voice_index,
			  uint8_t ks[DMR_AES_BURST_KS_BYTES])
{
	uint8_t block[16];
	unsigned from = DMR_AES_SF_SKIP_BYTES + (unsigned)(voice_index % 6) * DMR_AES_BURST_KS_BYTES;
	unsigned to = from + DMR_AES_BURST_KS_BYTES;

	memcpy(block, iv, 16);
	for (unsigned base = 0; base < to; base += 16) {
		aes256Encrypt(ctx, block, block);	// OFB: each block is the encryption of the one before
		for (unsigned i = 0; i < 16; i++)
			if (base + i >= from && base + i < to)
				ks[base + i - from] = block[i];
	}
}

void dmrAesApplyBurst(const uint8_t ks[DMR_AES_BURST_KS_BYTES], uint8_t ambe27[27], bool correctErrors)
{
	for (int f = 0; f < DMR_PRIVACY_AMBE_BURST_FRAMES; f++) {
		uint8_t *frame = &ambe27[f * DMR_PRIVACY_AMBE_FRAME_BYTES];
		const uint8_t *k = &ks[f * DMR_AES_FRAME_KS_BYTES];
		uint8_t bits[DMR_PRIVACY_AMBE_FRAME_BITS];

		if (correctErrors)
			ambeFecDecode(frame, bits, NULL);
		else
			dmrPrivacyAmbeExtract(frame, bits);
		for (int i = 0; i < DMR_PRIVACY_AMBE_FRAME_BITS; i++)
			bits[i] ^= (uint8_t)((k[i >> 3] >> (7 - (i & 7))) & 1u);
		dmrPrivacyAmbeEncode(bits, frame);
	}
}

// Advertise the next superframe's MI (the second word of this IV). The three frame positions
// carry one extended Golay word each, a nibble per burst from A (most significant) to F.
static void txLateEntry(uint8_t *voice, const uint8_t *iv, uint8_t voice_index)
{
	uint32_t mi = ((uint32_t)iv[4] << 24) | ((uint32_t)iv[5] << 16) | ((uint32_t)iv[6] << 8) | iv[7];
	uint8_t crc = 0;

	for (int bit = 31; bit >= 0; bit--) {
		unsigned feedback = ((mi >> bit) ^ (crc >> 3)) & 1u;
		crc = (uint8_t)(((crc << 1) & 15) ^ (feedback ? 3 : 0));
	}
	uint16_t data[3] = { (uint16_t)(mi >> 20), (uint16_t)((mi >> 8) & 0xfff),
			     (uint16_t)(((mi & 0xff) << 4) | (crc ^ 15)) };
	for (int f = 0; f < 3; f++) {
		// Late-entry words measured on air differ from the AMBE Golay encoder in parity bits 9 and 0.
		unsigned parity = data[f] & 0xdc;
		parity ^= parity >> 4;
		parity ^= parity >> 2;
		parity ^= parity >> 1;
		uint32_t word = ambeGolay24Encode(data[f]) ^ ((parity & 1u) * 0x201u);
		unsigned nibble = word >> (4 * (5 - voice_index));
		uint8_t *frame = &voice[f * DMR_PRIVACY_AMBE_FRAME_BYTES];

		// Nibble MSB first in on-air bits 71, 67, 63, 59 (zero based).
		frame[8] = (uint8_t)((frame[8] & 0xee) | ((nibble >> 3) & 1) | ((nibble & 4) << 2));
		frame[7] = (uint8_t)((frame[7] & 0xee) | ((nibble >> 1) & 1) | ((nibble & 1) << 4));
	}
}

// --- Keys and the call stack's hook ---

void dmrAesStateInit(dmrAesState_t *st)
{
	memset(st, 0, sizeof(*st));
	st->aes_slot = -1;
	st->tx_slot = -1;
}

bool dmrAesSetTx(dmrAesState_t *st, int8_t slot)
{
	if (slot < 0) {
		st->tx_slot = -1;
		return true;
	}
	if (slot >= DMR_AES_KEY_SLOTS || st->keys[slot].alg == 0) {
		st->tx_slot = -1;
		return false;
	}
	st->tx_slot = slot;
	return true;
}

bool dmrAesTxKey(const dmrAesState_t *st, uint8_t *alg, uint8_t *key_id)
{
	if (st->tx_slot < 0 || st->keys[st->tx_slot].alg == 0)
		return false;
	*alg = st->keys[st->tx_slot].alg;
	*key_id = st->keys[st->tx_slot].key_id;
	return true;
}

static void use_key(dmrAesState_t *st, int8_t slot)
{
	if (st->aes_slot != slot) {
		aes256Init(&st->aes, st->keys[slot].key);
		st->aes_slot = slot;
	}
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

void dmrAesWipe(void *p, unsigned n)
{
	volatile uint8_t *v = (volatile uint8_t *)p;

	while (n--)
		*v++ = 0;
}

void dmrAesSeqInit(dmrAesSeq_t *q)
{
	memset(q, 0, sizeof(*q));
}

void dmrAesSeqStart(dmrAesSeq_t *q, uint32_t saved)
{
	dmrAesSeqInit(q);
	q->next = saved;	// left stays 0: the positions from here on are reserved by the next save, not this one
	q->end = saved;
}

uint32_t dmrAesSeqAdvance(uint32_t w, uint32_t n)
{
	while (n--)
		w = dmrAesMiNext(w);
	return w;
}

void dmrAesSeqSettle(dmrAesSeq_t *q)
{
	if (q->tx_mi == 0)
		return;
	uint32_t used = q->tx_used < q->left ? q->tx_used : q->left;

	if (used != 0)
		q->next = dmrAesSeqAdvance(q->tx_mi, used);
	q->left -= used;
	q->pending_used += used;
	q->tx_mi = 0;
	q->tx_used = 0;
}

uint32_t dmrAesSeqSeed(const uint8_t key[DMR_AES_KEY_BYTES], const uint32_t uid[3], uint32_t own_id, uint32_t now_ms)
{
	aes256_ctx_t aes;
	uint8_t b[16] = { 0 };

	// CBC-MAC over two fixed-length blocks: the chip's UID with a tag, then the own ID and the uptime
	for (int k = 0; k < 3; k++)
		put32(&b[4 * k], uid != NULL ? uid[k] : 0);
	memcpy(&b[12], "MIS1", 4);
	aes256Init(&aes, key);
	aes256Encrypt(&aes, b, b);
	uint8_t c[16] = { 0 };

	put32(&c[0], own_id);
	put32(&c[4], now_ms);
	for (int k = 0; k < 16; k++)
		b[k] ^= c[k];
	aes256Encrypt(&aes, b, b);
	dmrAesWipe(&aes, sizeof(aes));
	uint32_t w = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

	return w != 0 ? w : 1;	// the LFSR's zero state never leaves zero
}

uint32_t dmrAesSeqReserve(dmrAesSeq_t *q)
{
	if (q->next == 0 || q->tx_mi != 0 || q->pending != 0 || q->left >= DMR_AES_SEQ_LOW)
		return 0;
	q->pending = dmrAesSeqAdvance(q->next, DMR_AES_SEQ_RESERVE);
	q->pending_used = 0;
	return q->pending;
}

void dmrAesSeqReserved(dmrAesSeq_t *q, uint32_t saved)
{
	if (q->pending == 0 || saved != q->pending)
		return;
	// The reservation ends DMR_AES_SEQ_RESERVE past where next was when it was made
	q->left = q->pending_used < DMR_AES_SEQ_RESERVE ? DMR_AES_SEQ_RESERVE - q->pending_used : 0;
	q->end = saved;
	q->pending = 0;
}

void dmrAesSeqDropPending(dmrAesSeq_t *q)
{
	q->pending = 0;
}

uint32_t dmrAesNewMi(uint32_t now_ms, void *ctx)
{
	dmrAesState_t *st = (dmrAesState_t *)ctx;
	dmrAesSeq_t *q = st != NULL ? st->seq : NULL;

	(void)now_ms;
	if (q == NULL || st->tx_slot < 0 || st->keys[st->tx_slot].alg == 0)
		return 0;
	dmrAesSeqSettle(q);
	if (q->next == 0 || q->left == 0)
		return 0;
	q->tx_mi = q->next;
	q->tx_used = 0;
	return q->tx_mi;
}

void dmrAesClearKeys(dmrAesState_t *st)
{
	dmrAesSeq_t *q = st->seq;

	dmrAesWipe(st, sizeof(*st));
	dmrAesStateInit(st);
	st->seq = q;
}

bool dmrAesSetKey(dmrAesState_t *st, uint8_t slot, uint8_t alg, uint8_t key_id, const uint8_t key[DMR_AES_KEY_BYTES])
{
	if (slot >= DMR_AES_KEY_SLOTS || (alg != 0 && alg != DMR_AES_ALG_AES256) || (alg != 0 && key == NULL))
		return false;

	dmrAesKey_t *k = &st->keys[slot];

	dmrAesWipe(k, sizeof(*k));
	if (alg != 0) {
		k->alg = alg;
		k->key_id = key_id;
		memcpy(k->key, key, DMR_AES_KEY_BYTES);
	}
	if (st->aes_slot == (int8_t)slot) {
		st->aes_slot = -1;
		dmrAesWipe(&st->aes, sizeof(st->aes));
	}
	if (st->tx_slot == (int8_t)slot && k->alg == 0)
		st->tx_slot = -1;
	return true;
}

bool dmrAesHasKeys(const dmrAesState_t *st)
{
	for (int i = 0; i < DMR_AES_KEY_SLOTS; i++)
		if (st->keys[i].alg != 0)
			return true;
	return false;
}

int8_t dmrAesFindKey(const dmrAesState_t *st, uint8_t alg, uint8_t key_id)
{
	for (int i = 0; i < DMR_AES_KEY_SLOTS; i++)
		if (st->keys[i].alg != 0 && st->keys[i].alg == alg && st->keys[i].key_id == key_id)
			return (int8_t)i;
	return -1;
}

bool dmrAesCallHook(uint8_t ambe27[27], const struct dmr_crypt_pos *pos, bool tx, void *ctx)
{
	dmrAesState_t *st = (dmrAesState_t *)ctx;

	if (st == NULL || pos == NULL || pos->voice_index >= 6 || pos->superframe >= DMR_AES_SF_MAX)
		return false;

	int8_t slot = dmrAesFindKey(st, pos->alg, pos->key_id);

	if (slot < 0)
		return false;
	if (tx) {
		// Only the over's own MI, and only positions inside the saved reservation (dmrAesSeq_t)
		dmrAesSeq_t *q = st->seq;

		if (q == NULL || q->tx_mi == 0 || pos->mi != q->tx_mi || pos->superframe >= q->left)
			return false;
		if (pos->superframe + 1u > q->tx_used)
			q->tx_used = pos->superframe + 1u;	// used from its first burst: a cut-short one counts
	}
	use_key(st, slot);
	if (!st->iv_valid || st->iv_mi != pos->mi || st->iv_sf != pos->superframe) {
		// The next superframe's IV is this one's shifted by a word: only the LFSR steps since are run
		if (st->iv_valid && st->iv_mi == pos->mi && pos->superframe == st->iv_sf + 1) {
			uint32_t w = ((uint32_t)st->iv[12] << 24) | ((uint32_t)st->iv[13] << 16) | ((uint32_t)st->iv[14] << 8) |
				     st->iv[15];

			w = dmrAesMiNext(w);
			memmove(st->iv, &st->iv[4], 12);
			st->iv[12] = (uint8_t)(w >> 24);
			st->iv[13] = (uint8_t)(w >> 16);
			st->iv[14] = (uint8_t)(w >> 8);
			st->iv[15] = (uint8_t)w;
		} else {
			dmrAesIv(pos->mi, pos->superframe, st->iv);
		}
		st->iv_mi = pos->mi;
		st->iv_sf = pos->superframe;
		st->iv_valid = true;
	}

	uint8_t ks[DMR_AES_BURST_KS_BYTES];

	dmrAesBurstKeystream(&st->aes, st->iv, pos->voice_index, ks);
	dmrAesApplyBurst(ks, ambe27, !tx);
	if (tx)
		txLateEntry(ambe27, st->iv, pos->voice_index);
	return true;
}
