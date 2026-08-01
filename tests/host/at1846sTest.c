// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AT1846S driver: pure helpers (frequency word, DCS Golay word, CTCSS threshold, tone flags) and the register cache,
// page handling and write sequences against a fake bus
#include <stdio.h>
#include <string.h>
#include "rf/at1846s/at1846s.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Fake bus ---

typedef struct {
	uint8_t reg;
	uint16_t val;
} wr_t;

static wr_t log_wr[512];
static int n_wr;
static uint32_t delay_total;
static uint16_t read_value;
static int n_rd;

static bool fake_write(void *ctx, uint8_t reg, uint16_t val)
{
	if (n_wr < (int)(sizeof(log_wr) / sizeof(log_wr[0]))) {
		log_wr[n_wr].reg = reg;
		log_wr[n_wr].val = val;
	}
	n_wr++;
	return true;
}

static bool fake_read(void *ctx, uint8_t reg, uint16_t *val)
{
	n_rd++;
	*val = read_value;
	return true;
}

static void fake_delay(uint32_t ms)
{
	delay_total += ms;
}

static const at1846s_bus_t bus = { fake_write, fake_read, fake_delay, NULL };

static void log_reset(void)
{
	n_wr = 0;
	n_rd = 0;
	delay_total = 0;
}

// Last value written to a page-0 register in the log, -1 if none
static int last_write(uint8_t reg)
{
	int page = -1, v = -1;
	for (int i = 0; i < n_wr; i++) {
		if (log_wr[i].reg == 0x7F) {
			page = log_wr[i].val;
		} else if (log_wr[i].reg == reg && page != 1) {
			v = log_wr[i].val;
		}
	}
	return v;
}

// Page selected by the last reg 0x7F write, -1 if none
static int last_page(void)
{
	int page = -1;
	for (int i = 0; i < n_wr; i++) {
		if (log_wr[i].reg == 0x7F) {
			page = log_wr[i].val;
		}
	}
	return page;
}

// --- Tests ---

static void test_pure(void)
{
	// Datasheet example: 409.75 MHz -> 0x640960
	CHECK(at1846s_freq_word(409750000) == 0x640960);
	CHECK(at1846s_freq_word(145500000) == 2328000);
	CHECK(at1846s_freq_word(146520031) == at1846s_freq_word(146520000));	// rounds to 62.5 Hz

	// Datasheet example: DCS 023 -> 0x763813
	CHECK(at1846s_dcs_word(023) == 0x763813);
	for (uint16_t code = 0; code < 0x200; code++) {
		uint32_t w = at1846s_dcs_word(code);
		CHECK((w & 0xFFFu) == (0x800u | code));
		CHECK(w < (1u << 23));
	}

	CHECK(at1846s_ctcss_reg(1000) == 10000);
	CHECK(at1846s_ctcss_threshold(10000) == 15);
	CHECK(at1846s_ctcss_threshold(6700) == 18);
	CHECK(at1846s_ctcss_threshold(25000 - 1) == 1);
	CHECK(at1846s_ctcss_threshold(24500) == 1);

	CHECK(at1846s_state_value(AT1846S_BW_12K5, AT1846S_STATE_RX) == 0x6026);
	CHECK(at1846s_state_value(AT1846S_BW_25K, AT1846S_STATE_FM_TX) == 0x7046);
	CHECK(at1846s_state_value(AT1846S_BW_25K, AT1846S_STATE_DMR_TX) == 0x60C6);
	CHECK(at1846s_state_value(AT1846S_BW_12K5, AT1846S_STATE_OFF) == 0x4006);

	CHECK(at1846s_tone_match(0, AT1846S_TONE_NONE));
	CHECK(!at1846s_tone_match(AT1846S_FLAG_SQ, AT1846S_TONE_CTCSS));
	CHECK(at1846s_tone_match(AT1846S_FLAG_CTCSS2, AT1846S_TONE_CTCSS));
	CHECK(at1846s_tone_match(AT1846S_FLAG_DCS, AT1846S_TONE_DCS));
	CHECK(!at1846s_tone_match(AT1846S_FLAG_DCS, AT1846S_TONE_DCS_INV));
	CHECK(at1846s_tone_match(AT1846S_FLAG_DCS_INV, AT1846S_TONE_DCS_INV));
}

static void test_cache(void)
{
	at1846s_t dev;
	at1846s_attach(&dev, &bus);
	log_reset();
	CHECK(at1846s_write(&dev, 0, 0x10, 0x1234));
	CHECK(n_wr == 2 && log_wr[0].reg == 0x7F && log_wr[0].val == 0 && log_wr[1].reg == 0x10);
	CHECK(at1846s_write(&dev, 0, 0x10, 0x1234));
	CHECK(n_wr == 2);					// unchanged: skipped
	CHECK(at1846s_write_force(&dev, 0, 0x10, 0x1234));
	CHECK(n_wr == 3);
	CHECK(at1846s_write(&dev, 1, 0x10, 0x1234));		// same register number on page 1 is a different register
	CHECK(n_wr == 5 && log_wr[3].reg == 0x7F && log_wr[3].val == 1);
	CHECK(at1846s_write(&dev, 0, 0x11, 1));
	CHECK(n_wr == 7 && log_wr[5].reg == 0x7F && log_wr[5].val == 0);

	// modify from the cache, no read
	CHECK(at1846s_modify(&dev, 0, 0x10, 0xFF00, 0x0056));
	CHECK(n_rd == 0 && log_wr[n_wr - 1].val == 0x1256);
	// modify of an unknown register reads it first
	read_value = 0x00C3;
	CHECK(at1846s_modify(&dev, 0, 0x3A, 0xFFE0, 0x0008));
	CHECK(n_rd == 1 && last_write(0x3A) == 0x00C8);
}

static void test_init(void)
{
	at1846s_t dev;
	at1846s_attach(&dev, &bus);
	log_reset();
	CHECK(at1846s_init(&dev));
	CHECK(log_wr[0].reg == 0x7F && log_wr[1].reg == 0x30 && log_wr[1].val == 0x0001);	// soft reset first
	CHECK(log_wr[2].reg == 0x7F);		// page selected again after the reset
	CHECK(log_wr[3].reg == 0x30 && log_wr[3].val == 0x0004);
	CHECK(delay_total == 360);
	CHECK(last_write(0x30) == 0x6006);	// idle at 12.5 kHz
	CHECK(last_write(0x15) == 0x1100);
	CHECK(last_write(0x3A) == 0x00C3);

	// Bandwidth again: nothing to do
	int before = n_wr;
	CHECK(at1846s_set_bandwidth(&dev, AT1846S_BW_12K5));
	CHECK(n_wr == before);
	CHECK(at1846s_set_bandwidth(&dev, AT1846S_BW_25K));
	CHECK(last_write(0x15) == 0x1F00 && last_write(0x30) == 0x7006);
	CHECK(last_page() == 0);		// back on page 0

	// Frequency: idle, 0x05, words, thresholds, all forced
	CHECK(at1846s_set_bandwidth(&dev, AT1846S_BW_12K5));
	log_reset();
	CHECK(at1846s_set_freq(&dev, 409750000));
	CHECK(n_wr == 5);
	CHECK(log_wr[0].reg == 0x30 && log_wr[0].val == 0x6006);
	CHECK(log_wr[1].reg == 0x05 && log_wr[1].val == 0x8763);
	CHECK(log_wr[2].reg == 0x29 && log_wr[2].val == 0x0064);
	CHECK(log_wr[3].reg == 0x2A && log_wr[3].val == 0x0960);
	CHECK(log_wr[4].reg == 0x49 && log_wr[4].val == 0x0C15);
	log_reset();
	CHECK(at1846s_set_freq(&dev, 409750000));
	CHECK(n_wr == 4);			// 0x49 cached, the rest always written
	CHECK(at1846s_set_state(&dev, AT1846S_STATE_FM_TX));
	CHECK(last_write(0x30) == 0x6046);

	// Mode tables
	CHECK(at1846s_set_mode(&dev, AT1846S_MODE_FM));
	CHECK(last_write(0x40) == 0x0030 && last_write(0x58) == 0xBC85);
	CHECK(at1846s_set_mode(&dev, AT1846S_MODE_DMR));
	CHECK(last_write(0x40) == 0x0031 && last_write(0x44) == 0x07FF);

	// Sub-audio
	CHECK(at1846s_set_rx_tone(&dev, AT1846S_TONE_CTCSS, 1000));
	CHECK(last_write(0x4D) == 10000 && last_write(0x5B) == 0x0F0F && (last_write(0x3A) & 0x1F) == 0x08);
	CHECK(at1846s_set_rx_tone(&dev, AT1846S_TONE_DCS_INV, 023));
	CHECK(last_write(0x4A) == 13440 && last_write(0x4B) == 0x0076 && last_write(0x4C) == 0x3813);
	CHECK((last_write(0x3A) & 0x1F) == 0x04);
	CHECK(at1846s_set_rx_tone(&dev, AT1846S_TONE_NONE, 0));
	CHECK((last_write(0x3A) & 0x1F) == 0x03 && last_write(0x4A) == 0);
	read_value = 0x00C0;
	CHECK(at1846s_set_tx_tone(&dev, AT1846S_TONE_CTCSS, 885));
	CHECK(last_write(0x4A) == 8850 && (last_write(0x4E) & 0x0600) == 0x0600);
	CHECK(at1846s_set_tx_tone(&dev, AT1846S_TONE_DCS, 023));
	CHECK(((last_write(0x4E) >> 8) & 0xC7) == 0x04);
	CHECK(at1846s_set_tx_tone(&dev, AT1846S_TONE_NONE, 0));
	CHECK((last_write(0x4E) & 0x0600) == 0 && last_write(0x4A) == 0);
}

int main(void)
{
	test_pure();
	test_cache();
	test_init();
	if (failures) {
		printf("# FAILED %d\n", failures);
		return 1;
	}
	printf("# PASSED at1846sTest\n");
	return 0;
}
