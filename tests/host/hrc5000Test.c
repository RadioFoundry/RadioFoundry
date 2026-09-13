// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C5000 variant of the HR_C6000 driver (docs/spec/hardware-md380.md 4.2-4.5, gotchas 2.6, 5.2, 11.1): the
// configuration page moved to 0, the registers that differ (0x10, 0x14-0x16, 0x21 / 0x22, 0x40 bit 3, 0x50's LCSS,
// 0x5F) and the chip-made CRCs, against a fake configuration port that logs every transfer by the page on the wire
#include <stdio.h>
#include <string.h>
#include "modem/hrc6000/hrc6000_dmr.h"

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

static bool fake_voice(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	if (!write) {
		memset(buf, 0, len);
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc6000_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, fake_voice };

static int count_writes(uint8_t page, uint8_t reg)
{
	int n = 0;
	for (int i = 0; i < nx; i++) {
		n += !xlog[i].read && xlog[i].page == page && xlog[i].reg == reg;
	}
	return n;
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

// --- Pure differences ---

static void test_pure(void)
{
	hrc6000_t c6 = { 0 }, c5 = { 0 };
	hrc6000_attach(&c6, &bus);
	hrc6000_attach_chip(&c5, &bus, HRC6000_CHIP_C5000);
	CHECK(!hrc6000_is_c5000(&c6) && hrc6000_is_c5000(&c5));
	CHECK(hrc6000_page(&c6, HRC6000_PAGE_SYS) == 4 && hrc6000_page(&c5, HRC6000_PAGE_SYS) == 0);
	for (uint8_t p = 1; p <= 3; p++) {
		CHECK(hrc6000_page(&c5, p) == p);	// aux, data and vocoder pages are the same on both
	}
	CHECK(hrc6000_dmr_mode_value(&c6, false) == 0x6E && hrc6000_dmr_mode_value(&c6, true) == 0x6E);
	CHECK(hrc6000_dmr_mode_value(&c5, false) == 0x6B && hrc6000_dmr_mode_value(&c5, true) == 0x4F);
	CHECK(hrc6000_has_mcu_crc(&c6) && !hrc6000_has_mcu_crc(&c5));
	CHECK(hrc6000_has_sync_reg(&c6) && !hrc6000_has_sync_reg(&c5));
	CHECK(hrc6000_tx_start_access(&c6) && !hrc6000_tx_start_access(&c5));
	CHECK(hrc6000_chip_crc_len(&c5, DMR_BURST_CSBK) == 10);
	CHECK(hrc6000_chip_crc_len(&c5, DMR_BURST_MBC_HEADER) == 10);
	CHECK(hrc6000_chip_crc_len(&c5, DMR_BURST_DATA_HEADER) == 10);
	CHECK(hrc6000_chip_crc_len(&c5, DMR_BURST_RATE_12_DATA) == 0);
	CHECK(hrc6000_chip_crc_len(&c5, DMR_BURST_MBC_CONT) == 0);
	CHECK(hrc6000_chip_crc_len(&c6, DMR_BURST_CSBK) == 0);
	// Voice 0x50 for A-F: 0x08 | index << 4 | LCSS (the stock values 08 19 2B 3B 4A 58)
	static const uint8_t want[6] = { 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58 };
	for (uint8_t i = 0; i < 6; i++) {
		CHECK((hrc6000_dmr_reg50(DMR_BURST_VOICE, i) | hrc6000_voice_lcss(&c5, i)) == want[i]);
		CHECK(hrc6000_voice_lcss(&c6, i) == 0);
	}
	CHECK(hrc6000_c5000_ref_dac(0x80) == 0x800 && hrc6000_c5000_ref_dac(0x00) == 0x600);
}

// --- Register traffic ---

static void test_init_and_id(void)
{
	hrc6000_t c5 = { 0 };
	hrc6000_attach_chip(&c5, &bus, HRC6000_CHIP_C5000);
	nx = 0;
	CHECK(hrc6000_power_on_init(&c5, 7));
	CHECK(count_page(4) == 0);			// nothing on the HR_C6000's configuration page
	CHECK(count_writes(0, 0x10) > 0);		// ... it all went to page 0
	CHECK(count_writes(0, 0x5F) == 0);		// not the HR_C6000's sync type register
	CHECK(last_write(0, 0x21) == 0x00 && last_write(0, 0x22) == 0x07);	// set once at init
	CHECK(count_writes(1, 0x10) == 1);		// page 1 blocks unchanged
	CHECK(count_writes(3, 0x00) == 1);		// vocoder flush on page 3

	nx = 0;
	CHECK(hrc6000_set_dmr_id(&c5, 0x123456u));
	CHECK(last_write(0, 0x14) == 0x56 && last_write(0, 0x15) == 0x34 && last_write(0, 0x16) == 0x12);

	hrc6000_t c6 = { 0 };
	hrc6000_attach(&c6, &bus);
	nx = 0;
	CHECK(hrc6000_set_dmr_id(&c6, 0x123456u));
	CHECK(last_write(4, 0x14) == 0x12 && last_write(4, 0x16) == 0x56);
	CHECK(!hrc6000_c5000_dmr_path(&c6, true, 1, 2, 3));	// HR_C5000 only
	nx = 0;
	CHECK(hrc6000_power_on_init(&c6, 7));
	CHECK(count_writes(4, 0x5F) == 2 && count_page(0) == 0);	// the HR_C6000 untouched

	nx = 0;
	CHECK(hrc6000_c5000_dmr_path(&c5, true, 0x41, 0x42, 7));
	CHECK(last_write(0, 0x0D) == 0x8C && last_write(0, 0x26) == 0xFE && last_write(0, 0x45) == 0x41 &&
	      last_write(0, 0x46) == 0x42 && last_write(0, 0x35) == 0x1E && last_write(0, 0x3F) == 0x04);
	nx = 0;
	CHECK(hrc6000_c5000_dmr_path(&c5, false, 0x41, 0x42, 7));
	CHECK(last_write(0, 0x26) == 0xFD && count_writes(0, 0x0D) == 0 && count_writes(0, 0x45) == 0);

	nx = 0;
	CHECK(hrc6000_c5000_set_ref(&c5, 0x70));
	CHECK(last_write(0, 0x47) == 0x0F && last_write(0, 0x04) == 0x0F && last_write(0, 0x48) == 0x00);
	nx = 0;
	CHECK(hrc6000_c5000_set_ref(&c5, 0x90));
	CHECK(last_write(0, 0x47) == 0xEF && last_write(0, 0x48) == 0x03);
}

// --- The slot engine on an HR_C5000 ---

static uint32_t t_us;
static bool fake_rf(void *ctx, bool tx) { return true; }
static uint32_t fake_now(void *ctx) { return t_us; }
static void fake_nop(void *ctx) { }
static const hrc6000_dmr_ops_t ops = { fake_rf, fake_now, fake_nop, fake_nop, fake_nop, NULL, NULL };
static hrc6000_dmr_t eng;
static hrc6000_t c5;

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
	hrc6000_attach_chip(&c5, &bus, HRC6000_CHIP_C5000);
	nx = 0;
	CHECK(hrc6000_dmr_enter(&c5, 0x40, 0x40, 7));
	CHECK(last_write(0, 0x10) == 0x6B && count_page(4) == 0);

	t_us = 1000000;
	hrc6000_dmr_init(&eng, &c5, &ops);
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

int main(void)
{
	test_pure();
	test_init_and_id();
	test_engine();
	printf("hrc5000Test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
