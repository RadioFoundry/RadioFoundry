// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Serialized AES provisioning record in the codeplug settings area. No DMR engine dependency.
#ifndef RFOUNDRY_CODEPLUG_PRIVACY_H
#define RFOUNDRY_CODEPLUG_PRIVACY_H
#include <string.h>
#include "codeplug_kv.h"
#define CODEPLUG_AES_SLOTS 2
#define CODEPLUG_AES_RECORD_SIZE 68
#define CODEPLUG_PRIVACY_CHANNELS 1024
#define CODEPLUG_PRIVACY_MAP_SIZE (1 + 2 * CODEPLUG_PRIVACY_CHANNELS)

typedef struct {
	uint8_t key[CODEPLUG_AES_SLOTS][32];
	uint8_t id[CODEPLUG_AES_SLOTS];
	uint8_t valid;
} codeplug_aes_keys_t;

// 0 absent (local keys remain usable), 1 authoritative record, -1 malformed (no keys usable).
static inline int codeplug_aes_load(codeplug_aes_keys_t *out)
{
	uint8_t record[CODEPLUG_AES_RECORD_SIZE];
	memset(out, 0, sizeof(*out));
	int n = codeplug_kv_get(CODEPLUG_KEY_AES_KEYS, record, sizeof(record));
	if (n < 0) return 0;
	if (n < (int)sizeof(record) || record[0] != 1) return -1;
	out->valid = record[1] & 3u;
	for (unsigned i = 0; i < CODEPLUG_AES_SLOTS; i++) {
		out->id[i] = record[2 + 33 * i];
		memcpy(out->key[i], record + 3 + 33 * i, 32);
	}
	if (out->valid == 3 && out->id[0] == out->id[1]) {
		out->valid = 0;
		return -1;
	}
	return 1;
}
#endif
