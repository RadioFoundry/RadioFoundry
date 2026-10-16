// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// FD6818 transceiver driver (docs/spec/hardware-dm32.md 8)
#include <stddef.h>
#include "fd6818.h"
#include "rf/subaudio/subaudio.h"

#define READ_BIT	0x80u
#define BIT_US		1u	// clock low and high time
#define READ_TURN_US	2u	// after releasing the data line, before clocking the reply in

typedef fd6818_regval_t regval_t;

// 8.2 power-on init, in order (register 0x09 is a 16-entry table indexed by its top nibble). The earlier port also
// sent 0xB3, 0xB4 (and 0xC7 in the DMR set): with bit 7 set those are read cycles, during which the MCU kept driving
// the data line against the chip. The radio's own firmware never sends them, so they are left out
static const regval_t init_table[] = {
	{ 0x00, 0x8000 }, { 0x00, 0x0000 }, { 0x37, 0x1D0F }, { 0x13, 0x03BE }, { 0x12, 0x037B }, { 0x11, 0x027B },
	{ 0x10, 0x007A }, { 0x14, 0x0019 }, { 0x49, 0x2A38 }, { 0x7B, 0x8420 }, { 0x48, 0x33EF }, { 0x1F, 0x5454 },
	{ 0x25, 0xC1BA }, { 0x77, 0x88EF }, { 0x3E, 0xA037 }, { 0x13, 0x037E }, { 0x7D, 0xE35E }, { 0x33, 0xFE00 },
	{ 0x34, 0x0009 }, { 0x33, 0xFC00 }, { 0x34, 0x0019 }, { 0x10, 0x007A }, { 0x11, 0x027B }, { 0x12, 0x037B },
	{ 0x13, 0x03BE }, { 0x14, 0x0019 }, { 0x1F, 0x5454 }, { 0x30, 0xBFF1 }, { 0x37, 0x1D1F }, { 0x3E, 0xA037 }, { 0x47, 0x0100 }, { 0x48, 0xBBBF },
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
	{ 0x37, 0x1F1F }, { 0x30, 0x0000 }, { 0x30, 0xBFF1 }, { 0x47, 0x0140 }, { 0x3F, 0x0000 }, { 0x47, 0x0140 },
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

#define TBL_N(t)	(uint16_t)(sizeof(t) / sizeof((t)[0]))

static const regval_t rx_dm32[] = { { 0x30, FD6818_STATE_RX } };
static const regval_t tx_dm32[] = { { 0x30, FD6818_STATE_TX } };
static const regval_t fm_tx_prepare_dm32[] = { { 0x3F, 0x0000 }, { 0x40, 0x34EA }, { 0x37, 0x1D0F } };

const fd6818_variant_t fd6818_variant_dm32 = {
	.init = init_table, .init_n = TBL_N(init_table),
	.post_init = post_init_table, .post_init_n = TBL_N(post_init_table),
	.fm = fm_table, .fm_n = TBL_N(fm_table),
	.dmr = dmr_table, .dmr_n = TBL_N(dmr_table),
	.rx = rx_dm32, .rx_n = TBL_N(rx_dm32),
	.tx = tx_dm32, .tx_n = TBL_N(tx_dm32),
	.fm_tx_prepare = fm_tx_prepare_dm32, .fm_tx_prepare_n = TBL_N(fm_tx_prepare_dm32),
	.bw_fm_25k = FD6818_BW_FM_25K, .bw_fm_12k5 = FD6818_BW_FM_12K5, .bw_dmr = FD6818_BW_DMR,
	.band_words = fd6818_band_words,
	.park_10hz = 14400000u,
};

uint16_t fd6818_ctcss_word(uint16_t hz_x10)
{
	// Hz x 20.64888 = (Hz x 10) x 2.064888 = (Hz x 10) x 258111 / 125000 (exact; fits 32 bits up to 1664 Hz x 10,
	// so the product is done in 64 bits for any input)
	return (uint16_t)(((uint64_t)hz_x10 * 258111u + 62500u) / 125000u);
}

void fd6818_attach(fd6818_t *dev, const fd6818_bus_t *bus)
{
	fd6818_attach_variant(dev, bus, &fd6818_variant_dm32);
}

void fd6818_attach_variant(fd6818_t *dev, const fd6818_bus_t *bus, const fd6818_variant_t *variant)
{
	dev->bus = bus;
	dev->variant = variant;
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
	// The address byte goes out as given: bit 7 is the chip's read flag, so callers write registers below 0x80
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
	const fd6818_variant_t *v = dev->variant;
	fd6818_write(dev, v->init[0].reg, v->init[0].val);	// soft reset
	if (b->delay_ms != NULL) {
		b->delay_ms(10);
	}
	write_table(dev, v->init + 1, (size_t)v->init_n - 1);
	write_table(dev, v->post_init, v->post_init_n);
	dev->freq_10hz = v->park_10hz;
}

void fd6818_set_mode(fd6818_t *dev, fd6818_mode_t mode)
{
	const fd6818_variant_t *v = dev->variant;
	if (mode == FD6818_MODE_DMR) {
		if (v->dmr != NULL) {
			write_table(dev, v->dmr, v->dmr_n);
		}
	} else {
		write_table(dev, v->fm, v->fm_n);
	}
}

void fd6818_set_freq(fd6818_t *dev, uint32_t hz)
{
	uint32_t f = (hz + 5u) / 10u;
	uint16_t r1a, r40;
	fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);
	if (dev->variant->band_words(f, &r1a, &r40)) {
		if (r1a != 0) {
			fd6818_write(dev, FD6818_REG_BAND_A, r1a);
		}
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
	const fd6818_variant_t *v = dev->variant;
	if (state != FD6818_STATE_IDLE && dev->state != FD6818_STATE_IDLE) {
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);	// never RX <-> TX directly
	}
	if (state == FD6818_STATE_RX) {
		write_table(dev, v->rx, v->rx_n);
	} else if (state == FD6818_STATE_TX) {
		write_table(dev, v->tx, v->tx_n);
	} else {
		fd6818_write(dev, FD6818_REG_STATE, state);
	}
}

void fd6818_fm_tx_prepare(fd6818_t *dev)
{
	write_table(dev, dev->variant->fm_tx_prepare, dev->variant->fm_tx_prepare_n);
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

// 0x51: b15 sub-audio on, b13 DCS inverted, b12 CTCSS (0 = DCS); b6:0 gain
#define SUBAU_ON	0x8000u
#define SUBAU_DCS_INV	0x2000u
#define SUBAU_CTCSS	0x1000u
#define SUBAU_GAIN	0x45u
#define CTCSS_WORD_ALT	0x2000u		// 0x07 bit 13: the second tone word (the tail tone)
#define DCS_RATE_WORD	0x0AD7u		// 134.4 Hz, the DCS bit rate
#define DCS_HIGH_HALF	0x8000u

void fd6818_set_rx_tone(fd6818_t *dev, fd6818_tone_t type, uint16_t value)
{
	switch (type) {
	case FD6818_TONE_CTCSS:
		fd6818_write(dev, FD6818_REG_CTCSS_FREQ, fd6818_ctcss_word(value));
		fd6818_write(dev, FD6818_REG_CTCSS_FREQ, CTCSS_WORD_ALT | 0x01CEu);
		fd6818_write(dev, FD6818_REG_CSS_THRESH, 0x028F);
		fd6818_write(dev, FD6818_REG_SUBAUDIO, SUBAU_ON | SUBAU_CTCSS | SUBAU_GAIN);
		break;
	case FD6818_TONE_DCS:
	case FD6818_TONE_DCS_INV: {
		uint32_t word = subaudio_dcs_word(value);
		fd6818_write(dev, FD6818_REG_CTCSS_FREQ, DCS_RATE_WORD);	// the rate first
		fd6818_write(dev, FD6818_REG_DCS_CODE, (uint16_t)(word & 0xFFFu));
		fd6818_write(dev, FD6818_REG_DCS_CODE, (uint16_t)(DCS_HIGH_HALF | ((word >> 12) & 0xFFFu)));
		fd6818_write(dev, FD6818_REG_SUBAUDIO,
			(uint16_t)(SUBAU_ON | (type == FD6818_TONE_DCS_INV ? SUBAU_DCS_INV : 0u) | SUBAU_GAIN));
		break;
	}
	default:
		fd6818_write(dev, FD6818_REG_CTCSS_FREQ, 0x0470);
		fd6818_write(dev, FD6818_REG_CTCSS_FREQ, CTCSS_WORD_ALT | 0x01CEu);
		fd6818_write(dev, FD6818_REG_SUBAUDIO, 0x0000);
		break;
	}
}

bool fd6818_tone_found(uint16_t r0c, fd6818_tone_t type)
{
	switch (type) {
	case FD6818_TONE_CTCSS: return (r0c & (1u << 10)) != 0;
	case FD6818_TONE_DCS: return (r0c & (1u << 14)) != 0;
	case FD6818_TONE_DCS_INV: return (r0c & (1u << 15)) != 0;
	default: return true;
	}
}

bool fd6818_rx_tone_ok(fd6818_t *dev, fd6818_tone_t type)
{
	return type == FD6818_TONE_NONE || fd6818_tone_found(fd6818_read(dev, FD6818_REG_CSS_STATUS), type);
}

void fd6818_rx_sleep(fd6818_t *dev, bool sleep, fd6818_mode_t mode)
{
	if (sleep) {
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);
		fd6818_write(dev, FD6818_REG_POWER, mode == FD6818_MODE_DMR ? 0x1D00 : 0x1D11);
	} else {
		fd6818_write(dev, FD6818_REG_POWER, 0x1F1F);
		fd6818_write(dev, FD6818_REG_POWER, 0x1F0F);
	}
}

void fd6818_dmr_burst(fd6818_t *dev, bool tx, uint16_t rx_bw)
{
	const fd6818_variant_t *v = dev->variant;
	if (tx ? v->burst_tx != NULL : v->burst_rx != NULL) {
		write_table(dev, tx ? v->burst_tx : v->burst_rx, tx ? v->burst_tx_n : v->burst_rx_n);
		return;
	}
	if (tx) {
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);
		write_table(dev, dev->variant->tx, dev->variant->tx_n);
	} else {
		fd6818_write(dev, FD6818_REG_BANDWIDTH, rx_bw);
		fd6818_write(dev, FD6818_REG_STATE, FD6818_STATE_IDLE);
		write_table(dev, dev->variant->rx, dev->variant->rx_n);
	}
}
