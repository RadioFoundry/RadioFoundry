// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The settings of the radio core that the codeplug's settings area holds, one row per key (docs/spec/codeplug-settings.md
// 3). The same table on every radio: a setting is added here, and the radio core, the menu and the debug link follow
// it. Header only so each test links without a new source file
#ifndef RFOUNDRY_RADIO_KEYS_H
#define RFOUNDRY_RADIO_KEYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "dmr/dmrDataService.h"
#include "codeplug/codeplug_kv.h"

typedef struct {
	uint16_t key;
	uint8_t type;		// CODEPLUG_T_*: how the value is encoded
	bool built;		// the build has the setting (its CONFIG switch); else the row is skipped
	uint16_t off;		// where the setting sits in dmrDataSettings_t
} radio_key_t;

static const radio_key_t radio_keys[] = {
	{ CODEPLUG_KEY_ARS_SERVER, CODEPLUG_T_ID, CONFIG_DMR_ARS, offsetof(dmrDataSettings_t, arsServer) },
	{ CODEPLUG_KEY_IP_GATEWAY, CODEPLUG_T_ID, CONFIG_DMR_IP_GATEWAY, offsetof(dmrDataSettings_t, ipGatewayId) },
};
#define RADIO_KEYS_N	(sizeof(radio_keys) / sizeof(radio_keys[0]))

static inline const radio_key_t *radio_key_find(uint16_t key)
{
	for (size_t i = 0; i < RADIO_KEYS_N; i++) {
		if (radio_keys[i].key == key && radio_keys[i].built) {
			return &radio_keys[i];
		}
	}
	return NULL;
}

static inline uint32_t *radio_key_slot(const radio_key_t *k, dmrDataSettings_t *set)
{
	return (uint32_t *)((uint8_t *)set + k->off);
}

static inline uint32_t radio_key_value(const radio_key_t *k, const dmrDataSettings_t *set)
{
	return *(const uint32_t *)((const uint8_t *)set + k->off);
}

// Put every key the codeplug has into set (an absent key leaves the setting as it is): true if anything changed
static inline bool radio_keys_apply(dmrDataSettings_t *set)
{
	bool changed = false;
	for (size_t i = 0; i < RADIO_KEYS_N; i++) {
		uint32_t id;
		uint32_t *slot = radio_key_slot(&radio_keys[i], set);
		if (radio_keys[i].built && codeplug_kv_get_id(radio_keys[i].key, &id) && *slot != id) {
			*slot = id;
			changed = true;
		}
	}
	return changed;
}

// Write a change of one setting to its key, else radio_keys_apply would put the codeplug's value back. False when it
// couldn't (a CPS session holds the flash) while the codeplug has the key; with no key the change is the radio's own
static inline bool radio_key_to_codeplug(const radio_key_t *k, uint32_t was, uint32_t id)
{
	uint32_t held;
	return id == was || codeplug_kv_set_u32(k->key, id) || !codeplug_kv_get_id(k->key, &held);
}

// The same for every setting of the table that differs between was and set
static inline bool radio_keys_to_codeplug(const dmrDataSettings_t *was, const dmrDataSettings_t *set)
{
	for (size_t i = 0; i < RADIO_KEYS_N; i++) {
		if (radio_keys[i].built &&
		    !radio_key_to_codeplug(&radio_keys[i], radio_key_value(&radio_keys[i], was),
					   radio_key_value(&radio_keys[i], set))) {
			return false;
		}
	}
	return true;
}

#endif
