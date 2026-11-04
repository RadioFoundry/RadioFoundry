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
#include "rf/bk4819/bk4819.h"
#include "rf/fd6818/fd6818sh.h"
#include "rf/at1141/at1141.h"
#include "rf/superhet/superhet.h"
#include "rf/bk4819/bk4819_superhet.h"
#include "rf/fd6818/fd6818sh_superhet.h"
#include "rf/at1141/at1141_superhet.h"

#define IF_HZ		51550000u
#define VHF_BELOW_HZ	280000000u	// the band split of the RF paths
// The band flags, high for VHF: the main chip's carries the TX band, the LO's the RX band (hardware-db25d.md 4.8)
#define GPIO_BAND	3u		// FD6818 and AT1141, main and LO
#define BK_MAIN_BAND	2u		// BK4819 main (PRO)
#define BK_LO_BAND	1u		// BK4819 LO (PRO). Low, VHF hears nothing
// The LO's output that follows ADC 7 (hardware-db25d.md 3.3); what it drives is unknown
#define GPIO_LO_AUX	4u		// FD6818 and AT1141 LO
#define BK_LO_AUX	2u		// BK4819 LO (PRO)
#define ID_TRIES	3u
#define ID_RESET_MS	100u		// stock: soft reset, this long, release, this long, read again
#define REG_ID		0x00
#define REG_PAGE	0x7F		// the FD6818 family's page select: page 0 before the reset

typedef enum { CHIP_NONE, CHIP_BK4819, CHIP_FD6818, CHIP_7813, CHIP_AT1141 } chip_t;

static rfbus_3w_t bus_main, bus_lo;
static bk4819_t bk_main, bk_lo;
static fd6818sh_t fd_main, fd_lo;
static at1141_t at_lo;
static chip_t main_chip, lo_chip;
static uint8_t main_band, lo_band;	// the band flags' GPIO pins
static uint8_t lo_aux;
static uint16_t main_id, lo_id;
static superhet_main_t sh_main;
static superhet_lo_t sh_lo;

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

// ---- Detection (4.2) ----

static chip_t kind(uint16_t id, bool lo)
{
	switch (id) {
	case BK4819_ID: return CHIP_BK4819;
	case FD6818SH_ID: return CHIP_FD6818;
	case FD6818SH_ID_7813: return lo ? CHIP_7813 : CHIP_NONE;	// seen only as the LO
	case AT1141_ID: return lo ? CHIP_AT1141 : CHIP_NONE;	// a synthesizer: never the transceiver
	default: return CHIP_NONE;
	}
}

// Register 0; a chip that answers with none of the known IDs gets the stock's soft reset (page 0 first) and a retry
static uint16_t identify(rfbus_t *bus, bool lo, chip_t *k)
{
	uint16_t id = 0;
	for (unsigned i = 0; i < ID_TRIES; i++) {
		id = rfbus_read(bus, REG_ID);
		*k = kind(id, lo);
		if (*k != CHIP_NONE) {
			break;
		}
		rfbus_write(bus, REG_PAGE, 0);
		rfbus_write(bus, REG_ID, 0x8000);
		hal_delay_ms(ID_RESET_MS);
		rfbus_write(bus, REG_ID, 0x0000);
		hal_delay_ms(ID_RESET_MS);
	}
	return id;
}

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
	main_id = identify(&bus_main.bus, false, &main_chip);
	lo_id = identify(&bus_lo.bus, true, &lo_chip);
	if (main_chip == CHIP_NONE || lo_chip == CHIP_NONE) {
		return false;
	}

	// The main chip's init, the LO's, then the main chip's analog set, as the stock and the PRO's earlier code. The LO
	// is above the signal (f + 51.55 MHz): DCS arrives inverted (4.7)
	main_band = main_chip == CHIP_BK4819 ? BK_MAIN_BAND : GPIO_BAND;
	lo_band = lo_chip == CHIP_BK4819 ? BK_LO_BAND : GPIO_BAND;
	lo_aux = lo_chip == CHIP_BK4819 ? BK_LO_AUX : GPIO_LO_AUX;
	if (main_chip == CHIP_BK4819) {
		bk4819_attach(&bk_main, &bus_main.bus);
		bk_main.rx_inverted = true;
		bk4819_init_main(&bk_main);
		bk4819_write(&bk_main, BK4819_REG_PA, BK4819_PA_MAX);	// stock: once at detection, never changed
		sh_main = (superhet_main_t){ &bk4819_superhet_main, &bk_main };
	} else {
		fd6818sh_attach(&fd_main, &bus_main.bus, FD6818SH_MAIN);
		fd_main.rx_inverted = true;
		fd6818sh_init_main(&fd_main);
		hrc_pin_in(PIN_RF_CSS);
		sh_main = (superhet_main_t){ &fd6818sh_superhet_main, &fd_main };
	}
	switch (lo_chip) {
	case CHIP_BK4819:
		bk4819_attach(&bk_lo, &bus_lo.bus);
		bk4819_init_lo(&bk_lo, BK4819_LO_PA_MAX);
		sh_lo = (superhet_lo_t){ &bk4819_superhet_lo, &bk_lo };
		break;
	case CHIP_AT1141:
		at1141_attach(&at_lo, &bus_lo.bus);
		at1141_init(&at_lo);
		at1141_disable(&at_lo);
		sh_lo = (superhet_lo_t){ &at1141_superhet_lo, &at_lo };
		break;
	default:
		fd6818sh_attach(&fd_lo, &bus_lo.bus, lo_chip == CHIP_7813 ? FD6818SH_LO_7813 : FD6818SH_LO);
		fd6818sh_init_lo(&fd_lo);
		sh_lo = (superhet_lo_t){ &fd6818sh_superhet_lo, &fd_lo };
		break;
	}
	if (main_chip == CHIP_BK4819) {
		bk4819_analog_mode(&bk_main);
	} else {
		fd6818sh_analog_mode(&fd_main);
	}
	return true;
}

uint16_t db25d_rf_main_id(void) { return main_id; }
uint16_t db25d_rf_lo_id(void) { return lo_id; }

static char letter(chip_t k)
{
	switch (k) {
	case CHIP_BK4819: return 'B';
	case CHIP_FD6818: return '6';
	case CHIP_7813: return '4';
	case CHIP_AT1141: return 'T';
	default: return '?';
	}
}

void db25d_rf_variant(char out[4])
{
	out[0] = 'D';
	out[1] = letter(main_chip);
	out[2] = letter(lo_chip);
	out[3] = '\0';
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

static bk4819_tone_t bk_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS: return BK4819_TONE_CTCSS;
	case TONE_DCS: return BK4819_TONE_DCS;
	case TONE_DCS_INV: return BK4819_TONE_DCS_INV;
	default: return BK4819_TONE_NONE;
	}
}

static superhet_af_t sh_af(db25d_af_t af)
{
	return af == DB25D_AF_FM ? SUPERHET_AF_FM : af == DB25D_AF_DMR ? SUPERHET_AF_DMR : SUPERHET_AF_MUTE;
}

void db25d_main_idle(void) { sh_main.ops->idle(sh_main.chip); }
void db25d_main_rx_state(void) { sh_main.ops->rx(sh_main.chip); }

void db25d_main_band(uint32_t hz) { superhet_main_gpio(&sh_main, main_band, hz < VHF_BELOW_HZ); }
void db25d_main_to_if(void) { sh_main.ops->to_if(sh_main.chip, IF_HZ); }
void db25d_main_bandwidth(radio_bw_t bw) { sh_main.ops->bandwidth(sh_main.chip, bw == RADIO_BW_25K); }
void db25d_main_rx_tone(uint8_t type, uint16_t value) { sh_main.ops->rx_tone(sh_main.chip, sh_tone(type), value); }

bool db25d_main_tone_heard(uint16_t r0c, uint8_t type)
{
	if (main_chip == CHIP_FD6818) {
		// The FD6818 has no status bit for it: its GPIO0 carries the compare result to PTB27
		return type == TONE_NONE || hrc_pin_read(PIN_RF_CSS);
	}
	return bk4819_heard(&bk_main, r0c, bk_tone(type));
}

void db25d_main_af(db25d_af_t af) { sh_main.ops->af(sh_main.chip, sh_af(af)); }
void db25d_main_af_gain(uint8_t gain) { sh_main.ops->af_gain(sh_main.chip, gain); }

void db25d_main_status(uint16_t *r67, uint16_t *r65, uint16_t *r0c)
{
	// The same registers on both chip kinds
	*r67 = rfbus_read(&bus_main.bus, BK4819_REG_RSSI);
	*r65 = rfbus_read(&bus_main.bus, BK4819_REG_NOISE);
	*r0c = rfbus_read(&bus_main.bus, BK4819_REG_CSS_STATUS);
}

void db25d_main_tx_prepare(uint8_t type, uint16_t value, uint32_t hz, radio_bw_t bw)
{
	(void)bw;	// neither chip's deviation is switched by bandwidth: the FD6818's goes by band and sub-audio
	db25d_main_band(hz);
	sh_main.ops->tx_prepare(sh_main.chip, sh_tone(type), value, hz, db25d_cal_pa_gain(hz));
}

void db25d_main_tx_tune(uint32_t hz) { sh_main.ops->tx_tune(sh_main.chip, hz); }
void db25d_main_tx_end(void) { sh_main.ops->tx_end(sh_main.chip); }
bool db25d_main_tail(bool on) { return sh_main.ops->tail(sh_main.chip, on); }
void db25d_main_tone(uint16_t hz, uint8_t gain) { sh_main.ops->tone(sh_main.chip, hz, gain); }
bool db25d_main_tone_on(void) { return sh_main.ops->tone_on(sh_main.chip); }
void db25d_main_tone_key(bool down) { sh_main.ops->tone_key(sh_main.chip, down); }
void db25d_main_dtmf(uint16_t low_hz, uint16_t high_hz) { sh_main.ops->dtmf(sh_main.chip, low_hz, high_hz); }
void db25d_main_dmr_enter(uint8_t af_gain) { sh_main.ops->dmr_enter(sh_main.chip, af_gain); }
void db25d_main_dmr_leave(void) { sh_main.ops->dmr_leave(sh_main.chip); }

void db25d_main_dmr_tx_prepare(uint32_t hz)
{
	db25d_main_band(hz);
	sh_main.ops->dmr_tx_prepare(sh_main.chip, db25d_cal_pa_gain(hz));
}

void db25d_main_burst(uint32_t hz, bool tx) { sh_main.ops->burst(sh_main.chip, hz, tx); }

// ---- LO ----

void db25d_lo_tune(uint32_t hz)
{
	superhet_lo_gpio(&sh_lo, lo_band, hz - IF_HZ < VHF_BELOW_HZ);
	sh_lo.ops->tune(sh_lo.chip, hz);
}

void db25d_lo_off(void) { sh_lo.ops->off(sh_lo.chip); }
void db25d_lo_resume(void) { sh_lo.ops->resume(sh_lo.chip); }
void db25d_lo_aux(bool high) { superhet_lo_gpio(&sh_lo, lo_aux, high); }
