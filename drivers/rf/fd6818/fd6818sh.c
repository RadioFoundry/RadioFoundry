// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FD6818 in a superheterodyne: main transceiver and LO (docs/spec/hardware-db25d.md 4.8)
#include <stddef.h>
#include "fd6818sh.h"
#include "rf/subaudio/subaudio.h"

#define RESET_MS	10u
#define CAL_MS		50u		// offset calibration: 0x04 = 0x0300, then this long
#define PAGE_1		0x0001u

#define REG_BAND	0x1A
#define REG_FREQ_LO	0x38
#define REG_FREQ_HI	0x39
#define REG_CTCSS	0x07
#define REG_DCS		0x08
#define REG_GPIO_OUT	0x25
#define REG_GPIO_SEL_LO	0x26		// GPIO 0-3 output select, a nibble each
#define REG_GPIO_SEL_HI	0x27		// GPIO 4-7
#define REG_PA		0x28
#define REG_DEV		0x40
#define REG_BW		0x43
#define REG_AF		0x47
#define REG_SUB		0x51
#define REG_SUB_DETECT	0x52
#define REG_AUDIO_GAIN	0x53
#define REG_TONE_CTRL	0x70
#define REG_TONE1	0x71
#define REG_TONE2	0x72
#define REG_AFC		0x73
#define REG_DC		0x7E

#define SUB_OFF		0x0000u
#define SUB_TX_NONE	0x1050u		// the chip's default: sub-audio off
#define SUB_CTCSS	0x90C5u
#define SUB_DCS		0x80B8u
#define SUB_DCS_INV	0xA0B8u
#define DETECT_DEF	0x028Fu
#define DETECT_TAIL	0x8000u
#define CTCSS_SLOT1	0x2000u		// 0x07 bit 13: the second tone slot
#define CTCSS_55HZ	0x0470u		// the tail tone after CTCSS
#define DCS_RATE	0x0AD7u		// 134.4 Hz: the DCS bit rate, also the tail tone after DCS
#define TONE_ON		0x8000u
#define DTMF_CTRL	0x8989u		// both generators, gain 9
#define AFC_ANALOG	0x4E42u
#define AFC_DIGITAL	0x4E52u
#define MIC_ANALOG	48u
#define MIC_DIGITAL	27u
#define LO_PA_LEVEL	15u

#define GPIO_SEL_CSS_CMP	4u	// sub-audio compare result
#define GPIO_SEL_SQ		2u

typedef struct {
	uint8_t reg;
	uint16_t val;
} regval_t;

// Power-on table, after the soft reset. Page 1 between the 0x7F writes. The LO's differs in 0x35 and 0x40 and adds 0x53
static const regval_t init_page0[] = {
	{ 0x17, 0x06E4 }, { 0x19, 0x930F }, { 0x1A, 0x2F50 }, { 0x1F, 0x8258 }, { 0x22, 0x3C14 }, { 0x23, 0x9C0F },
	{ 0x2A, 0x2202 }, { 0x35, 0xF008 }, { 0x37, 0x0781 }, { 0x3A, 0x8000 }, { 0x40, 0x39D0 }, { 0x43, 0x4048 },
	{ 0x44, 0x4ACC }, { 0x45, 0x6762 }, { 0x49, 0x282C }, { 0x4A, 0x5446 }, { 0x4B, 0x2342 }, { 0x4F, 0x2F2E },
	{ 0x58, 0xE217 }, { 0x73, 0x4E42 }, { 0x74, 0x1211 }, { 0x77, 0xC0FF }, { 0x78, 0x4C4A }, { 0x7D, 0x8000 },
	{ 0x7E, 0x3425 },
};

static const regval_t init_page1[] = {
	{ 0x16, 0x81C1 }, { 0x17, 0x02B8 }, { 0x18, 0x8FD8 }, { 0x19, 0x6891 }, { 0x1B, 0x2000 }, { 0x1C, 0x076F },
	{ 0x1D, 0xB9FD }, { 0x1E, 0x6392 }, { 0x1F, 0xC1D1 }, { 0x20, 0x220A }, { 0x21, 0xE7C0 }, { 0x22, 0x0D16 },
	{ 0x23, 0xD1E7 }, { 0x24, 0x0A0D }, { 0x27, 0x18DA }, { 0x28, 0xC3B3 }, { 0x29, 0x2113 }, { 0x2A, 0x84E2 },
	{ 0x2B, 0x3B6B }, { 0x36, 0x9D06 },
};

// 26 MHz crystal (N = 5,200,000 in 5 Hz steps), then the AGC gain set the main chip uses
static const regval_t init_xtal[] = { { 0x3B, 0x5880 }, { 0x3C, 0x4F00 }, { 0x3D, 0x2762 } };
static const regval_t init_agc[] = {
	{ 0x10, 0x0000 }, { 0x11, 0x011A }, { 0x12, 0x031B }, { 0x13, 0x035E }, { 0x7B, 0xA996 }, { 0x7C, 0xD8C1 },
	{ 0x7D, 0x8000 },
};

// The 0x7813's table; it ends with its own calibration
static const regval_t init_7813[] = {
	{ 0x7F, 0x0000 }, { 0x00, 0x8000 }, { 0x00, 0x0000 }, { 0x10, 0x0038 }, { 0x18, 0x2125 }, { 0x1F, 0x0EF8 },
	{ 0x30, 0x5FE0 }, { 0x34, 0x086F }, { 0x35, 0xF008 }, { 0x40, 0x39D0 }, { 0x49, 0x26AC }, { 0x4E, 0x2A1C },
	{ 0x77, 0x88EF }, { 0x7B, 0xC3A8 }, { 0x7D, 0x4200 }, { 0x7F, 0x0001 }, { 0x10, 0x8308 }, { 0x17, 0x0AD6 },
	{ 0x18, 0x7A18 }, { 0x19, 0x7915 }, { 0x1A, 0x0728 }, { 0x1D, 0x39FD }, { 0x22, 0x1E36 }, { 0x21, 0xC8F7 },
	{ 0x35, 0x100F }, { 0x36, 0x1D59 }, { 0x7F, 0x0000 }, { 0x03, 0xBFF1 }, { 0x04, 0x0300 }, { 0x03, 0x0000 },
	{ 0x18, 0x0125 }, { 0x04, 0x0300 }, { 0x7F, 0x0001 }, { 0x1C, 0x04D3 }, { 0x1D, 0x29A1 }, { 0x27, 0x891B },
	{ 0x7F, 0x0000 }, { 0x50, 0x073C }, { 0x53, 0x93DD }, { 0x70, 0x0000 }, { 0x7E, 0x349E },
};

// The 0x7813's synthesizer set before a frequency
static const regval_t tune_7813[] = {
	{ 0x3A, 0x00E0 }, { 0x40, 0x38D0 }, { 0x43, 0x5009 }, { 0x4D, 0x347D }, { 0x7F, 0x0001 }, { 0x16, 0x02FE },
	{ 0x7F, 0x0000 },
};

// Band edges in MHz (a frequency belongs to the first edge at or above it) and the 0x1A word of each band
static const uint16_t band_edge[] = { 23, 33, 46, 66, 93, 132, 185, 263, 371, 560, 740, 1120 };
#define BANDS	(sizeof(band_edge) / sizeof(band_edge[0]))
static const uint16_t band_word[BANDS] = {
	0xDF21, 0x6F41, 0xCF22, 0x5F42, 0xBF24, 0x4F44, 0xAF28, 0x3F48, 0x9F30, 0x2F50, 0x0000, 0x1F80,
};

// 0x40 by band: the high byte (0 = none for that band), the low byte by even / odd band and sub-audio
static const uint8_t dev_high[BANDS] = { 0x34, 0x35, 0x35, 0x36, 0x36, 0x37, 0x37, 0x38, 0x38, 0x39, 0x00, 0x3A };

// 0x7813 VCO sections: thresholds in MHz, then 0x3C, 0x3D, 0x40, 0x1A, 0x2A per section
static const uint16_t sect_edge[] = { 32, 44, 63, 88, 126, 176, 251, 353 };
#define SECTIONS	(sizeof(sect_edge) / sizeof(sect_edge[0]) + 1u)
static const uint16_t sect_word[SECTIONS][5] = {
	{ 0x1009, 0xD89E, 0x34DA, 0x1F41, 0x62F2 }, { 0x1007, 0x6276, 0x345C, 0x1F22, 0xC2F2 },
	{ 0x1004, 0xEC4F, 0x35D0, 0x1F42, 0x52F2 }, { 0x1003, 0xB13B, 0x355C, 0x1F24, 0xB2F2 },
	{ 0x1002, 0x7627, 0x36D0, 0x1F44, 0x42F2 }, { 0x1001, 0xD89E, 0x366C, 0x1F28, 0xA2F2 },
	{ 0x1001, 0x3B14, 0x37D0, 0x1F48, 0x32F2 }, { 0x1000, 0xEC4F, 0x375C, 0x1F30, 0x92F2 },
	{ 0x1000, 0x9D8A, 0x38D0, 0x1F50, 0x22F2 },
};

// 0x53 by volume: bits 9:5 the RX gain, the rest picks the coarse step (the upper steps one higher)
static const uint8_t vol_gain[] = { 0x00, 0x01, 0x03, 0x06, 0x0A, 0x0E, 0x11, 0x13, 0x15, 0x04, 0x07, 0x09, 0x0B,
				    0x0D, 0x0F, 0x11, 0x13, 0x15, 0x17, 0x19 };
#define VOL_STEPS	(sizeof(vol_gain) / sizeof(vol_gain[0]))
#define VOL_COARSE_FROM	9u
#define VOL_LOW		0x941Du
#define VOL_HIGH	0xF41Du
#define VOL_DIGITAL	0x957Du

static uint32_t mhz(uint32_t hz)
{
	return hz / 1000000u;
}

static unsigned band_index(uint32_t hz)
{
	uint32_t m = mhz(hz);
	unsigned i = 0;
	while (i < BANDS && band_edge[i] < m) {
		i++;
	}
	return i;
}

uint16_t fd6818sh_band_word(uint32_t hz)
{
	unsigned i = band_index(hz);
	return i < BANDS ? band_word[i] : 0;
}

uint16_t fd6818sh_tx_dev(uint32_t hz, bool subaudio)
{
	unsigned i = band_index(hz);
	if (i >= BANDS || dev_high[i] == 0) {
		return 0;
	}
	// With sub-audio the fine level is lower, which leaves room for the tone
	uint8_t low = (i & 1u) == 0 ? (subaudio ? 0x32u : 0x5Cu) : (subaudio ? 0x98u : 0xD0u);
	return (uint16_t)(dev_high[i] << 8 | low);
}

uint16_t fd6818sh_pa_word(uint8_t v)
{
	if (v > 15) {
		v = 15;
	}
	return (uint16_t)((v >> 3 ? 0x38u : 0u) | (v & 7u));
}

uint16_t fd6818sh_mic_word(uint8_t v)
{
	if (v > 63) {
		v = 63;
	}
	return (uint16_t)(v << 8 | 0x06u);
}

uint16_t fd6818sh_volume_word(uint8_t v)
{
	if (v > 15) {
		v = 15;
	}
	unsigned i = (unsigned)v * (VOL_STEPS - 1u) / 15u;
	return (uint16_t)(vol_gain[i] << 5 | (i < VOL_COARSE_FROM ? VOL_LOW : VOL_HIGH));
}

uint16_t fd6818sh_ctcss_word(uint16_t hz_x10)
{
	// Hz x 2^27 / 6.5 MHz = (Hz x 10) x 2.064888, truncated as the working firmware does
	return (uint16_t)(((uint64_t)hz_x10 * 258111u) / 125000u);
}

uint16_t fd6818sh_tone_word(uint16_t hz)
{
	// Hz x 2^21 / 203125 = Hz x 10.3244, truncated
	return (uint16_t)((uint32_t)hz * 103244u / 10000u);
}

uint16_t fd6818sh_dtmf_word(uint16_t hz)
{
	return (uint16_t)(((uint32_t)hz * 103244u + 5000u) / 10000u);
}

uint8_t fd6818sh_7813_section(uint32_t hz)
{
	uint32_t m = mhz(hz);
	uint8_t i = 0;
	while (i < SECTIONS - 1u && sect_edge[i] < m) {
		i++;
	}
	return i;
}

// ---- Register access ----

static void wr(fd6818sh_t *dev, uint8_t reg, uint16_t val)
{
	dev->bus->write(reg, val);
}

static void write_table(fd6818sh_t *dev, const regval_t *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		wr(dev, t[i].reg, t[i].val);
	}
}

#define TABLE(dev, t)	write_table(dev, t, sizeof(t) / sizeof(t[0]))

static void page1(fd6818sh_t *dev, uint8_t reg, uint16_t val)
{
	wr(dev, FD6818SH_REG_PAGE, PAGE_1);
	wr(dev, reg, val);
	wr(dev, FD6818SH_REG_PAGE, 0);
}

static void wait_ms(fd6818sh_t *dev, uint32_t ms)
{
	if (dev->bus->delay_ms != NULL) {
		dev->bus->delay_ms(ms);
	}
}

static void write_freq(fd6818sh_t *dev, uint32_t hz)
{
	uint32_t f = hz / 10u;
	wr(dev, REG_FREQ_LO, (uint16_t)(f & 0xFFFFu));
	wr(dev, REG_FREQ_HI, (uint16_t)(f >> 16));
	dev->freq_10hz = f;
}

static void mic_gain(fd6818sh_t *dev, uint8_t v)
{
	page1(dev, 0x17, fd6818sh_mic_word(v));
	wr(dev, 0x18, 0x2125);
}

// GPIO output select nibble for pin (0..7)
static void gpio_select(fd6818sh_t *dev, uint8_t pin, uint8_t sel)
{
	uint8_t reg = pin < 4 ? REG_GPIO_SEL_LO : REG_GPIO_SEL_HI;
	unsigned shift = (pin & 3u) * 4u;
	uint16_t v = dev->bus->read(reg);
	wr(dev, reg, (uint16_t)((v & ~(0xFu << shift)) | ((uint16_t)sel << shift)));
}

void fd6818sh_gpio(fd6818sh_t *dev, uint8_t pin, bool high)
{
	if (pin > 7) {
		return;
	}
	// Bits 15:8 enable the outputs, low active
	uint16_t v = (uint16_t)(dev->r25 & ~((1u << pin) | (1u << (pin + 8u))));
	if (high) {
		v |= (uint16_t)(1u << pin);
	}
	if (v != dev->r25) {
		wr(dev, REG_GPIO_OUT, v);
		dev->r25 = v;
	}
}

// ---- Main transceiver ----

void fd6818sh_attach(fd6818sh_t *dev, const fd6818sh_bus_t *bus, fd6818sh_role_t role)
{
	dev->bus = bus;
	dev->role = role;
	dev->freq_10hz = 0;
	dev->state = FD6818SH_STATE_IDLE;
	dev->r25 = 0;
	dev->r7e = 0;
	dev->r53 = fd6818sh_volume_word(0);
	dev->tone_hz = 0;
	dev->tone_gain = 0;
	dev->pa_gain = 0;
	dev->wide = false;
	dev->digital = false;
	dev->tx_sub = false;
	dev->tail_on = false;
	dev->rx_inverted = false;
}

static void reset_and_table(fd6818sh_t *dev, bool lo)
{
	wr(dev, 0x00, 0x8000);
	wr(dev, 0x00, 0x0000);
	wait_ms(dev, RESET_MS);
	TABLE(dev, init_page0);
	if (lo) {
		wr(dev, 0x35, 0xF608);
		wr(dev, REG_DEV, 0x0000);
		wr(dev, REG_AUDIO_GAIN, 0x77FD);
	}
	wr(dev, FD6818SH_REG_PAGE, PAGE_1);
	TABLE(dev, init_page1);
	wr(dev, FD6818SH_REG_PAGE, 0);
	TABLE(dev, init_xtal);
}

static void offset_cal(fd6818sh_t *dev)
{
	wr(dev, FD6818SH_REG_STATE, 0xBDF1);
	wr(dev, 0x04, 0x0000);
	wr(dev, 0x04, 0x0300);
	wait_ms(dev, CAL_MS);
	wr(dev, FD6818SH_REG_STATE, FD6818SH_STATE_IDLE);
	dev->state = FD6818SH_STATE_IDLE;
}

void fd6818sh_init_main(fd6818sh_t *dev)
{
	reset_and_table(dev, false);
	TABLE(dev, init_agc);
	wr(dev, 0x7D, 0x42E2);
	wr(dev, REG_BAND, 0x2F50);
	wr(dev, REG_FREQ_LO, 0x1D38);	// parked at 457.516 MHz for the calibration
	wr(dev, REG_FREQ_HI, 0x02B7);
	offset_cal(dev);
	dev->r7e = (uint16_t)((dev->bus->read(REG_DC) & 0x0FFFu) | 0xB000u);
	wr(dev, REG_DC, dev->r7e);
	dev->r25 = dev->bus->read(REG_GPIO_OUT);
	// GPIO6 squelch, GPIO0 the sub-audio compare result (the working firmware puts the code stream there and decodes
	// it in software), GPIO3 and GPIO4 plain outputs
	gpio_select(dev, 6, GPIO_SEL_SQ);
	gpio_select(dev, FD6818SH_GPIO_CSS, GPIO_SEL_CSS_CMP);
	gpio_select(dev, 3, 0);
	gpio_select(dev, 4, 0);
	fd6818sh_gpio(dev, 4, false);
	dev->freq_10hz = 0;
}

void fd6818sh_set_state(fd6818sh_t *dev, uint16_t state)
{
	wr(dev, FD6818SH_REG_STATE, FD6818SH_STATE_IDLE);
	if (state != FD6818SH_STATE_IDLE) {
		wr(dev, FD6818SH_REG_STATE, state);
	}
	dev->state = state;
}

void fd6818sh_set_bandwidth(fd6818sh_t *dev, bool wide)
{
	dev->wide = wide;
	if (dev->digital) {
		return;
	}
	if (wide) {
		wr(dev, REG_BW, 0x3028);
	} else {
		wr(dev, REG_BW, 0x6648);
		wr(dev, 0x4D, 0x348D);
	}
}

void fd6818sh_analog_mode(fd6818sh_t *dev)
{
	fd6818sh_set_state(dev, FD6818SH_STATE_IDLE);
	dev->digital = false;
	wr(dev, REG_SUB, SUB_OFF);
	wr(dev, REG_AFC, AFC_ANALOG);
	wr(dev, 0x04, 0x0300);
	wr(dev, FD6818SH_REG_PAGE, PAGE_1);
	wr(dev, 0x1C, 0x04D7);
	wr(dev, 0x1D, 0x39FD);
	wr(dev, 0x27, 0x885B);
	wr(dev, FD6818SH_REG_PAGE, 0);
	wr(dev, 0x50, 0x073C);
	wr(dev, REG_TONE_CTRL, 0x0000);
	wr(dev, REG_DC, 0x341E);
	wr(dev, 0x44, 0x4ACC);
	mic_gain(dev, MIC_ANALOG);
	fd6818sh_set_bandwidth(dev, dev->wide);
	wr(dev, REG_AF, FD6818SH_AF_MUTE);
	wr(dev, REG_AUDIO_GAIN, dev->r53);
}

void fd6818sh_dmr_mode(fd6818sh_t *dev)
{
	fd6818sh_set_state(dev, FD6818SH_STATE_IDLE);
	dev->digital = true;
	wr(dev, REG_SUB, SUB_OFF);
	wr(dev, REG_AFC, AFC_DIGITAL);
	mic_gain(dev, MIC_DIGITAL);
	wr(dev, REG_SUB, SUB_TX_NONE);
	wr(dev, 0x04, 0x0300);
	wr(dev, FD6818SH_REG_PAGE, PAGE_1);
	wr(dev, 0x1C, 0x04D3);
	wr(dev, 0x1D, 0x29A1);
	wr(dev, 0x27, 0x891B);
	wr(dev, FD6818SH_REG_PAGE, 0);
	wr(dev, 0x50, 0x073C);
	wr(dev, REG_TONE_CTRL, 0x0000);
	wr(dev, REG_DC, (uint16_t)(dev->r7e | 0x0080u));	// bit 7: the microphone DC filter bypassed
	wr(dev, REG_BW, 0x7E48);
	wr(dev, REG_DEV, 0x0000);	// the modem modulates the reference, not the chip
	wr(dev, 0x44, 0x52CC);
	wr(dev, REG_AUDIO_GAIN, VOL_DIGITAL);
	wr(dev, REG_AF, FD6818SH_AF_DMR);
}

void fd6818sh_tune(fd6818sh_t *dev, uint32_t hz)
{
	wr(dev, REG_BAND, fd6818sh_band_word(hz));
	write_freq(dev, hz);
}

void fd6818sh_set_af(fd6818sh_t *dev, uint16_t af)
{
	if (dev->digital && af == FD6818SH_AF_RX) {
		af = FD6818SH_AF_DMR;
	}
	wr(dev, REG_AF, af);
}

void fd6818sh_set_af_gain(fd6818sh_t *dev, uint8_t gain)
{
	uint16_t w = fd6818sh_volume_word(gain);
	if (dev->digital || w == dev->r53) {
		dev->r53 = w;
		return;
	}
	dev->r53 = w;
	wr(dev, REG_AUDIO_GAIN, w);
}

// The sub-audio set, the same for both directions: tone words, then 0x51 and the detect thresholds
static void subaudio(fd6818sh_t *dev, fd6818sh_tone_t type, uint16_t value)
{
	switch (type) {
	case FD6818SH_TONE_CTCSS:
		wr(dev, REG_CTCSS, fd6818sh_ctcss_word(value));
		wr(dev, REG_CTCSS, CTCSS_SLOT1 | CTCSS_55HZ);
		wr(dev, REG_SUB, SUB_CTCSS);
		wr(dev, REG_SUB_DETECT, DETECT_DEF);
		break;
	case FD6818SH_TONE_DCS:
	case FD6818SH_TONE_DCS_INV: {
		uint32_t word = subaudio_dcs_word(value);
		wr(dev, REG_CTCSS, DCS_RATE);
		wr(dev, REG_CTCSS, CTCSS_SLOT1 | DCS_RATE);
		wr(dev, REG_DCS, (uint16_t)(word & 0xFFFu));
		wr(dev, REG_DCS, (uint16_t)(0x8000u | ((word >> 12) & 0x7FFu)));
		wr(dev, REG_SUB_DETECT, DETECT_DEF);
		wr(dev, REG_SUB, type == FD6818SH_TONE_DCS_INV ? SUB_DCS_INV : SUB_DCS);
		break;
	}
	default:
		break;
	}
}

void fd6818sh_set_rx_tone(fd6818sh_t *dev, fd6818sh_tone_t type, uint16_t value)
{
	wr(dev, REG_SUB, SUB_OFF);
	if (dev->rx_inverted && type == FD6818SH_TONE_DCS) {
		type = FD6818SH_TONE_DCS_INV;
	} else if (dev->rx_inverted && type == FD6818SH_TONE_DCS_INV) {
		type = FD6818SH_TONE_DCS;
	}
	subaudio(dev, type, value);
}

void fd6818sh_tx_prepare(fd6818sh_t *dev, fd6818sh_tone_t type, uint16_t value, uint32_t hz, uint8_t pa_gain)
{
	dev->tx_sub = type != FD6818SH_TONE_NONE;
	dev->pa_gain = pa_gain;
	wr(dev, REG_DEV, fd6818sh_tx_dev(hz, dev->tx_sub));
	if (dev->tx_sub) {
		subaudio(dev, type, value);
	} else {
		wr(dev, REG_SUB, SUB_TX_NONE);
	}
	wr(dev, REG_PA, fd6818sh_pa_word(pa_gain));
}

void fd6818sh_tx_tune(fd6818sh_t *dev, uint32_t hz)
{
	fd6818sh_tune(dev, hz);
	fd6818sh_set_state(dev, FD6818SH_STATE_TX);
}

// The working firmware ends every analog transmission so: CTCSS on the second slot (55 Hz, or 134.4 Hz after DCS),
// the tail flag, and after the tail 0x52 back to its default
bool fd6818sh_tx_tail(fd6818sh_t *dev, bool on)
{
	if (!on) {
		wr(dev, REG_SUB_DETECT, DETECT_DEF);
		dev->tail_on = false;
		return true;
	}
	if (!dev->tx_sub) {
		return false;
	}
	wr(dev, REG_SUB, SUB_CTCSS);
	wr(dev, REG_SUB_DETECT, DETECT_TAIL | DETECT_DEF);
	dev->tail_on = true;
	return true;
}

void fd6818sh_tx_end(fd6818sh_t *dev)
{
	if (dev->tail_on) {
		fd6818sh_tx_tail(dev, false);
	}
	dev->tx_sub = false;
	wr(dev, REG_TONE_CTRL, 0x0000);
	wr(dev, REG_AF, FD6818SH_AF_MUTE);
	wr(dev, REG_SUB, SUB_OFF);
	fd6818sh_set_state(dev, FD6818SH_STATE_RX);
	dev->tone_hz = 0;
}

void fd6818sh_tone_start(fd6818sh_t *dev, uint16_t hz, uint8_t gain)
{
	if (hz == 0) {
		fd6818sh_tone_stop(dev);
		return;
	}
	if (gain > 0x7F) {
		gain = 0x7F;
	}
	wr(dev, REG_TONE1, fd6818sh_tone_word(hz));
	wr(dev, REG_TONE_CTRL, (uint16_t)(TONE_ON | (uint16_t)gain << 8));
	if (dev->tone_hz == 0) {
		wr(dev, FD6818SH_REG_STATE, FD6818SH_STATE_TX_TONE);	// straight from TX, as the working firmware does
		dev->state = FD6818SH_STATE_TX_TONE;
		wr(dev, REG_AF, FD6818SH_AF_SIDETONE);
	}
	dev->tone_hz = hz;
	dev->tone_gain = gain;
}

void fd6818sh_dtmf_start(fd6818sh_t *dev, uint16_t low_hz, uint16_t high_hz)
{
	if (low_hz == 0) {
		fd6818sh_tone_stop(dev);
		return;
	}
	wr(dev, REG_TONE_CTRL, DTMF_CTRL);
	wr(dev, REG_TONE1, fd6818sh_dtmf_word(low_hz));
	wr(dev, REG_TONE2, fd6818sh_dtmf_word(high_hz));
	if (dev->tone_hz == 0) {
		wr(dev, FD6818SH_REG_STATE, FD6818SH_STATE_TX_TONE);
		dev->state = FD6818SH_STATE_TX_TONE;
		wr(dev, REG_AF, FD6818SH_AF_SIDETONE);
	}
	dev->tone_hz = low_hz;
	dev->tone_gain = 0;	// keying is for single tones only
}

void fd6818sh_tone_key(fd6818sh_t *dev, bool down)
{
	if (dev->tone_hz == 0) {
		return;
	}
	wr(dev, REG_TONE_CTRL, down ? (uint16_t)(TONE_ON | (uint16_t)dev->tone_gain << 8) : 0x0000u);
}

void fd6818sh_tone_stop(fd6818sh_t *dev)
{
	if (dev->tone_hz == 0) {
		return;
	}
	wr(dev, REG_TONE_CTRL, 0x0000);
	wr(dev, REG_AF, FD6818SH_AF_MUTE);
	wr(dev, FD6818SH_REG_STATE, FD6818SH_STATE_TX);
	dev->state = FD6818SH_STATE_TX;
	dev->tone_hz = 0;
}

void fd6818sh_dmr_tx_prepare(fd6818sh_t *dev, uint8_t pa_gain)
{
	dev->tx_sub = false;
	dev->pa_gain = pa_gain;
}

// The working firmware's TX and RX handlers: the band word, in TX the PA gain, the frequency, then the state
void fd6818sh_burst_tune(fd6818sh_t *dev, uint32_t hz, bool tx)
{
	wr(dev, REG_BAND, fd6818sh_band_word(hz));
	if (tx) {
		wr(dev, REG_PA, fd6818sh_pa_word(dev->pa_gain));
	}
	write_freq(dev, hz);
	fd6818sh_set_state(dev, tx ? FD6818SH_STATE_TX : FD6818SH_STATE_RX);
}

// ---- LO ----

void fd6818sh_init_lo(fd6818sh_t *dev)
{
	if (dev->role == FD6818SH_LO_7813) {
		TABLE(dev, init_7813);
	} else {
		reset_and_table(dev, true);
		offset_cal(dev);
		wr(dev, REG_BW, 0x3028);
		wr(dev, REG_AFC, AFC_DIGITAL);
		wr(dev, REG_DEV, 0x0000);
	}
	// Both: no modulation, sub-audio off, AFC off, the full output level, GPIO3 and GPIO4 low
	dev->r25 = dev->bus->read(REG_GPIO_OUT);
	wr(dev, REG_DEV, 0x0000);
	wr(dev, REG_SUB, SUB_TX_NONE);
	wr(dev, REG_AFC, AFC_DIGITAL);
	wr(dev, REG_PA, fd6818sh_pa_word(LO_PA_LEVEL));
	fd6818sh_gpio(dev, 3, false);
	fd6818sh_gpio(dev, 4, false);
	fd6818sh_set_state(dev, FD6818SH_STATE_IDLE);
	dev->freq_10hz = 0;
}

void fd6818sh_lo_tune(fd6818sh_t *dev, uint32_t hz)
{
	if (dev->freq_10hz != hz / 10u) {
		if (dev->role == FD6818SH_LO_7813) {
			const uint16_t *s = sect_word[fd6818sh_7813_section(hz)];
			TABLE(dev, tune_7813);
			wr(dev, 0x3C, s[0]);
			wr(dev, 0x3D, s[1]);
			wr(dev, REG_DEV, s[2]);
			wr(dev, REG_BAND, s[3]);
			wr(dev, 0x2A, s[4]);
			write_freq(dev, hz);
			wr(dev, REG_DEV, 0x0000);
		} else {
			fd6818sh_tune(dev, hz);
		}
	}
	fd6818sh_set_state(dev, FD6818SH_STATE_LO);
}

// The working firmware's TX: the LO idle, its frequency kept
void fd6818sh_lo_off(fd6818sh_t *dev)
{
	fd6818sh_set_state(dev, FD6818SH_STATE_IDLE);
}

void fd6818sh_lo_resume(fd6818sh_t *dev)
{
	fd6818sh_set_state(dev, FD6818SH_STATE_LO);
}
