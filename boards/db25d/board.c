// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Radioddity DB25-D PRO: board descriptor, IO mux and reset-time pin levels, key map (docs/spec/hardware-db25d.md)
#include "board.h"
#include "config.h"
#include "db25d.h"
#include "dmr_db25d.h"
#include "pins.h"
#include "platform.h"
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

// Nominal 20 W mobile. The radio refuses TX until the PA calibration can be read (radio_db25d.c)
static const char *const power_names[] = { "L", "H" };

// docs/ux/information-architecture.md 7.4 (mobile: front encoder and a few keys; the encoder push is OK). The hand
// mic adds the digits, * and # (marked down and up) and P4-P7
static const keymap_entry_t keymap[] = {
	{ KEY_ENC_PUSH, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_BE, ACT_BACK, ACT_BACK, false, false },
	{ KEY_P1, ACT_QUICK, ACT_QUICK, false, false },
	{ KEY_P2, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ KEY_P3, ACT_MONITOR, ACT_MONITOR, true, false },
	{ KEY_TONE, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ KEY_VOL, ACT_VOLUME, ACT_VOLUME, false, false },
	{ KEY_MIC_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false },
	{ KEY_MIC_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ KEY_MIC_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false },
	{ KEY_MIC_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ KEY_MIC_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false },
	{ KEY_MIC_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ KEY_MIC_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false },
	{ KEY_MIC_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ KEY_MIC_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false },
	{ KEY_MIC_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ KEY_MIC_STAR, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ KEY_MIC_HASH, ACT_NAV_UP, ACT_NAV_UP, false, true },
	{ KEY_MIC_P4, ACT_USER_3, ACT_USER_3, false, false },
	{ KEY_MIC_P5, ACT_USER_4, ACT_USER_4, false, false },
	{ KEY_MIC_P6, ACT_QUICK, ACT_QUICK, false, false },
	{ KEY_MIC_P7, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false },
};

#if CONFIG_CPS_OPENFW
// What the open-firmware CPS reads in the radio info (open-firmware-cps.md 4). qdmr and the CPS know no DB25-D, and the
// image layout is the STM32 radios' (flash_db25d.c), so the radio reports type 6 (MD-UV380) as the DM-32UV does. No
// MCU ROM area and no flash security registers: those areas are refused. The factory calibration is outside the
// image (flash_db25d.c), so there is no calibration window either
static uint32_t cps_radio_type(void)
{
	return 6;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
};
#endif

const board_t board = {
	.name = "DB25-D",
	.caps = CAP_ENCODER | CAP_NO_BATTERY | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_DMR ? CAP_DMR : 0),
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
#if CONFIG_GPS
	.gps_enable = hrc_gps_enable,
	.gps_present = hrc_gps_present,	// CAP_GPS: the module is fitted, but say so only once it has been heard
#endif
#if CONFIG_DIAG
	.diag_line = db25d_diag_line,
	.diag_reset = db25d_diag_reset,
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
	.flash = &db25d_flash,
#endif
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
};

// IO mux: only the fields this firmware needs are changed; the rest stay as the IAP loader left them (the stock
// application never touches the JTAG bits either, 7). Bit = 1 makes a pin a GPIO, 0 gives it to its peripheral
#define DIPLEX0_UART1		((1u << 9) | (1u << 10))	// UART1 RXD / TXD
#define DIPLEX1_SPK_GPIO	((1u << 11) | (1u << 12))	// the speaker route pins as GPIOs, as the stock firmware
#define DIPLEX2_LCD_GPIO	(0x3Fu << 3)			// the LCD pins (PTC2-PTC7) as GPIOs, as the stock firmware

// The power latch is driven high before anything else so the radio stays on once the power key is let go
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);

	IO_DIPLEX0 &= ~DIPLEX0_UART1;
	IO_DIPLEX1 |= DIPLEX1_SPK_GPIO;
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
	hrc_pin_out(PIN_SPK_MIC, true);		// low turns the backlight off too
	hrc_pin_out(PIN_SPK_BODY, true);
	hrc_pin_out(PIN_MIC_PATH, false);
	hrc_pin_out(PIN_AUDIO_SWITCH, true);
	hrc_pin_out(PIN_PARK_A17, true);
	hrc_pin_out(PIN_BCAST_AUDIO, false);
	hrc_pin_out(PIN_LED_GREEN, false);
	hrc_pin_out(PIN_LED_RED, false);
	hrc_pin_out(PIN_GPS_POWER, !GPS_POWER_ON_LEVEL);	// GPS off until board_init
	hrc_pin_out(PIN_LCD_CS, true);
	hrc_pin_out(PIN_LCD_RESET, true);

	hrc_pin_in(PIN_RF_DATA);
	hrc_pin_in(PIN_POWER_KEY);
}

// The GPS module on UART2 (hardware-db25d.md 3.1). Its RXD pad is PTA11, a GPIO until IO_DIPLEX0 bit 11 is cleared
// (HR_C7000 user guide 4.4.4.4); the stock firmware's mux for it hasn't been traced
static const hrc_gps_cfg_t gps_cfg = {
	.uart_base = UART2_BASE,
	.irq = IRQ_UART2,
	.power_pin = PIN_GPS_POWER,
	.power_on_level = GPS_POWER_ON_LEVEL,
	.rxd_mux_clear = 1u << 11,
};

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8; hardware-db25d.md 3.2): both TX supplies, both
// PA selects and the PA drive (DAC B to 0, powered down), with PTA23 (RX_SUPPLY) high as in receive. RAM code, register
// writes only. The main BK4819 may still be set to transmit (its 3-wire bus is a slow bit-banged transaction): with the
// supplies and the PA selects off its output reaches no powered PA, and the next start sets it up again
HRC_RAMFUNC void board_rf_safe(void)
{
	hrc_pin_force(PIN_TX_SUPPLY_A, false);
	hrc_pin_force(PIN_TX_SUPPLY_B, false);
	hrc_pin_force(PIN_UHF_TX, false);
	hrc_pin_force(PIN_VHF_TX, false);
	DAC_DATA_B = 0;
	DAC_PD_CTRL |= DAC_B_OFF;
	hrc_pin_force(PIN_RX_SUPPLY, true);
}

void board_early_init(void)
{
	io_init();
}

void board_init(void)
{
	db25d_power.init();
#if CONFIG_GPS
	hrc_gps_init(&gps_cfg);
#endif
	// Both calibration copies, the higher save counter wins (hardware-db25d.md 6.1). Without the NAND, the flash copy
	bool nand = db25d_nand_init();
	db25d_cal_load((const uint8_t *)DB25D_CAL_FLASH_ADDR, nand ? db25d_nand_read : NULL);
#if CONFIG_NAND_STORAGE
	// The core's storage (hardware-db25d.md 6.6): mounted before storage_init reads it. Without it the radio runs on
	// the default settings, as before
	db25d_flash.init();
#endif
}
