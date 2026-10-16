// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Baofeng DM-32UV: board descriptor, IO mux and reset-time pin levels, key map
#include "board.h"
#include "config.h"
#include "dm32.h"
#include "pins.h"
#include "platform.h"
#include "FreeRTOS.h"
#include "semphr.h"
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

// Power levels 0..8 of hardware-dm32.md 11.3 (level 9, the user value, isn't offered). Labels are nominal: the radio
// is rated 5 W VHF / 4 W UHF at high power; check against a meter at the bring-up
static const char *const power_names[] = { "L-", "L", "L+", "L++", "M", "M+", "M++", "H-", "H" };

// docs/ux/information-architecture.md 7.1 (the DM-32 has no top key; SK1 / SK2 are the side keys below the PTT)
static const keymap_entry_t keymap[] = {
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ KEY_LEFT, ACT_NAV_LEFT, ACT_USER_1, false, true },
	{ KEY_RIGHT, ACT_NAV_RIGHT, ACT_USER_4, false, true },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false },
	{ KEY_SK2, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): side key 1, side key 2
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };

const board_t board = {
	.name = "DM-32UV",
	.caps = CAP_KEYPAD | CAP_ENCODER | CAP_GPS | (CONFIG_DMR ? CAP_DMR : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 1,	// low
	.display = &dm32_display,
	.input = &dm32_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_count = sizeof(side_keys),
	.radio = &dm32_radio,
	.power = &dm32_power,
	.flash = &hrc_flash_spi0,
#if CONFIG_DIAG
	.diag_line = dm32_diag_line,
	.diag_reset = dm32_diag_reset,
#endif
#if CONFIG_DMR
	.dmr = &hrc_dmr,
	.audio = &hrc_audio_pcm,
	.vocoder = &hrc_vocoder,
#endif
#if CONFIG_HOSTLINK
	.host_port = &hrc_host_uart_stream,
#endif
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// IPv4 over PPP on the programming cable's UART (docs/spec/ppp.md)
#endif
	.gps_present = dm32_gps_present,	// CAP_GPS: the module is fitted, but say so only once it has been heard
	.system = &hrc_system,
};

static SemaphoreHandle_t lcdbus;

void dm32_lcdbus_lock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreTakeRecursive(lcdbus, portMAX_DELAY);
	}
}

void dm32_lcdbus_unlock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreGiveRecursive(lcdbus);
	}
}

// docs/spec/hardware-dm32.md 5.1: IO mux, pulls, directions and levels. The power latch is driven high before
// anything else so the radio stays on when the knob's own supply path lets go
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);

	IO_DIPLEX0 = 0xFFFDFD80u;			// JTAG, I2C0, UART1 RXD, PWM0; PTA7/8 GPIO
	IO_DIPLEX1 = 0xFFFFFE07u | 0x38000000u;		// SPI0 to the data flash; CS0 and the rest GPIO
	IO_DIPLEX2 = 0x0FC80100u;			// LCD bus GPIO until the display takes it; ADC B/C, DAC B/C analog
	IOMGR_REN0 = 0xFFFBFFEFu & 0xFEFFFFFFu;
	IOMGR_REN1 = 0x00FFFFFFu;
	IOMGR_REN2 = 0x0FFFFF70u | 0x0C000000u;

	// Outputs at their safe levels: transmitter and receivers off, speaker muted, chip selects high
	hrc_pin_out(PIN_FLASH_CS, true);
	hrc_pin_out(PIN_FD_CS, true);
	hrc_pin_out(PIN_FD_CLK, false);
	hrc_pin_out(PIN_UHF_PA, false);
	hrc_pin_out(PIN_VHF_PA, false);
	hrc_pin_out(PIN_UHF_LNA, false);
	hrc_pin_out(PIN_VHF_LNA, false);
	hrc_pin_out(PIN_MIC_PATH, true);
	hrc_pin_out(PIN_SPK_SOURCE, false);	// the codec until FM receive opens the squelch
	hrc_pin_out(PIN_AMP_EN, false);
	hrc_pin_out(PIN_LED_RED, false);
	hrc_pin_out(PIN_LED_GREEN, false);
	hrc_pin_out(PIN_KEY_BACKLIGHT, false);
	hrc_pin_out(PIN_LCD_BACKLIGHT, false);
	hrc_pin_out(PIN_GPS_POWER, true);	// GPS off until something wants it
	hrc_pin_out(PIN_LCD_RESET, true);
	hrc_pin_out(PIN_LCD_RD, true);
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_out((uint8_t)(PIN_KEY_ROW0 + r), true);	// rows idle high
	}
	hrc_pin_out(PIN_LCD_DB6, false);

	// Inputs, all without interrupts (the old port enabled GPIO B interrupts on every input: spec 16 #9)
	hrc_pin_in(PIN_POWER_SWITCH);
	hrc_pin_in(PIN_PTT);
	for (uint8_t c = PIN_KEY_COL1; c <= PIN_KEY_COL4; c++) {
		hrc_pin_in(c);
	}
	hrc_pin_in(PIN_ENC_A);
	hrc_pin_in(PIN_ENC_B);
	hrc_pin_in(PIN_FD_GPIO0);
	hrc_pin_in(PIN_FD_GPIO1);
	hrc_pin_in(PIN_FD_DATA);
}

void board_early_init(void)
{
	io_init();
}

// Everything the core stores lives above the write floor (hardware-dm32.md 10.1): the open-firmware codeplug's 64 KB
// "EEPROM" image at 0x700000 (CODEPLUG_OPENFW_EEPROM_BASE in board.mk), OpenDM32's calibration copy left alone at
// 0x710000, the settings ring and the sector journal after it, the 1 MB open-firmware "flash" image at 0x720000
// (CODEPLUG_OPENFW_FLASH_BASE) with room above it for the DMR ID database, the native codeplug in the last 1 MB. The
// factory calibration is read only, so the open-firmware CPS gets no calibration window
static const hal_flash_map_t flash_map = {
	.settings_addr = 0x711000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x719000u,	// 2 sectors
	.openfw_eeprom = 0x700000u,
	.openfw_flash = 0x720000u,
	.openfw_end = 0xF00000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0xF00000u,
	.native_size = 0x100000u,
};
static const hrc_flash_cfg_t flash_cfg = {
	.cs_pin = PIN_FLASH_CS,
	.write_floor = 0x700000u,	// hardware-dm32.md 10.1
	.map = &flash_map,
};

void board_init(void)
{
	hrc_flash_spi0_configure(&flash_cfg);
	lcdbus = xSemaphoreCreateRecursiveMutex();
	dm32_power.init();
	dm32_flash.init();
	bf_cal_load(&hrc_flash_spi0, 0x710000u);	// OpenDM32 keeps a decoded copy there
	dm32_gps_init();
}
