// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Clean implementation from FIPS 197, SP 800-38A, ETSI, and docs/spec/dmr-aes256.md.
#include <string.h>
#include "config.h"
#if CONFIG_DMR_AES256
#include "dmr_aes.h"
#include "dmr/ambe/ambeFec.h"
#include "dmr/fec/dmrFec.h"
#include "dmr/call/dmr_lc.h"

static const uint8_t sbox[256] = {
	0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
	0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
	0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
	0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
	0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
	0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
	0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
	0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
	0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
	0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
	0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
	0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
	0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
	0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
	0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
	0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16
};

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0));
}

void dmr_aes_expand(uint8_t round[240], const uint8_t key[32])
{
	memcpy(round, key, 32);
	uint8_t rc = 1;
	for (unsigned n = 32; n < 240; n += 4) {
		uint8_t t[4];
		memcpy(t, round + n - 4, 4);
		if (n % 32 == 0) {
			uint8_t first = t[0];
			t[0] = sbox[t[1]] ^ rc;
			t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[first];
			rc = xtime(rc);
		} else if (n % 32 == 16) {
			for (unsigned j = 0; j < 4; j++) t[j] = sbox[t[j]];
		}
		for (unsigned j = 0; j < 4; j++) round[n + j] = round[n + j - 32] ^ t[j];
	}
}

void dmr_aes_block(const uint8_t round[240], uint8_t b[16])
{
	for (unsigned j = 0; j < 16; j++) b[j] ^= round[j];
	for (unsigned r = 1; r <= 14; r++) {
		uint8_t t[16];
		for (unsigned row = 0; row < 4; row++)
			for (unsigned col = 0; col < 4; col++)
				t[4 * col + row] = sbox[b[4 * ((col + row) % 4) + row]];
		if (r != 14) {
			for (unsigned c = 0; c < 16; c += 4) {
				uint8_t all = t[c] ^ t[c+1] ^ t[c+2] ^ t[c+3], first = t[c];
				for (unsigned j = 0; j < 3; j++) t[c+j] ^= all ^ xtime(t[c+j] ^ t[c+j+1]);
				t[c+3] ^= all ^ xtime(t[c+3] ^ first);
			}
		}
		for (unsigned j = 0; j < 16; j++) b[j] = t[j] ^ round[16 * r + j];
	}
}

static uint32_t read32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void write32(uint8_t *p, uint32_t x)
{
	for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(x >> (24 - 8 * i));
}

uint32_t dmr_aes_iv(uint32_t mi, uint8_t iv[16])
{
	write32(iv, mi);
	for (unsigned i = 4; i < 16; i++) {
		iv[i] = 0;
		for (unsigned j = 0; j < 8; j++) {
			uint32_t bit = ((mi >> 31) ^ (mi >> 21) ^ (mi >> 1) ^ mi) & 1u;
			mi = (mi << 1) | bit;
			iv[i] = (uint8_t)((iv[i] << 1) | bit);
		}
	}
	return read32(iv + 4);
}

void dmr_aes_reset(dmr_aes_t *s)
{
	volatile uint8_t *p = (volatile uint8_t *)s;
	for (unsigned i = 0; i < sizeof(*s); i++) p[i] = 0;
}

static void prepare(dmr_aes_t *s)
{
	uint8_t b[16];
	s->next_mi = dmr_aes_iv(s->mi, b);
	dmr_aes_block(s->round, b); // The first OFB block is reserved, not voice.
	for (unsigned offset = 0; offset < sizeof(s->stream); offset += 16) {
		dmr_aes_block(s->round, b);
		unsigned n = sizeof(s->stream) - offset;
		memcpy(s->stream + offset, b, n < 16 ? n : 16);
	}
	s->ready = s->key_valid;
}

bool dmr_aes_begin(dmr_aes_t *s, const dmr_aes_keys_t *keys, uint8_t id, uint32_t mi)
{
	dmr_aes_reset(s);
	if (!keys) return false;
	for (unsigned i = 0; i < DMR_AES_SLOTS; i++) {
		if ((keys->valid & (1u << i)) && keys->id[i] == id) {
			dmr_aes_expand(s->round, keys->key[i]);
			s->key_valid = true; s->id = id; s->mi = mi;
			prepare(s);
			return true;
		}
	}
	return false;
}

bool dmr_aes_pi(dmr_aes_t *s, const dmr_aes_keys_t *keys, const uint8_t pi[12], uint32_t dst)
{
	uint16_t crc = dmr_crc_ccitt(pi, 10) ^ DMR_CRC_MASK_PI;
	if (pi[0] != DMR_AES_ALGORITHM || pi[1] != 0x10 ||
					pi[10] != (uint8_t)(crc >> 8) || pi[11] != (uint8_t)crc ||
					(((uint32_t)pi[7] << 16 | (uint32_t)pi[8] << 8 | pi[9]) != dst)) {
		dmr_aes_reset(s);
		return false;
	}
	return dmr_aes_begin(s, keys, pi[2], read32(pi + 3));
}

void dmr_aes_make_pi(const dmr_aes_t *s, uint32_t dst, uint8_t pi[12])
{
	pi[0] = DMR_AES_ALGORITHM; pi[1] = 0x10; pi[2] = s->id;
	write32(pi + 3, s->mi);
	pi[7] = (uint8_t)(dst >> 16); pi[8] = (uint8_t)(dst >> 8); pi[9] = (uint8_t)dst;
	uint16_t crc = dmr_crc_ccitt(pi, 10) ^ DMR_CRC_MASK_PI;
	pi[10] = (uint8_t)(crc >> 8); pi[11] = (uint8_t)crc;
}

void dmr_aes_advance(dmr_aes_t *s)
{
	s->mi = s->next_mi;
	s->next_valid = false;
	s->mask = 0;
	prepare(s);
}

static uint8_t crc4(uint32_t mi)
{
	uint8_t r = 0;
	for (unsigned i = 0; i < 32; i++) {
		uint8_t feedback = (uint8_t)((r >> 3) ^ (mi >> (31 - i))) & 1u;
		r = (uint8_t)(((r << 1) & 15u) ^ (feedback ? 3u : 0u));
	}
	return r ^ 15u;
}

static uint8_t nibble(const uint8_t *frame)
{
	static const uint8_t position[4] = { 71, 67, 63, 59 };
	uint8_t n = 0;
	for (unsigned j = 0; j < 4; j++) n = (uint8_t)((n << 1) | ((frame[position[j] / 8] >> (7 - position[j] % 8)) & 1u));
	return n;
}

static void put_nibble(uint8_t *frame, uint8_t n)
{
	static const uint8_t position[4] = { 71, 67, 63, 59 };
	for (unsigned j = 0; j < 4; j++) {
		uint8_t mask = (uint8_t)(0x80u >> (position[j] % 8));
		frame[position[j] / 8] = (uint8_t)((frame[position[j] / 8] & ~mask) | ((n & (8u >> j)) ? mask : 0));
	}
}

void dmr_aes_voice(dmr_aes_t *s, uint8_t voice[27], uint8_t index, bool tx)
{
	uint64_t signaling = (uint64_t)s->next_mi << 4 | crc4(s->next_mi);
	for (unsigned f = 0; f < 3; f++) {
		uint8_t bits[49];
		ambe_fec_result_t result;
		ambeFecDecode(voice + 9 * f, bits, &result);
		if (!tx && result.c0_failed) continue; // Preserve the failure for the vocoder's concealment.
		unsigned offset = 7 * (3 * index + f);
		for (unsigned i = 0; i < 49; i++) bits[i] ^= (s->stream[offset + i / 8] >> (7 - i % 8)) & 1u;
		ambeFecEncode(bits, voice + 9 * f);
		if (tx) {
			uint16_t part = (uint16_t)((signaling >> (24 - 12 * f)) & 0xFFFu);
			uint32_t word = ambeGolay24Encode(part);
			put_nibble(voice + 9 * f, (uint8_t)(word >> (20 - 4 * index)) & 15u);
		}
	}
}

uint32_t dmr_aes_announcement(uint8_t id)
{
	uint16_t word = dmrFecHamming15113Encode((uint16_t)((uint16_t)id << 3 | 5u));
	word = (uint16_t)((word << 1) | (__builtin_popcount(word) & 1));
	uint32_t out = 0;
	for (unsigned i = 0; i < 32; i++) {
		unsigned column = i / 2;
		if (word & (0x8000u >> column)) out |= 0x80000000u >> ((17 * i) % 32);
	}
	return out;
}

static bool announcement(const uint8_t emb[4], uint8_t *id)
{
	uint32_t received = read32(emb);
	int best = 4;
	bool found = false;
	for (unsigned k = 0; k < 256; k++) {
		int distance = __builtin_popcount(received ^ dmr_aes_announcement((uint8_t)k));
		if (distance < best) { best = distance; *id = (uint8_t)k; found = true; }
	}
	return found;
}

bool dmr_aes_receive(dmr_aes_t *s, const dmr_aes_keys_t *keys, const dmr_burst_t *b,
		     uint32_t slot, uint32_t now, uint8_t voice[27])
{
	if (b->kind != DMR_BURST_VOICE || b->len < 27 || b->voice_index >= 6) return false;
	unsigned index = b->voice_index;
	if (s->have_index) {
		unsigned advances = index <= s->index ? 1u : 0u;
		if (slot && s->slot) {
			uint32_t delta = slot - s->slot;
			if (delta > 24 || delta % 2 || (s->index + delta / 2) % 6 != index) {
				s->ready = s->key_valid = s->next_valid = false; s->mask = 0;
			} else advances = (s->index + delta / 2) / 6;
		} else if ((uint32_t)(now - s->time) >= 300) {
			s->ready = s->key_valid = s->next_valid = false; s->mask = 0;
		}
		while (advances--) {
			if (s->key_valid && (s->ready || s->next_valid)) dmr_aes_advance(s);
			else { s->ready = false; s->mask = 0; }
		}
	}
	s->have_index = true; s->index = (uint8_t)index; s->slot = slot; s->time = now;
	for (unsigned f = 0; f < 3; f++) {
		unsigned shift = 20 - 4 * index;
		s->fragments[f] = (s->fragments[f] & ~(15u << shift)) | (uint32_t)nibble(b->payload + 9 * f) << shift;
	}
	s->mask |= (uint8_t)(1u << index);
	bool playable = s->ready;
	if (index == 5 && s->mask == 63) {
		uint64_t message = 0;
		bool good = true;
		for (unsigned f = 0; f < 3; f++) {
			uint16_t data = 0;
			if (ambeGolay24Decode(s->fragments[f], &data) < 0) good = false;
			message = (message << 12) | data;
		}
		uint32_t mi = (uint32_t)(message >> 4);
		if (good && (message & 15u) == crc4(mi)) {
			uint8_t id = s->id;
			bool announced = b->emb_valid && announcement(b->emb, &id);
			if (!s->key_valid && announced) {
				// Prepare only the following superframe; never play this F using its look-ahead MI.
				if (dmr_aes_begin(s, keys, id, mi)) {
					s->ready = false; s->next_valid = true; s->next_mi = mi;
					s->have_index = true; s->index = 5; s->slot = slot; s->time = now;
				}
			} else if (announced && id != s->id) {
				// The current F still uses the old key; mute the next superframe until reacquired.
				s->ready = s->key_valid = s->next_valid = false;
			} else if (s->key_valid) {
				s->next_mi = mi; s->next_valid = true;
			}
		}
	}
	if (playable) {
		memcpy(voice, b->payload, 27);
		dmr_aes_voice(s, voice, (uint8_t)index, false);
	}
	return playable;
}
#endif
