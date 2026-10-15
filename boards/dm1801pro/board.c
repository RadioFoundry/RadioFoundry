// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Baofeng DM-1801 Pro: board descriptor, IO mux and reset-time pin levels, key map
#include "board.h"
#include "config.h"
#include "dm1801pro.h"
#include "pins.h"
#include "platform.h"
#include "cal/baofeng/baofeng_cal.h"

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 470000000, true },
};

// The factory calibration holds PA drive points for three power levels (hardware-dm1801pro.md 6.4): low, middle, high
static const char *const power_names[] = { "L", "M", "H" };

// docs/ux/information-architecture.md 7.6. The key bits are provisional (input_dm1801pro.c)
static const keymap_entry_t keymap[] = {
	{ KEY_MENU, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_BACK, ACT_BACK, ACT_BACK, false, false },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ KEY_LEFT, ACT_NAV_LEFT, ACT_USER_1, false, true },
	{ KEY_RIGHT, ACT_NAV_RIGHT, ACT_USER_4, false, true },
	{ KEY_VFO_MR, ACT_USER_3, ACT_USER_3, false, false },
	{ KEY_AB, ACT_ZONE_UP, ACT_ZONE_UP, false, false },
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
	{ KEY_SOS, ACT_NONE, ACT_EMERGENCY, false, false },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): side key 1, side key 2
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };

const board_t board = {
	.name = "DM-1801 Pro",
	.caps = CAP_KEYPAD,
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,	// low
	.display = &dm1801pro_display,
	.input = &dm1801pro_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_count = sizeof(side_keys),
	.radio = &dm1801pro_radio,
	.power = &dm1801pro_power,
	.flash = &hrc_flash_spi0,
#if CONFIG_DIAG
	.diag_line = dm1801pro_diag_line,
	.diag_reset = dm1801pro_diag_reset,
#endif
#if CONFIG_HOSTLINK
	.host_port = &hrc_host_uart_stream,
#endif
	.system = &hrc_system,
};

// IO mux: only the fields this firmware needs are changed; the rest stay as the boot loader left them. Bit = 1 makes a
// pin a GPIO, 0 gives it to its peripheral (hardware-dm1801pro.md 4)
#define DIPLEX1_SPI0		(0x3Fu << 3)	// SPI0 SCLK / MOSI / MISO
#define DIPLEX2_ADC_BC		((1u << 20) | (1u << 21))	// ADC channels B and C analog

// The power latch is driven high before anything else so the radio stays on once the power key is let go
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);

	IO_DIPLEX1 &= ~DIPLEX1_SPI0;
	IO_DIPLEX2 &= ~(DIPLEX2_ADC_BC | IO_DIPLEX2_DAC_B);

	// Outputs at their safe levels: transmitter and receiver paths off, speaker muted, chip selects high
	hrc_pin_out(PIN_FLASH_CS, true);
	hrc_pin_out(PIN_RF_CS, true);
	hrc_pin_out(PIN_RF_CLK, false);
	hrc_pin_out(PIN_TX_VHF, false);
	hrc_pin_out(PIN_TX_UHF, false);
	hrc_pin_out(PIN_RX_VHF, false);
	hrc_pin_out(PIN_RX_UHF, false);
	hrc_pin_out(PIN_MIC_PATH, true);
	hrc_pin_out(PIN_SPK_SOURCE, true);
	hrc_pin_out(PIN_FM_AUDIO, false);
	hrc_pin_out(PIN_BCAST_EN, true);
	hrc_pin_out(PIN_LED_RED, false);
	hrc_pin_out(PIN_LED_GREEN, false);
	hrc_pin_out(PIN_LCD_BACKLIGHT, false);
	hrc_pin_out(PIN_VIBRATION, false);
	hrc_pin_out(PIN_LCD_CS, true);
	hrc_pin_out(PIN_LCD_RESET, true);

	hrc_pin_in(PIN_RF_DATA);
	hrc_pin_in(PIN_POWER_KEY);
}

// Everything the core stores lives above the write floor (hardware-dm1801pro.md 6.1): the data flash holds the
// radio's own data below 0x700000 (settings, contacts, fonts, voice prompts, recordings), so the open-firmware
// codeplug's 64 KB "EEPROM" image sits at 0x700000 (CODEPLUG_OPENFW_EEPROM_BASE in board.mk), the settings ring and the
// sector journal after it, the 1 MB open-firmware "flash" image at 0x720000 (CODEPLUG_OPENFW_FLASH_BASE) and the native
// codeplug in the last 1 MB. The factory calibration is read only, so the open-firmware CPS gets no calibration window
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
	.write_floor = 0x700000u,
	.map = &flash_map,
};

void board_early_init(void)
{
	io_init();
}

void board_init(void)
{
	hrc_flash_spi0_configure(&flash_cfg);
	dm1801pro_power.init();
	hrc_flash_spi0.init();
	bf_cal_load(&hrc_flash_spi0, 0);	// no decoded copy on this radio
}
