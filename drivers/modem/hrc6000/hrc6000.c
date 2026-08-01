// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 driver: register access, power-on init and FM modes (docs/spec/modem-hrc6000.md 1.1, 3, 8;
// docs/spec/hardware-dm1701.md 12.4, 12.5). Values marked (trace) in the spec are kept as given.
#include <stddef.h>
#include "hrc6000.h"

#define CMD_READ	0x80u
#define CMD_EXT		0x40u

typedef struct {
	uint8_t page;
	uint8_t reg;
	uint8_t val;
} hrc_row_t;

// Step 2
static const hrc_row_t init_step2[] = {
	{ 4, 0xA1, 0x80 }, { 4, 0x10, 0xF3 }, { 4, 0x5F, 0xF0 }, { 4, 0x40, 0x43 },
	{ 4, 0x07, 0x0B }, { 4, 0x08, 0xB8 }, { 4, 0x09, 0x00 },	// 450 kHz IF
	{ 4, 0x06, 0x21 }, { 4, 0x00, 0xFF }, { 4, 0x01, 0xB0 },
	{ 4, 0x02, 0x00 }, { 4, 0x03, 0x00 }, { 4, 0x04, 0x00 }, { 4, 0x05, 0x00 },
	{ 4, 0x01, 0xF8 },
};

// Step 3: page 4 from 0x11 (trace)
static const uint8_t init_sys_block[] = {
	0x80, 0x0C, 0x22, 0x01, 0x00, 0x00, 0x33, 0xEF, 0x00, 0xFF, 0xFF, 0xFF, 0xF0, 0xF0, 0x10, 0x00,
	0x00, 0x06, 0x3B, 0xF8, 0x0E, 0xFD, 0x40, 0xFF, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x06, 0x0B, 0x00,
	0x17, 0x02, 0xFF, 0xE0, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
_Static_assert(sizeof(init_sys_block) == 44, "step 3 block is 44 bytes");

// Step 7: page 1 blocks (trace). The 0x10 block holds the CRC masks
static const uint8_t aux_10[] = {
	0x69, 0x69, 0x96, 0x96, 0x96, 0x99, 0x99, 0x99, 0xA5, 0xA5, 0xAA, 0xAA, 0xCC, 0xCC, 0x00, 0xF0,
	0x01, 0xFF, 0x01, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t aux_30[] = {
	0x00, 0x00, 0x20, 0x3C, 0xFF, 0xFF, 0x3F, 0x50, 0x07, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t aux_40[] = { 0x00, 0x01, 0x01, 0x02, 0x01, 0x1E, 0xF0 };
static const uint8_t aux_50[] = { 0x00, 0x08, 0xEB, 0x78, 0x67 };
_Static_assert(sizeof(aux_10) == 32 && sizeof(aux_30) == 16, "page 1 block sizes");

// Steps 8-11
static const hrc_row_t init_step8_11[] = {
	{ 1, 0x52, 0x08 }, { 1, 0x53, 0xEB }, { 1, 0x54, 0x78 }, { 1, 0x45, 0x1E }, { 1, 0x37, 0x50 }, { 1, 0x35, 0xFF },
	{ 4, 0x39, 0x02 }, { 4, 0x3D, 0x0A }, { 4, 0x83, 0xFF }, { 4, 0x87, 0x00 }, { 4, 0x65, 0x0A }, { 4, 0x1D, 0xFF },
	{ 4, 0x1E, 0xF1 }, { 4, 0xE2, 0x06 }, { 4, 0xE4, 0x27 }, { 4, 0xE3, 0x52 }, { 4, 0xE5, 0x1A }, { 4, 0xE1, 0x0F },
	{ 4, 0xD1, 0xC4 }, { 4, 0x25, 0x0E }, { 4, 0x26, 0xFD }, { 4, 0x64, 0x00 }, { 4, 0x10, 0x6B }, { 4, 0x81, 0x19 },
	{ 4, 0x01, 0xF0 }, { 4, 0xE4, 0x27 }, { 4, 0xE5, 0x1A }, { 4, 0x37, 0x9E }, { 4, 0xE0, 0xC9 }, { 4, 0x25, 0x0E },
	{ 4, 0x26, 0xFD }, { 4, 0x48, 0x00 }, { 4, 0x47, 0x21 }, { 4, 0x1F, 0x10 },	// color code 1
	{ 1, 0x54, 0x78 }, { 1, 0x24, 0x00 }, { 1, 0x25, 0x00 }, { 1, 0x26, 0x00 }, { 1, 0x27, 0x00 },
	{ 4, 0x41, 0x40 }, { 4, 0x56, 0x00 }, { 4, 0x5C, 0x09 }, { 4, 0x5F, 0xF0 },
};

// Step 12: MS-sourced data sync (ETSI TS 102 361-1); the chip sends the voice sync as its inverse
static const uint8_t ms_data_sync[] = { 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 };

// Step 13
static const hrc_row_t init_step13[] = {
	{ 4, 0x11, 0x80 }, { 4, 0x81, 0x19 }, { 4, 0x85, 0x00 },
};

// DTMF tone table (page 1, extended 0x11A-0x129): value = f x 65536 / 32000, low byte first
static const uint16_t dtmf_hz[] = { 697, 770, 852, 941, 1209, 1336, 1477, 1633 };
#define DTMF_TABLE_ADDR	0x11Au

// FM RX (modem-hrc6000.md 8)
static const hrc_row_t fm_rx_rows[] = {
	{ 4, 0x60, 0x00 }, { 4, 0xE0, 0x89 }, { 4, 0x10, 0x80 }, { 4, 0x34, 0x3C }, { 4, 0x81, 0x19 }, { 4, 0x85, 0x00 },
	{ 4, 0x26, 0xFD },
};

// --- Register access ---

void hrc6000_attach(hrc6000_t *dev, const hrc6000_bus_t *bus)
{
	dev->bus = bus;
}

bool hrc6000_write(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t val)
{
	uint8_t hdr[2] = { (uint8_t)(page & 7u), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, &val, NULL, 1);
}

bool hrc6000_read(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t *val)
{
	uint8_t hdr[2] = { (uint8_t)(CMD_READ | (page & 7u)), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, NULL, val, 1);
}

bool hrc6000_write_burst(hrc6000_t *dev, uint8_t page, uint8_t reg, const uint8_t *data, uint16_t len)
{
	uint8_t hdr[2] = { (uint8_t)(page & 7u), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, data, NULL, len);
}

bool hrc6000_read_burst(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t *data, uint16_t len)
{
	uint8_t hdr[2] = { (uint8_t)(CMD_READ | (page & 7u)), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, NULL, data, len);
}

bool hrc6000_write_ext(hrc6000_t *dev, uint8_t page, uint16_t addr, uint8_t val)
{
	uint8_t hdr[3] = { (uint8_t)(CMD_EXT | (page & 7u)), (uint8_t)(addr & 0xFFu), (uint8_t)((addr >> 8) & 7u) };
	return dev->bus->transfer(dev->bus->ctx, hdr, 3, &val, NULL, 1);
}

static bool write_rows(hrc6000_t *dev, const hrc_row_t *rows, uint32_t n)
{
	bool ok = true;
	for (uint32_t i = 0; i < n; i++) {
		ok &= hrc6000_write(dev, rows[i].page, rows[i].reg, rows[i].val);
	}
	return ok;
}

#define ROWS(a)	(a), (sizeof(a) / sizeof((a)[0]))

// --- Chip operations ---

static bool init_clock(hrc6000_t *dev)
{
	bool ok = hrc6000_write(dev, 4, 0x0A, 0x80);	// run from XTALI
	ok &= hrc6000_write(dev, 4, 0x0B, 0x28);	// PLL M
	ok &= hrc6000_write(dev, 4, 0x0C, 0x33);	// PLL N / NO
	dev->bus->delay_us(1000);			// datasheet: more than 500 us before switching to the PLL
	ok &= hrc6000_write(dev, 4, 0x0A, 0x00);	// run from the PLL
	ok &= hrc6000_write(dev, 4, 0xB9, 0x05);	// system clock divider
	ok &= hrc6000_write(dev, 4, 0xBA, 0x04);	// codec clock divider
	ok &= hrc6000_write(dev, 4, 0xBB, 0x02);	// CLKOUT divider
	return ok;
}

static bool flush_vocoder(hrc6000_t *dev)
{
	uint8_t fill[128];
	for (uint32_t i = 0; i < sizeof(fill); i++) {
		fill[i] = 0x7F;
	}
	return hrc6000_write_burst(dev, HRC6000_PAGE_VOCODER, 0x00, fill, sizeof(fill));
}

static bool write_dtmf_table(hrc6000_t *dev)
{
	bool ok = true;
	for (uint32_t i = 0; i < sizeof(dtmf_hz) / sizeof(dtmf_hz[0]); i++) {
		uint16_t v = (uint16_t)(((uint32_t)dtmf_hz[i] * 65536u + 16000u) / 32000u);
		uint16_t addr = (uint16_t)(DTMF_TABLE_ADDR + 2u * i);
		ok &= hrc6000_write_ext(dev, HRC6000_PAGE_AUX, addr, (uint8_t)(v & 0xFFu));
		ok &= hrc6000_write_ext(dev, HRC6000_PAGE_AUX, (uint16_t)(addr + 1u), (uint8_t)(v >> 8));
	}
	return ok;
}

bool hrc6000_set_dmr_id(hrc6000_t *dev, uint32_t id)
{
	bool ok = hrc6000_write(dev, 4, 0x14, (uint8_t)(id >> 16));
	ok &= hrc6000_write(dev, 4, 0x15, (uint8_t)(id >> 8));
	ok &= hrc6000_write(dev, 4, 0x16, (uint8_t)id);
	return ok;
}

bool hrc6000_set_mic_gain(hrc6000_t *dev, uint8_t gain)
{
	if (gain > 31u) {
		gain = 31u;
	}
	return hrc6000_write(dev, 4, 0xE4, (uint8_t)(0x20u + gain));
}

bool hrc6000_power_on_init(hrc6000_t *dev, uint8_t dmr_mic_gain)
{
	bool ok = init_clock(dev);					// 1
	ok &= write_rows(dev, ROWS(init_step2));			// 2
	ok &= hrc6000_write_burst(dev, 4, 0x11, init_sys_block, sizeof(init_sys_block));	// 3
	ok &= hrc6000_write(dev, 4, 0x00, 0x2A);			// 4: partial reset
	ok &= hrc6000_write(dev, 4, 0x06, 0x22);			//    vocoder buffer access
	ok &= flush_vocoder(dev);					// 5
	ok &= hrc6000_write(dev, 4, 0x06, 0x20);			// 6
	ok &= hrc6000_set_dmr_id(dev, 1);				//    placeholder until the radio ID is known
	ok &= hrc6000_write_burst(dev, 1, 0x10, aux_10, sizeof(aux_10));	// 7
	ok &= hrc6000_write_burst(dev, 1, 0x30, aux_30, sizeof(aux_30));
	ok &= hrc6000_write_burst(dev, 1, 0x40, aux_40, sizeof(aux_40));
	ok &= hrc6000_write_burst(dev, 1, 0x50, aux_50, sizeof(aux_50));
	ok &= write_rows(dev, ROWS(init_step8_11));			// 8-11
	ok &= hrc6000_write_burst(dev, 1, 0x04, ms_data_sync, sizeof(ms_data_sync));	// 12
	ok &= write_rows(dev, ROWS(init_step13));			// 13
	ok &= hrc6000_set_mic_gain(dev, dmr_mic_gain);			// 14
	ok &= write_dtmf_table(dev);
	return ok;
}

hrc6000_fm_dev_t hrc6000_fm_deviation(bool uhf, bool wide)
{
	static const hrc6000_fm_dev_t table[2][2] = {
		// VHF: 12.5 kHz, 25 kHz (0x68 is about where 25 kHz voice starts to distort)
		{ { 0x37, 0x10, 0x04, 0x3A }, { 0x73, 0x18, 0x06, 0x75 } },
		// UHF
		{ { 0x12, 0x06, 0x01, 0x12 }, { 0x24, 0x09, 0x02, 0x24 } },
	};
	return table[uhf ? 1 : 0][wide ? 1 : 0];
}

bool hrc6000_set_ref_offset(hrc6000_t *dev, int8_t offset, bool fm_tx)
{
	uint8_t raw = (uint8_t)offset;
	bool ok = hrc6000_write(dev, 4, 0x47, raw);
	ok &= hrc6000_write(dev, 4, 0x48, (uint8_t)((!fm_tx && offset < 0) ? 0x03 : 0x00));
	ok &= hrc6000_write(dev, 4, 0x04, raw);
	return ok;
}

bool hrc6000_set_mod_gain(hrc6000_t *dev, uint8_t mod2, uint8_t mod1)
{
	bool ok = hrc6000_write(dev, 4, 0x45, mod2);
	ok &= hrc6000_write(dev, 4, 0x46, mod1);
	return ok;
}

bool hrc6000_fm_audio(hrc6000_t *dev, bool on)
{
	return hrc6000_write(dev, 4, 0x36, on ? 0x02 : 0x00);
}

bool hrc6000_set_mode_fm_rx(hrc6000_t *dev)
{
	return write_rows(dev, ROWS(fm_rx_rows));
}

bool hrc6000_set_mode_fm_tx(hrc6000_t *dev, const hrc6000_fm_tx_t *cfg)
{
	hrc6000_fm_dev_t dev_coef = hrc6000_fm_deviation(cfg->uhf, cfg->wide);
	bool ok = hrc6000_write(dev, 4, 0x10, 0x80);	// FM
	ok &= hrc6000_write(dev, 4, 0xE0, 0xC9);	// codec under MCU control, mic on
	ok &= hrc6000_set_mic_gain(dev, cfg->mic_gain);
	ok &= hrc6000_write(dev, 4, 0xC2, 0x00);	// mic AGC off
	ok &= hrc6000_write(dev, 4, 0xE5, 0x1A);
	ok &= hrc6000_write(dev, 4, 0x25, 0x0E);
	ok &= hrc6000_write(dev, 4, 0x26, 0xFE);	// FM receive path off
	ok &= hrc6000_write(dev, 4, 0x83, 0xFF);	// clear interrupts
	ok &= hrc6000_write(dev, 4, 0x87, 0x00);
	ok &= hrc6000_set_mod_gain(dev, cfg->mod2, cfg->mod1);
	ok &= hrc6000_set_ref_offset(dev, cfg->ref_offset, true);
	ok &= hrc6000_write(dev, 4, 0x49, 0xFF);	// modulation limits
	ok &= hrc6000_write(dev, 4, 0x4A, 0xFF);
	ok &= hrc6000_write(dev, 4, 0x35, dev_coef.voice);
	if (cfg->tone == HRC6000_TONE_CTCSS) {
		ok &= hrc6000_write(dev, 4, 0xA0, dev_coef.ctcss);
	} else if (cfg->tone == HRC6000_TONE_DCS) {
		ok &= hrc6000_write(dev, 4, 0xA0, dev_coef.dcs);
	}
	ok &= hrc6000_write(dev, 4, 0x3F, 0x04);	// TX limiter
	ok &= hrc6000_write(dev, 4, 0x34, 0x3C);	// pre/de-emphasis on, 3 kHz filter
	ok &= hrc6000_write(dev, 4, 0x3E, 0x08);
	ok &= hrc6000_write(dev, 1, 0x50, 0x00);
	ok &= hrc6000_write(dev, 1, 0x51, 0x00);
	ok &= hrc6000_write(dev, 4, 0x60, 0x80);	// analog voice TX on
	return ok;
}
