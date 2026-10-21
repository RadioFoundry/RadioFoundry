// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 driver: register access, power-on init and FM modes (docs/spec/modem-hrc6000.md 1.1, 3, 8;
// docs/spec/hardware-dm1701.md 12.4, 12.5). Values marked (trace) in the spec are kept as given.
#include <stddef.h>
#include "hrc6000.h"
#include "hal_dmr_modem.h"

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

// --- Chip description for the shared code ---

static uint8_t dmr_mode(bool repeater)
{
	(void)repeater;
	return 0x6E;
}

static uint8_t crc_len(uint8_t kind)
{
	(void)kind;
	return 0;	// bursts go as given
}

static uint8_t voice_lcss(uint8_t voice_index)
{
	(void)voice_index;
	return 0;	// the chip inserts the embedded LC's LCSS itself
}

static bool set_mic_gain(hrc_t *dev, uint8_t gain)
{
	if (gain > 31u) {
		gain = 31u;
	}
	return hrc_write(dev, 4, 0xE4, (uint8_t)(dev->mic_gain_base + gain));
}

static bool dmr_enter(hrc_t *dev, uint8_t mod2, uint8_t mod1, uint8_t mic_gain)
{
	bool ok = hrc_write(dev, 4, 0x60, 0x00);	// analog voice TX off (FM leftovers)
	ok &= hrc_write(dev, 4, 0x01, 0xF0);		// two-point modulation TX, IF RX
	ok &= hrc_set_mod_gain(dev, mod2, mod1);	// DMR pair
	ok &= hrc_write(dev, 4, 0x10, dmr_mode(false));	// DMR, Tier II, layer 2
	ok &= hrc_write(dev, 4, 0xE0, (uint8_t)(0x89u | dev->mic_input));	// codec under MCU control, mic on
	ok &= hrc_set_mic_gain(dev, mic_gain);
	ok &= hrc_write(dev, 4, 0x26, 0xFD);
	ok &= hrc_write(dev, 4, 0x81, 0x19);		// system interrupt mask: post access, received data, PHY
	ok &= hrc_write(dev, 4, 0x83, 0xFF);		// clear interrupts
	return ok;
}

// modem-hrc6000.md 6.2: access policy and encoder start at each transmission
static void tx_begin(hrc_t *dev, uint8_t access_reg)
{
	hrc_write(dev, 4, 0x20, 0x00);
	hrc_write(dev, 4, 0x21, access_reg);
	hrc_write(dev, 4, 0x22, 0x86);
}

static const hrc_chip_t chip = {
	.cfg_page = 4,
	.id_low_first = false,
	.mcu_crc = true,
	.sync_reg = true,
	.mode_per_channel = false,
	.tx_begin = tx_begin,
	.dmr_mode = dmr_mode,
	.crc_len = crc_len,
	.voice_lcss = voice_lcss,
	.set_mic_gain = set_mic_gain,
	.dmr_enter = dmr_enter,
};

void hrc6000_attach(hrc_t *dev, const hrc_bus_t *bus)
{
	hrc_attach(dev, bus, &chip);
}

static bool write_rows(hrc_t *dev, const hrc_row_t *rows, uint32_t n)
{
	bool ok = true;
	for (uint32_t i = 0; i < n; i++) {
		ok &= hrc_write(dev, rows[i].page, rows[i].reg, rows[i].val);
	}
	return ok;
}

#define ROWS(a)	(a), (sizeof(a) / sizeof((a)[0]))

// --- Chip operations ---

const hrc6000_clock_t hrc6000_clock_29m4912 = { 0x28, 0x33, 0x05, 0x04, 0x02 };
const hrc6000_clock_t hrc6000_clock_12m288 = { 0x40, 0x32, 0x05, 0x04, 0x02 };

static bool init_clock(hrc_t *dev, const hrc6000_clock_t *clk)
{
	bool ok = hrc_write(dev, 4, 0x0A, 0x80);	// run from XTALI
	ok &= hrc_write(dev, 4, 0x0B, clk->pll_m);	// PLL M
	ok &= hrc_write(dev, 4, 0x0C, clk->pll_n);	// PLL N / NO
	dev->bus->delay_us(1000);			// datasheet: more than 500 us before switching to the PLL
	ok &= hrc_write(dev, 4, 0x0A, 0x00);	// run from the PLL
	ok &= hrc_write(dev, 4, 0xB9, clk->sys_div);	// system clock divider
	ok &= hrc_write(dev, 4, 0xBA, clk->codec_div);	// codec clock divider
	ok &= hrc_write(dev, 4, 0xBB, clk->clkout_div);	// CLKOUT divider
	return ok;
}

static bool write_dtmf_table(hrc_t *dev)
{
	bool ok = true;
	for (uint32_t i = 0; i < sizeof(dtmf_hz) / sizeof(dtmf_hz[0]); i++) {
		uint16_t v = (uint16_t)(((uint32_t)dtmf_hz[i] * 65536u + 16000u) / 32000u);
		uint16_t addr = (uint16_t)(DTMF_TABLE_ADDR + 2u * i);
		ok &= hrc_write_ext(dev, HRC_PAGE_AUX, addr, (uint8_t)(v & 0xFFu));
		ok &= hrc_write_ext(dev, HRC_PAGE_AUX, (uint16_t)(addr + 1u), (uint8_t)(v >> 8));
	}
	return ok;
}

bool hrc6000_power_on_init(hrc_t *dev, uint8_t dmr_mic_gain)
{
	return hrc6000_power_on_init_clock(dev, dmr_mic_gain, &hrc6000_clock_29m4912);
}

bool hrc6000_power_on_init_clock(hrc_t *dev, uint8_t dmr_mic_gain, const hrc6000_clock_t *clk)
{
	bool ok = init_clock(dev, clk);					// 1
	ok &= write_rows(dev, ROWS(init_step2));			// 2
	ok &= hrc_write_burst(dev, 4, 0x11, init_sys_block, sizeof(init_sys_block));	// 3
	ok &= hrc_write(dev, 4, 0x00, 0x2A);			// 4: partial reset
	ok &= hrc_write(dev, 4, 0x06, 0x22);			//    vocoder buffer access
	ok &= hrc_flush_vocoder(dev);				// 5
	ok &= hrc_write(dev, 4, 0x06, 0x20);			// 6
	ok &= hrc_set_dmr_id(dev, 1);				//    placeholder until the radio ID is known
	ok &= hrc_write_burst(dev, 1, 0x10, aux_10, sizeof(aux_10));	// 7
	ok &= hrc_write_burst(dev, 1, 0x30, aux_30, sizeof(aux_30));
	ok &= hrc_write_burst(dev, 1, 0x40, aux_40, sizeof(aux_40));
	ok &= hrc_write_burst(dev, 1, 0x50, aux_50, sizeof(aux_50));
	ok &= write_rows(dev, ROWS(init_step8_11));			// 8-11
	ok &= hrc_write_burst(dev, 1, 0x04, hrc_ms_data_sync, sizeof(hrc_ms_data_sync));	// 12
	ok &= write_rows(dev, ROWS(init_step13));			// 13
	ok &= hrc_set_mic_gain(dev, dmr_mic_gain);			// 14
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

bool hrc6000_set_ref_offset(hrc_t *dev, int8_t offset, bool fm_tx)
{
	uint8_t raw = (uint8_t)offset;
	bool ok = hrc_write(dev, 4, 0x47, raw);
	ok &= hrc_write(dev, 4, 0x48, (uint8_t)((!fm_tx && offset < 0) ? 0x03 : 0x00));
	ok &= hrc_write(dev, 4, 0x04, raw);
	return ok;
}

bool hrc6000_fm_audio(hrc_t *dev, bool on)
{
	return hrc_write(dev, 4, 0x36, on ? 0x02 : 0x00);
}

bool hrc6000_set_mode_fm_rx(hrc_t *dev)
{
	return write_rows(dev, ROWS(fm_rx_rows));
}

bool hrc6000_set_mode_fm_tx(hrc_t *dev, const hrc6000_fm_tx_t *cfg)
{
	hrc6000_fm_dev_t dev_coef = hrc6000_fm_deviation(cfg->uhf, cfg->wide);
	bool ok = hrc_write(dev, 4, 0x10, 0x80);	// FM
	ok &= hrc_write(dev, 4, 0xE0, (uint8_t)(0x89u | dev->mic_input));	// codec under MCU control, mic on
	ok &= hrc_set_mic_gain(dev, cfg->mic_gain);
	ok &= hrc_write(dev, 4, 0xC2, 0x00);	// mic AGC off
	ok &= hrc_write(dev, 4, 0xE5, 0x1A);
	ok &= hrc_write(dev, 4, 0x25, 0x0E);
	ok &= hrc_write(dev, 4, 0x26, 0xFE);	// FM receive path off
	ok &= hrc_write(dev, 4, 0x83, 0xFF);	// clear interrupts
	ok &= hrc_write(dev, 4, 0x87, 0x00);
	ok &= hrc_set_mod_gain(dev, cfg->mod2, cfg->mod1);
	ok &= hrc6000_set_ref_offset(dev, cfg->ref_offset, true);
	ok &= hrc_write(dev, 4, 0x49, 0xFF);	// modulation limits
	ok &= hrc_write(dev, 4, 0x4A, 0xFF);
	ok &= hrc_write(dev, 4, 0x35, dev_coef.voice);
	if (cfg->tone == HRC6000_TONE_CTCSS) {
		ok &= hrc_write(dev, 4, 0xA0, dev_coef.ctcss);
	} else if (cfg->tone == HRC6000_TONE_DCS) {
		ok &= hrc_write(dev, 4, 0xA0, dev_coef.dcs);
	}
	ok &= hrc_write(dev, 4, 0x3F, 0x04);	// TX limiter
	ok &= hrc_write(dev, 4, 0x34, 0x3C);	// pre/de-emphasis on, 3 kHz filter
	ok &= hrc_write(dev, 4, 0x3E, 0x08);
	ok &= hrc_write(dev, 1, 0x50, 0x00);
	ok &= hrc_write(dev, 1, 0x51, 0x00);
	ok &= hrc_write(dev, 4, 0x60, 0x80);	// analog voice TX on
	return ok;
}

bool hrc6000_set_rx_gain(hrc_t *dev, int8_t db)
{
	int g = db < -31 ? -31 : db > 31 ? 31 : db;
	uint8_t v = (uint8_t)(0x80 | (g > 0 ? 0x40 : 0x00) | ((unsigned)g & 0x1Fu));
	return hrc_write(dev, 4, 0x37, v);
}
