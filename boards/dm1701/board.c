// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Baofeng DM-1701: board descriptor, key map and what sets it apart in the UV380 family (boards/uv380_family/uvf.h)
#include "board.h"
#include "config.h"
#include "../uv380_family/uvf.h"
#include "hrc/dmr_glue.h"
#include "hrc/vocoder.h"
#include "pins.h"
#include "fsmc_keypad/fsmc_keypad.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#include "usb/usb_ncm.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

// Matrix positions, [row][column D0..D7] (hardware-dm1701.md 10.1, DM-1701 / RT84 table; side keys: section 20)
const uint8_t uvf_key_matrix[3][8] = {
	{ KEY_1, KEY_4, KEY_7, KEY_STAR, KEY_UP, KEY_FN_RIGHT, KEY_FN_LEFT, STM32_KEYPAD_NO_KEY },
	{ KEY_2, KEY_5, KEY_8, KEY_0, KEY_DOWN, KEY_RED, KEY_GREEN, STM32_KEYPAD_NO_KEY },
	{ KEY_3, KEY_6, KEY_9, KEY_HASH, STM32_KEYPAD_NO_KEY, KEY_SK1, KEY_SK3, KEY_SK2 },
};

// hardware-dm1701.md: 9.2 (type 2 is "vertically flipped; treated as type 3 with RGB order": the bench radio reports 2,
// and the type 2 MADCTL showed the picture mirrored top to bottom), 11.1 (raw / 37.5 per 0.1 V), 11.2 (measured on the
// bench: 26 at the bottom, 2063 at the top; spec 29..2075), 7 and 12.1 (the DM-1701 rows)
const uvf_variant_t uvf_variant = {
	.panel_type = { 0, 1, 3, 3 },
	.panel_rgb = { false, false, true, false },
	.bat_mul = 8, .bat_div = 3,
	.vol_min = 26, .vol_break = 318, .vol_max = 2060,
	.frac = { { 450, 750, 250, 530 }, { 460, 730, 140, 360 } },
	.cal_k = { { 610, 1000, 850, 1300 }, { 650, 1000, 860, 1700 } },
};

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

static const char *const power_names[] = { "50mW", "250mW", "500mW", "750mW", "1W", "2W", "3W", "4W", "5W" };

// The side keys are SK1 (above PTT, row 2 D5; held with PTT at power-on it is the bootloader combination, which the
// running firmware never sees), SK2 and SK3 (below PTT), top to bottom (hardware-dm1701.md 20). SK3: short = power
// level; held 1.5 s = emergency where it is built (information-architecture.md 7.1), else speak status
#if CONFIG_DMR_EMERGENCY
#define SK3_LONG	ACT_EMERGENCY
#else
#define SK3_LONG	ACT_USER_3
#endif

// docs/ux/information-architecture.md 7.1
static const keymap_entry_t keymap[] = {
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false, '*' },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false, '#' },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false, 0 },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false, 0 },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false, 0 },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false, 0 },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false, 0 },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false, 0 },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false, 0 },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false, 0 },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false, 0 },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false, 0 },
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_SK2, ACT_AB, ACT_USER_2, false, false, 0 },
	{ KEY_SK3, ACT_USER_3, SK3_LONG, false, false, 0 },
	{ KEY_FN_LEFT, ACT_USER_1, ACT_USER_1, false, false, 0 },
	{ KEY_FN_RIGHT, ACT_USER_4, ACT_USER_4, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom, then P1 and P2
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2, KEY_SK3, KEY_FN_LEFT, KEY_FN_RIGHT };
static const char *const side_key_names[] = { "SK1", "SK2", "SK3", "P1", "P2" };

#if CONFIG_CPS_OPENFW
// open-firmware-cps.md 4: type 8 is the DM-1701 with a BGR panel, 10 with an RGB one (the factory panel type 2 is
// driven in RGB order, hardware-dm1701.md 9.2)
static uint32_t cps_radio_type(void)
{
	uint8_t type = 0;

	if (stm32_spi_flash_read_security(3, 0x1D, &type, 1) && (type & 3u) == 2u) {
		return 10;
	}
	return 8;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
	.rom_base = 0x08000000u,
	.rom_size = 1024u * 1024u,
	.security_read = stm32_spi_flash_cps_security_read,
};
#endif

const board_t board = {
	.name = "DM-1701",
	.caps = CAP_KEYPAD | CAP_ENCODER | CAP_VOLUME_POT | (CONFIG_DMR ? CAP_DMR : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 4,	// 1 W
	.display = &uvf_display,
	.input = &uvf_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_names = side_key_names,
	.side_key_count = sizeof(side_keys),
	.radio = &uvf_radio,
	.power = &uvf_power,
	.flash = &stm32_spi_flash,
#if CONFIG_DIAG
	.diag_line = uvf_diag_line,
	.diag_reset = uvf_diag_reset,
#endif
#if CONFIG_DMR
	.dmr = &stm32_hrc_dmr,
	.audio = &uvf_audio,
	.vocoder = &stm32_vocoder,
#if CONFIG_AUDIO_PROBE || CONFIG_NET_USRP || CONFIG_APRS_RX
	.audio_tap = &uvf_audio_tap,
#endif
#endif
#if CONFIG_USB
	.host_port = &usb_cdc_stream,
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// PPP on the USB serial port, the network adapter under it (board_init)
#elif CONFIG_USB_NETWORK
	.netif = &usb_ncm_netif,
#endif
#endif
	.system = &stm32_system,
	// hardware-dm1701.md 11.1: low battery 6.2 V (back above 6.4 V); critical 6.0 V (3.0 V a cell) is our choice, the
	// spec gives none (below 5.5 V the board reads as switched off)
	.battery_low_mv = 6200,
	.battery_critical_mv = 6000,
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
#if CONFIG_APRS
	.afsk = &uvf_afsk,
#endif
};

