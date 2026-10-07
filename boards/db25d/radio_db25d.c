// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D PRO radio: two BK4819 on one bit-banged 3-wire bus, the superhet's RX paths and analog FM receive
// (docs/spec/hardware-db25d.md 3.2, 4).
//
// Receive on f: the LO chip synthesizes f + 51.55 MHz, the main chip receives and demodulates at the 51.55 MHz IF.
// Transmit is refused for now: the PA power control (DAC B) has no known safe value until the calibration page can be
// read (9). Boards whose chips don't identify as BK4819 (the older FD6818 batches) aren't driven at all.
#include "db25d.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/bk4819/bk4819.h"

#define DAC_PD_CTRL		REG32(DAC_BASE + 0x00)	// bit n = 1 powers channel n down (A 0, B 1, C 2)
#define DAC_DATA_B		REG32(DAC_BASE + 0x0C)	// PA power control, 0 = off
#define DAC_B_OFF		(1u << 1)

#define IF_HZ			51550000u
#define UHF_FROM_HZ		280000000u

static bk4819_t rf_main, rf_lo;
static bool supported;		// both chips answered as BK4819
static uint16_t main_id, lo_id;
static uint32_t rx_hz;
static radio_bw_t rx_bw = RADIO_BW_12K5;
static subtone_t rx_tone;
static bool have_rx;
static uint16_t last_rssi_raw, last_noise_raw;

// ---- 3-wire bus: clock and data shared, one enable per chip ----

static void rf_clk(bool level) { hrc_pin_write(PIN_RF_CLK, level); }
static void rf_data_out(bool level) { hrc_pin_out(PIN_RF_DATA, level); }
static void rf_data_release(void) { hrc_pin_in(PIN_RF_DATA); }
static bool rf_data_in(void) { return hrc_pin_read(PIN_RF_DATA); }
static void cs_main(bool level) { hrc_pin_write(PIN_RF_SEN_MAIN, level); }
static void cs_lo(bool level) { hrc_pin_write(PIN_RF_SEN_LO, level); }

static const bk4819_bus_t bus_main = {
	.cs = cs_main,
	.clk = rf_clk,
	.data_out = rf_data_out,
	.data_release = rf_data_release,
	.data_in = rf_data_in,
	.delay_us = hal_delay_us,
	.crit_enter = hrc_irq_save,
	.crit_exit = hrc_irq_restore,
	.delay_ms = hal_delay_ms,
};

static const bk4819_bus_t bus_lo = {
	.cs = cs_lo,
	.clk = rf_clk,
	.data_out = rf_data_out,
	.data_release = rf_data_release,
	.data_in = rf_data_in,
	.delay_us = hal_delay_us,
	.crit_enter = hrc_irq_save,
	.crit_exit = hrc_irq_restore,
	.delay_ms = hal_delay_ms,
};

// ---- RF paths (3.2) ----

static void tx_paths_off(void)
{
	DAC_DATA_B = 0;
	hrc_pin_write(PIN_TX_SUPPLY_A, false);
	hrc_pin_write(PIN_TX_SUPPLY_B, false);
	hrc_pin_write(PIN_UHF_TX, false);
	hrc_pin_write(PIN_VHF_TX, false);
}

static void rx_paths(uint32_t hz, bool on)
{
	bool uhf = hz >= UHF_FROM_HZ;
	hrc_pin_write(PIN_RX_SUPPLY, on);
	hrc_pin_write(PIN_RX_FRONT_END, on);
	hrc_pin_write(PIN_UHF_RX, on && uhf);
	hrc_pin_write(PIN_VHF_RX, on && !uhf);
}

static bool in_band(uint32_t hz)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		if (hz >= board.bands[i].lo_hz && hz <= board.bands[i].hi_hz) {
			return true;
		}
	}
	return false;
}

static void init(void)
{
	hrc_pin_out(PIN_RF_SEN_MAIN, true);
	hrc_pin_out(PIN_RF_SEN_LO, true);
	hrc_pin_out(PIN_RF_CLK, false);
	hrc_pin_in(PIN_RF_DATA);
	tx_paths_off();
	DAC_PD_CTRL |= DAC_B_OFF;	// the PA control stays powered down while TX is refused
	rx_paths(0, false);

	bk4819_attach(&rf_main, &bus_main);
	bk4819_attach(&rf_lo, &bus_lo);
	main_id = bk4819_read(&rf_main, BK4819_REG_ID);
	lo_id = bk4819_read(&rf_lo, BK4819_REG_ID);
	supported = main_id == BK4819_ID && lo_id == BK4819_ID;
	if (!supported) {
		return;
	}
	bk4819_init_main(&rf_main);
	bk4819_init_lo(&rf_lo, BK4819_LO_PA_MAX);
	bk4819_set_af(&rf_main, BK4819_AF_MUTE);
}

static void set_mode(radio_mode_t mode)
{
	(void)mode;	// FM only for now (CONFIG_DMR is off on this board)
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone_in)
{
	if (!supported || !in_band(hz)) {
		return false;
	}
	rx_hz = hz;
	rx_bw = bw;
	rx_tone = rx_tone_in != NULL ? *rx_tone_in : (subtone_t){ TONE_NONE, 0 };
	have_rx = true;

	tx_paths_off();
	bk4819_set_state(&rf_main, BK4819_STATE_IDLE);
	bk4819_lo_tune(&rf_lo, hz + IF_HZ);
	bk4819_set_freq(&rf_main, IF_HZ);
	bk4819_set_bandwidth(&rf_main, bw == RADIO_BW_25K ? BK4819_BW_25K : BK4819_BW_12K5);
	bk4819_set_rx_ctcss(&rf_main, rx_tone.type == TONE_CTCSS ? rx_tone.value : 0);
	rx_paths(hz, true);
	bk4819_set_state(&rf_main, BK4819_STATE_RX);
	return true;
}

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)hz;
	(void)bw;
	(void)power;
	(void)tx_tone;
	return false;	// no PA calibration yet (module comment)
}

static void tx_off(void)
{
	if (have_rx) {
		rx(rx_hz, rx_bw, &rx_tone);
	}
}

static void status(radio_status_t *st)
{
	if (!supported) {
		st->rssi_dbm = -160;
		st->noise = 0;
		st->carrier = false;
		st->tone_ok = false;
		return;
	}
	uint16_t r67 = bk4819_read(&rf_main, BK4819_REG_RSSI);
	uint16_t r65 = bk4819_read(&rf_main, BK4819_REG_NOISE);
	last_rssi_raw = r67;
	last_noise_raw = r65;
	// At the main chip's input: the front end's gain isn't taken out yet (needs the calibration page)
	st->rssi_dbm = bk4819_rssi_dbm(r67);
	st->noise = (uint8_t)(r65 & 0x7Fu);
	st->carrier = false;
	st->tone_ok = rx_tone.type != TONE_CTCSS || bk4819_ctcss_found(&rf_main);
}

// The amplifier pins follow the DM-32's polarities for the same pins (INF, 3.3): to check on the radio
static void audio(bool on)
{
	if (supported) {
		bk4819_set_af(&rf_main, on ? BK4819_AF_FM : BK4819_AF_MUTE);
	}
	hrc_pin_write(PIN_SPK_SOURCE, !on);
	hrc_pin_write(PIN_AUDIO_A, on);
	hrc_pin_write(PIN_AUDIO_B, on);
}

static void volume(uint8_t percent)
{
	if (supported) {
		bk4819_set_af_gain(&rf_main, (uint8_t)((percent * 15u + 50u) / 100u));
	}
}

void db25d_radio_diag(db25d_radio_diag_t *d)
{
	d->main_id = main_id;
	d->lo_id = lo_id;
	d->rssi_raw = last_rssi_raw;
	d->noise_raw = last_noise_raw;
	d->rf_writes = rf_main.writes + rf_lo.writes;
}

const hal_radio_t db25d_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.volume = volume,
};
