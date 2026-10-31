// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// QA4818-class transceiver driver: the pure helpers (frequency word, superhet plan, sub-bands, CTCSS detector
// thresholds, drive word, RSSI) and the register sequences on a fake register bus (docs/spec/hardware-d578uv.md 5)
#include <stdio.h>
#include <string.h>
#include "rf/qa4818/qa4818.h"
#include "rf/subaudio/subaudio.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- A fake chip behind an rfbus_t: logs writes, answers reads from its registers ---

typedef struct {
	uint8_t reg;
	uint16_t val;
} frame_t;

typedef struct {
	rfbus_t bus;	// first
	frame_t log[512];
	int n;
	uint16_t regs[128];
	int reads;
} fake_t;

static int delays_ms;

static void fake_write(rfbus_t *bus, uint8_t reg, uint16_t val)
{
	fake_t *f = (fake_t *)bus;
	if (f->n < 512) {
		f->log[f->n].reg = reg;
		f->log[f->n].val = val;
		f->n++;
	}
	f->regs[reg & 0x7F] = val;
}

static uint16_t fake_read(rfbus_t *bus, uint8_t reg)
{
	fake_t *f = (fake_t *)bus;
	f->reads++;
	return f->regs[reg & 0x7F];
}

static void fake_delay(uint32_t ms)
{
	delays_ms += (int)ms;
}

static void fake_init(fake_t *f)
{
	memset(f, 0, sizeof(*f));
	f->bus.write = fake_write;
	f->bus.read = fake_read;
	f->bus.delay_ms = fake_delay;
}

// The log from index from on equals want (n frames); prints the first difference
static bool seq_eq(const fake_t *f, int from, const frame_t *want, int n, int line)
{
	if (f->n - from != n) {
		printf("FAIL line %d: %d frames, want %d\n", line, f->n - from, n);
		failures++;
		return false;
	}
	for (int i = 0; i < n; i++) {
		const frame_t *g = &f->log[from + i];
		if (g->reg != want[i].reg || g->val != want[i].val) {
			printf("FAIL line %d: frame %d is %02X=%04X, want %02X=%04X\n", line, i, g->reg, g->val, want[i].reg,
			       want[i].val);
			failures++;
			return false;
		}
	}
	return true;
}

#define SEQ(f, from, ...) do { static const frame_t w_[] = { __VA_ARGS__ }; \
	seq_eq(f, from, w_, (int)(sizeof(w_) / sizeof(w_[0])), __LINE__); } while (0)

static void test_pure(void)
{
	CHECK(qa4818_freq_word(446000000u) == 44600000u);
	CHECK(qa4818_freq_word(446000004u) == 44600000u);
	CHECK(qa4818_freq_word(446000005u) == 44600001u);

	qa4818_rx_plan_t p;
	qa4818_rx_plan(446000000u, 38850000u, &p);	// UHF: low-side LO
	CHECK(p.lo_hz == 407150000u && p.lo_subband == 0);
	qa4818_rx_plan(400000000u, 38850000u, &p);	// 361.15 MHz is still at least 300
	CHECK(p.lo_hz == 361150000u && p.lo_subband == 0);
	qa4818_rx_plan(146520000u, 38850000u, &p);	// VHF: high-side LO
	CHECK(p.lo_hz == 185370000u && p.lo_subband == 2);
	qa4818_rx_plan(120000000u, 38850000u, &p);	// LO below 160 MHz
	CHECK(p.lo_hz == 158850000u && p.lo_subband == 3);
	qa4818_rx_plan(338849999u, 38850000u, &p);	// just under the 300 MHz rule
	CHECK(p.lo_hz == 377699999u && p.lo_subband == 2);

	CHECK(qa4818_tx_subband(446000000u) == 0);
	CHECK(qa4818_tx_subband(400000000u) == 0);
	CHECK(qa4818_tx_subband(399999999u) == 2);
	CHECK(qa4818_tx_subband(222000000u) == 2);
	CHECK(qa4818_tx_subband(146520000u) == 3);

	CHECK(qa4818_ctcss_detect(625) == 0x22A8);
	CHECK(qa4818_ctcss_detect(670) == 0x22A8);
	CHECK(qa4818_ctcss_detect(693) == 0x2328);
	CHECK(qa4818_ctcss_detect(700) == 0x2328);	// not in the set: the next tone up
	CHECK(qa4818_ctcss_detect(1000) == 0x2316);
	CHECK(qa4818_ctcss_detect(1567) == 0x23E8);
	CHECK(qa4818_ctcss_detect(2541) == 0x2667);
	CHECK(qa4818_ctcss_detect(2600) == 0x200A);	// above the set: the catch-all

	CHECK(qa4818_drive_word(446000000u, false, 0x40) == 0x3980);
	CHECK(qa4818_drive_word(446000000u, true, 0x40) == 0x3880);
	CHECK(qa4818_drive_word(446000000u, false, 0x90) == 0x3820);	// drive << 1 carries: one step down
	CHECK(qa4818_drive_word(222000000u, false, 0x10) == 0x3820);
	CHECK(qa4818_drive_word(146520000u, false, 0x10) == 0x3720);
	CHECK(qa4818_drive_word(146520000u, true, 0x10) == 0x3620);

	CHECK(qa4818_rssi_dbm(0x46) == -125);
	CHECK(qa4818_rssi_dbm(0x140) == 0);
	CHECK(qa4818_rssi_dbm(0xFE80) == -96);		// only bits 8:0
}

static void test_init(void)
{
	fake_t f;
	qa4818_t c;

	fake_init(&f);
	delays_ms = 0;
	qa4818_attach(&c, &f.bus, true);
	qa4818_init_lo(&c);
	SEQ(&f, 0,
	    { 0x7F, 0 }, { 0x00, 0x8000 }, { 0x00, 0 },
	    { 0x30, 0x5FE0 }, { 0x34, 0x086F }, { 0x35, 0xF708 }, { 0x40, 0 }, { 0x1B, 0x8200 }, { 0x1F, 0x3EC8 },
	    { 0x7F, 1 }, { 0x16, 0x9AE8 }, { 0x7F, 0 }, { 0x40, 0 },
	    { 0x3C, 0x1000 }, { 0x3D, 0x9D8A }, { 0x1A, 0xEF50 }, { 0x2A, 0x22F2 },	// sub-band 0, the LO's 0x1A
	    { 0x38, 0xF2B4 }, { 0x39, 0x0297 },					// 435.125 MHz
	    { 0x03, 0xBDF1 }, { 0x04, 0 }, { 0x04, 0x0300 }, { 0x03, 0 },
	    { 0x28, 0x0005 }, { 0x28, 0x0022 });
	CHECK(delays_ms == 40);
	CHECK(c.subband == 0);

	fake_init(&f);
	delays_ms = 0;
	qa4818_attach(&c, &f.bus, false);
	qa4818_init_trx(&c, 0x0001, 0x26AC);
	SEQ(&f, 0,
	    { 0x03, 0 }, { 0x7F, 0 }, { 0x00, 0x8000 }, { 0x00, 0 },
	    { 0x10, 0x0038 }, { 0x18, 0x4125 }, { 0x26, 0 }, { 0x27, 0 }, { 0x25, 0x0001 },
	    { 0x1F, 0x0EF8 }, { 0x30, 0x5FE0 }, { 0x34, 0x086F }, { 0x35, 0xF208 }, { 0x40, 0x39D0 }, { 0x49, 0x26AC },
	    { 0x4B, 0x2442 }, { 0x50, 0x073C }, { 0x77, 0x88EF }, { 0x7B, 0xC3A8 }, { 0x7D, 0x4200 },
	    { 0x10, 0x0038 }, { 0x11, 0x031A }, { 0x11, 0x025A }, { 0x12, 0x035B }, { 0x13, 0x0396 }, { 0x49, 0x26AC },
	    { 0x7F, 1 }, { 0x10, 0x8308 }, { 0x15, 0xC9A7 }, { 0x17, 0x0AA8 }, { 0x18, 0x0A18 }, { 0x19, 0x7900 },
	    { 0x1A, 0x0728 }, { 0x1D, 0x39FD }, { 0x35, 0x100F }, { 0x7F, 0 },
	    { 0x7F, 1 }, { 0x1B, 0x0004 }, { 0x1C, 0x04D7 }, { 0x1D, 0x39FD }, { 0x27, 0x091B }, { 0x36, 0x9D08 },
	    { 0x21, 0xC8F7 }, { 0x22, 0x2136 }, { 0x2A, 0x8537 }, { 0x2B, 0x3B20 }, { 0x7F, 0 },
	    { 0x3C, 0x1000 }, { 0x3D, 0x9D8A }, { 0x1A, 0x1F50 }, { 0x2A, 0x22F2 },	// the transceiver's 0x1A
	    { 0x38, 0xF2B4 }, { 0x39, 0x0297 },
	    { 0x03, 0xBDF1 }, { 0x04, 0x0044 }, { 0x04, 0x0344 }, { 0x03, 0 },
	    { 0x28, 0x0005 },
	    { 0x04, 0x0302 }, { 0x09, 0x0000 }, { 0x09, 0x105B }, { 0x09, 0x205F }, { 0x09, 0x3056 }, { 0x09, 0x4061 },
	    { 0x09, 0x5054 }, { 0x09, 0x604C }, { 0x09, 0x7067 }, { 0x09, 0x807B }, { 0x09, 0x9076 },
	    { 0x7F, 1 }, { 0x11, 0x06FF }, { 0x14, 0x8C29 }, { 0x7F, 0 });
	CHECK(delays_ms == 20);
	CHECK(f.reads == 1);
	CHECK(c.gpio == 0x0001);
}

static void test_tune_gpio(void)
{
	fake_t f;
	qa4818_t c;
	fake_init(&f);
	qa4818_attach(&c, &f.bus, false);

	qa4818_tune(&c, QA4818_SUBBAND_IF, 38850000u);
	SEQ(&f, 0, { 0x3C, 0x1007 }, { 0x3D, 0x6276 }, { 0x1A, 0x1F22 }, { 0x2A, 0xC2F2 }, { 0x38, 0x47C8 }, { 0x39, 0x003B });
	int n = f.n;
	qa4818_tune(&c, QA4818_SUBBAND_IF, 38850000u);	// the same sub-band: frequency only
	SEQ(&f, n, { 0x38, 0x47C8 }, { 0x39, 0x003B });
	n = f.n;
	qa4818_tune(&c, 3, 146520000u);
	SEQ(&f, n, { 0x3C, 0x1001 }, { 0x3D, 0xD89E }, { 0x1A, 0x1F28 }, { 0x2A, 0xA2F2 }, { 0x38, 0x9260 }, { 0x39, 0x00DF });
	n = f.n;
	qa4818_tune(&c, QA4818_SUBBANDS, 146520000u);	// out of range: no sub-band write
	CHECK(f.n == n + 2);

	n = f.n;
	qa4818_gpio(&c, 0x00D8, 0x0018);
	SEQ(&f, n, { 0x25, 0x0018 });
	n = f.n;
	qa4818_gpio(&c, 0x00D8, 0x0018);		// unchanged: no write
	CHECK(f.n == n);
	qa4818_gpio(&c, 0x0020, 0x0020);
	qa4818_gpio(&c, 0x0020, 0);
	SEQ(&f, n, { 0x25, 0x0038 }, { 0x25, 0x0018 });
}

static void test_paths(void)
{
	fake_t f;
	qa4818_t c;
	fake_init(&f);
	qa4818_attach(&c, &f.bus, false);

	qa4818_path(&c, false, false, true, 0x26AC);
	SEQ(&f, 0,
	    { 0x10, 0x0038 }, { 0x11, 0x031A }, { 0x11, 0x025A }, { 0x12, 0x035B }, { 0x13, 0x0396 }, { 0x49, 0x26AC },
	    { 0x7F, 1 }, { 0x1B, 0x0004 }, { 0x1C, 0x04D7 }, { 0x1D, 0x39FD }, { 0x27, 0x091B }, { 0x36, 0x9D08 },
	    { 0x21, 0xC8F7 }, { 0x22, 0x2136 }, { 0x2A, 0x8537 }, { 0x2B, 0x3B20 }, { 0x7F, 0 },
	    { 0x44, 0x43EC }, { 0x4B, 0x7EB7 }, { 0x73, 0x564A }, { 0x7E, 0x342E });
	int n = f.n;
	qa4818_path(&c, true, false, false, 0x26AC);	// TX, 25 kHz
	CHECK(f.regs[0x36] == 0x1D08 && f.regs[0x73] == 0x464A);
	n = f.n;
	qa4818_path(&c, false, true, false, 0x26AC);	// DMR
	CHECK(f.regs[0x36] == 0x1D59 && f.regs[0x44] == 0x53EC && f.regs[0x7E] == 0x361E && f.regs[0x1B] == 0x07FF);
	CHECK(f.regs[0x13] == 0x0375 && f.regs[0x73] == 0x4E8A);	// the digital values without a second receiver
	(void)n;

	n = f.n;
	qa4818_rx_setup(&c, false);
	SEQ(&f, n,
	    { 0x53, 0x57DD }, { 0x23, 0x8F87 }, { 0x3A, 0x0040 }, { 0x43, 0x6009 },
	    { 0x7F, 1 }, { 0x16, 0x04EE }, { 0x18, 0x0A18 }, { 0x7F, 0 },
	    { 0x04, 0x0302 }, { 0x7F, 1 }, { 0x25, 0xC02A }, { 0x26, 0x169C }, { 0x28, 0xECD8 }, { 0x29, 0x0AC9 },
	    { 0x30, 0x8B0B }, { 0x31, 0x35C0 }, { 0x7F, 0 }, { 0x50, 0x073C }, { 0x53, 0x57DD });
	n = f.n;
	qa4818_gain(&c, 0x52);
	SEQ(&f, n, { 0x4F, 0x7E2B }, { 0x78, 0x52FF });

	n = f.n;
	qa4818_rx_on(&c);
	SEQ(&f, n, { 0x34, 0x086F }, { 0x35, 0xF208 }, { 0x47, 0x6140 }, { 0x03, 0xBFF5 });
	n = f.n;
	qa4818_tx_on(&c);
	SEQ(&f, n, { 0x47, 0x6140 }, { 0x03, 0xC1FE });
	n = f.n;
	qa4818_lo_on(&c);
	SEQ(&f, n, { 0x03, 0 }, { 0x03, 0xC1FE });

	n = f.n;
	qa4818_fm_tx(&c, 146520000u);
	SEQ(&f, n,
	    { 0x7F, 1 }, { 0x17, 0x03B8 }, { 0x19, 0x7800 }, { 0x22, 0x1B00 }, { 0x1B, 0x0004 }, { 0x27, 0x88D3 },
	    { 0x1E, 0x4494 }, { 0x1F, 0xC1F3 }, { 0x20, 0x22A7 }, { 0x23, 0xD124 }, { 0x24, 0x087F }, { 0x7F, 0 },
	    { 0x3A, 0x0040 }, { 0x43, 0x6009 }, { 0x28, 0x0018 });
	qa4818_fm_tx(&c, 446000000u);
	CHECK(f.regs[0x28] == 0x0009);

	// DMR TX: the stock digital order (output stage, 0x18, page 1 0x17, tone off, drive off), nothing of the FM set
	f.regs[0x51] = 0x90C5;
	n = f.n;
	qa4818_dmr_tx(&c, 146520000u);
	SEQ(&f, n,
	    { 0x28, 0x0018 }, { 0x18, 0x4125 }, { 0x7F, 1 }, { 0x17, 0x0AA8 }, { 0x7F, 0 }, { 0x51, 0x1050 },
	    { 0x52, 0x028A }, { 0x40, 0x0000 }, { 0x51, 0x1000 });
	qa4818_dmr_tx(&c, 446000000u);
	CHECK(f.regs[0x28] == 0x0009 && f.regs[0x40] == 0);
}

static void test_tones(void)
{
	fake_t f;
	qa4818_t c;
	fake_init(&f);
	qa4818_attach(&c, &f.bus, false);

	uint16_t w100 = subaudio_ctcss_word(1000, false);
	CHECK(w100 == 2064);		// 100.0 Hz x 20.64888, truncated
	qa4818_rx_tone(&c, QA4818_TONE_CTCSS, 1000);
	SEQ(&f, 0, { 0x52, 0x2316 }, { 0x07, 2064 }, { 0x07, 0x2470 }, { 0x51, 0x90C5 });

	int n = f.n;
	qa4818_rx_tone(&c, QA4818_TONE_DCS, 023);	// word 0x763813
	SEQ(&f, n, { 0x08, 0x0813 }, { 0x08, 0x8763 }, { 0x07, 0x0AD7 }, { 0x07, 0x2AD7 }, { 0x51, 0x80B0 }, { 0x52, 0x028A });
	n = f.n;
	qa4818_rx_tone(&c, QA4818_TONE_DCS_INV, 023);	// ~0x763813 & 0x7FFFFF = 0x09C7EC
	SEQ(&f, n, { 0x08, 0x07EC }, { 0x08, 0x809C }, { 0x07, 0x0AD7 }, { 0x07, 0x2AD7 }, { 0x51, 0x80B0 }, { 0x52, 0x028A });
	n = f.n;
	qa4818_rx_tone(&c, QA4818_TONE_NONE, 0);
	SEQ(&f, n, { 0x51, 0x1050 }, { 0x52, 0x028A });

	n = f.n;
	qa4818_tx_tone(&c, QA4818_TONE_CTCSS, 1000, 0x45, 0x30);
	SEQ(&f, n, { 0x07, 2064 }, { 0x07, 2064 | 0x2000 }, { 0x51, 0x90C5 }, { 0x52, 0x028A });
	n = f.n;
	qa4818_tx_tone(&c, QA4818_TONE_DCS, 023, 0x45, 0xB0);	// the level is 7 bits
	SEQ(&f, n, { 0x08, 0x0813 }, { 0x08, 0x8763 }, { 0x07, 0x0AD7 }, { 0x07, 0x2AD7 }, { 0x51, 0x80B0 }, { 0x52, 0x028A });
	n = f.n;
	qa4818_tx_tone(&c, QA4818_TONE_NONE, 0, 0x45, 0x30);
	SEQ(&f, n, { 0x07, 0x0473 }, { 0x07, 0x2473 }, { 0x51, 0x1050 }, { 0x52, 0x028A });

	// Drive: 0x40, then 0x51's low 7 bits replaced, read-modify-write
	f.regs[0x51] = 0x90C5;
	n = f.n;
	int reads = f.reads;
	qa4818_tx_drive(&c, 446000000u, false, 0x40, 0x12);
	CHECK(f.reads == reads + 1);
	CHECK(f.n - n == 13);
	CHECK(f.log[f.n - 2].reg == 0x40 && f.log[f.n - 2].val == 0x3980);
	CHECK(f.log[f.n - 1].reg == 0x51 && f.log[f.n - 1].val == 0x9092);
	n = f.n;
	qa4818_tx_drive_off(&c);
	SEQ(&f, n, { 0x40, 0 }, { 0x51, 0x9080 });
}

// FM TX tones: the stock 1750 Hz burst's sequence and its DTMF set-up, then the microphone again
static void test_tx_tones(void)
{
	CHECK(qa4818_tone_word(1750) == 0x2346);	// 18060 / 2: the stock firmware's 10.32 x Hz, halved
	CHECK(qa4818_tone_word(1000) == 5160);
	CHECK(qa4818_tone_word(2100) == 10836);
	CHECK(qa4818_tone_word(1) == 5);		// 10.32 truncates to 10

	CHECK(qa4818_drive_raw(0x3900, 0x32, false) == 0x3964);
	CHECK(qa4818_drive_raw(0x3900, 0x32, true) == 0x3864);
	CHECK(qa4818_drive_raw(0x3900, 0xB4, false) == 0x3868);	// level << 1 carries: one step down
	CHECK(qa4818_drive_raw(0x3900, 0xB4, true) == 0x3768);
	CHECK(qa4818_tone_drive_word(446000000u, false) == 0x3964);
	CHECK(qa4818_tone_drive_word(300000000u, false) == 0x3868);	// "above 300 MHz" in the stock compare
	CHECK(qa4818_tone_drive_word(222000000u, false) == 0x3868);
	CHECK(qa4818_tone_drive_word(200000000u, false) == 0x3704);
	CHECK(qa4818_tone_drive_word(146520000u, true) == 0x3604);
	CHECK(qa4818_line_drive_word(446000000u, false, 0x40) == 0x3980);
	CHECK(qa4818_line_drive_word(222000000u, false, 0x40) == 0x3880);
	CHECK(qa4818_line_drive_word(146520000u, true, 0x40) == 0x3680);

	fake_t f;
	qa4818_t c;
	fake_init(&f);
	qa4818_attach(&c, &f.bus, false);

	// The burst: page 1 0x19, the tone drive, page 1 0x1B = 5, then the tone word and the generator on
	qa4818_tx_aux(&c, false, 0x3964);
	qa4818_tone_start(&c, 1750);
	SEQ(&f, 0,
	    { 0x7F, 1 }, { 0x19, 0x7800 }, { 0x7F, 0 }, { 0x40, 0x3964 }, { 0x7F, 1 }, { 0x1B, 0x0005 }, { 0x7F, 0 },
	    { 0x71, 0x2346 }, { 0x70, 0xA000 });
	int n = f.n;
	qa4818_tone_gen(&c, false);
	qa4818_tone_gen(&c, true);
	SEQ(&f, n, { 0x70, 0 }, { 0x70, 0xA000 });

	// The microphone again: 0x1B, the microphone's level (0x18, page 1 0x17), the generator off, then the voice drive
	// with 0x51's level (read-modify-write)
	f.regs[0x51] = 0x90C5;
	n = f.n;
	int reads = f.reads;
	qa4818_tx_mic(&c, 0x3980, 0x12, QA4818_MIC_GAIN_DEFAULT, 0x37);
	SEQ(&f, n,
	    { 0x7F, 1 }, { 0x1B, 0x0004 }, { 0x7F, 0 }, { 0x18, 0x2125 }, { 0x7F, 1 }, { 0x17, 0x03B8 }, { 0x7F, 0 },
	    { 0x70, 0 }, { 0x40, 0x3980 }, { 0x51, 0x9092 });
	CHECK(f.reads == reads + 1);

	// The microphone's level: the stock arithmetic, both fields masked to their width
	n = f.n;
	qa4818_mic_level(&c, 7, 0x15);
	SEQ(&f, n, { 0x18, 0x3925 }, { 0x7F, 1 }, { 0x17, 0x02A8 }, { 0x7F, 0 });
	n = f.n;
	qa4818_mic_level(&c, 2, 0xFF);
	SEQ(&f, n, { 0x18, 0x1125 }, { 0x7F, 1 }, { 0x17, 0x03F8 }, { 0x7F, 0 });

	// The audio input pin (the stock DTMF): 0x18 and page 1 0x17 bit 0 as well
	n = f.n;
	qa4818_tx_aux(&c, true, 0x3880);
	SEQ(&f, n,
	    { 0x7F, 1 }, { 0x19, 0x7800 }, { 0x7F, 0 }, { 0x18, 0x4125 }, { 0x7F, 1 }, { 0x17, 0x03B9 }, { 0x7F, 0 },
	    { 0x40, 0x3880 }, { 0x7F, 1 }, { 0x1B, 0x0005 }, { 0x7F, 0 });
}

static void test_status(void)
{
	fake_t f;
	qa4818_t c;
	fake_init(&f);
	qa4818_attach(&c, &f.bus, false);
	f.regs[0x0C] = 0x0002;
	f.regs[0x65] = 0xFF2A;
	f.regs[0x67] = 0xFE46;
	f.regs[0x60] = 0x0001;
	qa4818_status_t s;
	qa4818_status(&c, &s);
	CHECK(s.signal);
	CHECK(s.noise == 0x2A);
	CHECK(s.rssi == 0x046);
	CHECK(s.tone_flags == 0x0001);
	CHECK(f.n == 0);		// reads only
	f.regs[0x0C] = 0x0001;
	qa4818_status(&c, &s);
	CHECK(!s.signal);
}

// The output register from another task while the chip is on page 1 (between a page-1 block's writes): page 0's
// 0x25 is written, then page 1 again; on page 0 the one write
static void test_gpio_page(void)
{
	fake_t f;
	qa4818_t c;

	fake_init(&f);
	qa4818_attach(&c, &f.bus, false);
	c.page = 1;
	qa4818_gpio(&c, 0x0004, 0x0004);
	CHECK(f.n == 3 && f.log[0].reg == QA4818_REG_PAGE && f.log[0].val == 0 && f.log[1].reg == QA4818_REG_GPIO &&
	      f.log[1].val == 0x0004 && f.log[2].reg == QA4818_REG_PAGE && f.log[2].val == 1 && c.page == 1);
	fake_init(&f);
	qa4818_attach(&c, &f.bus, false);
	qa4818_gpio(&c, 0x0004, 0x0004);
	qa4818_gpio(&c, 0x0004, 0x0004);	// unchanged: nothing
	CHECK(f.n == 1 && f.log[0].reg == QA4818_REG_GPIO);
}

int main(void)
{
	test_pure();
	test_init();
	test_tune_gpio();
	test_paths();
	test_tones();
	test_tx_tones();
	test_status();
	test_gpio_page();

	if (failures) {
		printf("qa4818Test: %d failure(s)\n", failures);
		return 1;
	}
	printf("qa4818Test: all passed\n");
	return 0;
}
