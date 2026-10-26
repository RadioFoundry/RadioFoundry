// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radioddity GD-88, SoC A: board descriptor, IO mux and reset-time pin levels, key map (docs/spec/hardware-gd88.md 9).
// A runs the display, keys, GPS, the PC link on UART1 and its own transceiver; SoC B is the second receiver
// (board.rx2, rx2_gd88.c, boards/gd88b).
#include "board.h"
#include "config.h"
#include "gd88.h"
#include "pins.h"
#include "platform.h"
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif
#if CONFIG_BCAST_FM
#include "bcast/rda5807m/rda5807m_hal.h"
#endif

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

// The page's two PA tables. The radio refuses TX until the PA calibration can be read (radio_gd88.c), and without
// GD88_TX=1 altogether
static const char *const power_names[] = { "L", "H" };

// docs/ux/information-architecture.md 7.1. Green and red are select and back; the original firmware labels the right
// soft key A/B on its screen. P1-P3 are the side and top keys
static const keymap_entry_t keymap[] = {
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_SOFT_LEFT, ACT_VFO_TOGGLE, ACT_COPY_VFO, false, false, 0 },
	{ KEY_SOFT_RIGHT, ACT_AB, ACT_DUAL_WATCH, false, false, 0 },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false, 0 },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false, 0 },
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
	{ KEY_P1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_P2, ACT_TALK_TARGET, ACT_USER_2, false, false, 0 },
	{ KEY_P3, ACT_USER_3, ACT_USER_3, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys (core/input/keyassign.h): side key 1, side key 2, top key
static const uint8_t side_keys[] = { KEY_P1, KEY_P2, KEY_P3 };

#if CONFIG_CPS_OPENFW
// What the open-firmware CPS reads in the radio info (open-firmware-cps.md 4): type 6, as the other HR_C7000 boards
// report, since the CPS knows no GD-88. The factory calibration is outside the image, so no calibration window
static uint32_t cps_radio_type(void)
{
	return 6;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
};
#endif

const board_t board = {
	.name = "GD-88",
	.caps = CAP_KEYPAD | CAP_ENCODER | CAP_VOLUME_POT | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_DMR ? CAP_DMR : 0) | (CONFIG_BCAST_FM ? CAP_BCAST_FM : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,
	.display = &gd88_display,
	.input = &gd88_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_count = sizeof(side_keys),
	.radio = &gd88_radio,
	.power = &gd88_power,
	.battery_low_mv = 7000,
	.battery_critical_mv = 6000,
#if CONFIG_GPS
	.gps_enable = hrc_gps_enable,
	.gps_present = hrc_gps_present,
#endif
	.rx2 = &gd88_rx2,
#if CONFIG_DIAG
	.diag_line = gd88_diag_line,
	.diag_reset = gd88_diag_reset,
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
	.netif = &hostlink_ppp_netif,	// IPv4 over PPP on the host port's UART1 (docs/spec/ppp.md)
#endif
	.system = &hrc_system,
#if CONFIG_NAND_STORAGE
	.flash = &gd88_flash,
#endif
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
#if CONFIG_BCAST_FM
	.bcast = &rda5807m_hal,
#endif
};

// IO mux: only the fields this firmware needs are changed. Bit = 1 makes a pin a GPIO, 0 gives it to its peripheral
#define DIPLEX0_UART1		((1u << 9) | (1u << 10))	// UART1 RXD / TXD
#define DIPLEX1_SPI0		0x1F8u				// SPI0 to the NAND

// The power latch is driven high before anything else so the radio stays on once the knob's own path lets go
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);

	IO_DIPLEX0 &= ~DIPLEX0_UART1;
	IO_DIPLEX1 &= ~DIPLEX1_SPI0;
	// The three DAC outputs on their pins
	IO_DIPLEX2 &= ~(IO_DIPLEX2_DAC_A | IO_DIPLEX2_DAC_B | IO_DIPLEX2_DAC_C);

	// Outputs at their safe levels: transmitter off (the chain all zero), speaker off, chip selects high
	hrc_pin_out(GD88_PIN_NAND_CS, true);
	hrc_pin_out(GD88_PIN_RF_SEN_MAIN, true);
	hrc_pin_out(GD88_PIN_RF_SEN_LO, true);
	hrc_pin_out(GD88_PIN_RF_CLK, false);
	hrc_pin_out(PIN_TX_RX, false);
	hrc_pin_out(PIN_DIG_AUDIO, false);
	hrc_pin_out(PIN_SPK_AMP, false);
	hrc_pin_out(PIN_PARK_A30, false);
	hrc_pin_out(PIN_GPS_POWER, !GPS_POWER_ON_LEVEL);	// GPS off until board_init
	hrc_pin_out(PIN_LCD_RESET, true);
	gd88_chain_init();
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_out((uint8_t)(PIN_KEY_ROW0 + r), false);	// rows idle low
	}

	hrc_pin_in(GD88_PIN_RF_DATA);
	hrc_pin_in(PIN_POWER_KEY);
	hrc_pin_in(PIN_RF_STATUS0);
	hrc_pin_in(PIN_RF_STATUS1);
}

#if CONFIG_GPS
// The GPS module on UART2. Its RXD pad is a GPIO until IO_DIPLEX0 bits 11 and 12 are cleared
static const hrc_gps_cfg_t gps_cfg = {
	.uart_base = UART2_BASE,
	.irq = IRQ_UART2,
	.power_pin = PIN_GPS_POWER,
	.power_on_level = GPS_POWER_ON_LEVEL,
	.rxd_mux_clear = (1u << 11) | (1u << 12),
};
#endif

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8): the TX / RX line PTB2 low, every chain output
// to 0 (its power-on state: no audio or TX route), the PA drive (DAC B to 0, powered down) and PTC20 low. RAM code,
// register writes only. The main BK4819 may still be set to transmit: with PTB2 low the RF switch is on receive, and
// the next start sets it up again
HRC_RAMFUNC void board_rf_safe(void)
{
	hrc_pin_force(PIN_TX_RX, false);
	DAC_DATA_B = 0;
	DAC_PD_CTRL |= DAC_B_OFF;
	hrc_pin_force(PIN_DIG_AUDIO, false);
	hrc_pin_force(PIN_CHAIN_LATCH, true);
	hrc_pin_force(PIN_CHAIN_CLK, false);
	hrc_pin_force(PIN_CHAIN_DATA, false);
	gd88_chain_zero();
}

void board_early_init(void)
{
	io_init();
}

void board_init(void)
{
	gd88_power.init();
#if CONFIG_GPS
	hrc_gps_init(&gps_cfg);
#endif
	// Both calibration copies, the higher save counter wins. Without the NAND, the flash copy
	bool nand = gd88_nand_init();
	gd88_cal_load((const uint8_t *)GD88_CAL_FLASH_ADDR, nand ? gd88_nand_read : NULL);
#if CONFIG_NAND_STORAGE
	// The core's storage: mounted before storage_init reads it. Without it the radio runs on the default settings
	gd88_flash.init();
#endif
	gd88_rx2_init();
#if CONFIG_BCAST_FM
	gd88_bcast_init();
#endif
}
