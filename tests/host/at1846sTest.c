// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AT1846S driver: pure helpers (frequency word, CTCSS threshold, tone flags) and the register cache,
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
	// D023's Golay word 0x763813 bit-reversed: the chip sends and compares from bit 22 (found on air: the guide's
	// unreversed example went out as D023 read backwards)
	CHECK(last_write(0x4A) == 13440 && last_write(0x4B) == 0x0064 && last_write(0x4C) == 0x0E37);
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

// TX DCS on the chip (the words themselves: subaudioTest)
static void test_dcs(void)
{
	// TX DCS on the chip: words in 0x4B / 0x4C, DCS selected, cdcss_sel 23-bit with the polarity bit
	at1846s_t dev;
	at1846s_attach(&dev, &bus);
	log_reset();
	read_value = 0x00FF;		// whatever the chip holds in 0x4E's low bits
	CHECK(at1846s_set_tx_tone(&dev, AT1846S_TONE_DCS, 0754));
	CHECK(last_write(0x4A) == 13440 && last_write(0x4D) == 0);
	CHECK(last_write(0x4B) == 0x001B && last_write(0x4C) == 0xCF82);	// 0x20F9EC reversed
	CHECK((last_write(0x4E) & 0x07C0) == 0x0480 && (last_write(0x4E) & 0x003F) == 0x003F);
	CHECK(at1846s_set_tx_tone(&dev, AT1846S_TONE_DCS_INV, 0754));
	CHECK((last_write(0x4E) & 0x07C0) == 0x0580);
	CHECK(at1846s_set_tx_tone(&dev, AT1846S_TONE_CTCSS, 1000));
	CHECK((last_write(0x4E) & 0x0600) == 0x0600 && last_write(0x4B) == 0 && last_write(0x4C) == 0);
}

// CTCSS register values and the match thresholds over the standard range
static void test_ctcss(void)
{
	static const struct { uint16_t hz_x10, reg; uint8_t th; } t[] = {
		{ 670, 6700, 18 }, { 885, 8850, 16 }, { 1000, 10000, 15 }, { 1318, 13180, 11 }, { 1622, 16220, 8 },
		{ 2035, 20350, 4 }, { 2336, 23360, 1 }, { 2400, 24000, 1 }, { 2418, 24180, 1 }, { 2541, 25410, 1 },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		CHECK(at1846s_ctcss_reg(t[i].hz_x10) == t[i].reg);
		CHECK(at1846s_ctcss_threshold(t[i].reg) == t[i].th);
	}
	at1846s_t dev;
	at1846s_attach(&dev, &bus);
	read_value = 0x00C3;
	log_reset();
	CHECK(at1846s_set_rx_tone(&dev, AT1846S_TONE_CTCSS, 2541));
	CHECK(last_write(0x4D) == 25410 && last_write(0x5B) == 0x0101 && last_write(0x4A) == 0);
	CHECK(at1846s_set_tx_tone(&dev, AT1846S_TONE_CTCSS, 670));
	CHECK(last_write(0x4A) == 6700 && last_write(0x4D) == 0);
}

// The whole 25 kHz table in order (5.4), page switches included, then idle at 25 kHz; and back to 12.5 kHz
static void test_bw_sequence(void)
{
	static const wr_t want25[] = {
		{ 0x7F, 0 }, { 0x3A, 0x40CB }, { 0x15, 0x1F00 }, { 0x32, 0x7564 }, { 0x3A, 0x00C3 }, { 0x59, 0x0B90 },	// 0x59 as at 12.5 kHz (gotchas 3.8)
		{ 0x3F, 0x29D1 }, { 0x3C, 0x1B34 }, { 0x48, 0x1E38 }, { 0x60, 0x0F17 }, { 0x62, 0x3767 }, { 0x65, 0x248A },
		{ 0x66, 0xFF2E }, { 0x7F, 1 }, { 0x06, 0x0024 }, { 0x07, 0x0214 }, { 0x08, 0x0224 }, { 0x09, 0x0314 },
		{ 0x0A, 0x0324 }, { 0x0B, 0x0344 }, { 0x0C, 0x0384 }, { 0x0D, 0x1384 }, { 0x0E, 0x1B84 }, { 0x0F, 0x3F84 },
		{ 0x12, 0xE0EB }, { 0x7F, 0 }, { 0x30, 0x7006 },
	};
	at1846s_t dev;
	at1846s_attach(&dev, &bus);
	log_reset();
	CHECK(at1846s_set_bandwidth(&dev, AT1846S_BW_25K));
	CHECK(n_wr == (int)(sizeof(want25) / sizeof(want25[0])));
	for (int i = 0; i < n_wr && i < (int)(sizeof(want25) / sizeof(want25[0])); i++) {
		if (log_wr[i].reg != want25[i].reg || log_wr[i].val != want25[i].val) {
			printf("  25k write %d: %02X=%04X, want %02X=%04X\n", i, log_wr[i].reg, log_wr[i].val, want25[i].reg,
				want25[i].val);
			CHECK(0);
		}
	}
	// 25 kHz RX and TX states
	CHECK(at1846s_set_state(&dev, AT1846S_STATE_RX) && last_write(0x30) == 0x7026);
	CHECK(at1846s_set_state(&dev, AT1846S_STATE_FM_TX) && last_write(0x30) == 0x7046);
	log_reset();
	CHECK(at1846s_set_freq(&dev, 461862500));
	CHECK(log_wr[0].reg == 0x30 && log_wr[0].val == 0x7006);	// idle keeps the 25 kHz bits
	CHECK(at1846s_freq_word(461862500) == 7389800);

	// Back to 12.5 kHz: the rows that differ, idle at 12.5 kHz
	log_reset();
	CHECK(at1846s_set_bandwidth(&dev, AT1846S_BW_12K5));
	CHECK(last_write(0x15) == 0x1100 && last_write(0x32) == 0x4495 && last_write(0x48) == 0x19B1);
	CHECK(last_write(0x59) == -1 && last_write(0x30) == 0x6006);	// 0x59 is the same at both bandwidths, not rewritten
	CHECK(last_page() == 0);
	// The mode table must be sent again after a bandwidth change (it shares registers)
	log_reset();
	CHECK(at1846s_set_mode(&dev, AT1846S_MODE_FM));
	CHECK(n_wr > 0 && last_write(0x58) == 0xBC85);
}

int main(void)
{
	test_pure();
	test_cache();
	test_init();
	test_dcs();
	test_ctcss();
	test_bw_sequence();
	if (failures) {
		printf("# FAILED %d\n", failures);
		return 1;
	}
	printf("# PASSED at1846sTest\n");
	return 0;
}
