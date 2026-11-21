// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Shared display and init match section 7; bezel insets remain unverified.
#include "board.h"
#include "config.h"
#if CONFIG_GPS
#error "This board has no GPS module"
#endif
#include "dm32.h"
#include "platform.h"
#include "hrc_afsk.h"
#include "hrc_bcast.h"
#if CONFIG_BCAST_FM
#include "bcast/rda5807m/rda5807m_hal.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif
#if CONFIG_RX_EXT
static bool rx_ext_on;
#endif
#include "bands.h"
#include "../dm32/board_keys.h"
const hal_flash_map_t board_flash_map = {
	.settings_addr = 0x711000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x719000u,	// 2 sectors
	.openfw_eeprom = 0x700000u,
	.openfw_flash = 0x720000u,
	.openfw_end = 0xE00000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0xF00000u,
	.native_size = 0x100000u,
};

const board_t board = {
	.name = "Baofeng DM-36",
#include "../dm32/board_fields.h"
};
