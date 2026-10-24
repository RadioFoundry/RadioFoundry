// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 RF side (boards/gd77/rf_gd77.c, c6k_gd77.c; docs/spec/hardware-gd77.md 5): the AT1846S driver with the radio's
// profile against a fake bus (init list, bandwidth table, reg 0x30 per state, the reg 0x05 rule), the band split of
// the path lines, the order of the switch sequences, the DAC0 ramp, the HR_C6000 board values and microphone wiring,
// the calibration's values in their AT1846S and HR_C6000 register fields, and the timeslot edge rule
#include <stdio.h>
#include <string.h>
#include "rf_gd77.h"
#include "c6k_gd77.h"
#include "dmr_gd77.h"
#include "modem/hrc6000/hrc6000.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Fake AT1846S bus ---

typedef struct {
	uint8_t reg;
	uint16_t val;
} wr_t;

static wr_t rf_log[512];
static int rf_n;
static uint32_t rf_waited;

static bool rf_write(void *ctx, uint8_t reg, uint16_t val)
{
	(void)ctx;
	if (rf_n < (int)(sizeof(rf_log) / sizeof(rf_log[0]))) {
		rf_log[rf_n] = (wr_t){ reg, val };
	}
	rf_n++;
	return true;
}

static bool rf_read(void *ctx, uint8_t reg, uint16_t *val)
{
	(void)ctx;
	(void)reg;
	*val = 0;
	return true;
}

static void rf_delay(uint32_t ms) { rf_waited += ms; }

static const at1846s_bus_t rf_bus = { rf_write, rf_read, rf_delay, NULL };

// Last value written to a register on a page, -1 if none (page 0 until the log selects one)
static int rf_last(uint8_t page, uint8_t reg)
{
	int p = 0, v = -1;
	for (int i = 0; i < rf_n; i++) {
		if (rf_log[i].reg == 0x7F) {
			p = rf_log[i].val;
		} else if (rf_log[i].reg == reg && p == page) {
			v = rf_log[i].val;
		}
	}
	return v;
}

static int rf_index(uint8_t reg, uint16_t val)
{
	for (int i = 0; i < rf_n; i++) {
		if (rf_log[i].reg == reg && rf_log[i].val == val) {
			return i;
		}
	}
	return -1;
}

static void test_reg05(void)
{
	CHECK(gd77_at1846s_reg05(136500000) == 0x8543);
	CHECK(gd77_at1846s_reg05(443625000) == 0x8543);
	CHECK(gd77_at1846s_reg05(466375000) == 0x8543);
	CHECK(gd77_at1846s_reg05(170625000) == 0x8643);
	CHECK(gd77_at1846s_reg05(511875000) == 0x8643);
	CHECK(gd77_at1846s_reg05(504634170) == 0x8643);
	CHECK(gd77_at1846s_reg05(136512500) == 0x8763);	// only the exact frequencies
	CHECK(gd77_at1846s_reg05(438125000) == 0x8763);
	CHECK(gd77_at1846s_reg05(145500000) == 0x8763);
}

static void test_at1846s_profile(void)
{
	at1846s_t dev;
	at1846s_attach(&dev, &rf_bus);
	at1846s_set_profile(&dev, &gd77_at1846s);
	rf_n = 0;
	rf_waited = 0;
	CHECK(at1846s_init(&dev));
	// Soft reset, then the radio's list in order: power on, clock mode, the table, the chip's calibration
	CHECK(rf_index(0x30, 0x0001) >= 0);
	int on = rf_index(0x30, 0x0004), clk = rf_index(0x04, 0x0FD0), first = rf_index(0x1F, 0x1000);
	int last = rf_index(0x63, 0x16AD), c1 = rf_index(0x30, 0x40A4), c2 = rf_index(0x30, 0x40A6);
	int done = rf_index(0x30, 0x4006);
	CHECK(rf_index(0x30, 0x0001) < on && on < clk && clk < first && first < last && last < c1 && c1 < c2 && c2 < done);
	CHECK(rf_last(0, 0x4E) == 0x2082);	// written twice, the second value stays
	CHECK(rf_last(0, 0x58) == 0xBCCD);
	CHECK(rf_last(0, 0x44) == 0x06FF);
	// 50 ms after the reset, 50 + 100 + 10 in the calibration, 200 after the bandwidth
	CHECK(rf_waited == 50 + 50 + 100 + 10 + 200);
	// The 12.5 kHz profile with its page 1 rows, and idle at 0x4006
	CHECK(rf_last(0, 0x15) == 0x1100 && rf_last(0, 0x3A) == 0x40C3 && rf_last(0, 0x48) == 0x1DB6);
	CHECK(rf_last(1, 0x06) == 0x0014 && rf_last(1, 0x0F) == 0x3F44 && rf_last(1, 0x12) == 0xE0EB);
	CHECK(rf_last(0, 0x30) == 0x4006);
	CHECK(rf_last(0, 0x59) == -1);	// none of the defaults' table

	// 25 kHz
	CHECK(at1846s_set_bandwidth(&dev, AT1846S_BW_25K));
	CHECK(rf_last(0, 0x32) == 0x7564 && rf_last(0, 0x3A) == 0x44C3 && rf_last(1, 0x0F) == 0x3F84);
	CHECK(rf_last(0, 0x30) == 0x7006);

	// Mode: no tables of its own
	int before = rf_n;
	CHECK(at1846s_set_mode(&dev, AT1846S_MODE_DMR));
	CHECK(at1846s_set_mode(&dev, AT1846S_MODE_FM));
	CHECK(rf_n == before);

	// Tune: idle, reg 0x05 by the rule, the words
	CHECK(at1846s_set_bandwidth(&dev, AT1846S_BW_12K5));
	rf_n = 0;
	CHECK(at1846s_set_freq(&dev, 443625000));
	CHECK(rf_log[0].reg == 0x30 && rf_log[0].val == 0x4006);
	CHECK(rf_log[1].reg == 0x05 && rf_log[1].val == 0x8543);
	CHECK(rf_log[2].reg == 0x29 && rf_log[2].val == 0x006C);
	CHECK(rf_log[3].reg == 0x2A && rf_log[3].val == 0x4E90);	// 443.625 MHz: 7,098,000 = 0x6C4E90
	rf_n = 0;
	CHECK(at1846s_set_freq(&dev, 438125000));
	CHECK(rf_log[1].val == 0x8763);
	CHECK(rf_log[2].val == 0x006A && rf_log[3].val == 0xF6D0);	// 438.125 MHz: 7,010,000

	// Reg 0x30 per state
	CHECK(at1846s_set_state(&dev, AT1846S_STATE_RX) && rf_last(0, 0x30) == 0x4026);
	CHECK(at1846s_set_state(&dev, AT1846S_STATE_FM_TX) && rf_last(0, 0x30) == 0x40C6);
	CHECK(at1846s_ctrl_value(&dev, AT1846S_BW_25K, AT1846S_STATE_RX) == 0x7026);
	CHECK(at1846s_ctrl_value(&dev, AT1846S_BW_12K5, AT1846S_STATE_DMR_TX) == 0x40C6);
	CHECK(at1846s_reg05_value(&dev, 170625000) == 0x8643);

	// Back to the driver's defaults
	at1846s_set_profile(&dev, NULL);
	CHECK(at1846s_ctrl_value(&dev, AT1846S_BW_12K5, AT1846S_STATE_IDLE) == 0x6006);
	CHECK(at1846s_reg05_value(&dev, 136500000) == 0x8763);
}

static void test_bands(void)
{
	gd77_rf_band_t v = gd77_rf_band(145500000), u = gd77_rf_band(438500000);
	CHECK(!v.uhf && v.tx_on == GD77_LINE_TX_VHF && v.rx_on == GD77_LINE_RX_VHF && v.rx_other == GD77_LINE_RX_UHF);
	CHECK(u.uhf && u.tx_on == GD77_LINE_TX_UHF && u.rx_on == GD77_LINE_RX_UHF && u.rx_other == GD77_LINE_RX_VHF);
	CHECK(gd77_rf_band(174000000).uhf == false);
	CHECK(gd77_rf_band(GD77_UHF_SPLIT_HZ).uhf == true);
	CHECK(gd77_rf_band(GD77_UHF_SPLIT_HZ - 1).uhf == false);
}

static int pos(const uint8_t *seq, int n, uint8_t step)
{
	for (int i = 0; i < n; i++) {
		if (seq[i] == step) {
			return i;
		}
	}
	return -1;
}

static void test_sequences(void)
{
	const uint8_t *t = gd77_rf_tx_seq, *r = gd77_rf_rx_seq;
	int nt = (int)sizeof(gd77_rf_tx_seq), nr = (int)sizeof(gd77_rf_rx_seq);
	// Transmit: the receiver path is off before anything else; the chip is tuned and transmitting before the TX path
	// and TX enable; the drive comes up last
	CHECK(pos(t, nt, GD77_RF_RX_PATH_OFF) == 0);
	CHECK(pos(t, nt, GD77_RF_TUNE) < pos(t, nt, GD77_RF_CHIP_TX));
	CHECK(pos(t, nt, GD77_RF_CHIP_TX) < pos(t, nt, GD77_RF_TX_PATH_ON));
	CHECK(pos(t, nt, GD77_RF_TX_PATH_ON) < pos(t, nt, GD77_RF_TX_EN_ON));
	CHECK(pos(t, nt, GD77_RF_RAMP_UP) == nt - 1);
	// Receive: TX enable off and the drive down before the path changes; the receiver path on after the chip receives
	CHECK(pos(r, nr, GD77_RF_TX_EN_OFF) == 0);
	CHECK(pos(r, nr, GD77_RF_RAMP_DOWN) < pos(r, nr, GD77_RF_TX_PATH_OFF));
	CHECK(pos(r, nr, GD77_RF_TX_PATH_OFF) < pos(r, nr, GD77_RF_TUNE));
	CHECK(pos(r, nr, GD77_RF_CHIP_RX) < pos(r, nr, GD77_RF_RX_PATH_ON));
	CHECK(pos(r, nr, GD77_RF_RX_PATH_ON) == nr - 1);
	CHECK(pos(r, nr, GD77_RF_TX_EN_ON) < 0 && pos(r, nr, GD77_RF_RAMP_UP) < 0);
}

static void test_ramp(void)
{
	// Up: rising, ending exactly at the drive (x 16); down: falling to 0
	uint16_t prev = 0;
	for (uint8_t k = 1; k <= GD77_RAMP_STEPS; k++) {
		uint16_t v = gd77_ramp_value(200, k, true);
		CHECK(v >= prev);
		prev = v;
	}
	CHECK(prev == 200u * 16u);
	CHECK(gd77_ramp_value(200, 1, true) == 200u * 16u / 19u);
	prev = 0xFFFF;
	for (uint8_t k = 1; k <= GD77_RAMP_STEPS; k++) {
		uint16_t v = gd77_ramp_value(200, k, false);
		CHECK(v <= prev);
		prev = v;
	}
	CHECK(prev == 0);
	CHECK(gd77_ramp_value(255, GD77_RAMP_STEPS, true) <= 0x0FFF);	// within the 12-bit DAC
	CHECK(gd77_ramp_value(0, 10, true) == 0);
	// Slow near full drive: the last steps up, the first steps down
	CHECK(gd77_ramp_wait_us(1, true) < gd77_ramp_wait_us(GD77_RAMP_STEPS, true));
	CHECK(gd77_ramp_wait_us(1, false) > gd77_ramp_wait_us(GD77_RAMP_STEPS, false));
	uint32_t total = 0;
	for (uint8_t k = 1; k <= GD77_RAMP_STEPS; k++) {
		total += gd77_ramp_wait_us(k, true);
	}
	CHECK(total < 1000u);	// well inside the RF TX interrupt's advance
}

// --- Fake HR_C6000 bus ---

typedef struct {
	uint8_t cmd, reg, val;
	uint16_t len;
} cw_t;

static cw_t c_log[1024];
static int c_n;

static bool c_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	(void)ctx;
	(void)hdr_len;
	if (rx != NULL) {
		memset(rx, 0, len);
	}
	if ((hdr[0] & 0x80u) == 0 && tx != NULL && c_n < 1024) {
		c_log[c_n++] = (cw_t){ hdr[0], hdr[1], tx[0], len };
	}
	return true;
}

static void c_delay(uint32_t v) { (void)v; }

static const hrc_bus_t c_bus = { .transfer = c_transfer, .delay_ms = c_delay, .delay_us = c_delay };

static int c_last(uint8_t page, uint8_t reg)
{
	int v = -1;
	for (int i = 0; i < c_n; i++) {
		if ((c_log[i].cmd & 7u) == page && c_log[i].reg == reg && c_log[i].len == 1) {
			v = c_log[i].val;
		}
	}
	return v;
}

static int c_burst(uint8_t page, uint8_t reg)
{
	for (int i = 0; i < c_n; i++) {
		if ((c_log[i].cmd & 7u) == page && c_log[i].reg == reg && c_log[i].len > 1) {
			return c_log[i].len;
		}
	}
	return -1;
}

static void test_c6k(void)
{
	hrc_t dev;
	hrc6000_attach(&dev, &c_bus);
	CHECK(dev.mic_input == HRC_MIC_LINEIN1 && dev.mic_gain_base == 0x20);	// the driver's defaults
	c_n = 0;
	CHECK(hrc6000_power_on_init_clock(&dev, 7, &hrc6000_clock_12m288));
	CHECK(c_last(4, 0xE4) == 0x27);
	CHECK(gd77_c6k_board_init(&dev));
	CHECK(dev.mic_input == HRC_MIC_MIC_P);
	CHECK(c_last(4, 0xE4) == 0x4B);		// LINEOUT +2 dB, first mic stage 0 dB, gain 11
	CHECK(c_last(4, 0x12) == 0x17 && c_last(4, 0xC0) == 0x0A);
	CHECK(c_last(4, 0x2F) == 0x0B && c_last(4, 0x32) == 0x02 && c_last(4, 0x33) == 0xFF);
	CHECK(c_burst(1, 0x10) == 32 && c_burst(1, 0x30) == 16 && c_burst(1, 0x40) == 7 && c_burst(1, 0x51) == 5);
	for (uint8_t i = 0; i < gd77_c6k_row_count; i++) {	// none of the registers the mode functions own
		uint8_t g = gd77_c6k_rows[i].reg;
		CHECK(g != 0x01 && g != 0x10 && g != 0x26 && g != 0x36 && g != 0x40 && g != 0x41 && g != 0x81 && g != 0xE0 &&
		      g != 0xE4);
	}

	// The microphone on MIC_P wherever the chip turns it on
	hrc6000_fm_tx_t fm = { .uhf = true, .wide = false, .tone = HRC6000_TONE_NONE, .mod2 = 0x80, .mod1 = 0x80,
			       .ref_offset = 0, .mic_gain = 11 };
	c_n = 0;
	CHECK(hrc6000_set_mode_fm_tx(&dev, &fm));
	CHECK(c_last(4, 0xE0) == 0x8B && c_last(4, 0xE4) == 0x4B);
	c_n = 0;
	CHECK(hrc6000_set_mode_fm_rx(&dev));
	CHECK(c_last(4, 0xE0) == 0x89);		// receive: microphone off

	// PCM out: the DAC to the line out and LINEOUT2 (reads return 0 here, so both are written)
	c_n = 0;
	CHECK(gd77_c6k_pcm_out(&dev, true));
	CHECK(c_last(4, 0xE2) == 0x02 && c_last(4, 0xE0) == 0x08 && c_last(4, 0x22) == 0x06);
}

// The calibration's values (cal_gd77.h) in their register fields: the other bits of each register keep their value
static void test_cal_regs(void)
{
	at1846s_t dev;
	at1846s_attach(&dev, &rf_bus);
	at1846s_set_profile(&dev, &gd77_at1846s);
	CHECK(at1846s_init(&dev));
	CHECK(at1846s_write(&dev, 0, 0x0A, 0xFFFF));	// so the kept bits show
	rf_n = 0;
	at1846s_tx_cal_t a = {
		.pa_drv = 0x0A, .mic_pga = 0x1A, .voice_gain = 0x0C, .gain_tx = 0x05, .fm_dev = 0x2C0, .tone_dev = 0x15,
	};
	CHECK(at1846s_set_tx_cal(&dev, &a));
	CHECK(rf_last(0, 0x0A) == (0x803F | (0x0A << 11) | (0x1A << 6)));	// bits 14:11 and 10:6
	CHECK(rf_last(0, 0x41) == ((0x470F & 0xFF80) | 0x0C));		// bits 6:0 of the profile's 0x470F
	CHECK(rf_last(0, 0x44) == 0x05FF);					// bits 11:8 of 0x06FF
	CHECK(rf_last(0, 0x59) == ((0x2C0 << 6) | 0x15));			// deviation 15:6, tone deviation 5:0
	CHECK(at1846s_set_rx_volume(&dev, 0x0E, 0x09));
	CHECK(rf_last(0, 0x44) == 0x05E9);					// bits 7:4 and 3:0, gain_tx kept
	// A value wider than its field is cut to the field and touches nothing else
	a.pa_drv = 0x1F; a.mic_pga = 0x3F; a.voice_gain = 0xFF; a.gain_tx = 0x1F; a.fm_dev = 0xFFFF; a.tone_dev = 0xFF;
	CHECK(at1846s_set_tx_cal(&dev, &a));
	CHECK(rf_last(0, 0x0A) == 0xFFFF && rf_last(0, 0x41) == 0x477F && rf_last(0, 0x44) == 0x0FE9);
	CHECK(rf_last(0, 0x59) == 0xFFFF);
	CHECK(at1846s_set_rx_volume(&dev, 0xFF, 0xFF) && rf_last(0, 0x44) == 0x0FFF);

	// The HR_C6000 two-point bias and offset
	hrc_t c;
	hrc6000_attach(&c, &c_bus);
	c_n = 0;
	CHECK(hrc6000_set_mod_bias(&c, 0x234, 0x7B));
	CHECK(c_last(4, 0x47) == 0x34 && c_last(4, 0x48) == 0x02 && c_last(4, 0x04) == 0x7B);
	CHECK(hrc6000_set_mod_bias(&c, 0x400, 0x80));
	CHECK(c_last(4, 0x47) == 0x00 && c_last(4, 0x48) == 0x04 && c_last(4, 0x04) == 0x80);
}

static void test_slot_edges(void)
{
	CHECK(gd77_slot_edge_new(1000, 0, false));
	CHECK(!gd77_slot_edge_new(1001, 1000, true));		// the other edge of the same pulse
	CHECK(!gd77_slot_edge_new(1000 + GD77_SLOT_MIN_US - 1, 1000, true));
	CHECK(gd77_slot_edge_new(31000, 1000, true));		// the next slot, 30 ms on
	CHECK(gd77_slot_edge_new(5000, 0xFFFFF000u, true));	// across the wrap
}

int main(void)
{
	test_reg05();
	test_at1846s_profile();
	test_bands();
	test_sequences();
	test_ramp();
	test_c6k();
	test_cal_regs();
	test_slot_edges();
	if (failures) {
		printf("gd77RfTest: %d failures\n", failures);
		return 1;
	}
	printf("gd77RfTest: all passed\n");
	return 0;
}
