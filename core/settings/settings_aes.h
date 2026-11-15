// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key provisioning validation, shared by local configuration frontends. Caller owns the settings lock.
#ifndef RFOUNDRY_SETTINGS_AES_H
#define RFOUNDRY_SETTINGS_AES_H
#include <string.h>
#include "settings.h"

static inline bool settings_aes_provision(settings_t *s, uint8_t slot, uint8_t id, uint8_t flags,
					 const uint8_t key[32], uint8_t *active)
{
	if (slot > SETTINGS_AES_KEYS || (flags & ~3u) || (slot == 0 && (flags & 1u)))
		return false;
	unsigned index = slot ? slot - 1u : 0;
	if (slot && (flags & 1u)) {
		for (unsigned i = 0; i < SETTINGS_AES_KEYS; i++)
			if (i != index && (s->aes_valid & (1u << i)) && s->aes_id[i] == id)
				return false;
	}
	if (slot) {
		s->aes_id[index] = id;
		s->aes_valid = (uint8_t)((s->aes_valid & ~(1u << index)) | ((flags & 1u) << index));
		if (flags & 1u)
			memcpy(s->aes_key[index], key, 32);
		else
			memset(s->aes_key[index], 0, 32);
	}
	*active = flags & 2u ? slot : s->aes_tx_slot;
	return true;
}
#endif
