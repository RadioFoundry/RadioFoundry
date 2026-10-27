// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC B RF and audio paths (common/gd88_rf.h): direct GPIO pairs (pins.h, docs/spec/hardware-gd88.md 10). B is a
// receiver only: the TX switches stay low, and the TX calls only make sure of it. Its audio goes to A, which routes it
// to the speaker; gd88b_path_mute() holds the source pair off while A hasn't given B the speaker.
//
// A's receiver also runs through B's pins: the front end of A's band and PTB1 (bench 2026-10-26: without them A hears
// nothing, with them A's noise floor rises 40 dB and a bench signal shows). A says where it receives with RX2_PATH
// (gd88b_path_main). Each front end is on while A or B receives in its band, and each receiver has its own path pin
// (pins.h): PTB1 A's, PTB0 B's, in FM and DMR alike.
#include "gd88_rf.h"
#include "gd88b.h"
#include "pins.h"
#include "platform.h"

#define UHF_FROM_HZ		280000000u

static const uint8_t init_low[] = {
	PIN_PA_SUP_UHF, PIN_PA_SUP_VHF, PIN_PA_SEL_UHF, PIN_PA_SEL_VHF, PIN_RX_A, PIN_RX_B, PIN_RX_FRONT_UHF,
	PIN_RX_FRONT_VHF, PIN_PARK_B13, PIN_PARK_B5, PIN_PARK_B12, PIN_PARK_B11, PIN_RX_BAND_UHF, PIN_RX_BAND_VHF,
	PIN_TX_PATH_UHF, PIN_TX_PATH_VHF, PIN_AUD_SRC0, PIN_AUD_SRC1,
};

static bool spk_on, spk_modem, muted = true;
static bool b_on, b_uhf;		// B's own receiver
static bool a_on, a_uhf;		// A's (RX2_PATH)

void gd88_path_init(void)
{
	for (uint8_t i = 0; i < sizeof(init_low); i++) {
		hrc_pin_out(init_low[i], false);
	}
	hrc_pin_out(PIN_AUD_VHF, true);
	hrc_pin_out(PIN_AUD_UHF, true);
	spk_on = spk_modem = false;
	muted = true;
	b_on = a_on = false;
}

// The front ends and the path pins from both receivers' state; interrupts off
static void rx_pins(void)
{
	hrc_pin_write(PIN_RX_FRONT_UHF, (b_on && b_uhf) || (a_on && a_uhf));
	hrc_pin_write(PIN_RX_FRONT_VHF, (b_on && !b_uhf) || (a_on && !a_uhf));
	hrc_pin_write(PIN_RX_A, a_on);
	hrc_pin_write(PIN_RX_B, b_on);
}

void gd88_path_rx(uint32_t hz, bool on, bool dmr)
{
	bool uhf = hz >= UHF_FROM_HZ;
	uint32_t s = hrc_irq_save();
	b_on = on;
	b_uhf = uhf;
	(void)dmr;	// the same path in FM and DMR
	hrc_pin_write(PIN_RX_BAND_UHF, on && uhf);
	hrc_pin_write(PIN_RX_BAND_VHF, on && !uhf);
	rx_pins();
	hrc_pin_write(PIN_AUD_VHF, !uhf);
	hrc_pin_write(PIN_AUD_UHF, uhf);
	hrc_irq_restore(s);
}

// B's own receiver off; A's path stays as A asked
void gd88_path_rx_front_off(void)
{
	uint32_t s = hrc_irq_save();
	b_on = false;
	rx_pins();
	hrc_irq_restore(s);
}

void gd88b_path_main(bool on, uint32_t hz)
{
	uint32_t s = hrc_irq_save();
	a_on = on;
	a_uhf = hz >= UHF_FROM_HZ;
	rx_pins();
	hrc_irq_restore(s);
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
