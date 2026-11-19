// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR voice encryption with AES-256 (CONFIG_DMR_AES): the DMRA scheme (algorithm ID 0x25) the stock firmware of
 * several radios sends, docs/spec/dmr-voice.md 5.1.
 *
 * A call carries a PI header (algorithm, key ID, a 32-bit message indicator MI). The MI is expanded by a 32-bit LFSR
 * into a word sequence; superframe n of the call uses words n..n+3 as its 128-bit IV. AES-256 in OFB mode from that
 * IV gives the superframe's keystream: the first 16 bytes are skipped, then each of the 18 vocoder frames takes 7
 * bytes, of which the first 49 bits (MSB first) are XORed onto the frame's 49 AMBE+2 parameter bits (after the FEC
 * decode, in dmrPrivacy.h's bit order).
 *
 * No RTOS calls, no allocation: the call stack's hook runs in the radio task, the keyring is owned by its caller.
 */
#ifndef RFOUNDRY_DMRAES_H
#define RFOUNDRY_DMRAES_H

#include <stdbool.h>
#include <stdint.h>

#define DMR_AES_ALG_AES256		0x25	// PI header algorithm ID
#define DMR_AES_KEY_BYTES		32
#define DMR_AES_KEY_SLOTS		1	// a channel's key
#define DMR_AES_SF_FRAMES		18	// vocoder frames in a superframe
#define DMR_AES_FRAME_KS_BYTES		7	// keystream bytes a frame takes (49 bits used)
#define DMR_AES_SF_SKIP_BYTES		16	// keystream bytes skipped at the start of each superframe
#define DMR_AES_BURST_KS_BYTES		(3 * DMR_AES_FRAME_KS_BYTES)	// 21

// --- AES-256 (FIPS-197), encryption only (OFB needs no decryption) ---
typedef struct {
	uint8_t rk[240];	// the 15 round keys
} aes256_ctx_t;

void aes256Init(aes256_ctx_t *ctx, const uint8_t key[DMR_AES_KEY_BYTES]);
void aes256Encrypt(const aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);

// --- The DMRA scheme ---

// The next word of the MI sequence: 32 steps of the LFSR (shift left, the new LSB is the parity of state & 0x80200003)
uint32_t dmrAesMiNext(uint32_t w);
// The 128-bit IV of superframe sf (0 = the one after the PI header) of a call whose PI header carried mi
void dmrAesIv(uint32_t mi, uint32_t sf, uint8_t iv[16]);
// The keystream of voice burst voice_index (0..5 = A..F) of the superframe with this IV: its 3 frames' 7 bytes each.
// Made again for each burst (at most 9 blocks) rather than kept for the superframe, to spare RAM
void dmrAesBurstKeystream(const aes256_ctx_t *ctx, const uint8_t iv[16], uint8_t voice_index,
			  uint8_t ks[DMR_AES_BURST_KS_BYTES]);
// XOR a burst's keystream onto its 3 frames in their on-air form (27 bytes). RX bursts are FEC corrected first
// (correctErrors), TX bursts are taken as encoded
void dmrAesApplyBurst(const uint8_t ks[DMR_AES_BURST_KS_BYTES], uint8_t ambe27[27], bool correctErrors);

// --- Keys and the call stack's hook ---
typedef struct {
	uint8_t alg;		// DMR_AES_ALG_AES256, 0 = an empty slot
	uint8_t key_id;		// as in the PI header
	uint8_t key[DMR_AES_KEY_BYTES];
} dmrAesKey_t;

// The hook's context: the keys, the one in use expanded, and the IV of the superframe in use. Words first, then
// bytes: no padding (a radio with little RAM holds two of these)
typedef struct {
	uint32_t iv_mi, iv_sf;
	uint32_t mi_count;	// MIs made since the start
	uint32_t mi_last;
	uint8_t iv[16];
	dmrAesKey_t keys[DMR_AES_KEY_SLOTS];
	aes256_ctx_t aes;
	int8_t aes_slot;	// the key aes holds, -1 = none
	bool iv_valid;
	int8_t tx_slot;		// our overs are encrypted with this key, -1 = sent in the clear
} dmrAesState_t;

void dmrAesStateInit(dmrAesState_t *st);
// Set (alg DMR_AES_ALG_AES256) or clear (alg 0) slot 0..DMR_AES_KEY_SLOTS-1. False: a bad slot or algorithm
bool dmrAesSetKey(dmrAesState_t *st, uint8_t slot, uint8_t alg, uint8_t key_id, const uint8_t key[DMR_AES_KEY_BYTES]);
bool dmrAesHasKeys(const dmrAesState_t *st);
// The slot holding a key for this algorithm and key ID, -1 = none
int8_t dmrAesFindKey(const dmrAesState_t *st, uint8_t alg, uint8_t key_id);
// Encrypt our overs with slot's key (-1 = clear). False: a bad slot or an empty one (then TX stays clear)
bool dmrAesSetTx(dmrAesState_t *st, int8_t slot);
// The key our overs use, if any: its algorithm and key ID
bool dmrAesTxKey(const dmrAesState_t *st, uint8_t *alg, uint8_t *key_id);
// The call stack's MI source (dmr_crypt_mi_fn): ctx is a dmrAesState_t. The TX key encrypts the time, a count and
// the previous MI, so MIs don't repeat for a key within a run, and after a restart only if a count and the
// millisecond of a key-up both come back (no hardware RNG on these radios). Never 0
uint32_t dmrAesNewMi(uint32_t now_ms, void *ctx);

// The call stack's crypt hook (dmr_crypt_fn, core/dmr/call/dmr_call.h): ctx is a dmrAesState_t. False: no key for the
// call's algorithm and key ID (the burst is not played)
struct dmr_crypt_pos;
bool dmrAesCallHook(uint8_t ambe27[27], const struct dmr_crypt_pos *pos, bool tx, void *ctx);

#endif
