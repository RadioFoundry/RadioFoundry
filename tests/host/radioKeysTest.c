// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's table of settings-area keys (core/radio/radio_keys.h, docs/spec/codeplug-settings.md 3) on a fake
// settings area: lookup, apply (present holds, absent leaves), and the write-back of a change.
#include <stdio.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "radio/radio_keys.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake settings area: one slot per key, a flag for "the flash is held by a CPS session" ----
static struct { bool present; uint32_t id; } kv[4];
static bool kv_held;

bool codeplug_kv_get_id(uint16_t key, uint32_t *id)
{
	if (key >= 4 || !kv[key].present || kv[key].id > CODEPLUG_DMR_ID_MAX) {
		return false;
	}
	*id = kv[key].id;
	return true;
}

bool codeplug_kv_set_u32(uint16_t key, uint32_t value)
{
	if (kv_held || key >= 4) {
		return false;
	}
	kv[key].present = true;
	kv[key].id = value;
	return true;
}

static int kv_writes;	// batches written

bool codeplug_kv_write(const codeplug_kv_op_t *ops, uint8_t n)
{
	// all or nothing, as the real one: every op checked before any is applied
	for (uint8_t i = 0; i < n; i++) {
		if (kv_held || ops[i].key >= 4 || ops[i].len != 4) {
			return false;
		}
	}
	for (uint8_t i = 0; i < n; i++) {
		const uint8_t *v = ops[i].value;
		kv[ops[i].key].present = true;
		kv[ops[i].key].id = v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
	}
	kv_writes++;
	return true;
}

int main(void)
{
	// the rows are unique, known keys of the assigned range, and an unknown key is not found
	for (size_t i = 0; i < RADIO_KEYS_N; i++) {
		CHECK(radio_keys[i].key >= 1 && radio_keys[i].key <= 0x7FFF);
		for (size_t j = i + 1; j < RADIO_KEYS_N; j++) {
			CHECK(radio_keys[i].key != radio_keys[j].key);
		}
	}
	CHECK(radio_key_find(0x7FFE) == NULL);
	const radio_key_t *ars = radio_key_find(CODEPLUG_KEY_ARS_SERVER);
	const radio_key_t *gw = radio_key_find(CODEPLUG_KEY_IP_GATEWAY);
	CHECK((CONFIG_DMR_ARS) == (ars != NULL) && (CONFIG_DMR_IP_GATEWAY) == (gw != NULL));
	if (ars == NULL || gw == NULL) {
		printf("%d checks, %d failed\n", checks, failures);
		return failures != 0;
	}

	dmrDataSettings_t set, was;
	memset(&set, 0, sizeof(set));
	set.arsServer = 3100111;
	set.ipGatewayId = 5;
	CHECK(radio_key_value(ars, &set) == 3100111 && radio_key_value(gw, &set) == 5);

	// absent keys leave the settings alone
	CHECK(!radio_keys_apply(&set) && set.arsServer == 3100111 && set.ipGatewayId == 5);
	// a present key holds its setting, 0 included; the other stays
	kv[CODEPLUG_KEY_ARS_SERVER].present = true;
	kv[CODEPLUG_KEY_ARS_SERVER].id = 0;
	CHECK(radio_keys_apply(&set) && set.arsServer == 0 && set.ipGatewayId == 5);
	CHECK(!radio_keys_apply(&set));	// nothing left to change
	// an ID above the maximum reads as absent
	kv[CODEPLUG_KEY_IP_GATEWAY].present = true;
	kv[CODEPLUG_KEY_IP_GATEWAY].id = CODEPLUG_DMR_ID_MAX + 1;
	CHECK(!radio_keys_apply(&set) && set.ipGatewayId == 5);

	// a change writes its key; one that doesn't differ writes nothing
	memset(&kv, 0, sizeof(kv));
	was = set;
	CHECK(radio_keys_to_codeplug(&was, &set) && !kv[CODEPLUG_KEY_ARS_SERVER].present && !kv[CODEPLUG_KEY_IP_GATEWAY].present);
	set.arsServer = 3100222;
	CHECK(radio_keys_to_codeplug(&was, &set) && kv[CODEPLUG_KEY_ARS_SERVER].id == 3100222 &&
	      !kv[CODEPLUG_KEY_IP_GATEWAY].present);
	// the flash held: refused while the codeplug has the key, a plain change of the radio's own when it doesn't
	kv_held = true;
	set.arsServer = 3100333;
	was.arsServer = 3100222;
	CHECK(!radio_keys_to_codeplug(&was, &set));
	memset(&kv, 0, sizeof(kv));
	CHECK(radio_keys_to_codeplug(&was, &set));
	kv_held = false;

	// two settings changed at once: one batch, both keys
	was = set;
	set.arsServer = 3100444;
	set.ipGatewayId = 3100555;
	kv_writes = 0;
	CHECK(radio_keys_to_codeplug(&was, &set) && kv_writes == 1 && kv[CODEPLUG_KEY_ARS_SERVER].id == 3100444 &&
	      kv[CODEPLUG_KEY_IP_GATEWAY].id == 3100555);

	printf("%d checks, %d failed\n", checks, failures);
	return failures != 0;
}
