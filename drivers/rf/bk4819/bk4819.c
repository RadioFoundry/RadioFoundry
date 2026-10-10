// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// BK4819 transceiver driver (docs/spec/hardware-db25d.md 4)
#include <stddef.h>
#include "bk4819.h"

#define READ_BIT	0x80u
#define BIT_US		1u	// clock low and high time
#define READ_TURN_US	2u	// after releasing the data line, before clocking the reply in
#define RESET_MS	10u

typedef struct {
	uint8_t reg;
	uint16_t val;
} regval_t;

// 4.3 power-on table shared by both chips, after the soft reset. Register 0x4F is the one value that differs
static const regval_t init_common[] = {
	{ 0x33, 0x0003 }, { 0x34, 0x0000 }, { 0x35, 0x0000 }, { 0x37, 0x1D0F }, { 0x47, 0x6042 }, { 0x13, 0x03BE },
	{ 0x12, 0x037B }, { 0x11, 0x027B }, { 0x10, 0x007A }, { 0x14, 0x0019 }, { 0x49, 0x2A38 }, { 0x7B, 0x8420 },
	{ 0x7D, 0xE950 }, { 0x48, 0xB3BF }, { 0x1F, 0x5454 }, { 0x3E, 0xA037 }, { 0x3F, 0x07EE }, { 0x2A, 0x4F18 },
	{ 0x28, 0x6B00 }, { 0x53, 0xC074 }, { 0x40, 0x352A }, { 0x29, 0xAB24 }, { 0x4B, 0x7126 }, { 0x4E, 0x6F15 },
};

static const regval_t init_main_tail[] = {
	{ 0x4F, 0x3332 }, { 0x27, 0xA430 }, { 0x48, 0xB3B3 }, { 0x37, 0x1D9F }, { 0x30, 0x0302 }, { 0x47, 0x6042 },
	{ 0x30, 0x0200 }, { 0x33, 0x0000 }, { 0x30, 0x0200 }, { 0x47, 0x6042 }, { 0x30, 0x0200 }, { 0x37, 0x1D0F },
	{ 0x7D, 0xE34A }, { 0x30, 0x0000 },
};

static const regval_t init_lo_tail[] = {
	{ 0x4F, 0x2F2E }, { 0x27, 0xA430 }, { 0x48, 0xB3B3 }, { 0x37, 0x1D9F }, { 0x30, 0x0302 }, { 0x47, 0x6042 },
	{ 0x30, 0x0000 }, { 0x33, 0x0000 }, { 0x30, 0x0000 }, { 0x47, 0x6042 }, { 0x30, 0x0000 }, { 0x37, 0x1D0F },
	{ 0x51, 0x0000 }, { 0x40, 0x0000 },
};

// After the 0x73 read-modify-write (bit 4 set: AFC off)
static const regval_t init_lo_after_afc[] = {
	{ 0x47, 0x6943 }, { 0x7E, 0x3006 }, { 0x43, 0x4048 }, { 0x47, 0x6042 },
};

uint16_t bk4819_squelch_rssi(int16_t open_dbm, uint8_t hyst_db)
{
	int32_t open = 2 * ((int32_t)open_dbm + 160);
	open = open < 0 ? 0 : open > 0xFF ? 0xFF : open;
	int32_t close = open - 2 * (int32_t)hyst_db;
	if (close < 0) {
		close = 0;
	}
	return (uint16_t)(open << 8 | close);
}

uint16_t bk4819_ctcss_word(uint16_t hz_x10)
{
	// Hz x 20.64888 = (Hz x 10) x 258111 / 125000, in 64 bits for any input
	return (uint16_t)(((uint64_t)hz_x10 * 258111u + 62500u) / 125000u);
}

void bk4819_attach(bk4819_t *dev, const bk4819_bus_t *bus)
{
	dev->bus = bus;
	dev->freq_10hz = 0;
	dev->state = 0;
	dev->af_gain = 0xB3B3u;
	dev->writes = 0;
}

static uint32_t enter(const bk4819_bus_t *b)
{
	return b->crit_enter != NULL ? b->crit_enter() : 0;
}

static void leave(const bk4819_bus_t *b, uint32_t s)
{
	if (b->crit_exit != NULL) {
		b->crit_exit(s);
	}
}

static void send_bits(const bk4819_bus_t *b, uint32_t v, int n)
{
	for (int i = n - 1; i >= 0; i--) {
		b->clk(false);
		b->data_out(((v >> i) & 1u) != 0);
		b->delay_us(BIT_US);
		b->clk(true);	// sampled by the chip on this edge
		b->delay_us(BIT_US);
	}
}

void bk4819_write(bk4819_t *dev, uint8_t reg, uint16_t val)
{
	const bk4819_bus_t *b = dev->bus;
	uint32_t s = enter(b);
	b->cs(false);
	send_bits(b, (uint32_t)(reg & 0x7Fu) << 16 | val, 24);
	b->clk(false);
	b->cs(true);
	leave(b, s);
	dev->writes++;
	if (reg == BK4819_REG_ENABLES) {
		dev->state = val;
	} else if (reg == BK4819_REG_AF_GAIN) {
		dev->af_gain = val;
	}
}

uint16_t bk4819_read(bk4819_t *dev, uint8_t reg)
{
	const bk4819_bus_t *b = dev->bus;
	uint32_t s = enter(b);
	b->cs(false);
	send_bits(b, (reg & 0x7Fu) | READ_BIT, 8);
	b->clk(false);
	b->data_release();
	b->delay_us(READ_TURN_US);
	uint16_t v = 0;
	for (int i = 0; i < 16; i++) {
		b->clk(false);
		b->delay_us(BIT_US);
		b->clk(true);
		v = (uint16_t)((v << 1) | (b->data_in() ? 1u : 0u));	// read while the clock is high
		b->delay_us(BIT_US);
	}
	b->clk(false);
	b->cs(true);
	leave(b, s);
	return v;
}

static void write_table(bk4819_t *dev, const regval_t *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		bk4819_write(dev, t[i].reg, t[i].val);
	}
}

static void reset(bk4819_t *dev)
{
	const bk4819_bus_t *b = dev->bus;
	b->cs(true);
	b->clk(false);
	bk4819_write(dev, BK4819_REG_ID, 0x8000);
	bk4819_write(dev, BK4819_REG_ID, 0x0000);
	if (b->delay_ms != NULL) {
		b->delay_ms(RESET_MS);
	}
	write_table(dev, init_common, sizeof(init_common) / sizeof(init_common[0]));
}

void bk4819_init_main(bk4819_t *dev)
{
	reset(dev);
	write_table(dev, init_main_tail, sizeof(init_main_tail) / sizeof(init_main_tail[0]));
	dev->freq_10hz = 0;
}

void bk4819_init_lo(bk4819_t *dev, uint16_t pa)
{
	reset(dev);
	write_table(dev, init_lo_tail, sizeof(init_lo_tail) / sizeof(init_lo_tail[0]));
	bk4819_write(dev, BK4819_REG_AFC, (uint16_t)(bk4819_read(dev, BK4819_REG_AFC) | BK4819_AFC_OFF));
	write_table(dev, init_lo_after_afc, sizeof(init_lo_after_afc) / sizeof(init_lo_after_afc[0]));
	bk4819_write(dev, BK4819_REG_PA, pa);
	bk4819_lo_off(dev);
	dev->freq_10hz = 0;
}

static void write_freq(bk4819_t *dev, uint32_t hz)
{
	uint32_t f = (hz + 5u) / 10u;
	bk4819_write(dev, BK4819_REG_FREQ_LO, (uint16_t)(f & 0xFFFFu));
	bk4819_write(dev, BK4819_REG_FREQ_HI, (uint16_t)(f >> 16));
	dev->freq_10hz = f;
}

void bk4819_set_freq(bk4819_t *dev, uint32_t hz)
{
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_IDLE);
	write_freq(dev, hz);
}

void bk4819_set_state(bk4819_t *dev, uint16_t state)
{
	if (state != BK4819_STATE_IDLE && dev->state != BK4819_STATE_IDLE) {
		bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_IDLE);
	}
	bk4819_write(dev, BK4819_REG_ENABLES, state);
}

void bk4819_set_bandwidth(bk4819_t *dev, uint16_t bw_reg)
{
	bk4819_write(dev, BK4819_REG_BANDWIDTH, bw_reg);
}

void bk4819_set_af(bk4819_t *dev, uint16_t af)
{
	bk4819_write(dev, BK4819_REG_AF, af);
}

void bk4819_set_af_gain(bk4819_t *dev, uint8_t gain)
{
	if (gain > 15) {
		gain = 15;
	}
	bk4819_write(dev, BK4819_REG_AF_GAIN, (uint16_t)((dev->af_gain & 0xFFF0u) | gain));
}

// 4.3 "LO tune": the enables go through idle with the power register set in between, then to the TX value
void bk4819_lo_tune(bk4819_t *dev, uint32_t hz)
{
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_IDLE);
	write_freq(dev, hz);
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_IDLE);
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_TX);
}

void bk4819_lo_off(bk4819_t *dev)
{
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_IDLE);
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_OFF);
}

void bk4819_set_rx_ctcss(bk4819_t *dev, uint16_t hz_x10)
{
	if (hz_x10 == 0) {
		bk4819_write(dev, BK4819_REG_SUBAUDIO, 0x0000);
		return;
	}
	bk4819_write(dev, BK4819_REG_CTCSS_FREQ, bk4819_ctcss_word(hz_x10));
	bk4819_write(dev, 0x52, 0x028F);			// detect thresholds (the FD6818's)
	bk4819_write(dev, BK4819_REG_SUBAUDIO, 0x1000);
}

bool bk4819_ctcss_found(bk4819_t *dev)
{
	return (bk4819_read(dev, BK4819_REG_CSS_STATUS) & 0x0400u) != 0;
}
