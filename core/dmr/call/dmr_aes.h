// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RFOUNDRY_DMR_AES_H
#define RFOUNDRY_DMR_AES_H
#include <stdbool.h>
#include <stdint.h>
#include "hal_dmr_modem.h"
#define DMR_AES_SLOTS 2
#define DMR_AES_ALGORITHM 0x25

typedef struct {
	uint8_t key[DMR_AES_SLOTS][32];
	uint8_t id[DMR_AES_SLOTS];
	uint8_t valid;
	uint8_t tx_slot; // 1-based; zero disables encrypted TX, keys remain available for RX
} dmr_aes_keys_t;

typedef struct {
	uint8_t round[240];
	uint8_t stream[126];
	uint32_t mi, next_mi;
	uint32_t fragments[3];
	uint32_t slot, time;
	uint8_t id, index, mask;
	bool ready, have_index, next_valid, key_valid;
} dmr_aes_t;

// Standard AES-256 encryption (FIPS 197) and OFB (SP 800-38A); exposed for independent known-answer tests.
void dmr_aes_expand(uint8_t round[240], const uint8_t key[32]);
void dmr_aes_block(const uint8_t round[240], uint8_t block[16]);
uint32_t dmr_aes_iv(uint32_t mi, uint8_t iv[16]);
void dmr_aes_reset(dmr_aes_t *s);
bool dmr_aes_begin(dmr_aes_t *s, const dmr_aes_keys_t *keys, uint8_t id, uint32_t mi);
bool dmr_aes_pi(dmr_aes_t *s, const dmr_aes_keys_t *keys, const uint8_t pi[12], uint32_t dst);
void dmr_aes_make_pi(const dmr_aes_t *s, uint32_t dst, uint8_t pi[12]);
void dmr_aes_advance(dmr_aes_t *s);
void dmr_aes_voice(dmr_aes_t *s, uint8_t voice[27], uint8_t index, bool tx);
uint32_t dmr_aes_announcement(uint8_t id);
// Inspect complete on-air fragments before voice FEC, track missing superframes, and optionally decrypt.
bool dmr_aes_receive(dmr_aes_t *s, const dmr_aes_keys_t *keys, const dmr_burst_t *b,
		     uint32_t slot, uint32_t now, uint8_t voice[27]);
#endif
