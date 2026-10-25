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

int main(void)
{
	test_chip();
	test_modes();
	test_engine();
	test_tick_rx();
	test_tick_rx_stale_headers();
	test_repeater_slot();
	printf("hrc5000Test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
