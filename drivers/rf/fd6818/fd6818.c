// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// FD6818 transceiver driver (docs/spec/hardware-dm32.md 8)
#include <stddef.h>
#include "fd6818.h"

#define READ_BIT	0x80u
#define BIT_US		1u	// clock low and high time
#define READ_TURN_US	2u	// after releasing the data line, before clocking the reply in

typedef struct {
	uint8_t reg;
	uint16_t val;
} regval_t;

// 8.2 power-on init, in order (register 0x09 is a 16-entry table indexed by its top nibble)
static const regval_t init_table[] = {
	{ 0x00, 0x8000 }, { 0x00, 0x0000 }, { 0x37, 0x1D0F }, { 0x13, 0x03BE }, { 0x12, 0x037B }, { 0x11, 0x027B },
	{ 0x10, 0x007A }, { 0x14, 0x0019 }, { 0x49, 0x2A38 }, { 0x7B, 0x8420 }, { 0x48, 0x33EF }, { 0x1F, 0x5454 },
	{ 0x25, 0xC1BA }, { 0x77, 0x88EF }, { 0x3E, 0xA037 }, { 0x13, 0x037E }, { 0x7D, 0xE35E }, { 0xB3, 0xFF00 },
	{ 0x33, 0xFE00 }, { 0xB4, 0x0000 }, { 0x34, 0x0009 }, { 0xB3, 0xFE00 }, { 0x33, 0xFC00 }, { 0xB4, 0x0009 },
	{ 0x34, 0x0019 }, { 0x10, 0x007A }, { 0x11, 0x027B }, { 0x12, 0x037B }, { 0x13, 0x03BE }, { 0x14, 0x0019 },
	{ 0x1F, 0x5454 }, { 0x30, 0xBFF1 }, { 0x37, 0x1D1F }, { 0x3E, 0xA037 }, { 0x47, 0x0100 }, { 0x48, 0xBBBF },
	{ 0x49, 0x2A38 }, { 0x4E, 0x4708 }, { 0x4F, 0x302F }, { 0x52, 0x028F }, { 0x7B, 0x8420 },
	{ 0x09, 0x006F }, { 0x09, 0x106B }, { 0x09, 0x2067 }, { 0x09, 0x3062 }, { 0x09, 0x4050 }, { 0x09, 0x5047 },
	{ 0x09, 0x603A }, { 0x09, 0x702C }, { 0x09, 0x8041 }, { 0x09, 0x9037 }, { 0x09, 0xA025 }, { 0x09, 0xB017 },
	{ 0x09, 0xC0E4 }, { 0x09, 0xD0CB }, { 0x09, 0xE0B5 }, { 0x09, 0xF09F },
};

// 8.2 post-init: receiver parked at 144.000 MHz, AF muted, volume
static const regval_t post_init_table[] = {
	{ 0x47, 0x0000 }, { 0x13, 0x037E }, { 0x47, 0x6042 }, { 0x7E, 0x302E }, { 0x2B, 0xC000 }, { 0x43, 0x3028 },
	{ 0x78, 0x6968 }, { 0x30, 0x0000 }, { 0x38, 0xBA00 }, { 0x39, 0x00DB }, { 0x32, 0x0244 }, { 0x07, 0x0470 },
	{ 0x07, 0x21CE }, { 0x51, 0x0000 }, { 0x3F, 0x0000 }, { 0x37, 0x1F0F }, { 0x30, 0x0000 }, { 0x30, 0xBFF1 },
	{ 0x47, 0x6042 }, { 0x48, 0xBBBF },
};

static const regval_t fm_table[] = {
	{ 0x30, 0x0000 }, { 0x13, 0x037E }, { 0x47, 0x6042 }, { 0x7E, 0x302E }, { 0x2B, 0xC000 }, { 0x43, 0x3028 },
	{ 0x07, 0x0470 }, { 0x07, 0x21CE }, { 0x51, 0x0000 }, { 0x3F, 0x0000 }, { 0x37, 0x1F0F }, { 0x30, 0x0000 },
	{ 0x30, 0xBFF1 }, { 0x47, 0x6042 }, { 0x48, 0xBBBF },
};

static const regval_t dmr_table[] = {
	{ 0x47, 0x0000 }, { 0x30, 0x0000 }, { 0x30, 0x0000 }, { 0x13, 0x037E }, { 0x47, 0x0140 }, { 0x48, 0xBBBF },
	{ 0x7E, 0xB03F }, { 0x2B, 0x0700 }, { 0x43, 0x4048 }, { 0x78, 0x6968 }, { 0x30, 0x0000 }, { 0x32, 0x0244 },
	{ 0x37, 0x1F1F }, { 0x30, 0x0000 }, { 0x30, 0xBFF1 }, { 0x47, 0x0140 }, { 0x3F, 0x0000 }, { 0xC7, 0x0140 },
	{ 0x47, 0x0140 },
};

// 8.3 band select: first row whose threshold the frequency reaches (10 Hz units)
static const struct {
	uint32_t from;
	uint16_t r1a, r40;
} bands[] = {
	{ 73999999u, 0x1F80, 0x3AF0 }, { 36999999u, 0x2F50, 0x39F0 }, { 26000000u, 0x9F30, 0x385C },
	{ 18499999u, 0x3F48, 0x38F0 }, { 12399999u, 0xAF28, 0x375C }, { 9299999u, 0x4F44, 0x37F0 },
	{ 6199999u, 0xBF24, 0x365C }, { 4699999u, 0x5F42, 0x36F0 }, { 3099999u, 0xCF22, 0x355C },
	{ 2399999u, 0x6F41, 0x35F0 }, { 1599999u, 0xDF21, 0x345C },
};

bool fd6818_band_words(uint32_t freq_10hz, uint16_t *r1a, uint16_t *r40)
{
	for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
		if (freq_10hz >= bands[i].from) {
			*r1a = bands[i].r1a;
			*r40 = bands[i].r40;
			return true;
		}
	}
	return false;
}

uint16_t fd6818_ctcss_word(uint16_t hz_x10)
{
	// Hz x 20.64888 = (Hz x 10) x 2.064888
	return (uint16_t)(((uint32_t)hz_x10 * 2064888u + 500000u) / 1000000u);
}

void fd6818_attach(fd6818_t *dev, const fd6818_bus_t *bus)
{
	dev->bus = bus;
	dev->freq_10hz = 0;
	dev->bw_reg = 0;
	dev->state = 0;
	dev->writes = 0;
}

static uint32_t enter(const fd6818_bus_t *b)
{
	return b->crit_enter != NULL ? b->crit_enter() : 0;
}

static void leave(const fd6818_bus_t *b, uint32_t s)
{
	if (b->crit_exit != NULL) {
		b->crit_exit(s);
	}
}

static void send_bits(const fd6818_bus_t *b, uint32_t v, int n)
{
	for (int i = n - 1; i >= 0; i--) {
		b->clk(false);
		b->data_out(((v >> i) & 1u) != 0);
		b->delay_us(BIT_US);
		b->clk(true);	// sampled by the chip on this edge
		b->delay_us(BIT_US);
	}
}

void fd6818_write(fd6818_t *dev, uint8_t reg, uint16_t val)
{
	const fd6818_bus_t *b = dev->bus;
	uint32_t s = enter(b);
	b->cs(false);
	// The address byte goes out as given: the init table writes 0xB3, 0xB4 and 0xC7, which have the read bit set.
	// What the chip makes of those is unknown; they are sent exactly as the working firmware sends them
	send_bits(b, (uint32_t)reg << 16 | val, 24);
	b->clk(false);
	b->cs(true);
	leave(b, s);
	dev->writes++;
	if (reg == FD6818_REG_STATE) {
		dev->state = val;
	} else if (reg == FD6818_REG_BANDWIDTH) {
		dev->bw_reg = val;
	}
}

uint16_t fd6818_read(fd6818_t *dev, uint8_t reg)
{
	const fd6818_bus_t *b = dev->bus;
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

static void write_table(fd6818_t *dev, const regval_t *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		fd6818_write(dev, t[i].reg, t[i].val);
	}
}

void fd6818_init(fd6818_t *dev)
{
	const fd6818_bus_t *b = dev->bus;
	b->cs(true);
	b->clk(false);
	fd6818_write(dev, init_table[0].reg, init_table[0].val);	// soft reset
	if (b->delay_ms != NULL) {
		b->delay_ms(10);
	}
	write_table(dev, init_table + 1, sizeof(init_table) / sizeof(init_table[0]) - 1);
	write_table(dev, post_init_table, sizeof(post_init_table) / sizeof(post_init_table[0]));
	dev->freq_10hz = 14400000u;
}

void fd6818_set_mode(fd6818_t *dev, fd6818_mode_t mode)
{
	if (mode == FD6818_MODE_DMR) {
		write_table(dev, dmr_table, sizeof(dmr_table) / sizeof(dmr_table[0]));
	} else {
		write_table(dev, fm_table, sizeof(fm_table) / sizeof(fm_table[0]));
	}
}

void fd6818_set_freq(fd6818_t *dev, uint32_t hz)
{
	uint32_t f = (hz + 5u) / 10u;
	uint16_t r1a, r40;
	fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);
	if (fd6818_band_words(f, &r1a, &r40)) {
		fd6818_write(dev, FD6818_REG_BAND_A, r1a);
		fd6818_write(dev, FD6818_REG_BAND_B, r40);
	}
	fd6818_write(dev, FD6818_REG_FREQ_LO, (uint16_t)(f & 0xFFFFu));
	fd6818_write(dev, FD6818_REG_FREQ_HI, (uint16_t)(f >> 16));
	dev->freq_10hz = f;
}

void fd6818_set_bandwidth(fd6818_t *dev, uint16_t bw_reg)
{
	fd6818_write(dev, FD6818_REG_BANDWIDTH, bw_reg);
}

void fd6818_set_state(fd6818_t *dev, uint16_t state)
{
	if (state != FD6818_STATE_IDLE && dev->state != FD6818_STATE_IDLE) {
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);	// never RX <-> TX directly
	}
	fd6818_write(dev, FD6818_REG_STATE, state);
}

void fd6818_fm_tx_prepare(fd6818_t *dev)
{
	fd6818_write(dev, 0x3F, 0x0000);
	fd6818_write(dev, 0x40, 0x34EA);
	fd6818_write(dev, 0x37, 0x1D0F);
}

void fd6818_set_tx_ctcss(fd6818_t *dev, uint16_t hz_x10)
{
	if (hz_x10 == 0) {
		fd6818_write(dev, FD6818_REG_SUBAUDIO, 0x0000);
		return;
	}
	fd6818_write(dev, FD6818_REG_CTCSS_FREQ, fd6818_ctcss_word(hz_x10));
	fd6818_write(dev, FD6818_REG_SUBAUDIO, 0x9045);	// TX CTCSS on, gain 0x45
}

void fd6818_set_rx_ctcss(fd6818_t *dev, uint16_t hz_x10)
{
	if (hz_x10 == 0) {
		fd6818_write(dev, FD6818_REG_SUBAUDIO, 0x0000);
		return;
	}
	fd6818_write(dev, FD6818_REG_CTCSS_FREQ, fd6818_ctcss_word(hz_x10));
	fd6818_write(dev, 0x52, 0x028F);			// detect thresholds
	fd6818_write(dev, FD6818_REG_SUBAUDIO, 0x1000);
}

void fd6818_dmr_burst(fd6818_t *dev, bool tx, uint16_t rx_bw)
{
	if (tx) {
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_TX);
	} else {
		fd6818_write(dev, FD6818_REG_BANDWIDTH, rx_bw);
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_RX);
	}
}
