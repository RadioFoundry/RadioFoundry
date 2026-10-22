// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC B RF and audio paths (common/gd88_rf.h): direct GPIO pairs (pins.h, docs/spec/hardware-gd88.md 10). B is a
// receiver only: the TX switches stay low, and the TX calls only make sure of it. Its audio goes to A, which routes it
// to the speaker; gd88b_path_mute() holds the source pair off while A hasn't given B the speaker.
#include "gd88_rf.h"
#include "gd88b.h"
#include "pins.h"
#include "platform.h"

#define UHF_FROM_HZ		280000000u

static const uint8_t init_low[] = {
	PIN_PA_SUP_UHF, PIN_PA_SUP_VHF, PIN_PA_SEL_UHF, PIN_PA_SEL_VHF, PIN_RX_M0_A, PIN_RX_M1_A, PIN_RX_FRONT_UHF,
	PIN_RX_FRONT_VHF, PIN_RX_M0_B, PIN_RX_M1_B, PIN_PARK_B12, PIN_PARK_B11, PIN_RX_BAND_UHF, PIN_RX_BAND_VHF,
	PIN_TX_PATH_UHF, PIN_TX_PATH_VHF, PIN_AUD_SRC0, PIN_AUD_SRC1,
};

static bool spk_on, spk_modem, muted = true;

void gd88_path_init(void)
{
	for (uint8_t i = 0; i < sizeof(init_low); i++) {
		hrc_pin_out(init_low[i], false);
	}
	hrc_pin_out(PIN_AUD_VHF, true);
	hrc_pin_out(PIN_AUD_UHF, true);
	spk_on = spk_modem = false;
	muted = true;
}

static void rx_pairs(bool on, bool dmr)
{
	hrc_pin_write(PIN_RX_M0_A, on && !dmr);
	hrc_pin_write(PIN_RX_M0_B, on && !dmr);
	hrc_pin_write(PIN_RX_M1_A, on && dmr);
	hrc_pin_write(PIN_RX_M1_B, on && dmr);
}

void gd88_path_rx(uint32_t hz, bool on, bool dmr)
{
	bool uhf = hz >= UHF_FROM_HZ;
	uint32_t s = hrc_irq_save();
	hrc_pin_write(PIN_RX_BAND_UHF, on && uhf);
	hrc_pin_write(PIN_RX_BAND_VHF, on && !uhf);
	hrc_pin_write(PIN_RX_FRONT_UHF, on && uhf);
	hrc_pin_write(PIN_RX_FRONT_VHF, on && !uhf);
	rx_pairs(on, dmr);
	hrc_pin_write(PIN_AUD_VHF, !uhf);
	hrc_pin_write(PIN_AUD_UHF, uhf);
	hrc_irq_restore(s);
}

void gd88_path_rx_front_off(void)
{
	hrc_pin_write(PIN_RX_FRONT_UHF, false);
	hrc_pin_write(PIN_RX_FRONT_VHF, false);
	rx_pairs(false, false);
}

// Receive only: the PA selects and supplies stay off
void gd88_path_tx_select(bool uhf)
{
	(void)uhf;
	gd88_path_tx_off();
}

void gd88_path_tx_supply(bool uhf)
{
	(void)uhf;
	gd88_path_tx_off();
}

// The supplies first, as the TX-off sequence does
void gd88_path_tx_off(void)
{
	hrc_pin_write(PIN_PA_SUP_UHF, false);
	hrc_pin_write(PIN_PA_SUP_VHF, false);
	hrc_pin_write(PIN_PA_SEL_UHF, false);
	hrc_pin_write(PIN_PA_SEL_VHF, false);
	hrc_pin_write(PIN_TX_PATH_UHF, false);
	hrc_pin_write(PIN_TX_PATH_VHF, false);
}

static void source(void)
{
	bool on = spk_on && !muted;
	hrc_pin_write(PIN_AUD_SRC0, on && !spk_modem);
	hrc_pin_write(PIN_AUD_SRC1, on && spk_modem);
}

void gd88_path_speaker(bool on, bool from_modem)
{
	uint32_t s = hrc_irq_save();
	spk_on = on;
	spk_modem = from_modem;
	source();
	hrc_irq_restore(s);
}

void gd88b_path_mute(bool mute)
{
	uint32_t s = hrc_irq_save();
	muted = mute;
	source();
	hrc_irq_restore(s);
}
