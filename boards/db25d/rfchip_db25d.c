// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D RF chips (rfchip_db25d.h; docs/spec/hardware-db25d.md 4.2, 4.8). Both chips sit on one bit-banged 3-wire bus
// with an enable each; every part answers the same frame (8-bit address, bit 7 = read, 16 data bits), so the frames
// go through the BK4819 driver's bus code whatever the chip is, and the FD6818 and AT1141 drivers get them as plain
// register reads and writes.
#include "rfchip_db25d.h"
#include "db25d.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/bk4819/bk4819.h"
#include "rf/fd6818/fd6818sh.h"
#include "rf/at1141/at1141.h"

#define IF_HZ		51550000u
#define VHF_BELOW_HZ	280000000u	// the band split of the RF paths
#define GPIO_BAND	3u		// on the FD6818 and AT1141: high for VHF (main: the TX band, LO: the RX band)
#define ID_TRIES	3u
#define ID_RESET_MS	100u		// stock: soft reset, this long, release, this long, read again

typedef enum { CHIP_NONE, CHIP_BK4819, CHIP_FD6818, CHIP_7813, CHIP_AT1141 } chip_t;

static bk4819_t bus_main_dev, bus_lo_dev;	// the frames; with a BK4819 the driver itself
static fd6818sh_t fd_main, fd_lo;
static at1141_t at_lo;
static chip_t main_chip, lo_chip;
static uint16_t main_id, lo_id;

// ---- 3-wire bus: clock and data shared, one enable per chip ----

static void rf_clk(bool level) { hrc_pin_write(PIN_RF_CLK, level); }
static void rf_data_out(bool level) { hrc_pin_out(PIN_RF_DATA, level); }
static void rf_data_release(void) { hrc_pin_in(PIN_RF_DATA); }
static bool rf_data_in(void) { return hrc_pin_read(PIN_RF_DATA); }
static void cs_main(bool level) { hrc_pin_write(PIN_RF_SEN_MAIN, level); }
static void cs_lo(bool level) { hrc_pin_write(PIN_RF_SEN_LO, level); }

static const bk4819_bus_t bus_main = {
	.cs = cs_main, .clk = rf_clk, .data_out = rf_data_out, .data_release = rf_data_release, .data_in = rf_data_in,
	.delay_us = hal_delay_us, .crit_enter = hrc_irq_save, .crit_exit = hrc_irq_restore, .delay_ms = hal_delay_ms,
};

static const bk4819_bus_t bus_lo = {
	.cs = cs_lo, .clk = rf_clk, .data_out = rf_data_out, .data_release = rf_data_release, .data_in = rf_data_in,
	.delay_us = hal_delay_us, .crit_enter = hrc_irq_save, .crit_exit = hrc_irq_restore, .delay_ms = hal_delay_ms,
};

static void main_wr(uint8_t reg, uint16_t val) { bk4819_write(&bus_main_dev, reg, val); }
static uint16_t main_rd(uint8_t reg) { return bk4819_read(&bus_main_dev, reg); }
static void lo_wr(uint8_t reg, uint16_t val) { bk4819_write(&bus_lo_dev, reg, val); }
static uint16_t lo_rd(uint8_t reg) { return bk4819_read(&bus_lo_dev, reg); }

static const fd6818sh_bus_t fd_bus_main = { .write = main_wr, .read = main_rd, .delay_ms = hal_delay_ms };
static const fd6818sh_bus_t fd_bus_lo = { .write = lo_wr, .read = lo_rd, .delay_ms = hal_delay_ms };
static const at1141_bus_t at_bus_lo = { .write = lo_wr, .read = lo_rd, .delay_ms = hal_delay_ms };

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
static uint16_t identify(bk4819_t *dev, bool lo, chip_t *k)
{
	uint16_t id = 0;
	for (unsigned i = 0; i < ID_TRIES; i++) {
		id = bk4819_read(dev, BK4819_REG_ID);
		*k = kind(id, lo);
		if (*k != CHIP_NONE) {
			break;
		}
		bk4819_write(dev, FD6818SH_REG_PAGE, 0);
		bk4819_write(dev, BK4819_REG_ID, 0x8000);
		hal_delay_ms(ID_RESET_MS);
		bk4819_write(dev, BK4819_REG_ID, 0x0000);
		hal_delay_ms(ID_RESET_MS);
	}
	return id;
}

bool db25d_rf_init(void)
{
	hrc_pin_out(PIN_RF_SEN_MAIN, true);
	hrc_pin_out(PIN_RF_SEN_LO, true);
	hrc_pin_out(PIN_RF_CLK, false);
	hrc_pin_in(PIN_RF_DATA);
	bk4819_attach(&bus_main_dev, &bus_main);
	bk4819_attach(&bus_lo_dev, &bus_lo);
	// The LO is above the signal (f + 51.55 MHz): DCS arrives inverted (4.7)
	bus_main_dev.rx_inverted = true;
	main_id = identify(&bus_main_dev, false, &main_chip);
	lo_id = identify(&bus_lo_dev, true, &lo_chip);
	if (main_chip == CHIP_NONE || lo_chip == CHIP_NONE) {
		return false;
	}

	if (main_chip == CHIP_BK4819) {
		bk4819_init_main(&bus_main_dev);
		bk4819_write(&bus_main_dev, BK4819_REG_PA, BK4819_PA_MAX);	// stock: once at detection, never changed
	} else {
		fd6818sh_attach(&fd_main, &fd_bus_main, FD6818SH_MAIN);
		fd_main.rx_inverted = true;
		fd6818sh_init_main(&fd_main);
	}
	switch (lo_chip) {
	case CHIP_BK4819:
		bk4819_init_lo(&bus_lo_dev, BK4819_LO_PA_MAX);
		break;
	case CHIP_AT1141:
		at1141_attach(&at_lo, &at_bus_lo);
		at1141_init(&at_lo);
		at1141_disable(&at_lo);
		break;
	default:
		fd6818sh_attach(&fd_lo, &fd_bus_lo, lo_chip == CHIP_7813 ? FD6818SH_LO_7813 : FD6818SH_LO);
		fd6818sh_init_lo(&fd_lo);
		break;
	}
	if (main_chip == CHIP_FD6818) {
		hrc_pin_in(PIN_RF_CSS);
	}
	if (main_chip == CHIP_BK4819) {
		bk4819_analog_mode(&bus_main_dev);
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
	return bus_main_dev.writes + bus_lo_dev.writes;
}

static bool fd(void)
{
	return main_chip == CHIP_FD6818;
}

// ---- Main transceiver ----

static bk4819_tone_t bk_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS: return BK4819_TONE_CTCSS;
	case TONE_DCS: return BK4819_TONE_DCS;
	case TONE_DCS_INV: return BK4819_TONE_DCS_INV;
	default: return BK4819_TONE_NONE;
	}
}

static fd6818sh_tone_t fd_tone(uint8_t type)
{
	switch (type) {
	case TONE_CTCSS: return FD6818SH_TONE_CTCSS;
	case TONE_DCS: return FD6818SH_TONE_DCS;
	case TONE_DCS_INV: return FD6818SH_TONE_DCS_INV;
	default: return FD6818SH_TONE_NONE;
	}
}

void db25d_main_idle(void)
{
	if (fd()) {
		fd6818sh_set_state(&fd_main, FD6818SH_STATE_IDLE);
	} else {
		bk4819_set_state(&bus_main_dev, BK4819_STATE_IDLE);
	}
}

void db25d_main_rx_state(void)
{
	if (fd()) {
		fd6818sh_set_state(&fd_main, FD6818SH_STATE_RX);
	} else {
		bk4819_set_state(&bus_main_dev, BK4819_STATE_RX);
	}
}

void db25d_main_to_if(void)
{
	if (fd()) {
		fd6818sh_set_state(&fd_main, FD6818SH_STATE_IDLE);
		fd6818sh_tune(&fd_main, IF_HZ);
	} else {
		bk4819_set_freq(&bus_main_dev, IF_HZ);
	}
}

void db25d_main_bandwidth(radio_bw_t bw)
{
	if (fd()) {
		fd6818sh_set_bandwidth(&fd_main, bw == RADIO_BW_25K);
	} else {
		bk4819_set_bandwidth(&bus_main_dev, bw == RADIO_BW_25K ? BK4819_BW_25K : BK4819_BW_12K5);
	}
}

void db25d_main_rx_tone(uint8_t type, uint16_t value)
{
	if (fd()) {
		fd6818sh_set_rx_tone(&fd_main, fd_tone(type), value);
	} else {
		bk4819_set_rx_tone(&bus_main_dev, bk_tone(type), value);
	}
}

bool db25d_main_tone_heard(uint16_t r0c, uint8_t type)
{
	if (fd()) {
		// The FD6818 has no status bit for it: its GPIO0 carries the compare result to PTB27
		return type == TONE_NONE || hrc_pin_read(PIN_RF_CSS);
	}
	return bk4819_heard(&bus_main_dev, r0c, bk_tone(type));
}

void db25d_main_af(db25d_af_t af)
{
	if (fd()) {
		fd6818sh_set_af(&fd_main, af == DB25D_AF_FM ? FD6818SH_AF_RX : af == DB25D_AF_DMR ? FD6818SH_AF_DMR : FD6818SH_AF_MUTE);
	} else {
		bk4819_set_af(&bus_main_dev, af == DB25D_AF_FM ? BK4819_AF_FM : af == DB25D_AF_DMR ? BK4819_AF_DMR : BK4819_AF_MUTE);
	}
}

void db25d_main_af_gain(uint8_t gain)
{
	if (fd()) {
		fd6818sh_set_af_gain(&fd_main, gain);
	} else {
		bk4819_set_af_gain(&bus_main_dev, gain);
	}
}

void db25d_main_status(uint16_t *r67, uint16_t *r65, uint16_t *r0c)
{
	*r67 = bk4819_read(&bus_main_dev, BK4819_REG_RSSI);
	*r65 = bk4819_read(&bus_main_dev, BK4819_REG_NOISE);
	*r0c = bk4819_read(&bus_main_dev, BK4819_REG_CSS_STATUS);
}

void db25d_main_tx_prepare(uint8_t type, uint16_t value, uint32_t hz, radio_bw_t bw)
{
	(void)bw;	// neither chip's deviation is switched by bandwidth: the FD6818's goes by band and sub-audio
	if (fd()) {
		fd6818sh_gpio(&fd_main, GPIO_BAND, hz < VHF_BELOW_HZ);
		fd6818sh_tx_prepare(&fd_main, fd_tone(type), value, hz, db25d_cal_pa_gain(hz));
	} else {
		// The BK4819's: the stock words (4.4)
		bk4819_tx_prepare(&bus_main_dev, bk_tone(type), value);
	}
}

void db25d_main_tx_tune(uint32_t hz)
{
	if (fd()) {
		fd6818sh_tx_tune(&fd_main, hz);
	} else {
		bk4819_tx_tune(&bus_main_dev, hz);
	}
}

void db25d_main_tx_end(void)
{
	if (fd()) {
		fd6818sh_tx_end(&fd_main);
	} else {
		bk4819_tx_end(&bus_main_dev);
	}
}

bool db25d_main_tail(bool on)
{
	if (fd()) {
		return fd6818sh_tx_tail(&fd_main, on);
	}
	if (on) {
		return bk4819_tx_tail_start(&bus_main_dev);
	}
	bk4819_tx_tail_end(&bus_main_dev);
	return true;
}

void db25d_main_tone(uint16_t hz, uint8_t gain)
{
	if (fd()) {
		fd6818sh_tone_start(&fd_main, hz, gain);
	} else {
		bk4819_tone_start(&bus_main_dev, hz, gain);
	}
}

bool db25d_main_tone_on(void)
{
	return fd() ? fd_main.tone_hz != 0 : bus_main_dev.tone_hz != 0;
}

void db25d_main_tone_key(bool down)
{
	if (fd()) {
		fd6818sh_tone_key(&fd_main, down);
	} else {
		bk4819_tone_key(&bus_main_dev, down);
	}
}

void db25d_main_dtmf(uint16_t low_hz, uint16_t high_hz)
{
	if (fd()) {
		fd6818sh_dtmf_start(&fd_main, low_hz, high_hz);
	} else {
		bk4819_dtmf_start(&bus_main_dev, low_hz, high_hz);
	}
}

void db25d_main_dmr_enter(uint8_t af_gain)
{
	if (fd()) {
		fd6818sh_dmr_mode(&fd_main);
		fd6818sh_set_af_gain(&fd_main, af_gain);
	} else {
		bk4819_dmr_rx_enter(&bus_main_dev);
		bk4819_set_af_gain(&bus_main_dev, af_gain);
	}
}

void db25d_main_dmr_leave(void)
{
	if (fd()) {
		fd6818sh_analog_mode(&fd_main);
	} else {
		bk4819_dmr_rx_end(&bus_main_dev);
	}
}

void db25d_main_dmr_tx_prepare(uint32_t hz)
{
	if (fd()) {
		fd6818sh_gpio(&fd_main, GPIO_BAND, hz < VHF_BELOW_HZ);
		fd6818sh_dmr_tx_prepare(&fd_main, db25d_cal_pa_gain(hz));
	} else {
		bk4819_dmr_tx_prepare(&bus_main_dev);
	}
}

void db25d_main_burst(uint32_t hz, bool tx)
{
	if (fd()) {
		fd6818sh_burst_tune(&fd_main, hz, tx);
	} else {
		bk4819_burst_tune(&bus_main_dev, hz, tx ? BK4819_STATE_TX_MAIN : BK4819_STATE_RX);
	}
}

// ---- LO ----

void db25d_lo_tune(uint32_t hz)
{
	bool vhf = hz - IF_HZ < VHF_BELOW_HZ;
	switch (lo_chip) {
	case CHIP_BK4819:
		bk4819_lo_tune(&bus_lo_dev, hz);
		break;
	case CHIP_AT1141:
		at1141_gpio(&at_lo, GPIO_BAND, vhf);
		if (at_lo.lo_hz != hz) {
			at1141_tune(&at_lo, hz);	// enables too
		} else {
			at1141_enable(&at_lo);
		}
		break;
	case CHIP_FD6818:
	case CHIP_7813:
		fd6818sh_gpio(&fd_lo, GPIO_BAND, vhf);
		fd6818sh_lo_tune(&fd_lo, hz);
		break;
	default:
		break;
	}
}

void db25d_lo_off(void)
{
	switch (lo_chip) {
	case CHIP_BK4819:
		bk4819_lo_off(&bus_lo_dev);
		break;
	case CHIP_AT1141:
		at1141_disable(&at_lo);	// stock TX: standby only
		break;
	case CHIP_FD6818:
	case CHIP_7813:
		fd6818sh_lo_off(&fd_lo);
		break;
	default:
		break;
	}
}

void db25d_lo_resume(void)
{
	switch (lo_chip) {
	case CHIP_BK4819:
		bk4819_lo_resume(&bus_lo_dev);
		break;
	case CHIP_AT1141:
		at1141_enable(&at_lo);	// stock RX: no wait, no retune
		break;
	case CHIP_FD6818:
	case CHIP_7813:
		fd6818sh_lo_resume(&fd_lo);
		break;
	default:
		break;
	}
}
