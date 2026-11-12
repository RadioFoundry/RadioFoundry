// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's settings that the codeplug's settings area holds, one row per key (docs/spec/codeplug-settings.md
// 3): the data service's and the Settings > Sound tones'. The same tables on every radio: a setting is added here, and
// the radio core, the menu and the debug link follow it through codeplug/codeplug_keys.h
#ifndef RFOUNDRY_RADIO_KEYS_H
#define RFOUNDRY_RADIO_KEYS_H

#include "config.h"
#include "dmr/dmrDataService.h"
#include "codeplug/codeplug_keys.h"
#include "radio/beep.h"

static const codeplug_key_row_t radio_keys[] = {
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_ARS_SERVER, CODEPLUG_T_ID, CONFIG_DMR_ARS, dmrDataSettings_t, arsServer, 0,
			 CODEPLUG_DMR_ID_MAX),
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_IP_GATEWAY, CODEPLUG_T_ID, CONFIG_DMR_IP_GATEWAY, dmrDataSettings_t, ipGatewayId, 0,
			 CODEPLUG_DMR_ID_MAX),
};
#define RADIO_KEYS_N	((uint8_t)(sizeof(radio_keys) / sizeof(radio_keys[0])))
_Static_assert(sizeof(radio_keys) / sizeof(radio_keys[0]) <= CODEPLUG_KEYS_MAX, "codeplug_keys_store's batch");

// The tones Settings > Sound turns off, one bool key each (absent: the radio's own copy). The radio's own copy is
// radio_settings_t.tones_off, bit n set when row n's tone is off; its owner unpacks it into this struct for the table
// and packs it back (radio_tones_unpack, radio_tones_pack). Every build with tones has them: each is a beep the radio
// core plays, FM or DMR
typedef struct {
	bool call_end;		// on: the tone when a DMR call heard ends
	bool tot_warn;
	bool battery;		// low and critical
	bool tx_deny;
} radio_tones_t;

static const codeplug_key_row_t radio_tone_rows[] = {
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_CALL_END_TONE, CODEPLUG_T_BOOL, CONFIG_BEEP, radio_tones_t, call_end, 0, 1),
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_TOT_WARN_TONE, CODEPLUG_T_BOOL, CONFIG_BEEP, radio_tones_t, tot_warn, 0, 1),
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_BATTERY_TONE, CODEPLUG_T_BOOL, CONFIG_BEEP, radio_tones_t, battery, 0, 1),
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_TX_DENY_TONE, CODEPLUG_T_BOOL, CONFIG_BEEP, radio_tones_t, tx_deny, 0, 1),
};
#define RADIO_TONE_ROWS_N	((uint8_t)(sizeof(radio_tone_rows) / sizeof(radio_tone_rows[0])))
#define RADIO_TONES_MASK	((uint8_t)((1u << RADIO_TONE_ROWS_N) - 1u))	// the bits of tones_off

// The beeps row n's tone covers (beep2 BEEP_NONE: one)
static const struct {
	uint8_t beep, beep2;	// beep_t
} radio_tone_beeps[] = {
	{ BEEP_CALL_END, BEEP_NONE },
	{ BEEP_TOT_WARN, BEEP_NONE },
	{ BEEP_BATTERY_LOW, BEEP_BATTERY_CRIT },
	{ BEEP_TX_DENY, BEEP_NONE },
};
_Static_assert(sizeof(radio_tone_beeps) / sizeof(radio_tone_beeps[0]) == RADIO_TONE_ROWS_N, "a beep pair per row");

static inline void radio_tones_unpack(uint8_t tones_off, radio_tones_t *t)
{
	for (uint8_t i = 0; i < RADIO_TONE_ROWS_N; i++) {
		codeplug_keys_set_field(&radio_tone_rows[i], t, (tones_off >> i & 1u) == 0);
	}
}

static inline uint8_t radio_tones_pack(const radio_tones_t *t)
{
	uint8_t off = 0;
	for (uint8_t i = 0; i < RADIO_TONE_ROWS_N; i++) {
		if (codeplug_keys_field(&radio_tone_rows[i], t) == 0) {
			off |= (uint8_t)(1u << i);
		}
	}
	return off;
}

// The row of a tone's key, or -1
static inline int radio_tone_row(uint16_t key)
{
	for (uint8_t i = 0; i < RADIO_TONE_ROWS_N; i++) {
		if (radio_tone_rows[i].key == key) {
			return i;
		}
	}
	return -1;
}

// Whether tones_off silences beep b
static inline bool radio_tone_off(uint8_t tones_off, uint8_t b)
{
	for (uint8_t i = 0; i < RADIO_TONE_ROWS_N; i++) {
		if ((tones_off >> i & 1u) != 0 && (radio_tone_beeps[i].beep == b || radio_tone_beeps[i].beep2 == b)) {
			return true;
		}
	}
	return false;
}

#endif
