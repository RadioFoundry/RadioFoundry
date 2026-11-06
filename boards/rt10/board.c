// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Retevis RT10: board descriptor, reset-time pin levels, key map
#include "board.h"
#include "config.h"
#include "rt10.h"
#include "pins.h"
#include "platform.h"
#if CONFIG_DMR
#include "dmr_glue.h"
#include "audio_pcm.h"
#include "vocoder.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif
#if CONFIG_APRS
#include "hrc_afsk.h"
#endif

static const band_t bands[] = {
	{ 902000000, 928000000, true },
};

// Power levels (hardware-rt10.md 5)
static const char *const power_names[] = { "L", "H" };

static const keymap_entry_t keymap[] = {
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_SK2, ACT_USER_1, ACT_USER_2, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };
static const char *const side_key_names[] = { "SK1", "SK2" };

const board_t board = {
	.name = "RT10",
	.caps = CONFIG_DMR ? CAP_DMR | CAP_BEEP_NOT_ON_AIR : 0,
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,
	.display = &rt10_display,
	.input = &rt10_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_names = side_key_names,
	.side_key_count = sizeof(side_keys),
	.radio = &rt10_radio,
	.power = &rt10_power,
	.flash = &hrc_flash_spi0,
#if CONFIG_DIAG
	.diag_line = rt10_diag_line,
#endif
#if CONFIG_DMR
	.dmr = &hrc_dmr,
	.audio = &hrc_audio_pcm,
	.vocoder = &hrc_vocoder,
#endif
#if CONFIG_APRS
	.afsk = &hrc_afsk,	// through the HR_C7000 (hardware-rt10.md 7.2)
#endif
#if CONFIG_HOSTLINK
	.host_port = &hrc_host_uart_stream,
#endif
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// IPv4 over PPP on the accessory jack's UART0 (docs/spec/ppp.md)
#endif
	.system = &hrc_system,
	.chip_reg = rt10_chip_reg,
};

// hardware-rt10.md 3: the boot ROM's reset-default IO mux is kept (UART0, JTAG); SPI0 to the data flash. The power
// latch goes high before anything else so the radio stays on
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);
	IO_DIPLEX1 = (IO_DIPLEX1 & ~0x1F8u) | 0x38000000u;	// SPI0 SCLK / MOSI / MISO; CS by GPIO

	// Outputs at their safe levels: transmitter off, receiver on, speaker muted, voice chip off
	hrc_pin_out(PIN_FLASH_CS, true);
	hrc_pin_out(PIN_TX_SUPPLY, false);
	hrc_pin_out(PIN_PA_SELECT, false);
	hrc_pin_out(PIN_RX_SUPPLY, true);
	hrc_pin_out(PIN_AMP_EN, false);
	hrc_pin_out(PIN_VOICE_EN, false);
	hrc_pin_out(PIN_VOICE_CLK, false);
	hrc_pin_out(PIN_VOICE_DATA, false);
	hrc_pin_out(PIN_LED_GREEN, false);
	hrc_pin_out(PIN_LED_RED, true);	// on from power-up, as the radio does it, until the core takes the LEDs
	hrc_pin_out(PIN_FD_SCL, true);	// the two-wire bus idle
	hrc_pin_in(PIN_FD_SDA);

	hrc_pin_in(PIN_POWER_SWITCH);
	hrc_pin_in(PIN_PTT);
	hrc_pin_in(PIN_SK1);
	hrc_pin_in(PIN_SK2);
	hrc_pin_in(PIN_KNOB_B0);
	hrc_pin_in(PIN_KNOB_B1);
	hrc_pin_in(PIN_KNOB_B2);
	hrc_pin_in(PIN_KNOB_B3);
}

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8): the PA select and the TX supply low, the
// receive supply high, the PA drive (DAC B) to 0 and powered down. RAM code, register writes only
HRC_RAMFUNC void board_rf_safe(void)
{
	hrc_pin_force(PIN_PA_SELECT, false);
	hrc_pin_force(PIN_TX_SUPPLY, false);
	hrc_pin_force(PIN_RX_SUPPLY, true);
	DAC_DATA_B = 0;
	DAC_PD_CTRL |= DAC_B_OFF;
}

void board_early_init(void)
{
	io_init();
}

// Everything the core stores lives in the upper half of the data flash, above the stock firmware's data (0x1300 up)
// and below the activation block (0x1FE000) and the factory calibration (0x1FF000), which are never written
// (hardware-rt10.md 4)
static const hal_flash_map_t flash_map = {
	.settings_addr = 0x100000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x108000u,	// 2 sectors
	.openfw_eeprom = 0x110000u,
	.openfw_flash = 0x130000u,
	.openfw_end = 0x1C0000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0x1C0000u,
	.native_size = 0x03E000u,
};
static const hrc_flash_cfg_t flash_cfg = {
	.cs_pin = PIN_FLASH_CS,
	.write_floor = 0x100000u,
	.map = &flash_map,
};

void board_init(void)
{
	hrc_flash_spi0_configure(&flash_cfg);
	rt10_power.init();
	hrc_flash_spi0.init();
}
