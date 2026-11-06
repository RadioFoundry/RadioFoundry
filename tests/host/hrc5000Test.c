// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C5000 driver (drivers/modem/hrc5000; docs/spec/hardware-md380.md 4) on its own with the shared code, no
// HR_C6000 driver linked: the set-ups' register traffic, and the slot engine on this chip (configuration page 0, the
// registers that differ, the chip-made CRCs), against a fake configuration port that logs every transfer by the page
// on the wire
#include <stdio.h>
#include <string.h>
#include "modem/hrc5000/hrc5000.h"
#include "modem/hrc_common/hrc6000_dmr.h"
#include "dmr/ambe/ambeFec.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

typedef struct {
	uint8_t page, reg, n;
	bool read;
	uint8_t data[32];
} xfer_t;

static xfer_t xlog[8192];
static int nx;
static uint8_t rd[8][256];

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint8_t page = hdr[0] & 7u, reg = hdr[1];
	bool read = (hdr[0] & 0x80u) != 0;
	if (nx < (int)(sizeof(xlog) / sizeof(xlog[0]))) {
		xfer_t *x = &xlog[nx];
		x->page = page;
		x->reg = reg;
		x->n = (uint8_t)len;
		x->read = read;
		if (!read && tx != NULL) {
			memcpy(x->data, tx, len < 32 ? len : 32);
		}
	}
	nx++;
	if (read && rx != NULL) {
		for (uint16_t i = 0; i < len; i++) {
			rx[i] = rd[page][(uint8_t)(reg + i)];
		}
	}
	return true;
}

// What a voice port read returns (the tick-read test sets it); zeros otherwise
static uint8_t voice_port[27];

static bool fake_voice(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	if (!write) {
		memset(buf, 0, len);
		memcpy(buf, voice_port, len < sizeof(voice_port) ? len : sizeof(voice_port));
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, fake_voice };

static int count_writes(uint8_t page, uint8_t reg)
{
	int n = 0;
	for (int i = 0; i < nx; i++) {
		n += !xlog[i].read && xlog[i].page == page && xlog[i].reg == reg;
	}
	return n;
}

// Index of the first write to a register in the log, -1 if none
static int first_write(uint8_t page, uint8_t reg)
{
	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].reg == reg) {
			return i;
		}
	}
	return -1;
}

static uint8_t xlog_byte(uint8_t page, uint8_t reg, int n)
{
	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].reg == reg) {
			return n < 32 ? xlog[i].data[n] : 0;
		}
	}
	return 0;
}

static int count_reads(uint8_t page, uint8_t reg)
{
	int n = 0;
	for (int i = 0; i < nx; i++) {
		n += xlog[i].read && xlog[i].page == page && xlog[i].reg == reg;
	}
	return n;
}

static int count_page(uint8_t page)
{
	int n = 0;
	for (int i = 0; i < nx; i++) {
		n += xlog[i].page == page;
	}
	return n;
}

static int last_write(uint8_t page, uint8_t reg)
{
	int v = -1;
	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].reg == reg) {
			v = xlog[i].data[0];
		}
	}
	return v;
}

// --- Chip description ---

static void test_chip(void)
{
	hrc5000_t c5;
	hrc5000_attach(&c5, &bus, false);
	const hrc_chip_t *c = c5.hrc.chip;
	CHECK(c->cfg_page == 0);
	CHECK(c->id_low_first && !c->mcu_crc && !c->sync_reg && c->mode_per_channel);
	CHECK(c->tx_begin == NULL && c->tx_end == NULL);	// nothing at TX start or end
	CHECK(c->dmr_mode(false) == 0x6B && c->dmr_mode(true) == 0x4F);
	CHECK(c->tc_last_rx);	// 0x52 bit 2: the last received slot's TC (issue 108)
	CHECK(c->crc_len(DMR_BURST_CSBK) == 10 && c->crc_len(DMR_BURST_MBC_HEADER) == 10);
	CHECK(c->crc_len(DMR_BURST_DATA_HEADER) == 10);
	CHECK(c->crc_len(DMR_BURST_RATE_12_DATA) == 0 && c->crc_len(DMR_BURST_MBC_CONT) == 0);
	// Voice 0x50 for A-F: 0x08 | index << 4 | LCSS
	static const uint8_t want[6] = { 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58 };
	for (uint8_t i = 0; i < 6; i++) {
		CHECK((hrc6000_dmr_reg50(DMR_BURST_VOICE, i) | c->voice_lcss(i)) == want[i]);
	}
	CHECK(hrc5000_ref_dac(0x80) == 0x800 && hrc5000_ref_dac(0x00) == 0x600);
	nx = 0;
	CHECK(hrc_write(&c5.hrc, HRC_PAGE_SYS, 0x10, 0x80));	// the configuration page is 0 on the wire
	CHECK(hrc_write(&c5.hrc, HRC_PAGE_AUX, 0x52, 0x08));	// the others are as named
	CHECK(count_writes(0, 0x10) == 1 && count_writes(1, 0x52) == 1 && count_page(4) == 0);
	nx = 0;
	CHECK(hrc_set_dmr_id(&c5.hrc, 0x123456u));		// low byte first
	CHECK(last_write(0, 0x14) == 0x56 && last_write(0, 0x15) == 0x34 && last_write(0, 0x16) == 0x12);
}

// --- Register traffic ---

static void test_modes(void)
{
	hrc5000_t c5;
	hrc5000_attach(&c5, &bus, false);
	nx = 0;
	CHECK(hrc5000_power_on(&c5));	// the clock, then FM receive
	CHECK(count_page(4) == 0);	// nothing on the HR_C6000's configuration page
	// Clock: 49.152 MHz from the 29.4912 MHz crystal (PLLM 40, PLLN 3, PLLDO 3)
	CHECK(last_write(0, 0x0B) == 0x28 && last_write(0, 0x0C) == 0x33 && last_write(0, 0x0A) == 0x00);
	CHECK(last_write(0, 0xBA) == 0x22 && last_write(0, 0xBB) == 0x11 && last_write(0, 0xB9) == 0x33);
	// FM receive
	CHECK(last_write(0, 0x10) == 0x80 && last_write(0, 0x07) == 0x0E && last_write(0, 0x08) == 0x10);
	CHECK(last_write(0, 0x09) == 0x00 && last_write(0, 0x06) == 0x00);
	CHECK(first_write(0, 0x06) > first_write(0, 0x00));	// FM: the vocoder word after the reset release
	CHECK(count_writes(0, 0x00) == 2 && xlog_byte(0, 0x00, 1) == 0xB0 && last_write(0, 0x00) == 0x28);
	CHECK(count_writes(0, 0x11) == 1 && xlog_byte(0, 0x11, 0) == 0x80 && xlog_byte(0, 0x11, 1) == 0x0A);
	CHECK(last_write(0, 0x0D) == 0x8C && last_write(0, 0x0E) == 0x44 && last_write(0, 0x0F) == 0xC8);
	CHECK(last_write(0, 0x25) == 0x0E && last_write(0, 0x26) == 0xFE);
	CHECK(last_write(0, 0x83) == 0xFF && last_write(0, 0x87) == 0x00 && last_write(0, 0x81) == 0x00);
	CHECK(last_write(0, 0x60) == 0x00);
	CHECK(count_writes(0, 0x01) == 0);			// UHF: 0x01 comes from the 0x00 block only
	// None of the HR_C6000's registers
	CHECK(count_writes(0, 0xA1) == 0 && count_writes(0, 0xE0) == 0 && count_writes(0, 0xE2) == 0);
	CHECK(count_writes(0, 0xE4) == 0 && count_writes(0, 0xD1) == 0 && count_writes(0, 0x5F) == 0);
	CHECK(count_writes(0, 0x36) == 0 && count_writes(3, 0x00) == 0);

	// VHF: the receive I/Q swap after the 0x00 block, every full set-up
	hrc5000_t v5;
	hrc5000_attach(&v5, &bus, true);
	nx = 0;
	CHECK(hrc5000_power_on(&v5));
	CHECK(count_writes(0, 0x01) == 1 && last_write(0, 0x01) == 0xF0);

	// FM transmit, then back: only the voice transmit path changes on the way back
	nx = 0;
	CHECK(hrc5000_set_mode_fm_tx(&c5, true, 0x41, 0x42));
	CHECK(last_write(0, 0x45) == 0x41 && last_write(0, 0x46) == 0x42 && last_write(0, 0x35) == 0x3C);
	CHECK(last_write(0, 0x3F) == 0x04 && last_write(0, 0x34) == 0x3C && last_write(0, 0x3E) == 0x08);
	CHECK(last_write(0, 0x37) == 0xC2 && last_write(0, 0x26) == 0xFE && last_write(0, 0x0F) == 0xC8);
	CHECK(last_write(1, 0x50) == 0x00 && last_write(1, 0x51) == 0x00 && last_write(0, 0x60) == 0x80);
	CHECK(count_writes(0, 0x00) == 0);	// no reset
	nx = 0;
	CHECK(hrc5000_set_mode_fm_tx(&c5, false, 1, 2));
	CHECK(last_write(0, 0x35) == 0x30);
	nx = 0;
	CHECK(hrc5000_set_mode_fm_rx(&c5));
	CHECK(nx == 2 && last_write(0, 0x0E) == 0x44 && last_write(0, 0x60) == 0x00);

	// The reference oscillator survives a full set-up (the 0x00 block zeroes 0x04)
	CHECK(hrc5000_set_ref(&c5, 0x70));
	CHECK(last_write(0, 0x47) == 0x0F && last_write(0, 0x04) == 0x0F && last_write(0, 0x48) == 0x00);

	// DMR
	nx = 0;
	CHECK(hrc5000_set_mode_dmr(&c5, 0x41, 0x42, 0xC8));
	CHECK(count_page(4) == 0);
	CHECK(last_write(0, 0x10) == 0x6B && last_write(0, 0xB9) == 0x32 && count_writes(0, 0x10) == 1);
	CHECK(last_write(0, 0x07) == 0x0B && last_write(0, 0x08) == 0xB8 && last_write(0, 0x09) == 0x00);
	CHECK(last_write(0, 0x06) == 0x21 && last_write(0, 0x40) == 0x43 && last_write(0, 0x41) == 0x40);
	CHECK(first_write(0, 0x06) >= 0 && first_write(0, 0x06) < first_write(0, 0x00));	// DMR: before the reset release
	CHECK(count_writes(0, 0x00) == 2 && xlog_byte(0, 0x00, 1) == 0xB0 && last_write(0, 0x00) == 0x28);
	CHECK(count_writes(0, 0x11) == 1 && xlog_byte(0, 0x11, 0) == 0x80 && xlog_byte(0, 0x11, 1) == 0x0A);
	CHECK(count_writes(1, 0x10) == 1 && xlog_byte(1, 0x10, 19) == 0x0F);	// 20 bytes
	CHECK(count_writes(1, 0x30) == 1 && xlog_byte(1, 0x30, 1) == 0x4E);
	CHECK(count_writes(1, 0x40) == 1 && count_writes(1, 0x50) == 1 && xlog_byte(1, 0x50, 4) == 0x00);
	CHECK(last_write(1, 0x52) == 0x08 && last_write(1, 0x53) == 0xEB && last_write(1, 0x54) == 0x78);
	CHECK(last_write(1, 0x45) == 0x1E && last_write(1, 0x37) == 0x50 && last_write(1, 0x35) == 0xFF);
	CHECK(last_write(0, 0x39) == 0x02 && last_write(0, 0x3D) == 0x0A && last_write(0, 0x65) == 0x0A);
	CHECK(last_write(0, 0x1D) == 0xFF && last_write(0, 0x1E) == 0xF1 && last_write(0, 0x64) == 0x00);
	CHECK(last_write(0, 0x0D) == 0x8C && last_write(0, 0x0E) == 0x44 && last_write(0, 0x0F) == 0xC8);
	CHECK(last_write(0, 0x37) == 0xC2 && last_write(0, 0x25) == 0x0E && last_write(0, 0x26) == 0xFD);
	CHECK(last_write(0, 0x45) == 0x41 && last_write(0, 0x46) == 0x42);
	CHECK(last_write(0, 0x81) == 0x19 && last_write(0, 0x85) == 0x00);
	CHECK(last_write(0, 0x47) == 0x0F && last_write(0, 0x04) == 0x0F);	// the reference set again
	CHECK(count_writes(0, 0x21) == 0 && count_writes(0, 0x22) == 0);	// access policy is in the 0x11 block
	CHECK(count_writes(0, 0x3F) == 0 && count_writes(0, 0x35) == 0);	// transmit only
	CHECK(count_writes(1, 0x24) == 1 && count_writes(1, 0x27) == 1);	// no privacy key
	CHECK(count_writes(0, 0xA1) == 0 && count_writes(0, 0xE0) == 0 && count_writes(0, 0xE4) == 0);

	nx = 0;
	CHECK(hrc5000_dmr_path(&c5, true, 0x41, 0x42, 0xC8));
	CHECK(last_write(0, 0x0D) == 0x8C && last_write(0, 0x26) == 0xFE && last_write(0, 0x45) == 0x41 &&
	      last_write(0, 0x46) == 0x42 && last_write(0, 0x35) == 0x1E && last_write(0, 0x3F) == 0x04);
	nx = 0;
	CHECK(hrc5000_dmr_path(&c5, false, 0x41, 0x42, 0xC8));
	CHECK(last_write(0, 0x26) == 0xFD && count_writes(0, 0x0D) == 0 && count_writes(0, 0x45) == 0);

	// A received call that plays: LINEOUT on while the route is open, idle values back at the end, DMR only
	nx = 0;
	CHECK(hrc5000_speaker_voice(&c5, true));
	CHECK(last_write(0, 0x0D) == 0x9C && last_write(0, 0x0E) == 0x4C && last_write(0, 0x37) == 0xC2);
	nx = 0;
	CHECK(hrc5000_dmr_path(&c5, false, 0x41, 0x42, 0xC8));	// a RX/TX switch mid-call keeps them
	CHECK(last_write(0, 0x0D) == 0x9C && last_write(0, 0x0E) == 0x4C);
	nx = 0;
	CHECK(hrc5000_dmr_path(&c5, true, 0x41, 0x42, 0xC8));	// transmit never has them
	CHECK(last_write(0, 0x0D) == 0x8C && last_write(0, 0x0E) == 0x44);
	nx = 0;
	CHECK(hrc5000_speaker_voice(&c5, false));
	CHECK(last_write(0, 0x0D) == 0x8C && last_write(0, 0x0E) == 0x44 && count_writes(0, 0x37) == 0);

	nx = 0;
	CHECK(hrc5000_set_ref(&c5, 0x90));
	CHECK(last_write(0, 0x47) == 0xEF && last_write(0, 0x48) == 0x03);

	// I2S clocks: the 0x11 block leaves 0x36 = 0xF0 (bit 7: CK and WS off); the set-ups never write 0x36 on their own
	nx = 0;
	CHECK(hrc5000_set_mode_dmr(&c5, 0x41, 0x42, 0xC8));
	CHECK(count_writes(0, 0x36) == 0);
	nx = 0;
	CHECK(hrc5000_i2s_clock(&c5, true));
	CHECK(count_writes(0, 0x36) == 1 && last_write(0, 0x36) == 0x00);	// the streaming value (0x70 left the clocks stopped)
	CHECK(hrc5000_i2s_clock(&c5, false));
	CHECK(last_write(0, 0x36) == 0xF0);
	// While the clocks are wanted a full set-up puts them back after its block
	CHECK(hrc5000_i2s_clock(&c5, true));
	nx = 0;
	CHECK(hrc5000_set_mode_dmr(&c5, 0x41, 0x42, 0xC8));
	CHECK(count_writes(0, 0x36) == 1 && last_write(0, 0x36) == 0x00 && first_write(0, 0x36) > first_write(0, 0x11));
	nx = 0;
	CHECK(hrc5000_set_mode_fm_rx(&c5));	// from DMR: a full FM set-up
	CHECK(last_write(0, 0x36) == 0x00);
	CHECK(hrc5000_i2s_clock(&c5, false));
	nx = 0;
	CHECK(hrc5000_set_mode_dmr(&c5, 0x41, 0x42, 0xC8));
	CHECK(count_writes(0, 0x36) == 0);	// off: the block's 0xF0 stands
}

// --- The slot engine on an HR_C5000 ---

static uint32_t t_us;
static bool fake_rf(void *ctx, bool tx) { return true; }
static uint32_t fake_now(void *ctx) { return t_us; }
static void fake_nop(void *ctx) { }
static const hrc6000_dmr_ops_t ops = { fake_rf, fake_now, fake_nop, fake_nop, fake_nop, NULL, NULL, NULL };
static hrc6000_dmr_t eng;
static hrc5000_t c5;

static dmr_tx_burst_t mk(uint8_t kind, uint8_t idx, bool host_crc)
{
	dmr_tx_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.voice_index = idx;
	b.host_crc = host_crc;
	b.len = kind == DMR_BURST_VOICE ? 27 : 12;
	for (int i = 0; i < b.len; i++) {
		b.payload[i] = (uint8_t)(0xA0 + i);
	}
	return b;
}

static void drain(void)
{
	dmr_event_t ev;
	dmr_burst_t b;
	while (hrc6000_dmr_pop(&eng, &ev, &b)) {
	}
}

static void test_engine(void)
{
	memset(rd, 0, sizeof(rd));
	hrc5000_attach(&c5, &bus, false);
	hrc_t *h = hrc5000_hrc(&c5);
	nx = 0;
	CHECK(hrc6000_dmr_enter(h, 0x40, 0x40, 0xC8));
	CHECK(last_write(0, 0x10) == 0x6B && count_page(4) == 0);

	t_us = 1000000;
	hrc6000_dmr_init(&eng, h, &ops);
	hrc6000_dmr_run(&eng);
	dmr_config_t cfg = { .own_id = 0x0A0B0C, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = true };
	nx = 0;
	hrc6000_dmr_configure(&eng, &cfg);
	CHECK(last_write(0, 0x10) == 0x4F);	// repeater channel
	CHECK(last_write(0, 0x14) == 0x0C);	// own ID, low byte first
	cfg.repeater = false;
	hrc6000_dmr_configure(&eng, &cfg);
	CHECK(last_write(0, 0x10) == 0x6B);
	drain();

	// A received report: 0x51 and 0x52 read, never 0x5F
	nx = 0;
	t_us += 30000;
	hrc6000_dmr_isr_timeslot(&eng);
	t_us += 5000;
	rd[0][0x82] = 0x08;
	rd[0][0x51] = 0x12;	// data class, voice LC header
	rd[0][0x52] = 0x10;
	hrc6000_dmr_isr_sys(&eng);
	rd[0][0x82] = 0;
	CHECK(count_reads(0, 0x51) >= 1);
	CHECK(count_reads(0, 0x5F) == 0 && count_reads(4, 0x5F) == 0);
	drain();

	// Voice: 0x50 carries the LCSS; no 0x20-0x22 at TX start; CSBK written as 10 bytes, never with CRC_MCU_Control
	hrc6000_dmr_halt(&eng);
	hrc6000_dmr_run(&eng);
	drain();
	nx = 0;
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_POLITE_CC));
	CHECK(count_writes(0, 0x20) == 0 && count_writes(0, 0x21) == 0 && count_writes(0, 0x22) == 0);
	for (uint8_t i = 0; i < 6; i++) {
		dmr_tx_burst_t v = mk(DMR_BURST_VOICE, i, false);
		CHECK(hrc6000_dmr_tx_queue(&eng, &v));
	}
	dmr_tx_burst_t csbk = mk(DMR_BURST_CSBK, 0, true);
	CHECK(hrc6000_dmr_tx_queue(&eng, &csbk));
	hrc6000_dmr_tx_end(&eng);

	static const uint8_t want50[7] = { 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58, 0x30 };
	uint8_t got50[16];
	int n50 = 0, csbk_len = -1;
	bool r40_crc = false;
	t_us = 5000000;
	for (int k = 0; k < 40; k++) {
		int from = nx;
		hrc6000_dmr_isr_timeslot(&eng);
		for (int i = from; i < nx; i++) {
			const xfer_t *x = &xlog[i];
			if (x->read) {
				continue;
			}
			if (x->page == 0 && x->reg == 0x50 && n50 < 16) {
				got50[n50++] = x->data[0];
			}
			if (x->page == 0 && x->reg == 0x40 && (x->data[0] & 0x08u)) {
				r40_crc = true;
			}
			if (x->page == 2 && x->reg == 0x00 && x->data[0] == 0xA0 && n50 >= 6) {
				csbk_len = x->n;
			}
		}
		t_us += 1500;
		hrc6000_dmr_isr_rftx(&eng);
		t_us += 28500;
		drain();
	}
	CHECK(n50 == 7);
	CHECK(memcmp(got50, want50, sizeof(want50)) == 0);
	CHECK(csbk_len == 10);		// the chip appends the CRC to the first 80 bits
	CHECK(!r40_crc);
	CHECK(count_page(4) == 0);
}


// Received voice on the HR_C5000 (hardware-md380.md 4.8): no report per burst. A call's header starts the decoder
// (0x22 DecodeStart); the voice port is read at every tick, the slot whose reads carry valid AMBE+2 frames is locked,
// its bursts come out A to F, and once the port stops changing the decoder is stopped (DecodeStop)
static void burst(uint8_t seed, bool good)
{
	for (int f = 0; f < 3; f++) {
		uint8_t p[AMBE_PARAM_BITS];
		for (int i = 0; i < AMBE_PARAM_BITS; i++) {
			p[i] = (uint8_t)(((seed * 7 + f * 13 + i) >> 1) & 1u);
		}
		ambeFecEncode(p, voice_port + 9 * f);
		if (!good) {
			voice_port[9 * f] ^= 0x88;	// air bits 0 and 4: C0 bits S0 and S1
			voice_port[9 * f + 1] ^= 0x88;	// S2 and S3: four errors, C0 fails
		}
	}
}

static void test_tick_rx(void)
{
	memset(rd, 0, sizeof(rd));
	memset(voice_port, 0, sizeof(voice_port));
	hrc5000_attach(&c5, &bus, false);
	hrc_t *h = hrc5000_hrc(&c5);
	CHECK(hrc6000_dmr_enter(h, 0x40, 0x40, 0xC8));
	t_us = 5000000;
	hrc6000_dmr_init(&eng, h, &ops);
	hrc6000_dmr_run(&eng);
	dmr_config_t cfg = { .own_id = 0x0A0B0C, .cc = 1, .access = DMR_ACCESS_IMPOLITE };
	hrc6000_dmr_configure(&eng, &cfg);
	drain();

	t_us += 30000;
	hrc6000_dmr_isr_timeslot(&eng);
	// The voice LC header: a data report, which starts the decoder
	nx = 0;
	t_us += 25000;
	rd[0][0x82] = 0x08;
	rd[0][0x51] = 0x12;
	rd[0][0x52] = 0x10;
	hrc6000_dmr_isr_sys(&eng);
	rd[0][0x82] = 0;
	CHECK(last_write(0, 0x22) == 0x27);
	drain();

	// 18 bursts on one slot (the even ticks), noise on the other; then the port keeps the last burst
	int voice = 0, in_order = 0, stop_at = -1;
	uint8_t want = 0;
	for (int k = 0; k < 60; k++) {
		bool ours = (k & 1) == 0 && k < 36;
		if (k < 36) {
			burst((uint8_t)k, ours);
		}
		t_us += 5000;
		nx = 0;
		hrc6000_dmr_isr_timeslot(&eng);
		if (stop_at < 0 && last_write(0, 0x22) == 0x17) {
			stop_at = k;
		}
		t_us += 25000;
		dmr_event_t ev;
		dmr_burst_t b;
		while (hrc6000_dmr_pop(&eng, &ev, &b)) {
			if (ev.kind == DMR_EV_BURST_RX && b.kind == DMR_BURST_VOICE) {
				voice++;
				in_order += b.voice_index == want;
				want = (uint8_t)((want + 1u) % 6u);
				uint8_t p[AMBE_PARAM_BITS];
				ambe_fec_result_t r;
				ambeFecDecode(b.payload, p, &r);
				CHECK(r.errors == 0);
			}
		}
	}
	CHECK(voice == 18);
	CHECK(in_order == 18);
	CHECK(stop_at > 35 && stop_at < 50);	// DecodeStop six frames after the last burst
	hrc6000_dmr_stats_t st;
	hrc6000_dmr_stats(&eng, &st);
	CHECK(st.tr_calls == 1 && st.tr_bursts == 18);
	CHECK(!eng.tr_active);		// the decoder was stopped after the port went quiet
}

// A call that starts with the voice port still holding the last burst of an earlier call, and the headers the sending
// radio repeats for about 600 ms before its voice: the old burst must not lock a slot or count as the call's voice,
// every header while the slot is still unknown starts the decoder again, neither may end the call before its voice,
// and the voice comes out on its headers' slot although the chip has it at the other tick phase
static void test_tick_rx_stale_headers(void)
{
	memset(rd, 0, sizeof(rd));
	memset(voice_port, 0, sizeof(voice_port));
	hrc5000_attach(&c5, &bus, false);
	hrc_t *h = hrc5000_hrc(&c5);
	CHECK(hrc6000_dmr_enter(h, 0x40, 0x40, 0xC8));
	t_us = 5000000;
	hrc6000_dmr_init(&eng, h, &ops);
	hrc6000_dmr_run(&eng);
	dmr_config_t cfg = { .own_id = 0x0A0B0C, .cc = 1, .access = DMR_ACCESS_IMPOLITE };
	hrc6000_dmr_configure(&eng, &cfg);
	drain();
	burst(2, true);		// the earlier call's last burst, still in the port (an even seed: unlike any burst below)

	int voice = 0, in_order = 0, stop_at = -1, starts = 0, same_ts = 0, hdr_ts = 0;
	uint8_t want = 0;
	for (int k = 0; k < 80; k++) {
		bool header = k < 20 && (k & 1) == 0;
		// The HR_C5000 has a burst in its voice RAM one slot later than the report of the same slot: the call's
		// voice is read at the other tick phase than its headers' (on air, 2026-10-19)
		bool ours = k >= 20 && (k & 1) == 1 && k < 57;
		if (ours) {
			burst((uint8_t)k, true);
		} else if (k >= 20 && k < 57) {
			burst((uint8_t)k, false);
		}
		t_us += 5000;
		nx = 0;
		hrc6000_dmr_isr_timeslot(&eng);
		if (stop_at < 0 && last_write(0, 0x22) == 0x17) {
			stop_at = k;
		}
		if (header) {
			nx = 0;
			t_us += 3500;
			rd[0][0x82] = 0x08;
			rd[0][0x51] = 0x12;
			rd[0][0x52] = 0x10;
			hrc6000_dmr_isr_sys(&eng);
			rd[0][0x82] = 0;
			starts += last_write(0, 0x22) == 0x27;
			t_us -= 3500;
		}
		t_us += 25000;
		dmr_event_t ev;
		dmr_burst_t b;
		while (hrc6000_dmr_pop(&eng, &ev, &b)) {
			if (ev.kind == DMR_EV_BURST_RX && b.kind == DMR_BURST_VOICE_LC_HEADER) {
				hdr_ts = ev.ts;
			}
			if (ev.kind == DMR_EV_BURST_RX && b.kind == DMR_BURST_VOICE) {
				voice++;
				same_ts += ev.ts == hdr_ts;
				in_order += b.voice_index == want;
				want = (uint8_t)((want + 1u) % 6u);
			}
		}
	}
	CHECK(voice == 18);
	CHECK(in_order == 18);
	CHECK(hdr_ts != 0 && same_ts == 18);	// the voice is on its header's slot, not the other one
	CHECK(starts == 10);		// DecodeStart at every header while the slot is unknown
	CHECK(stop_at > 56);		// the decoder runs until the voice is over
	hrc6000_dmr_stats_t st;
	hrc6000_dmr_stats(&eng, &st);
	CHECK(st.tr_calls == 1 && st.tr_bursts == 18);
}

// Through a repeater (issue 108): 0x52 bit 2 is the last received slot's TC, 1 = TS2 starting. Receive stays open in
// both slots once locked (closing one freezes the bit), and an aligned start calls tx_slot_found two ticks before its
// first burst, which goes out in the channel's slot
static int slot_found_calls;
static uint32_t slot_found_tick;
static void fake_slot_found(void *ctx) { slot_found_calls++; slot_found_tick = eng.tick; }
static const hrc6000_dmr_ops_t ops_found = { fake_rf, fake_now, fake_nop, fake_nop, fake_nop, NULL,
	fake_slot_found, NULL };

static int8_t rep_ts_starting(uint32_t tick) { return (tick & 1u) ? 2 : 1; }

// The chip as measured: the bit changes only when the slot that just ended was opened for receive. 0x41 set at a tick
// is for the slot starting at the next one, so the slot ending now was set two ticks back
static uint8_t rep_ctl[2];	// 0x41 as left by the last tick, and by the one before
static uint8_t rep_bit;

static void rep_tick(void)
{
	if ((rep_ctl[1] & 0x50u) == 0x50u) {
		rep_bit = rep_ts_starting(eng.tick + 1u) == 2 ? 0x04 : 0x00;	// a TS1 burst just ended: TS2 starts
	}
	rd[0][0x52] = (uint8_t)(0x10 | rep_bit);	// CC 1
	t_us += 30000;
	int from = nx;
	hrc6000_dmr_isr_timeslot(&eng);
	rep_ctl[1] = rep_ctl[0];
	for (int i = from; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == 0 && xlog[i].reg == 0x41) {
			rep_ctl[0] = xlog[i].data[0];
		}
	}
}

static void test_repeater_slot(void)
{
	memset(rd, 0, sizeof(rd));
	hrc5000_attach(&c5, &bus, false);
	hrc_t *h = hrc5000_hrc(&c5);
	CHECK(hrc6000_dmr_enter(h, 0x40, 0x40, 0xC8));
	t_us = 1000000;
	hrc6000_dmr_init(&eng, h, &ops_found);
	hrc6000_dmr_run(&eng);
	dmr_config_t cfg = { .own_id = 0x0A0B0C, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = true };
	hrc6000_dmr_configure(&eng, &cfg);
	CHECK(last_write(0, 0x10) == 0x4F);	// repeated operation; the engine picks the slot
	hrc6000_dmr_rx_lock_slot(&eng, 2);
	drain();

	nx = 0;
	rep_ctl[0] = rep_ctl[1] = 0x50;	// receiving since the restart
	rep_bit = 0;
	int closed = 0;
	for (int k = 0; k < 18; k++) {
		rep_tick();
		closed += (rep_ctl[0] & 0x50u) != 0x50u;
	}
	uint32_t ph = eng.tick & 1u;
	CHECK(eng.phase_ts[ph] == rep_ts_starting(eng.tick) && eng.phase_ts[ph ^ 1u] == 3 - rep_ts_starting(eng.tick));
	CHECK(closed == 0);	// the slot lock leaves the other slot open: closed, the bit would freeze

	slot_found_calls = 0;
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 2, DMR_ACCESS_IMPOLITE));
	dmr_tx_burst_t v = mk(DMR_BURST_VOICE, 0, false);
	CHECK(hrc6000_dmr_tx_queue(&eng, &v));
	uint32_t tx_tick = 0;
	for (int k = 0; k < 8 && tx_tick == 0; k++) {
		int from = nx;
		rep_tick();
		for (int i = from; i < nx; i++) {
			if (!xlog[i].read && xlog[i].page == 0 && xlog[i].reg == 0x41 && xlog[i].data[0] == 0x80) {
				tx_tick = eng.tick;
			}
		}
	}
	CHECK(slot_found_calls == 1);
	CHECK(tx_tick != 0 && tx_tick - slot_found_tick == 2u);
	CHECK(rep_ts_starting(tx_tick + 1u) == 2);	// set up at this tick, sent in the next slot: TS2
	// Three bursts, then receive again: the slot timing carries through the ticks not read during the transmission
	for (uint8_t i = 1; i < 3; i++) {
		dmr_tx_burst_t b = mk(DMR_BURST_VOICE, i, false);
		CHECK(hrc6000_dmr_tx_queue(&eng, &b));
	}
	hrc6000_dmr_tx_end(&eng);
	int lost = 0, wrong = 0;
	for (int k = 0; k < 24; k++) {
		rep_tick();
		uint32_t p = eng.tick & 1u;
		if (eng.tx_state == 0) {	// TXS_IDLE
			lost += eng.phase_ts[p] == 0;
			wrong += eng.phase_ts[p] != 0 && eng.phase_ts[p] != rep_ts_starting(eng.tick);
		}
	}
	CHECK(eng.tx_state == 0);
	CHECK(lost == 0 && wrong == 0);
	drain();
}

// Traffic on both slots of a duplex hotspot (issue 279), the channel on TS1. The chip as measured (4.9, and the tests
// above): receive stays open in both slots, the TC bit alternates, a slot's header is reported in the next slot, and
// its voice is in the port one tick after that, so the port holds a burst of one call or the other at every tick.
// TS1 starts at the even ticks (rep_ts_starting): its headers are reported after the odd ticks and its voice is read
// at the even ones.
// Every voice burst taken must keep its call's slot, and the TS1 call must be heard although TS2 is talking
static void vburst(uint8_t out[27], uint32_t key)
{
	uint32_t x = key * 2654435761u + 0x9E3779B9u;
	for (int f = 0; f < 3; f++) {
		uint8_t p[AMBE_PARAM_BITS];
		for (int i = 0; i < AMBE_PARAM_BITS; i++) {
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			p[i] = (uint8_t)(x & 1u);
		}
		ambeFecEncode(p, out + 9 * f);
	}
}

#define TWO_KEY(ts, n)	((uint32_t)(ts) << 16 | (uint32_t)(n))

typedef struct {
	int hdr_from, hdr_to;		// ticks of the call's headers (none when equal)
	int late_at;			// tick of a late entry report (-1 none)
	int voice_from, voice_to;	// ticks of its voice
} two_call_t;

typedef struct {
	int stale_every;	// one TC read in this many misses its update (0 none)
	int inv_from, inv_to;	// steps whose TC reads name the other slot
	int8_t lock_ts;		// rx_lock_slot: 1 a repeater channel on TS1, 0 both slots (a hotspot, hear all)
	int restart_at;		// step of an rx_restart from the stack (0 none): the slots are forgotten
	int alias;		// page 2 holds a talker alias block (FLCO 4) for each call's superframes 0, 2, 4... (1) or
				// 1, 3, 5... (2) instead of its LC (0 never)
	bool same;		// both calls have the same LC (a group bridged to both slots)
	int talker_at;		// step from which TS1's call has another talker, with no header (0 never)
	int mix_sf;		// TS2's superframe after which page 2 holds a mix of both calls' LCs, once (0 never); below
				// 0, page 2 holds the mix after every F from step -mix_sf on
} two_opts_t;

typedef struct {
	int sent[3];		// voice bursts per call slot
	int heard[3];		// ... taken, with that slot
	int unknown[3];		// ... taken with no slot number
	int wrong[3];		// ... taken with the other slot
	int idx_jump[3];	// ... taken with a burst index out of step with the call's earlier bursts
	int idx_off[3];		// ... with a burst index that isn't the call's (burst n is index n % 6 from its start)
	int lc_own[3];		// ... handed over with an LC, the call's own
	int lc_other[3];	// ... handed over with the other call's LC
	int lc_new[3];		// ... handed over with TS1's next talker's LC
	int alias[3];		// ... handed over with the call's own alias block
	int alias_other[3];	// ... handed over with the other call's alias block
	int lc_mix[3];		// ... handed over with the mixed LC
	int tc_wrong;		// ticks with a wrong slot number (0, unknown, isn't)
	int ctr_wrong;		// voice bursts whose slot counter isn't their report's (the tick before their read)
} two_result_t;

static const two_opts_t *two_o;	// the run's options

// Each call's LC: group call to TG ts from 0x0n0n0n (TS2's for both with same). Page 2 (not per slot) gets a call's LC
// with its header, and its embedded LC (or an alias block) by the tick after its F was read (the sticky LC rule's A
// look): at the other call's F it may hold this one's
static void two_lc(int ts, uint8_t lc[9])
{
	int of = two_o->same ? 2 : ts;
	uint8_t s = (uint8_t)(of * 0x11);
	const uint8_t v[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)of, s, s, s };
	memcpy(lc, v, 9);
}

static void two_lc_new(uint8_t lc[9])
{
	const uint8_t v[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x77, 0x77, 0x77 };
	memcpy(lc, v, 9);
}

static void two_alias(int ts, uint8_t lc[9])
{
	const uint8_t v[9] = { 0x04, 0x00, (uint8_t)ts, 'C', 'A', 'L', 'L', '0', (uint8_t)('0' + ts) };
	memcpy(lc, v, 9);
}

// Neither call's LC: a mix of both (on the bench: source 12087454, TG 3)
static void two_lc_mix(uint8_t lc[9])
{
	const uint8_t v[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xB8, 0x70, 0x9E };
	memcpy(lc, v, 9);
}

// Page 2 after the F of superframe sf of the call on ts, at step k
static void two_page2(int ts, uint32_t sf, int k, uint8_t lc[9])
{
	if ((ts == 2 && two_o->mix_sf > 0 && sf == (uint32_t)two_o->mix_sf) || (two_o->mix_sf < 0 && k >= -two_o->mix_sf)) {
		two_lc_mix(lc);
	} else if (two_o->alias != 0 && sf % 2u == (uint32_t)(two_o->alias - 1)) {
		two_alias(ts, lc);
	} else if (ts == 1 && two_o->talker_at > 0 && k >= two_o->talker_at) {
		two_lc_new(lc);
	} else {
		two_lc(ts, lc);
	}
}

static void two_count_lc(two_result_t *res, int ts, const uint8_t lc[9])
{
	uint8_t v[9];
	two_lc(ts, v);
	if (memcmp(lc, v, 9) == 0) {
		res->lc_own[ts]++;
		return;
	}
	two_lc(3 - ts, v);
	res->lc_other[ts] += memcmp(lc, v, 9) == 0;
	two_lc_new(v);
	res->lc_new[ts] += memcmp(lc, v, 9) == 0;
	two_alias(ts, v);
	res->alias[ts] += memcmp(lc, v, 9) == 0;
	two_alias(3 - ts, v);
	res->alias_other[ts] += memcmp(lc, v, 9) == 0;
	two_lc_mix(v);
	res->lc_mix[ts] += memcmp(lc, v, 9) == 0;
}

// Step k, at the engine's tick t (the one its timeslot interrupt is about to start): is a burst of the call on ts in
// the port (voice, read at t), or its report due (header, after t)
static bool two_voice(const two_call_t *c, int ts, int k, uint32_t t)
{
	return k >= c->voice_from && k < c->voice_to && rep_ts_starting(t) == ts;
}

static bool two_report(const two_call_t *c, int ts, int k, uint32_t t)
{
	return k >= c->hdr_from && k < c->hdr_to && rep_ts_starting(t - 1u) == ts;
}

static two_result_t two_slots(const two_call_t *c1, const two_call_t *c2, int ticks, const two_opts_t *o)
{
	two_o = o;
	memset(rd, 0, sizeof(rd));
	memset(voice_port, 0, sizeof(voice_port));
	hrc5000_attach(&c5, &bus, false);
	hrc_t *h = hrc5000_hrc(&c5);
	CHECK(hrc6000_dmr_enter(h, 0x40, 0x40, 0xC8));
	t_us = 1000000;
	hrc6000_dmr_init(&eng, h, &ops);
	hrc6000_dmr_run(&eng);
	dmr_config_t cfg = { .own_id = 0x0A0B0C, .cc = 1, .access = DMR_ACCESS_IMPOLITE, .repeater = true };
	hrc6000_dmr_configure(&eng, &cfg);
	hrc6000_dmr_rx_lock_slot(&eng, o->lock_ts);
	drain();

	two_result_t res;
	memset(&res, 0, sizeof(res));
	int first_n[3] = { -1, -1, -1 }, first_idx[3] = { 0, 0, 0 };
	rep_ctl[0] = rep_ctl[1] = 0x50;
	rep_bit = 0;
	uint32_t n_burst[3] = { 0, 0, 0 };
	int f_ts = 0;	// the call whose F was in the port at the last step
	for (int k = 0; k < ticks; k++) {
		if (f_ts != 0) {
			two_page2(f_ts, (n_burst[f_ts] - 1u) / 6u, k, rd[2]);
		}
		// The port: this tick's call burst, else what the idle slot leaves (no frame that checks)
		uint32_t t = eng.tick + 1u;
		int vts = two_voice(c1, 1, k, t) ? 1 : two_voice(c2, 2, k, t) ? 2 : 0;
		f_ts = vts != 0 && n_burst[vts] % 6u == 5u ? vts : 0;
		if (vts != 0) {
			vburst(voice_port, TWO_KEY(vts, n_burst[vts]++));
		} else {
			vburst(voice_port, TWO_KEY(3, (uint32_t)k));
			voice_port[0] ^= 0x88;
			voice_port[1] ^= 0x88;
			voice_port[9] ^= 0x88;
			voice_port[10] ^= 0x88;
			voice_port[18] ^= 0x88;
			voice_port[19] ^= 0x88;
		}
		// The TC as rep_tick has it, with a read now and then that missed its update
		bool stale = o->stale_every > 0 && k % o->stale_every == o->stale_every - 1;
		if ((rep_ctl[1] & 0x50u) == 0x50u && !stale) {
			bool inv = k >= o->inv_from && k < o->inv_to;
			rep_bit = (rep_ts_starting(eng.tick + 1u) == 2) != inv ? 0x04 : 0x00;
		}
		rd[0][0x52] = (uint8_t)(0x10 | rep_bit);
		t_us += 30000;
		nx = 0;
		hrc6000_dmr_isr_timeslot(&eng);
		rep_ctl[1] = rep_ctl[0];
		for (int i = 0; i < nx; i++) {
			if (!xlog[i].read && xlog[i].page == 0 && xlog[i].reg == 0x41) {
				rep_ctl[0] = xlog[i].data[0];
			}
		}
		uint32_t ph = eng.tick & 1u;
		res.tc_wrong += eng.phase_ts[ph] != 0 && eng.phase_ts[ph] != rep_ts_starting(eng.tick);
		// Reports: a voice LC header (data) or a late entry, 3.5 ms into the slot after the burst's
		uint8_t flags = 0;
		if (two_report(c1, 1, k, t) || two_report(c2, 2, k, t)) {
			flags = 0x08;
			rd[0][0x51] = 0x12;
			two_lc(two_report(c1, 1, k, t) ? 1 : 2, rd[2]);
		} else if (k == c1->late_at || k == c2->late_at) {
			flags = 0x10;
		}
		if (flags != 0) {
			t_us += 3500;
			rd[0][0x82] = flags;
			hrc6000_dmr_isr_sys(&eng);
			rd[0][0x82] = 0;
			t_us -= 3500;
		}
		if (o->restart_at > 0 && k == o->restart_at) {
			hrc6000_dmr_rx_restart(&eng);
		}
		dmr_event_t ev;
		dmr_burst_t b;
		while (hrc6000_dmr_pop(&eng, &ev, &b)) {
			if (ev.kind != DMR_EV_BURST_RX || b.kind != DMR_BURST_VOICE) {
				continue;
			}
			res.ctr_wrong += ev.arg != eng.tick - 1u;
			// Which call's burst this is
			for (int ts = 1; ts <= 2; ts++) {
				for (uint32_t n = 0; n < n_burst[ts]; n++) {
					uint8_t want[27];
					vburst(want, TWO_KEY(ts, n));
					if (memcmp(want, b.payload, sizeof(want)) == 0) {
						res.heard[ts] += ev.ts == ts;
						if (b.lc_valid) {
							two_count_lc(&res, ts, b.lc);
						}
						res.unknown[ts] += ev.ts == 0;
						res.wrong[ts] += ev.ts != 0 && ev.ts != ts;
						if (first_n[ts] < 0) {
							first_n[ts] = (int)n;
							first_idx[ts] = b.voice_index;
						}
						res.idx_jump[ts] += (b.voice_index + 6 - first_idx[ts]) % 6 !=
							((int)n - first_n[ts]) % 6;
						res.idx_off[ts] += b.voice_index != n % 6u;
					}
				}
			}
		}
	}
	for (int ts = 1; ts <= 2; ts++) {
		res.sent[ts] = (int)n_burst[ts];
	}
	return res;
}

static void two_print(const char *what, const two_result_t *r)
{
	printf("  %s: tc %d wrong, slot counter %d wrong\n", what, r->tc_wrong, r->ctr_wrong);
	for (int ts = 1; ts <= 2; ts++) {
		printf("    TS%d: %d sent, %d heard, %d unknown, %d wrong; index %d out of step, %d not the call's; LC %d own, "
		       "%d other's, %d next talker's, %d mixed; alias %d own, %d other's\n", ts, r->sent[ts],
		       r->heard[ts], r->unknown[ts], r->wrong[ts], r->idx_jump[ts], r->idx_off[ts], r->lc_own[ts],
		       r->lc_other[ts], r->lc_new[ts], r->lc_mix[ts], r->alias[ts], r->alias_other[ts]);
	}
}

#define TWO_CHECK(what, r, cond) do { int f_ = failures; CHECK(cond); if (failures != f_) two_print(what, &(r)); } while (0)

static void test_two_slots(void)
{
	const two_opts_t rpt = { 0, 0, 0, 1, 0, 0, false, 0, 0 };
	const two_call_t none = { 0, 0, -1, 0, 0 };

	// TS2 talking first; a TS1 call starts with its headers while TS2 goes on, and outlasts it
	two_call_t ts2 = { 20, 30, -1, 30, 200 };
	two_call_t ts1 = { 80, 90, -1, 90, 240 };
	two_result_t r = two_slots(&ts1, &ts2, 260, &rpt);
	TWO_CHECK("TS2 first", r, r.tc_wrong == 0 && r.wrong[1] == 0 && r.wrong[2] == 0 && r.ctr_wrong == 0);
	TWO_CHECK("TS2 first", r, r.heard[1] == r.sent[1] && r.idx_off[1] == 0);	// all of TS1, A-F in place

	// TS2 already running when the radio comes in (its late entry report only); TS1's headers come mid-stream.
	// One TC read in seven misses its update
	two_call_t ts2b = { 0, 0, 6, 0, 200 };
	two_call_t ts1b = { 40, 50, -1, 50, 160 };
	const two_opts_t stale = { 7, 0, 0, 1, 0, 0, false, 0, 0 };
	r = two_slots(&ts1b, &ts2b, 200, &stale);
	TWO_CHECK("TS2 running", r, r.tc_wrong == 0 && r.wrong[1] == 0 && r.wrong[2] == 0);
	TWO_CHECK("TS2 running", r, r.heard[1] == r.sent[1] && r.idx_off[1] == 0);

	// One call, joined late on a radio that has heard no header since it started: its slot is the call's too, and
	// every burst read after the late entry report is taken
	two_call_t ts1c = { 0, 0, 30, 24, 120 };
	r = two_slots(&ts1c, &none, 140, &rpt);
	TWO_CHECK("late entry", r, r.tc_wrong == 0 && r.wrong[1] == 0 && r.heard[1] == 45 && r.idx_jump[1] == 0);

	// A receiver restart mid-call (the stack's rx_restart, a color code change): the slots are forgotten until the TC
	// locks again, but nothing names the other slot. The call keeps its lock (bursts with no slot number meanwhile)
	// and its burst index
	two_call_t ts1d = { 20, 30, -1, 30, 200 };
	const two_opts_t blip = { 0, 0, 0, 1, 100, 0, false, 0, 0 };
	r = two_slots(&ts1d, &none, 220, &blip);
	TWO_CHECK("TC blip", r, r.wrong[1] == 0 && r.heard[1] + r.unknown[1] == r.sent[1] && r.unknown[1] > 0);
	TWO_CHECK("TC blip", r, r.idx_off[1] == 0);

	// The TC wrong from the start: TS2's call is taken as TS1 (nothing to tell them apart) until the TC comes right
	// (the lock unwinds one agreement per read: about 60 reads here) and names its phase TS2. Then the lock and
	// TS2's header phase go, and TS1's call, already talking, is found
	two_call_t ts2e = { 10, 20, -1, 20, 240 };
	two_call_t ts1e = { 50, 60, -1, 60, 240 };
	const two_opts_t wrong = { 0, 0, 70, 1, 0, 0, false, 0, 0 };
	r = two_slots(&ts1e, &ts2e, 260, &wrong);
	TWO_CHECK("TC wrong at first", r, r.wrong[1] == 0 && r.heard[1] >= 45 && r.idx_jump[1] == 0);

	// Both slots heard (a hotspot, hear all): a TS2 header with no voice after it (the call's voice lost), then a
	// TS1 call joined by late entry: the late entry frees the read from the header's phase
	const two_opts_t both = { 0, 0, 0, 0, 0, 0, false, 0, 0 };
	two_call_t ts2f = { 20, 30, -1, 0, 0 };
	two_call_t ts1f = { 0, 0, 40, 34, 150 };
	r = two_slots(&ts1f, &ts2f, 160, &both);
	TWO_CHECK("late entry after a header", r, r.tc_wrong == 0 && r.wrong[1] == 0 && r.heard[1] == 55);

	// Both slots heard: TS2 talking with nothing reported, TS1's headers come: the read locks TS1's call, the one its
	// header started, and names it TS1
	two_call_t ts2g = { 0, 0, -1, 0, 200 };
	two_call_t ts1g = { 40, 50, -1, 50, 160 };
	r = two_slots(&ts1g, &ts2g, 200, &both);
	TWO_CHECK("both slots", r, r.tc_wrong == 0 && r.wrong[1] == 0 && r.wrong[2] == 0);
	TWO_CHECK("both slots", r, r.heard[1] == r.sent[1] && r.idx_off[1] == 0);

	// After a quiet period the receiver restarts as the call's single header comes, and the slot is unknown for
	// its first bursts: they are skipped (on a repeater channel nothing is taken without its slot), but counted, so
	// the rest come out with their own index
	two_call_t ts1h = { 20, 22, -1, 22, 120 };
	r = two_slots(&ts1h, &none, 140, &rpt);
	TWO_CHECK("header at a restart", r, r.wrong[1] == 0 && r.unknown[1] == 0 && r.heard[1] >= r.sent[1] - 3);
	TWO_CHECK("header at a restart", r, r.idx_off[1] == 0);

	// The embedded LCs of two calls (page 2 isn't per slot): no voice burst of one call is handed over with the
	// other's LC, which the core takes for a new call on the channel (HIL: the wanted call went quiet with
	// the other call's source shown). TS1 talking first, then TS2 with its headers
	two_call_t ts1i = { 20, 30, -1, 30, 240 };
	two_call_t ts2i = { 80, 90, -1, 90, 240 };
	r = two_slots(&ts1i, &ts2i, 260, &rpt);
	TWO_CHECK("TS1 first, LC", r, r.lc_other[1] == 0 && r.heard[1] == r.sent[1]);
	// TS2 first, as above
	r = two_slots(&ts1, &ts2, 260, &rpt);
	TWO_CHECK("TS2 first, LC", r, r.lc_other[1] == 0);
	// TS2 with its headers, then TS1 joined by late entry: TS1's own LC is found (at the A after its F) and only that
	two_call_t ts2j = { 20, 30, -1, 30, 240 };
	two_call_t ts1j = { 0, 0, 86, 74, 240 };
	r = two_slots(&ts1j, &ts2j, 260, &rpt);
	TWO_CHECK("late entry beside a call, LC", r, r.lc_other[1] == 0 && r.lc_own[1] > 0);
	// Both slots heard (a hotspot, hear all), TS1 first: neither call gets the other's LC
	two_call_t ts1k = { 20, 30, -1, 30, 240 };
	two_call_t ts2k = { 50, 60, -1, 60, 240 };
	r = two_slots(&ts1k, &ts2k, 260, &both);
	TWO_CHECK("both slots, LC", r, r.lc_other[1] == 0 && r.lc_other[2] == 0);
	// One call: its embedded LC is still handed over (nothing to hold back)
	r = two_slots(&ts1c, &none, 140, &rpt);
	TWO_CHECK("late entry, LC", r, r.lc_own[1] > 0);

	// Talker alias blocks in page 2 every other superframe (FLCO 4). One call with its headers: its alias blocks and
	// LCs are handed over
	const two_opts_t alias1 = { 0, 0, 0, 1, 0, 2, false, 0, 0 };
	r = two_slots(&ts1i, &none, 260, &alias1);
	TWO_CHECK("one call, alias", r, r.heard[1] == r.sent[1] && r.alias[1] > 0 && r.lc_own[1] > 0);
	// One call joined late, an alias block in its first superframe: it is never taken for the call's LC
	const two_opts_t alias0 = { 0, 0, 0, 1, 0, 1, false, 0, 0 };
	r = two_slots(&ts1c, &none, 140, &alias0);
	TWO_CHECK("late entry, alias", r, r.heard[1] == 45 && r.lc_own[1] > 0 && r.alias[1] > 0);
	// Two calls with alias blocks: neither call's go with the other's bursts, and the wanted call keeps its LC
	r = two_slots(&ts1i, &ts2i, 260, &alias1);
	TWO_CHECK("two calls, alias", r, r.heard[1] == r.sent[1] && r.lc_own[1] > 0 && r.lc_other[1] == 0);
	TWO_CHECK("two calls, alias", r, r.alias_other[1] == 0);
	r = two_slots(&ts1j, &ts2j, 260, &alias1);
	TWO_CHECK("late entry beside a call, alias", r, r.lc_own[1] > 0 && r.lc_other[1] == 0 && r.alias_other[1] == 0);

	// Both checks pass with the wanted call's own LC: TS2 first, as above
	r = two_slots(&ts1, &ts2, 260, &rpt);
	TWO_CHECK("TS2 first, own LC", r, r.heard[1] == r.sent[1] && r.lc_own[1] > 0);

	// The same LC on both slots (a group bridged to both): TS1's call, with its headers, gets it
	const two_opts_t same = { 0, 0, 0, 1, 0, 0, true, 0, 0 };
	r = two_slots(&ts1, &ts2, 260, &same);
	TWO_CHECK("same LC on both slots", r, r.heard[1] == r.sent[1] && r.lc_own[1] > 0);

	// Another talker on TS1 with no header (its terminator missed), the other slot quiet: the new LC is handed over
	const two_opts_t talker = { 0, 0, 0, 1, 0, 0, false, 150, 0 };
	r = two_slots(&ts1i, &none, 260, &talker);
	TWO_CHECK("next talker, no header", r, r.heard[1] == r.sent[1] && r.lc_own[1] > 0 && r.lc_new[1] > 0);

	// A TS2 call heard, then a new frequency (rx_restart), where TS1 carries the same call joined by late entry: the
	// old frequency's slot LCs are forgotten, so TS1 gets the LC
	two_call_t ts2m = { 20, 30, -1, 30, 100 };
	two_call_t ts1m = { 0, 0, 130, 124, 240 };
	const two_opts_t moved = { 0, 0, 0, 1, 110, 0, true, 0, 0 };
	r = two_slots(&ts1m, &ts2m, 260, &moved);
	TWO_CHECK("new frequency, same call", r, r.heard[1] > 0 && r.lc_own[1] > 0);

	// A TS2 call that ended with no terminator heard, then a TS1 call with its headers whose talker changes with no
	// header and no gap: TS2's call may go on unseen, so the new LC may be a mix of the two and TS1 keeps its own
	two_call_t ts2n = { 20, 30, -1, 30, 60 };
	two_call_t ts1n = { 80, 90, -1, 90, 240 };
	r = two_slots(&ts1n, &ts2n, 260, &talker);
	TWO_CHECK("next talker after the other call", r, r.heard[1] == r.sent[1] && r.lc_own[1] > 0 && r.lc_new[1] == 0);
	// ... and a TS1 call with the ended TS2 call's LC, joined by late entry (the same group re-keyed on the other
	// slot): page 2 shows only that LC at TS1's F, so it is TS1's too after SLOT_LC_ADOPT_READS superframes
	two_call_t ts1o = { 0, 0, 124, 120, 260 };
	const two_opts_t same_both = { 0, 0, 0, 0, 0, 0, true, 0, 0 };
	r = two_slots(&ts1o, &ts2n, 280, &same);
	TWO_CHECK("late entry, the ended call's LC", r, r.heard[1] > 0 && r.lc_own[1] > 0);
	r = two_slots(&ts1o, &ts2n, 280, &same_both);
	TWO_CHECK("late entry, the ended call's LC, both slots", r, r.heard[1] > 0 && r.lc_own[1] > 0);

	// Page 2 once holds neither call's LC (a mix of both), read at TS1's F: a late entry beside the other call, and a
	// call with its headers, don't take it, and still get their own
	const two_opts_t mix = { 0, 0, 0, 1, 0, 0, false, 0, 4 };
	r = two_slots(&ts1j, &ts2j, 260, &mix);
	TWO_CHECK("late entry, a mixed LC", r, r.lc_mix[1] == 0 && r.lc_other[1] == 0 && r.lc_own[1] > 0);
	const two_opts_t mix2 = { 0, 0, 0, 1, 0, 0, false, 0, 9 };
	r = two_slots(&ts1, &ts2, 260, &mix2);
	TWO_CHECK("headers, a mixed LC", r, r.lc_mix[1] == 0 && r.heard[1] == r.sent[1] && r.lc_own[1] > 0);
	// ... and on a repeater channel (TS2's voice unseen, so its busy window lapses) a mix that stays in page 2 from
	// mid-call on (bench): TS1 keeps the LC its headers gave
	const two_opts_t mix3 = { 0, 0, 0, 1, 0, 0, false, 0, -160 };
	two_call_t ts2s = { 20, 30, -1, 30, 330 };
	two_call_t ts1s = { 80, 90, -1, 90, 330 };
	r = two_slots(&ts1s, &ts2s, 340, &mix3);
	TWO_CHECK("headers, a mix that stays", r, r.lc_mix[1] == 0 && r.heard[1] == r.sent[1]);
}

int main(void)
{
	test_chip();
	test_modes();
	test_engine();
	test_tick_rx();
	test_tick_rx_stale_headers();
	test_repeater_slot();
	test_two_slots();
	printf("hrc5000Test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
