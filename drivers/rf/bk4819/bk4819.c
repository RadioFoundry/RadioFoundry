// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// BK4819 transceiver driver (docs/spec/hardware-db25d.md 4)
#include <stddef.h>
#include "bk4819.h"
#include "rf/subaudio/subaudio.h"

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
	{ 0x7D, 0xE34A }, { 0x30, BK4819_STATE_IDLE_MAIN },
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

uint16_t bk4819_ctcss_word_tx(uint16_t hz_x10)
{
	return (uint16_t)(((uint64_t)hz_x10 * 258111u) / 125000u);
}

uint16_t bk4819_tone_word(uint16_t hz)
{
	// Hz x 10.32444 = Hz x 1290555 / 125000
	return (uint16_t)(((uint64_t)hz * 1290555u + 62500u) / 125000u);
}

uint16_t bk4819_pa_reg(uint8_t level)
{
	if (level == 0) {
		return 0;
	}
	if (level > 15) {
		level = 15;
	}
	return (uint16_t)(0x80u | (uint16_t)(level * 63u / 15u));
}

void bk4819_attach(bk4819_t *dev, const bk4819_bus_t *bus)
{
	dev->bus = bus;
	dev->freq_10hz = 0;
	dev->state = 0;
	dev->af_gain = 0xB3B3u;
	dev->writes = 0;
	dev->digital = false;
	dev->saved_27 = 0;
	dev->saved_48 = 0;
	dev->tone_hz = 0;
	dev->tone_gain = 0;
	dev->idle = BK4819_STATE_IDLE;
	dev->rx_inverted = false;
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
	dev->idle = BK4819_STATE_IDLE_MAIN;
	dev->freq_10hz = 0;
}

void bk4819_init_lo(bk4819_t *dev, uint16_t pa)
{
	reset(dev);
	write_table(dev, init_lo_tail, sizeof(init_lo_tail) / sizeof(init_lo_tail[0]));
	bk4819_write(dev, BK4819_REG_AFC, (uint16_t)(bk4819_read(dev, BK4819_REG_AFC) | BK4819_AFC_OFF));
	write_table(dev, init_lo_after_afc, sizeof(init_lo_after_afc) / sizeof(init_lo_after_afc[0]));
	bk4819_write(dev, BK4819_REG_PA, pa);
	dev->idle = BK4819_STATE_IDLE;
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
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	write_freq(dev, hz);
}

void bk4819_set_state(bk4819_t *dev, uint16_t state)
{
	if (state == BK4819_STATE_IDLE) {
		state = dev->idle;
	}
	if (state != dev->idle && dev->state != dev->idle) {
		bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	}
	bk4819_write(dev, BK4819_REG_ENABLES, state);
}

void bk4819_set_bandwidth(bk4819_t *dev, uint16_t bw_reg)
{
	bk4819_write(dev, 0x52, 0x028F);	// detect thresholds, written by the stock channel set before the bandwidth
	bk4819_write(dev, BK4819_REG_BANDWIDTH, bw_reg);
	bk4819_write(dev, 0x4F, bw_reg == BK4819_BW_25K ? BK4819_4F_25K : BK4819_4F_12K5);
}

void bk4819_analog_mode(bk4819_t *dev)
{
	bk4819_write(dev, 0x2A, 0x4F18);
	bk4819_write(dev, BK4819_REG_AFC, (uint16_t)(bk4819_read(dev, BK4819_REG_AFC) & ~BK4819_AFC_OFF));
	bk4819_write(dev, BK4819_REG_AF, BK4819_AF_MUTE);
	bk4819_write(dev, BK4819_REG_AGC_DC, 0x302E);
	bk4819_write(dev, BK4819_REG_AF_FILTER, 0xC000);
	bk4819_write(dev, BK4819_REG_TX_AUDIO, BK4819_TX_AUDIO_LIVE);
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

// 4.5 DMR receive: the stock digital mode set, in its order. 0x27 and 0x48 are saved for the end; the stock firmware
// leaves its digital values there when it goes back to analog, ODR doesn't
static const regval_t dmr_rx_set[] = {
	{ 0x2A, 0x4711 }, { BK4819_REG_SUBAUDIO, 0x0000 }, { 0x52, 0x028F }, { BK4819_REG_AF, 0x6943 },
	{ BK4819_REG_AGC_DC, 0x3006 }, { BK4819_REG_BANDWIDTH, BK4819_BW_DMR }, { 0x40, 0x0000 }, { 0x27, 0x0430 },
	{ BK4819_REG_AF_FILTER, 0x8707 }, { BK4819_REG_AF_GAIN, BK4819_AF_GAIN_DMR }, { BK4819_REG_AF, BK4819_AF_DMR },
};

void bk4819_dmr_rx_enter(bk4819_t *dev)
{
	if (!dev->digital) {
		dev->saved_27 = bk4819_read(dev, 0x27);
		dev->saved_48 = dev->af_gain;
		dev->digital = true;
	}
	bk4819_write(dev, dmr_rx_set[0].reg, dmr_rx_set[0].val);
	bk4819_write(dev, dmr_rx_set[1].reg, dmr_rx_set[1].val);
	bk4819_write(dev, BK4819_REG_AFC, (uint16_t)(bk4819_read(dev, BK4819_REG_AFC) | BK4819_AFC_OFF));
	write_table(dev, &dmr_rx_set[2], sizeof(dmr_rx_set) / sizeof(dmr_rx_set[0]) - 2u);
}

// The stock analog mode set (it also turns the AFC back on and mutes the AF), then what ODR's analog receive had
void bk4819_dmr_rx_end(bk4819_t *dev)
{
	if (!dev->digital) {
		return;
	}
	dev->digital = false;
	bk4819_analog_mode(dev);
	bk4819_write(dev, 0x27, dev->saved_27);
	bk4819_write(dev, BK4819_REG_AF_GAIN, dev->saved_48);
}

// 4.3 "LO tune": the enables go through idle with the power register set in between, then to the TX value. The
// frequency is written only when it changed: the stock firmware leaves the LO's frequency alone across a transmission
void bk4819_lo_tune(bk4819_t *dev, uint32_t hz)
{
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	if (dev->freq_10hz != (hz + 5u) / 10u) {
		write_freq(dev, hz);
	}
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_TX);
}

// Stock TX: the LO's 0x30 = 0 and nothing else (0x37, 0x36 and the frequency stay)
void bk4819_lo_off(bk4819_t *dev)
{
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
}

// The DCS word in 0x08: the low 12 bits, then the high 12 bits marked by bit 15
static void dcs_code(bk4819_t *dev, uint16_t code)
{
	uint32_t word = subaudio_dcs_word(code);
	bk4819_write(dev, BK4819_REG_DCS_CODE, (uint16_t)(word & 0xFFFu));
	bk4819_write(dev, BK4819_REG_DCS_CODE, (uint16_t)(BK4819_DCS_HIGH_HALF | ((word >> 12) & 0xFFFu)));
}

static uint16_t dcs_subaudio(bk4819_tone_t type)
{
	return (uint16_t)(BK4819_SUBAUDIO_DCS | (type == BK4819_TONE_DCS_INV ? BK4819_SUBAUDIO_DCS_INV : 0u));
}

void bk4819_set_rx_tone(bk4819_t *dev, bk4819_tone_t type, uint16_t value)
{
	switch (type) {
	case BK4819_TONE_CTCSS:
		bk4819_write(dev, BK4819_REG_CTCSS_FREQ, bk4819_ctcss_word(value));
		bk4819_write(dev, 0x52, 0x028F);			// detect thresholds (the FD6818's)
		bk4819_write(dev, BK4819_REG_SUBAUDIO, 0x1000);
		break;
	case BK4819_TONE_DCS:
	case BK4819_TONE_DCS_INV:
		// The stock order: the rate, 0x51, the code, the rate again with bit 14
		bk4819_write(dev, BK4819_REG_CTCSS_FREQ, BK4819_DCS_RATE);
		bk4819_write(dev, BK4819_REG_SUBAUDIO, dcs_subaudio(type));
		dcs_code(dev, value);
		bk4819_write(dev, BK4819_REG_CTCSS_FREQ, BK4819_DCS_RATE_RX);
		break;
	default:
		bk4819_write(dev, BK4819_REG_SUBAUDIO, 0x0000);
		break;
	}
}

bool bk4819_tone_found(uint16_t r0c, bk4819_tone_t type)
{
	switch (type) {
	case BK4819_TONE_CTCSS: return (r0c & (1u << 10)) != 0;
	case BK4819_TONE_DCS: return (r0c & (1u << 14)) != 0;
	case BK4819_TONE_DCS_INV: return (r0c & (1u << 15)) != 0;
	default: return true;
	}
}

bool bk4819_heard(const bk4819_t *dev, uint16_t r0c, bk4819_tone_t type)
{
	if (dev->rx_inverted && type == BK4819_TONE_DCS) {
		type = BK4819_TONE_DCS_INV;
	} else if (dev->rx_inverted && type == BK4819_TONE_DCS_INV) {
		type = BK4819_TONE_DCS;
	}
	return bk4819_tone_found(r0c, type);
}

bool bk4819_rx_tone_ok(bk4819_t *dev, bk4819_tone_t type)
{
	return type == BK4819_TONE_NONE || bk4819_heard(dev, bk4819_read(dev, BK4819_REG_CSS_STATUS), type);
}

void bk4819_tx_prepare(bk4819_t *dev, bk4819_tone_t type, uint16_t value)
{
	dev->tx_dcs = type == BK4819_TONE_DCS || type == BK4819_TONE_DCS_INV;
	switch (type) {
	case BK4819_TONE_CTCSS:
		bk4819_write(dev, 0x40, BK4819_TX_DEV_CTCSS);
		bk4819_write(dev, BK4819_REG_SUBAUDIO, BK4819_SUBAUDIO_TX_CTCSS);
		bk4819_write(dev, BK4819_REG_CTCSS_FREQ, bk4819_ctcss_word_tx(value));
		break;
	case BK4819_TONE_DCS:
	case BK4819_TONE_DCS_INV:
		// The stock order: deviation, rate, 0x51, then the code
		bk4819_write(dev, 0x40, BK4819_TX_DEV_CTCSS);
		bk4819_write(dev, BK4819_REG_CTCSS_FREQ, BK4819_DCS_RATE);
		bk4819_write(dev, BK4819_REG_SUBAUDIO, dcs_subaudio(type));
		dcs_code(dev, value);
		break;
	default:
		bk4819_write(dev, 0x40, BK4819_TX_DEV_PLAIN);
		bk4819_write(dev, BK4819_REG_SUBAUDIO, BK4819_SUBAUDIO_TX_NONE);
		break;
	}
}

void bk4819_tx_tune(bk4819_t *dev, uint32_t hz)
{
	bk4819_set_freq(dev, hz);
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_set_state(dev, BK4819_STATE_TX_MAIN);
}

// 4.4 TX tone: the stock order. Main chip in TX; 0x30 = 0xC3FA takes the microphone ADC out, the tone generator is
// the only audio. 0x50 bit 15 is only set while the tone is set up
void bk4819_tone_start(bk4819_t *dev, uint16_t hz, uint8_t gain)
{
	if (hz == 0) {
		bk4819_tone_stop(dev);
		return;
	}
	if (gain > BK4819_TONE_GAIN_MAX) {
		gain = BK4819_TONE_GAIN_MAX;
	}
	if (dev->tone_hz == 0) {
		bk4819_write(dev, BK4819_REG_AF, 0x6342u);	// AF select 3, the stock side tone
		bk4819_write(dev, BK4819_REG_TONE_CTRL, (uint16_t)(0x8000u | (uint16_t)gain << 8));
		bk4819_write(dev, BK4819_REG_TX_AUDIO, BK4819_TX_AUDIO_TONE);
		bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
		bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
		bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_TX_TONE);
	} else {
		bk4819_write(dev, BK4819_REG_TONE_CTRL, (uint16_t)(0x8000u | (uint16_t)gain << 8));
	}
	dev->tone_hz = hz;
	dev->tone_gain = gain;
	// The stock word is the product truncated: 1750 Hz = 18067
	bk4819_write(dev, BK4819_REG_TONE1_FREQ, (uint16_t)((uint32_t)hz * 103244u / 10000u));
	bk4819_write(dev, BK4819_REG_TX_AUDIO, BK4819_TX_AUDIO_LIVE);
}

// Keyed by the tone generator's enable (0x70 bit 15) while the chip stays at 0xC3FA
void bk4819_tone_key(bk4819_t *dev, bool down)
{
	if (dev->tone_hz == 0) {
		return;
	}
	bk4819_write(dev, BK4819_REG_TONE_CTRL, down ? (uint16_t)(0x8000u | (uint16_t)dev->tone_gain << 8) : 0x0000u);
}

void bk4819_tone_stop(bk4819_t *dev)
{
	if (dev->tone_hz == 0) {
		return;
	}
	bk4819_write(dev, BK4819_REG_AF, BK4819_AF_MUTE);
	bk4819_write(dev, BK4819_REG_TX_AUDIO, BK4819_TX_AUDIO_LIVE);
	bk4819_write(dev, BK4819_REG_TONE_CTRL, 0x0000);
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_TX_MAIN);
	bk4819_write(dev, 0x40, BK4819_TX_DEV_CTCSS);
	dev->tone_hz = 0;
}

#define CSS_DETECT_DEF	0x028Fu		// the thresholds every channel set writes
#define CSS_TAIL	0x8000u

// The stock tone-stop at the end of a transmission with the TX flag already clear: it ends in the RX state
void bk4819_tx_end(bk4819_t *dev)
{
	if (dev->tail_on) {
		bk4819_tx_tail_end(dev);
	}
	dev->tx_dcs = false;
	bk4819_write(dev, BK4819_REG_AF, BK4819_AF_MUTE);
	bk4819_write(dev, BK4819_REG_TX_AUDIO, BK4819_TX_AUDIO_LIVE);
	bk4819_write(dev, BK4819_REG_TONE_CTRL, 0x0000);
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_RX);
	dev->tone_hz = 0;
}

bool bk4819_tx_tail_start(bk4819_t *dev)
{
	if (!dev->tx_dcs) {
		return false;
	}
	bk4819_write(dev, BK4819_REG_CSS_DETECT, CSS_TAIL | CSS_DETECT_DEF);
	dev->tail_on = true;
	return true;
}

void bk4819_tx_tail_end(bk4819_t *dev)
{
	bk4819_write(dev, BK4819_REG_CSS_DETECT, CSS_DETECT_DEF);
	dev->tail_on = false;
}

// 4.5 DMR transmit: the stock digital mode set's TX words. The main chip's microphone path stays on (0xC3FE), but with
// the deviation off nothing of it reaches the carrier
void bk4819_dmr_tx_prepare(bk4819_t *dev)
{
	dev->tx_dcs = false;
	bk4819_write(dev, 0x40, BK4819_TX_DEV_OFF);
	bk4819_write(dev, BK4819_REG_SUBAUDIO, BK4819_SUBAUDIO_OFF);
}

// The stock per-burst tune (TX and back to the IF alike): the frequency is written in whatever state the chip is in
void bk4819_burst_tune(bk4819_t *dev, uint32_t hz, uint16_t state)
{
	write_freq(dev, hz);
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	bk4819_write(dev, BK4819_REG_ENABLES, state);
}

void bk4819_lo_resume(bk4819_t *dev)
{
	bk4819_write(dev, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_write(dev, BK4819_REG_ENABLES, dev->idle);
	bk4819_write(dev, BK4819_REG_ENABLES, BK4819_STATE_TX);
}
