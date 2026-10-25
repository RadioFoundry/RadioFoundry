// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TYT MD-UV380 / MD-UV390 / Retevis RT3S, and the MD-UV390 Plus 10 W (POWER=10w): board descriptor, key map and what sets
// the radio apart in the UV380 family (boards/uv380_family/uvf.h, docs/spec/hardware-mduv380.md 1)
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

// The Plus 10 W check (hardware-mduv380.md 4): security register 3 byte 0x1C reads 0xF4 on that radio. An image for the
// other version would drive the PA band select through the backlight pin, so tx() refuses on a mismatch. 0 skips the
// check (a bench knob, board.mk MDUV380_DEFS) while the marker is unconfirmed on a radio
#ifndef MDUV380_10W_CHECK
#define MDUV380_10W_CHECK	1
#endif
#define MARK_10W		0xF4u

// Matrix positions, [row][column D0..D7] (hardware-mduv380.md 5): no P1/P2, no top key, no third side key
const uint8_t uvf_key_matrix[3][8] = {
	{ KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_0, KEY_STAR },
	{ KEY_GREEN, KEY_UP, KEY_DOWN, KEY_7, KEY_8, KEY_9, KEY_HASH, KEY_RED },
	{ STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY,
	  STM32_KEYPAD_NO_KEY, KEY_SK2, KEY_SK1 },
};

static uint8_t mark_10w;	// security register 3 byte 0x1C, read at start-up (0xFF until then or when unreadable)

static const char *tx_block(void)
{
#if MDUV380_10W_CHECK
#if RFOUNDRY_POWER_10W
	if (mark_10w != MARK_10W) {
		return "not a 10 W radio";
	}
#else
	if (mark_10w == MARK_10W) {
		return "10 W radio, 5 W image";
	}
#endif
#endif
	return NULL;
}

#if CONFIG_GPS
// The G models' module (hardware-mduv380.md 9): NMEA on USART1 RX (PA10), power on PA9, high = on. The same board ships
// without it, so the module is powered for the listen window only and switched off again when nothing is heard. A
// sentence keeps it on, and every sentence goes to the APRS position source (CONFIG_APRS)
static const uint32_t gps_rates[] = { 9600 };

static void gps_absent(void)
{
	PIN_WRITE(PIN_GPS_POWER, false);
}

static const stm32_gps_cfg_t gps = {
	.uart = USART1, .irq = USART1_IRQn,
	.rx_port = PIN_PORT(PIN_GPS_RX), .rx_pin = PIN_MASK(PIN_GPS_RX), .rx_af = GPIO_AF7_USART1,
	.rates = gps_rates, .n_rates = 1, .listen_ms = 5000,
	.absent = gps_absent,
};

void USART1_IRQHandler(void)
{
	stm32_gps_irq();
}
#endif

static void init(void)
{
	if (!stm32_spi_flash_read_security(3, 0x1C, &mark_10w, 1)) {
		mark_10w = 0xFF;
	}
#if CONFIG_GPS
	PIN_WRITE(PIN_GPS_POWER, true);	// an output, low since reset (system_uvf.c)
	stm32_gps_init(&gps);
#endif
}

// hardware-mduv380.md: 4 (every factory type kept, always RGB order), 6 (raw / 40.3 per 0.1 V; the volume pot ends of
// the MD-UV380 22..2070 and the RT3S 23..2020, break 309 / 317, which one image can't tell apart: the higher bottom plus
// 7 counts of margin, where the RT3S reads unsteadily, the lower top so both reach 100 %, the break between), 7 and 8
// (the 5 W rows; the Plus 10 W's: its pot 19..2054 / 308 and its own rows)
const uvf_variant_t uvf_variant = {
	.panel_type = { 0, 1, 2, 3 },
	.panel_rgb = { true, true, true, true },
	.bat_mul = 1000, .bat_div = 403,
#if RFOUNDRY_POWER_10W
	.vol_min = 26, .vol_break = 308, .vol_max = 2054,
	.frac = { { 580, 830, 210, 450 }, { 550, 750, 170, 430 } },
	.cal_k = { { 350, 650, 1250, 1400 }, { 400, 700, 1200, 1350 } },
#else
	.vol_min = 30, .vol_break = 313, .vol_max = 2020,
	.frac = { { 350, 700, 340, 610 }, { 400, 700, 250, 550 } },
	.cal_k = { { 630, 1000, 1000, 1100 }, { 720, 1000, 1050, 1250 } },
#endif
	.init = init,
	.tx_block = tx_block,
};

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

// The UV380 family's nine levels; the Plus 10 W's top four are 2, 3, 5 and 10 W (hardware-mduv380.md 7)
#if RFOUNDRY_POWER_10W
static const char *const power_names[] = { "50mW", "250mW", "500mW", "750mW", "1W", "2W", "3W", "5W", "10W" };
#define BOARD_NAME	"MD-UV380 10W"
#else
static const char *const power_names[] = { "50mW", "250mW", "500mW", "750mW", "1W", "2W", "3W", "4W", "5W" };
#define BOARD_NAME	"MD-UV380"
#endif

// docs/ux/information-architecture.md 7.1 on a radio without function or top keys (a few-key map): the zone and power
// are reached from the menus. SK1 is the upper side key (held with PTT at power-on it is the bootloader combination,
// which the running firmware never sees), SK2 the lower one (hardware-mduv380.md 5)
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
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };
static const char *const side_key_names[] = { "SK1", "SK2" };

#if CONFIG_CPS_OPENFW
// open-firmware-cps.md 4: type 6 is the MD-UV380 family, every panel and power version (hardware-mduv380.md 10)
static uint32_t cps_radio_type(void)
{
	return 6;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
	.rom_base = 0x08000000u,
	.rom_size = 1024u * 1024u,
	.security_read = stm32_spi_flash_cps_security_read,
};
#endif

const board_t board = {
	.name = BOARD_NAME,
	// CAP_GPS: the G models; gps_present says whether this one has the module (hardware-mduv380.md 9)
	.caps = CAP_KEYPAD | CAP_ENCODER | CAP_VOLUME_POT | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_DMR ? CAP_DMR : 0),
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
	// The family's thresholds (hardware-mduv380.md 6): low battery 6.2 V, critical 6.0 V
	.battery_low_mv = 6200,
	.battery_critical_mv = 6000,
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
#if CONFIG_APRS
	.afsk = &uvf_afsk,
#endif
};
