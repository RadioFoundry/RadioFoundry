// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D RF chips (rfchip_db25d.h; docs/spec/hardware-db25d.md 4.2, 4.8). Both chips sit on one bit-banged 3-wire bus
// with an enable each; every part answers the same frame, so each chip gets an rfbus_3w_t and its driver.
#include "rfchip_db25d.h"
#include "db25d.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/rfbus/rfbus.h"
#include "rf/superhet/superhet_chips.h"

#define IF_HZ		51550000u
#define VHF_BELOW_HZ	280000000u	// the band split of the RF paths
// The band flags, high for VHF: the main chip's carries the TX band, the LO's the RX band (hardware-db25d.md 4.8)
#define GPIO_BAND	3u		// FD6818 and AT1141, main and LO
#define BK_MAIN_BAND	2u		// BK4819 main (PRO)
#define BK_LO_BAND	1u		// BK4819 LO (PRO). Low, VHF hears nothing
// The LO's output that follows ADC 7 (hardware-db25d.md 3.3); what it drives is unknown
#define GPIO_LO_AUX	4u		// FD6818 and AT1141 LO
#define BK_LO_AUX	2u		// BK4819 LO (PRO)

static rfbus_3w_t bus_main, bus_lo;
static superhet_chips_t rf;
static uint8_t main_band, lo_band;	// the band flags' GPIO pins
static uint8_t lo_aux;

static bool css_line(void) { return hrc_pin_read(PIN_RF_CSS); }

static const superhet_chips_cfg_t rf_cfg = {
	.bus_main = &bus_main.bus, .bus_lo = &bus_lo.bus, .css_line = css_line, .at1141_restart = false,
};

// ---- 3-wire bus: clock and data shared, one enable per chip ----

static void rf_clk(bool level) { hrc_pin_write(PIN_RF_CLK, level); }
static void rf_data_out(bool level) { hrc_pin_out(PIN_RF_DATA, level); }
static void rf_data_release(void) { hrc_pin_in(PIN_RF_DATA); }
static bool rf_data_in(void) { return hrc_pin_read(PIN_RF_DATA); }
static void cs_main(bool level) { hrc_pin_write(PIN_RF_SEN_MAIN, level); }
static void cs_lo(bool level) { hrc_pin_write(PIN_RF_SEN_LO, level); }

static const rfbus_3w_pins_t pins_main = {
	.cs = cs_main, .clk = rf_clk, .data_out = rf_data_out, .data_release = rf_data_release, .data_in = rf_data_in,
	.delay_us = hal_delay_us, .crit_enter = hrc_irq_save, .crit_exit = hrc_irq_restore, .delay_ms = hal_delay_ms,
};

static const rfbus_3w_pins_t pins_lo = {
	.cs = cs_lo, .clk = rf_clk, .data_out = rf_data_out, .data_release = rf_data_release, .data_in = rf_data_in,
	.delay_us = hal_delay_us, .crit_enter = hrc_irq_save, .crit_exit = hrc_irq_restore, .delay_ms = hal_delay_ms,
};

void db25d_rf_bus_init(void)
{
	hrc_pin_out(PIN_RF_SEN_MAIN, true);
	hrc_pin_out(PIN_RF_SEN_LO, true);
	hrc_pin_out(PIN_RF_CLK, false);
	hrc_pin_in(PIN_RF_DATA);
	rfbus_3w_attach(&bus_main, &pins_main);
	rfbus_3w_attach(&bus_lo, &pins_lo);
}

bool db25d_rf_init(void)
{
	// The LO is above the signal (f + 51.55 MHz): DCS arrives inverted (4.7)
	if (!superhet_chips_init(&rf, &rf_cfg)) {
		return false;
	}
	main_band = rf.main_kind == SUPERHET_CHIP_BK4819 ? BK_MAIN_BAND : GPIO_BAND;
	lo_band = rf.lo_kind == SUPERHET_CHIP_BK4819 ? BK_LO_BAND : GPIO_BAND;
	lo_aux = rf.lo_kind == SUPERHET_CHIP_BK4819 ? BK_LO_AUX : GPIO_LO_AUX;
	superhet_chips_rx_inverted(&rf, true);
	if (rf.main_kind != SUPERHET_CHIP_BK4819) {
		hrc_pin_in(PIN_RF_CSS);
	}
	return true;
}

uint16_t db25d_rf_main_id(void) { return rf.main_id; }
uint16_t db25d_rf_lo_id(void) { return rf.lo_id; }

void db25d_rf_variant(char out[4])
{
	out[0] = 'D';
	out[1] = superhet_chip_letter(rf.main_kind);
	out[2] = superhet_chip_letter(rf.lo_kind);
	out[3] = '\0';
}

const char *db25d_rf_chip_name(bool lo)
{
	return superhet_chip_name(lo ? rf.lo_kind : rf.main_kind);
}

uint32_t db25d_rf_writes(void)
{
	return bus_main.bus.writes + bus_lo.bus.writes;
}

// ---- Main transceiver: the board's steps around the chip's ops ----

static superhet_tone_t sh_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS: return SUPERHET_TONE_CTCSS;
	case TONE_DCS: return SUPERHET_TONE_DCS;
	case TONE_DCS_INV: return SUPERHET_TONE_DCS_INV;
	default: return SUPERHET_TONE_NONE;
	}
}

static superhet_af_t sh_af(db25d_af_t af)
{
	return af == DB25D_AF_FM ? SUPERHET_AF_FM : af == DB25D_AF_DMR ? SUPERHET_AF_DMR : SUPERHET_AF_MUTE;
}

void db25d_main_idle(void) { rf.main.ops->idle(rf.main.chip); }
void db25d_main_rx_state(void) { rf.main.ops->rx(rf.main.chip); }

void db25d_main_band(uint32_t hz) { superhet_main_gpio(&rf.main, main_band, hz < VHF_BELOW_HZ); }
void db25d_main_to_if(void) { rf.main.ops->to_if(rf.main.chip, IF_HZ); }
void db25d_main_bandwidth(radio_bw_t bw) { rf.main.ops->bandwidth(rf.main.chip, bw == RADIO_BW_25K); }
void db25d_main_rx_tone(uint8_t type, uint16_t value) { rf.main.ops->rx_tone(rf.main.chip, sh_tone(type), value); }

bool db25d_main_tone_heard(uint16_t r0c, uint8_t type)
{
	return superhet_chips_tone_heard(&rf, r0c, sh_tone(type));
}

void db25d_main_af(db25d_af_t af) { rf.main.ops->af(rf.main.chip, sh_af(af)); }
void db25d_main_af_gain(uint8_t gain) { rf.main.ops->af_gain(rf.main.chip, gain); }

void db25d_main_status(uint16_t *r67, uint16_t *r65, uint16_t *r0c)
{
	superhet_chips_status(&rf, r67, r65, r0c);
}

void db25d_main_tx_prepare(uint8_t type, uint16_t value, uint32_t hz, radio_bw_t bw)
{
	(void)bw;	// neither chip's deviation is switched by bandwidth: the FD6818's goes by band and sub-audio
	db25d_main_band(hz);
	rf.main.ops->tx_prepare(rf.main.chip, sh_tone(type), value, hz, db25d_cal_pa_gain(hz));
}

void db25d_main_tx_tune(uint32_t hz) { rf.main.ops->tx_tune(rf.main.chip, hz); }
void db25d_main_tx_end(void) { rf.main.ops->tx_end(rf.main.chip); }
bool db25d_main_tail(bool on) { return rf.main.ops->tail(rf.main.chip, on); }
void db25d_main_tone(uint16_t hz, uint8_t gain) { rf.main.ops->tone(rf.main.chip, hz, gain); }
bool db25d_main_tone_on(void) { return rf.main.ops->tone_on(rf.main.chip); }
void db25d_main_tone_key(bool down) { rf.main.ops->tone_key(rf.main.chip, down); }
void db25d_main_dtmf(uint16_t low_hz, uint16_t high_hz) { rf.main.ops->dtmf(rf.main.chip, low_hz, high_hz); }
void db25d_main_dmr_enter(uint8_t af_gain) { rf.main.ops->dmr_enter(rf.main.chip, af_gain); }
void db25d_main_dmr_leave(void) { rf.main.ops->dmr_leave(rf.main.chip); }

void db25d_main_dmr_tx_prepare(uint32_t hz)
{
	db25d_main_band(hz);
	rf.main.ops->dmr_tx_prepare(rf.main.chip, db25d_cal_pa_gain(hz));
}

void db25d_main_burst(uint32_t hz, bool tx) { rf.main.ops->burst(rf.main.chip, hz, tx); }

// ---- LO ----

void db25d_lo_tune(uint32_t hz)
{
	superhet_lo_gpio(&rf.lo, lo_band, hz - IF_HZ < VHF_BELOW_HZ);
	rf.lo.ops->tune(rf.lo.chip, hz);
}

void db25d_lo_off(void) { rf.lo.ops->off(rf.lo.chip); }
void db25d_lo_resume(void) { rf.lo.ops->resume(rf.lo.chip); }
void db25d_lo_aux(bool high) { superhet_lo_gpio(&rf.lo, lo_aux, high); }
