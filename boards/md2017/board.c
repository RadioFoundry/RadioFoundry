// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TYT MD-2017 / Retevis RT82: board descriptor, key map and what sets the radio apart in the UV380 family
// (boards/uv380_family/uvf.h, docs/spec/hardware-md2017.md). Its wiring is the family's UVF_PINS_MD2017 pin map
#include "board.h"
#include "config.h"
#include "../uv380_family/uvf.h"
#include "hrc/dmr_glue.h"
#include "hrc/vocoder.h"
#include "pins.h"
#include "fsmc_keypad/fsmc_keypad.h"
#if CONFIG_GPS
#include "gps_uart/gps_uart.h"
#endif
#if CONFIG_USB
#include "usb/usb_device.h"
#include "usb/usb_ncm.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

// Matrix positions, [row][column D0..D7] (hardware-md2017.md 5): the MD-UV380's digits, MENU and BACK where it has up
// and down, P1 and P2 where it has green and red, SK3 and the ring's center on row 2. The ring's four directions and
// the orange top key have their own lines (input_uvf.c)
const uint8_t uvf_key_matrix[3][8] = {
	{ KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_0, KEY_STAR },
	{ KEY_FN_LEFT, KEY_GREEN, KEY_RED, KEY_7, KEY_8, KEY_9, KEY_HASH, KEY_FN_RIGHT },
	{ STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, KEY_SK3, KEY_OK, KEY_SK2,
	  KEY_SK1 },
};

static void init(void)
{
#if CONFIG_GPS
	uvf_gps_init();	// the GPS builds' module on PA10 / PA9 (hardware-md2017.md 9)
#endif
}

// hardware-md2017.md: 2 (the main AT1846S at 0x2E, the second receiver's at 0x71), 4 (the MD-UV380's panel table: the
// TYT images run the same init), 7 (battery and volume pot: the DM-1701's divider and the MD-2017 pot ends, both
// unconfirmed), 8 (the DM-1701's calibration layout; the power fractions and k factors are the DM-1701's until measured)
const uvf_variant_t uvf_variant = {
	.rf_addr = { 0x2E, 0x71 },
	.panel_type = { 0, 1, 2, 3 },
	.panel_rgb = { true, true, true, true },
	.bat_mul = 8, .bat_div = 3,
	.vol_min = 32, .vol_break = 321, .vol_max = 2048,
	.frac = { { 450, 750, 250, 530 }, { 460, 730, 140, 360 } },
	.cal_k = { { 610, 1000, 850, 1300 }, { 650, 1000, 860, 1700 } },
	.init = init,
};

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

// The UV380 family's nine levels (hardware-dm1701.md 7)
static const char *const power_names[] = { "50mW", "250mW", "500mW", "750mW", "1W", "2W", "3W", "4W", "5W" };

// docs/ux/information-architecture.md 7.1. MENU and the ring's center select, BACK goes back, the ring's directions
// are the arrow keys (no channel knob: the arrows step the channel). The side keys top to bottom are SK1, SK2 and SK3
// (hardware-md2017.md 5); P1 and P2 the programmable front keys; the orange top key is the emergency key, held, where
// that is built
#if CONFIG_DMR_EMERGENCY
#define TOP_LONG	ACT_EMERGENCY
#else
#define TOP_LONG	ACT_NONE
#endif

static const keymap_entry_t keymap[] = {
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_OK, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, false, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, false, 0 },
	{ KEY_LEFT, ACT_NAV_LEFT, ACT_NAV_LEFT, false, false, 0 },
	{ KEY_RIGHT, ACT_NAV_RIGHT, ACT_NAV_RIGHT, false, false, 0 },
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
	{ KEY_SK3, ACT_USER_3, ACT_USER_3, false, false, 0 },
	{ KEY_FN_LEFT, ACT_USER_1, ACT_USER_1, false, false, 0 },
	{ KEY_FN_RIGHT, ACT_USER_4, ACT_USER_4, false, false, 0 },
	{ KEY_TOP, ACT_NONE, TOP_LONG, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom, the front
// keys, the top key
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2, KEY_SK3, KEY_FN_LEFT, KEY_FN_RIGHT, KEY_TOP };
static const char *const side_key_names[] = { "SK1", "SK2", "SK3", "P1", "P2", "Top" };

#if CONFIG_CPS_OPENFW
// open-firmware-cps.md 4: type 9 is the MD-2017
static uint32_t cps_radio_type(void)
{
	return 9;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
	.rom_base = 0x08000000u,
	.rom_size = 1024u * 1024u,
	.security_read = stm32_spi_flash_cps_security_read,
};
#endif

const board_t board = {
	.name = "MD-2017",
	// CAP_GPS: the GPS builds; gps_present says whether this one has the module (hardware-md2017.md 9)
	.caps = CAP_KEYPAD | CAP_VOLUME_POT | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_DMR ? CAP_DMR : 0),
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
#if CONFIG_GPS
	.gps_present = stm32_gps_present,
#endif
	// The family's thresholds (hardware-dm1701.md 11.1): low battery 6.2 V, critical 6.0 V
	.battery_low_mv = 6200,
	.battery_critical_mv = 6000,
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
#if CONFIG_APRS
	.afsk = &uvf_afsk,
#endif
};
