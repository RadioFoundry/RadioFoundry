// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Group key fill's message (keyfill/groupkey.h, docs/spec/group-key-fill.md 4)
#include "config.h"
#if CONFIG_GROUP_KEY_FILL	// the whole file
#include <string.h>
#include "keyfill/groupkey.h"
#include "support/hkdf.h"

#define OFF_SESSION	2
#define OFF_SENDER	10
#define OFF_ALG		14
#define OFF_KEY_ID	15
#define OFF_KEY_LEN	16
#define OFF_KEY		17
#define OFF_TAG		49
#define TAG_LEN		16

static const char INFO_KEY[] = "RFGKF1 key";
static const char INFO_MAC[] = "RFGKF1 mac";

// K_enc and K_mac of a session under the channel key (4)
static void derive(const uint8_t chan_key[GK_KEY_LEN], const uint8_t session[GK_SESSION_LEN], uint8_t k_enc[32],
		   uint8_t k_mac[32])
{
	uint8_t prk[SHA256_LEN];

	hkdf_extract(session, GK_SESSION_LEN, chan_key, GK_KEY_LEN, prk);
	hkdf_expand(prk, INFO_KEY, sizeof INFO_KEY - 1, k_enc, 32);
	hkdf_expand(prk, INFO_MAC, sizeof INFO_MAC - 1, k_mac, 32);
	crypto_wipe(prk, sizeof prk);
}

static void tag_of(const uint8_t k_mac[32], const uint8_t *msg, uint8_t out[TAG_LEN])
{
	uint8_t full[SHA256_LEN];

	hmac_sha256(k_mac, 32, msg, OFF_TAG, full);
	memcpy(out, full, TAG_LEN);
	crypto_wipe(full, sizeof full);
}

void gk_build(uint8_t out[GK_MSG_LEN], const uint8_t chan_key[GK_KEY_LEN], const uint8_t session[GK_SESSION_LEN],
	      uint32_t sender, uint8_t key_id, const uint8_t key[GK_KEY_LEN])
{
	uint8_t k_enc[32], k_mac[32];

	derive(chan_key, session, k_enc, k_mac);
	out[0] = 'G';
	out[1] = 1;
	memcpy(&out[OFF_SESSION], session, GK_SESSION_LEN);
	out[OFF_SENDER] = (uint8_t)(sender >> 24);
	out[OFF_SENDER + 1] = (uint8_t)(sender >> 16);
	out[OFF_SENDER + 2] = (uint8_t)(sender >> 8);
	out[OFF_SENDER + 3] = (uint8_t)sender;
	out[OFF_ALG] = GK_ALG_AES256;
	out[OFF_KEY_ID] = key_id;
	out[OFF_KEY_LEN] = GK_KEY_LEN;
	for (int i = 0; i < GK_KEY_LEN; i++)
		out[OFF_KEY + i] = key[i] ^ k_enc[i];
	tag_of(k_mac, out, &out[OFF_TAG]);
	crypto_wipe(k_enc, sizeof k_enc);
	crypto_wipe(k_mac, sizeof k_mac);
}

gk_result_t gk_open(const uint8_t *msg, int len, uint32_t src, const uint8_t chan_key[GK_KEY_LEN], gk_item_t *out)
{
	memset(out, 0, sizeof *out);
	if (msg == NULL || len != GK_MSG_LEN || msg[0] != 'G' || msg[1] != 1 || msg[OFF_ALG] != GK_ALG_AES256 ||
	    msg[OFF_KEY_LEN] != GK_KEY_LEN)
		return GK_BAD_FORMAT;
	uint32_t sender = ((uint32_t)msg[OFF_SENDER] << 24) | ((uint32_t)msg[OFF_SENDER + 1] << 16) |
			  ((uint32_t)msg[OFF_SENDER + 2] << 8) | msg[OFF_SENDER + 3];
	if (sender != src)
		return GK_BAD_SENDER;

	uint8_t k_enc[32], k_mac[32], tag[TAG_LEN];
	derive(chan_key, &msg[OFF_SESSION], k_enc, k_mac);
	tag_of(k_mac, msg, tag);
	bool ok = crypto_equal(tag, &msg[OFF_TAG], TAG_LEN);
	if (ok) {
		memcpy(out->session, &msg[OFF_SESSION], GK_SESSION_LEN);
		out->sender = sender;
		out->alg = msg[OFF_ALG];
		out->key_id = msg[OFF_KEY_ID];
		for (int i = 0; i < GK_KEY_LEN; i++)
			out->key[i] = msg[OFF_KEY + i] ^ k_enc[i];
	}
	crypto_wipe(k_enc, sizeof k_enc);
	crypto_wipe(k_mac, sizeof k_mac);
	crypto_wipe(tag, sizeof tag);
	return ok ? GK_OK : GK_BAD_TAG;
}

void gk_wipe(gk_item_t *it)
{
	crypto_wipe(it, sizeof *it);
}

#endif	// CONFIG_GROUP_KEY_FILL
