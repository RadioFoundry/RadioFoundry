// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The data service's settings that the codeplug's settings area holds, one row per key (docs/spec/codeplug-settings.md
// 3). The same table on every radio: a setting is added here, and the radio core, the menu and the debug link follow
// it through codeplug/codeplug_keys.h
#ifndef RFOUNDRY_RADIO_KEYS_H
#define RFOUNDRY_RADIO_KEYS_H

#include "config.h"
#include "dmr/dmrDataService.h"
#include "codeplug/codeplug_keys.h"

static const codeplug_key_row_t radio_keys[] = {
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_ARS_SERVER, CODEPLUG_T_ID, CONFIG_DMR_ARS, dmrDataSettings_t, arsServer, 0,
			 CODEPLUG_DMR_ID_MAX),
	CODEPLUG_KEY_ROW(CODEPLUG_KEY_IP_GATEWAY, CODEPLUG_T_ID, CONFIG_DMR_IP_GATEWAY, dmrDataSettings_t, ipGatewayId, 0,
			 CODEPLUG_DMR_ID_MAX),
};
#define RADIO_KEYS_N	((uint8_t)(sizeof(radio_keys) / sizeof(radio_keys[0])))
_Static_assert(sizeof(radio_keys) / sizeof(radio_keys[0]) <= CODEPLUG_KEYS_MAX, "codeplug_keys_store's batch");

#endif
