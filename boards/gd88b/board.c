// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radioddity GD-88, SoC B: board descriptor, reset-time pin levels and the transmitter-off hook
// (docs/spec/hardware-gd88.md 10). B is the second receiver: no display, keys, power control or PC link. A powers it
// and talks to it over UART3.
// The descriptor holds what the shared radio and DMR code reads (the bands) and the DMR parts the receiver uses.
#include "board.h"
#include "config.h"
#include "gd88_rf.h"
#include "gd88b.h"
#include "pins.h"
#include "platform.h"

static const band_t bands[] = {
	{ 136000000, 174000000, false },
	{ 400000000, 480000000, false },
};

const board_t board = {
	.name = "GD-88 B",
	.caps = CONFIG_DMR ? CAP_DMR : 0,
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.radio = &gd88_radio,
#if CONFIG_DMR
	.dmr = &hrc_dmr,
	.audio = &hrc_audio_pcm,
	.vocoder = &hrc_vocoder,
#endif
	.system = &hrc_system,
	.diag_line = gd88b_diag_line,
};

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8): every TX switch low and the PA drive (DAC B)
// at 0, powered down. RAM code, register writes only
HRC_RAMFUNC void board_rf_safe(void)
{
	hrc_pin_force(PIN_PA_SUP_UHF, false);
	hrc_pin_force(PIN_PA_SUP_VHF, false);
	hrc_pin_force(PIN_PA_SEL_UHF, false);
	hrc_pin_force(PIN_PA_SEL_VHF, false);
	hrc_pin_force(PIN_TX_PATH_UHF, false);
	hrc_pin_force(PIN_TX_PATH_VHF, false);
	DAC_DATA_B = 0;
	DAC_PD_CTRL |= DAC_B_OFF;
}

void board_early_init(void)
{
	// Off the programming cable's lines first: A's UART1 and PTT use them (pins.h)
	IO_DIPLEX0 |= DIPLEX0_UART1;
	hrc_pin_in(PIN_CABLE_RXD);
	hrc_pin_in(PIN_CABLE_TXD);
	// The three DAC outputs on their pins, as on A.
	// TODO(gd88): unknown, needs a radio: whether B's DAC pins are wired as A's
	IO_DIPLEX2 &= ~(IO_DIPLEX2_DAC_A | IO_DIPLEX2_DAC_B | IO_DIPLEX2_DAC_C);
	gd88_path_init();
	hrc_pin_out(GD88_PIN_RF_SEN_MAIN, true);
	hrc_pin_out(GD88_PIN_RF_SEN_LO, true);
	hrc_pin_out(GD88_PIN_RF_CLK, false);
	hrc_pin_in(GD88_PIN_RF_DATA);
	hrc_pin_out(PIN_LED1, false);
	hrc_pin_out(PIN_LED2, false);
}

void board_init(void)
{
	// B's own factory page, its program-flash copy.
	// TODO(gd88): unknown, needs a radio: B's NAND chip select, for the NAND copy of its page
	gd88_cal_load((const uint8_t *)GD88_CAL_FLASH_ADDR, NULL);
}

void gd88b_leds(uint8_t leds)
{
	hrc_pin_write(PIN_LED1, (leds & 1u) != 0);
	hrc_pin_write(PIN_LED2, (leds & 2u) != 0);
}
