// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Radioddity DB25-D PRO: board descriptor, IO mux and reset-time pin levels, key map (docs/spec/hardware-db25d.md)
#include "board.h"
#include "config.h"
#include "db25d.h"
#include "pins.h"
#include "platform.h"

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

// Nominal 20 W mobile. The radio refuses TX until the PA calibration can be read (radio_db25d.c)
static const char *const power_names[] = { "L", "H" };

// docs/ux/information-architecture.md 7.4 (mobile: front encoder and a few keys). The keys' matrix positions are
// provisional (input_db25d.c); the hand mic's keys aren't decoded yet
static const keymap_entry_t keymap[] = {
	{ KEY_MENU, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_ENC_PUSH, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_BE, ACT_BACK, ACT_BACK, false, false },
	{ KEY_P1, ACT_QUICK, ACT_QUICK, false, false },
	{ KEY_P2, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ KEY_P3, ACT_MONITOR, ACT_MONITOR, true, false },
	{ KEY_TONE, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ KEY_VOL, ACT_USER_1, ACT_USER_1, false, false },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false },
};

const board_t board = {
	.name = "DB25-D",
	.caps = CAP_ENCODER,
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,
	.display = &db25d_display,
	.input = &db25d_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.radio = &db25d_radio,
	.power = &db25d_power,
#if CONFIG_DIAG
	.diag_line = db25d_diag_line,
	.diag_reset = db25d_diag_reset,
#endif
#if CONFIG_HOSTLINK
	.host_port = &hrc_host_uart_stream,
#endif
	.system = &hrc_system,
};

// IO mux: only the fields this firmware needs are changed; the rest stay as the IAP loader left them (the stock
// application never touches the JTAG bits either, 7). Bit = 1 makes a pin a GPIO, 0 gives it to its peripheral
#define DIPLEX0_UART1		((1u << 9) | (1u << 10))	// UART1 RXD / TXD
#define DIPLEX2_LCD_GPIO	(0x3Fu << 3)			// the LCD pins (PTC2-PTC7) as GPIOs, as the stock firmware

// The power latch is driven high before anything else so the radio stays on once the power key is let go
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);

	IO_DIPLEX0 &= ~DIPLEX0_UART1;
	// The three DAC outputs on their pins, as the stock firmware
	IO_DIPLEX2 = (IO_DIPLEX2 | DIPLEX2_LCD_GPIO) & ~(IO_DIPLEX2_DAC_A | IO_DIPLEX2_DAC_B | IO_DIPLEX2_DAC_C);

	// Outputs at their safe levels: transmitter off, receiver paths off, speaker muted, chip selects high
	hrc_pin_out(PIN_NAND_CS, true);
	hrc_pin_out(PIN_RF_SEN_MAIN, true);
	hrc_pin_out(PIN_RF_SEN_LO, true);
	hrc_pin_out(PIN_RF_CLK, false);
	hrc_pin_out(PIN_TX_SUPPLY_A, false);
	hrc_pin_out(PIN_TX_SUPPLY_B, false);
	hrc_pin_out(PIN_UHF_TX, false);
	hrc_pin_out(PIN_VHF_TX, false);
	hrc_pin_out(PIN_UHF_RX, false);
	hrc_pin_out(PIN_VHF_RX, false);
	hrc_pin_out(PIN_RX_FRONT_END, false);
	hrc_pin_out(PIN_RX_SUPPLY, true);
	hrc_pin_out(PIN_SPK_SOURCE, true);
	hrc_pin_out(PIN_AUDIO_A, false);
	hrc_pin_out(PIN_AUDIO_B, false);
	hrc_pin_out(PIN_BCAST_AUDIO, false);
	hrc_pin_out(PIN_LED_A, false);
	hrc_pin_out(PIN_LED_B, false);
	hrc_pin_out(PIN_GPS_POWER, false);	// polarity unknown; nothing reads the GPS yet
	hrc_pin_out(PIN_LCD_CS, true);
	hrc_pin_out(PIN_LCD_RESET, true);

	hrc_pin_in(PIN_RF_DATA);
	hrc_pin_in(PIN_POWER_KEY);
}

void board_early_init(void)
{
	io_init();
}

void board_init(void)
{
	db25d_power.init();
	// Both calibration copies, the higher save counter wins (hardware-db25d.md 6.1). Without the NAND, the flash copy
	db25d_cal_load((const uint8_t *)DB25D_CAL_FLASH_ADDR, db25d_nand_init() ? db25d_nand_read : NULL);
}
