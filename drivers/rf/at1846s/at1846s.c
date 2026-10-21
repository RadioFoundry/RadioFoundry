// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AT1846S RF transceiver driver (docs/spec/hardware-dm1701.md section 5; AT1846S datasheet and programming guide)
#include <stddef.h>
#include "at1846s.h"
#include "rf/subaudio/subaudio.h"

#define REG_PAGE	0x7F
#define REG_CTRL	0x30

typedef struct {
	uint8_t reg;
	uint16_t val;
} reg_val_t;

typedef at1846s_bw_row_t bw_row_t;

// 5.2 steps 3-30, after the soft reset
static const reg_val_t init_regs[] = {
	{ 0x30, 0x0004 },	// power on
	{ 0x04, 0x0FD0 },	// 25.6 / 26 MHz clock mode
	{ 0x0A, 0x7C20 },
	{ 0x13, 0xA100 },
	{ 0x1F, 0x1001 },	// GPIO6 squelch out, GPIO1 CSS out
	{ 0x31, 0x0031 },
	{ 0x33, 0x44A5 },	// AGC number
	{ 0x34, 0x2B89 },	// RX digital gain
	{ 0x41, 0x4122 },	// digital voice gain
	{ 0x42, 0x1052 },	// VOX shut threshold
	{ 0x43, 0x0100 },	// FM deviation
	{ 0x44, 0x07FF },	// RX / TX gain
	{ 0x3A, 0x00C3 },
	{ 0x59, 0x0B90 },	// deviation
	{ 0x47, 0x7F2F },
	{ 0x4F, 0x2C62 },
	{ 0x53, 0x0094 },
	{ 0x54, 0x2A3C },
	{ 0x55, 0x0081 },
	{ 0x56, 0x0B02 },	// squelch detection time
	{ 0x57, 0x1C00 },	// RSSI low-pass bypass
	{ 0x58, 0x9CDD },
	{ 0x5A, 0x06DB },
	{ 0x63, 0x16AD },	// pre-emphasis bypass threshold
	{ 0x0F, 0x8A24 },
	{ 0x05, 0x8763 },
	{ 0x30, 0x40A4 },	// prepare calibration
	{ 0x30, 0x40A6 },	// calibration enable
};

// 5.4 (the page register rows are handled by the page logic)
static const bw_row_t bw_rows[] = {
	{ 0, 0x3A, { 0x44CB, 0x40CB } },
	{ 0, 0x15, { 0x1100, 0x1F00 } },
	{ 0, 0x32, { 0x4495, 0x7564 } },
	{ 0, 0x3A, { 0x00C3, 0x00C3 } },
	{ 0, 0x59, { 0x0B90, 0x0B90 } },	// CTCSS dev (bits 5:0) not doubled: 25 kHz mode already doubles it (HIL ratio 4.0)
	{ 0, 0x3F, { 0x29D1, 0x29D1 } },
	{ 0, 0x3C, { 0x1B34, 0x1B34 } },
	{ 0, 0x48, { 0x19B1, 0x1E38 } },
	{ 0, 0x60, { 0x0F17, 0x0F17 } },
	{ 0, 0x62, { 0x1425, 0x3767 } },
	{ 0, 0x65, { 0x2494, 0x248A } },
	{ 0, 0x66, { 0xEB2E, 0xFF2E } },
	{ 1, 0x06, { 0x0014, 0x0024 } },
	{ 1, 0x07, { 0x020C, 0x0214 } },
	{ 1, 0x08, { 0x0214, 0x0224 } },
	{ 1, 0x09, { 0x030C, 0x0314 } },
	{ 1, 0x0A, { 0x0314, 0x0324 } },
	{ 1, 0x0B, { 0x0324, 0x0344 } },
	{ 1, 0x0C, { 0x0344, 0x0384 } },
	{ 1, 0x0D, { 0x1344, 0x1384 } },
	{ 1, 0x0E, { 0x1B44, 0x1B84 } },
	{ 1, 0x0F, { 0x3F44, 0x3F84 } },
	{ 1, 0x12, { 0xE0EB, 0xE0EB } },
};

// 5.5
static const reg_val_t fm_regs[] = {
	{ 0x33, 0x44A5 },
	{ 0x41, 0x4431 },
	{ 0x42, 0x10F0 },
	{ 0x43, 0x00A9 },
	{ 0x58, 0xBC85 },	// filters on, de-emphasis off (the HR_C6000 does it)
	{ 0x44, 0x06CC },
	{ 0x3A, 0x00C3 },
	{ 0x40, 0x0030 },	// bit 0 clear: no low-frequency baseband
};

static const reg_val_t dmr_regs[] = {
	{ 0x40, 0x0031 },	// bit 0 set: low-frequency baseband for 4FSK
	{ 0x15, 0x1100 },
	{ 0x32, 0x4495 },
	{ 0x3A, 0x00C3 },
	{ 0x3C, 0x1B34 },
	{ 0x3F, 0x29D1 },
	{ 0x41, 0x4122 },
	{ 0x42, 0x1052 },
	{ 0x43, 0x0100 },
	{ 0x48, 0x19B1 },
	{ 0x58, 0x9CDD },	// audio filters off
	{ 0x44, 0x07FF },	// AF gain 100 %
};

// 5.3: [state][bw]
static const uint16_t ctrl_values[AT1846S_STATE_COUNT][2] = {
	[AT1846S_STATE_OFF] = { 0x4006, 0x7006 },
	[AT1846S_STATE_IDLE] = { 0x6006, 0x7006 },
	[AT1846S_STATE_RX] = { 0x6026, 0x7026 },
	[AT1846S_STATE_FM_TX] = { 0x6046, 0x7046 },
	[AT1846S_STATE_DMR_TX] = { 0x60C6, 0x60C6 },
};

#define RX_DETECT_MASK	0x001Fu	// reg 0x3A bits 4:0
#define RX_DETECT_BASE	0x0003u	// the mode tables' value for those bits
#define TX_SEL_KEEP	0xF9FFu	// reg 0x4E: everything but bits 10:9
#define TX_DCS_KEEP	0x383Fu	// reg 0x4E: keep bits 13:11 and 5:0
#define TX_DCS_NORMAL	0x0480u	// reg 0x4E: bits 10:9 = 10 (DCS), cdcss_sel bits 8:6 = 010 (23-bit, normal)
#define TX_DCS_INV	0x0580u	// cdcss_sel bits 8:6 = 110 (23-bit, inverted)

// --- Pure helpers ---

uint32_t at1846s_freq_word(uint32_t hz)
{
	return (uint32_t)(((uint64_t)hz * 2u + 62u) / 125u);	// hz / 62.5, rounded
}

uint16_t at1846s_state_value(at1846s_bw_t bw, at1846s_state_t state)
{
	return ctrl_values[state][bw == AT1846S_BW_25K ? 1 : 0];
}

uint16_t at1846s_ctrl_value(const at1846s_t *dev, at1846s_bw_t bw, at1846s_state_t state)
{
	if (dev->prof == NULL) {
		return at1846s_state_value(bw, state);
	}
	return dev->prof->ctrl[state][bw == AT1846S_BW_25K ? 1 : 0];
}

uint16_t at1846s_reg05_value(const at1846s_t *dev, uint32_t hz)
{
	return dev->prof != NULL && dev->prof->reg05 != NULL ? dev->prof->reg05(hz) : 0x8763u;
}

uint8_t at1846s_ctcss_threshold(uint16_t tone_reg)
{
	if (tone_reg > 24000u) {
		return 1;
	}
	return (uint8_t)((25000u - tone_reg) / 1000u);
}

bool at1846s_tone_match(uint16_t flags, at1846s_tone_t type)
{
	switch (type) {
	case AT1846S_TONE_CTCSS:
		return (flags & AT1846S_FLAG_CTCSS2) != 0;
	case AT1846S_TONE_DCS:
		return (flags & AT1846S_FLAG_DCS) != 0;
	case AT1846S_TONE_DCS_INV:
		return (flags & AT1846S_FLAG_DCS_INV) != 0;
	default:
		return true;
	}
}

// --- Register access ---

static bool cached(const at1846s_t *dev, uint8_t page, uint8_t reg)
{
	return (dev->valid[page][reg >> 5] & (1u << (reg & 31u))) != 0;
}

static void cache_set(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t val)
{
	dev->cache[page][reg] = val;
	dev->valid[page][reg >> 5] |= 1u << (reg & 31u);
}

static void cache_drop(at1846s_t *dev, uint8_t page, uint8_t reg)
{
	dev->valid[page][reg >> 5] &= ~(1u << (reg & 31u));
}

void at1846s_cache_clear(at1846s_t *dev)
{
	for (int p = 0; p < 2; p++) {
		for (int i = 0; i < 4; i++) {
			dev->valid[p][i] = 0;
		}
	}
	dev->page = -1;
}

void at1846s_attach(at1846s_t *dev, const at1846s_bus_t *bus)
{
	dev->bus = bus;
	dev->prof = NULL;
	at1846s_cache_clear(dev);
	dev->bw = -1;
	dev->mode = -1;
	dev->state = AT1846S_STATE_OFF;
}

void at1846s_set_profile(at1846s_t *dev, const at1846s_profile_t *prof)
{
	dev->prof = prof;
	dev->bw = -1;
	dev->mode = -1;
}

static bool select_page(at1846s_t *dev, uint8_t page)
{
	if (dev->page == (int8_t)page) {
		return true;
	}
	if (!dev->bus->write(dev->bus->ctx, REG_PAGE, page)) {
		dev->page = -1;
		return false;
	}
	dev->page = (int8_t)page;
	return true;
}

bool at1846s_write_force(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t val)
{
	page &= 1u;
	reg &= 0x7Fu;
	if (!select_page(dev, page)) {
		return false;
	}
	if (!dev->bus->write(dev->bus->ctx, reg, val)) {
		cache_drop(dev, page, reg);
		return false;
	}
	cache_set(dev, page, reg, val);
	return true;
}

bool at1846s_write(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t val)
{
	page &= 1u;
	reg &= 0x7Fu;
	if (cached(dev, page, reg) && dev->cache[page][reg] == val) {
		return true;
	}
	return at1846s_write_force(dev, page, reg, val);
}

bool at1846s_read(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t *val)
{
	page &= 1u;
	reg &= 0x7Fu;
	return select_page(dev, page) && dev->bus->read(dev->bus->ctx, reg, val);
}

bool at1846s_modify(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t keep, uint16_t set)
{
	uint16_t val;
	page &= 1u;
	reg &= 0x7Fu;
	if (cached(dev, page, reg)) {
		val = dev->cache[page][reg];
	} else if (!at1846s_read(dev, page, reg, &val)) {
		return false;
	}
	return at1846s_write(dev, page, reg, (uint16_t)((val & keep) | set));
}

static bool write_list(at1846s_t *dev, const reg_val_t *list, uint32_t n)
{
	bool ok = true;
	for (uint32_t i = 0; i < n; i++) {
		ok &= at1846s_write(dev, 0, list[i].reg, list[i].val);
	}
	return ok;
}

// --- Chip operations ---

bool at1846s_init(at1846s_t *dev)
{
	at1846s_cache_clear(dev);
	dev->bw = -1;
	dev->mode = -1;
	bool ok = select_page(dev, 0);
	ok &= at1846s_write_force(dev, 0, REG_CTRL, 0x0001);	// soft reset
	dev->bus->delay_ms(50);
	at1846s_cache_clear(dev);	// the reset put every register back to its default
	ok &= select_page(dev, 0);
	if (dev->prof != NULL) {
		for (uint32_t i = 0; i < dev->prof->init_len; i++) {
			const at1846s_step_t *st = &dev->prof->init[i];
			ok &= at1846s_write_force(dev, 0, st->reg, st->val);
			if (st->wait_ms != 0) {
				dev->bus->delay_ms(st->wait_ms);
			}
		}
	} else {
		ok &= write_list(dev, init_regs, sizeof(init_regs) / sizeof(init_regs[0]));
		dev->bus->delay_ms(100);
		ok &= at1846s_write(dev, 0, REG_CTRL, 0x4006);		// calibration done
		dev->bus->delay_ms(10);
	}
	ok &= at1846s_set_bandwidth(dev, AT1846S_BW_12K5);
	dev->bus->delay_ms(200);
	dev->state = AT1846S_STATE_IDLE;
	return ok;
}

bool at1846s_init_finish(at1846s_t *dev)
{
	return at1846s_write(dev, 0, 0x15, 0x1100);
}

bool at1846s_set_bandwidth(at1846s_t *dev, at1846s_bw_t bw)
{
	if (dev->bw == (int8_t)bw) {
		return true;
	}
	int col = bw == AT1846S_BW_25K ? 1 : 0;
	const bw_row_t *rows = dev->prof != NULL ? dev->prof->bw : bw_rows;
	uint32_t n = dev->prof != NULL ? dev->prof->bw_len : sizeof(bw_rows) / sizeof(bw_rows[0]);
	bool ok = true;
	for (uint32_t i = 0; i < n; i++) {
		ok &= at1846s_write(dev, rows[i].page, rows[i].reg, rows[i].val[col]);
	}
	ok &= select_page(dev, 0);
	ok &= at1846s_write_force(dev, 0, REG_CTRL, at1846s_ctrl_value(dev, bw, AT1846S_STATE_IDLE));
	dev->state = AT1846S_STATE_IDLE;
	dev->bw = ok ? (int8_t)bw : -1;
	dev->mode = -1;		// the table overwrote mode registers (0x15, 0x32, 0x3A, ...)
	return ok;
}

bool at1846s_set_mode(at1846s_t *dev, at1846s_mode_t mode)
{
	if (dev->mode == (int8_t)mode) {
		return true;
	}
	if (dev->prof != NULL && dev->prof->no_mode_tables) {
		dev->mode = (int8_t)mode;
		return true;
	}
	bool ok = mode == AT1846S_MODE_DMR ? write_list(dev, dmr_regs, sizeof(dmr_regs) / sizeof(dmr_regs[0]))
					   : write_list(dev, fm_regs, sizeof(fm_regs) / sizeof(fm_regs[0]));
	dev->mode = ok ? (int8_t)mode : -1;
	return ok;
}

static at1846s_bw_t cur_bw(const at1846s_t *dev)
{
	return dev->bw == AT1846S_BW_25K ? AT1846S_BW_25K : AT1846S_BW_12K5;
}

bool at1846s_set_freq(at1846s_t *dev, uint32_t hz)
{
	uint32_t word = at1846s_freq_word(hz);
	bool ok = at1846s_write_force(dev, 0, REG_CTRL, at1846s_ctrl_value(dev, cur_bw(dev), AT1846S_STATE_IDLE));
	dev->state = AT1846S_STATE_IDLE;
	ok &= at1846s_write_force(dev, 0, 0x05, at1846s_reg05_value(dev, hz));
	ok &= at1846s_write_force(dev, 0, 0x29, (uint16_t)((word >> 16) & 0x3FFFu));
	ok &= at1846s_write_force(dev, 0, 0x2A, (uint16_t)(word & 0xFFFFu));
	ok &= at1846s_write(dev, 0, 0x49, 0x0C15);	// squelch open / shut thresholds
	return ok;
}

bool at1846s_set_state(at1846s_t *dev, at1846s_state_t state)
{
	bool ok = at1846s_write_force(dev, 0, REG_CTRL, at1846s_ctrl_value(dev, cur_bw(dev), state));
	dev->state = (uint8_t)state;
	return ok;
}

static bool dcs_code_regs(at1846s_t *dev, uint16_t code)
{
	uint32_t word = subaudio_dcs_word(code);
	bool ok = at1846s_write(dev, 0, 0x4B, (uint16_t)(word >> 16));
	ok &= at1846s_write(dev, 0, 0x4C, (uint16_t)(word & 0xFFFFu));
	return ok;
}

bool at1846s_set_tx_tone(at1846s_t *dev, at1846s_tone_t type, uint16_t value)
{
	bool ok;
	switch (type) {
	case AT1846S_TONE_CTCSS:
		ok = at1846s_write(dev, 0, 0x4A, at1846s_ctcss_reg(value));
		ok &= at1846s_write(dev, 0, 0x4D, 0);
		ok &= at1846s_write(dev, 0, 0x4B, 0);
		ok &= at1846s_write(dev, 0, 0x4C, 0);
		ok &= at1846s_modify(dev, 0, 0x4E, TX_SEL_KEEP, 0x0600);
		return ok;
	case AT1846S_TONE_DCS:
	case AT1846S_TONE_DCS_INV:
		ok = at1846s_write(dev, 0, 0x4A, AT1846S_DCS_TONE_REG);
		ok &= at1846s_write(dev, 0, 0x4D, 0);
		ok &= dcs_code_regs(dev, value);
		ok &= at1846s_modify(dev, 0, 0x4E, TX_DCS_KEEP, type == AT1846S_TONE_DCS_INV ? TX_DCS_INV : TX_DCS_NORMAL);
		return ok;
	default:
		ok = at1846s_write(dev, 0, 0x4A, 0);
		ok &= at1846s_write(dev, 0, 0x4D, 0);
		ok &= at1846s_modify(dev, 0, 0x4E, TX_SEL_KEEP, 0);
		return ok;
	}
}

bool at1846s_set_rx_tone(at1846s_t *dev, at1846s_tone_t type, uint16_t value)
{
	bool ok;
	switch (type) {
	case AT1846S_TONE_CTCSS: {
		uint16_t reg = at1846s_ctcss_reg(value);
		uint8_t th = at1846s_ctcss_threshold(reg);
		ok = at1846s_write(dev, 0, 0x4A, 0);
		ok &= at1846s_write(dev, 0, 0x4B, 0);
		ok &= at1846s_write(dev, 0, 0x4C, 0);
		ok &= at1846s_write(dev, 0, 0x4D, reg);
		ok &= at1846s_write(dev, 0, 0x5B, (uint16_t)((th << 8) | th));
		ok &= at1846s_modify(dev, 0, 0x4E, TX_SEL_KEEP, 0);	// no TX selection left over from a DCS transmission
		ok &= at1846s_modify(dev, 0, 0x3A, (uint16_t)~RX_DETECT_MASK, 0x0008);
		return ok;
	}
	case AT1846S_TONE_DCS:
	case AT1846S_TONE_DCS_INV:
		ok = at1846s_write(dev, 0, 0x4A, AT1846S_DCS_TONE_REG);
		ok &= at1846s_write(dev, 0, 0x4D, 0);
		ok &= dcs_code_regs(dev, value);
		ok &= at1846s_modify(dev, 0, 0x4E, TX_DCS_KEEP, type == AT1846S_TONE_DCS_INV ? TX_DCS_INV : TX_DCS_NORMAL);
		ok &= at1846s_modify(dev, 0, 0x3A, (uint16_t)~RX_DETECT_MASK, type == AT1846S_TONE_DCS_INV ? 0x0004 : 0x0002);
		return ok;
	default:
		ok = at1846s_write(dev, 0, 0x4A, 0);
		ok &= at1846s_write(dev, 0, 0x4D, 0);
		ok &= at1846s_modify(dev, 0, 0x4E, TX_SEL_KEEP, 0);
		ok &= at1846s_modify(dev, 0, 0x3A, (uint16_t)~RX_DETECT_MASK, RX_DETECT_BASE);
		return ok;
	}
}

bool at1846s_read_signal(at1846s_t *dev, uint8_t *rssi_db, uint8_t *noise_db)
{
	uint16_t v;
	if (!at1846s_read(dev, 0, 0x1B, &v)) {
		return false;
	}
	*rssi_db = (uint8_t)(v >> 8);
	*noise_db = (uint8_t)(v & 0xFFu);
	return true;
}

bool at1846s_read_flags(at1846s_t *dev, uint16_t *flags)
{
	return at1846s_read(dev, 0, 0x1C, flags);
}
