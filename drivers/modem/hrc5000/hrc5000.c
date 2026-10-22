// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C5000 driver (hrc5000.h). Register meanings are the user manual's where it has them; the manual has no entry
// for 0x25, 0x26, 0x37, 0x39, 0x3D, 0x41, 0x45, 0x47, 0x48 or 0xB9-0xBB, whose values are the MD-380's own.
// The chip's registers are on page 0 (HRC_PAGE_SYS, which the chip description maps there) and page 1 (HRC_PAGE_AUX).
#include <stddef.h>
#include "hrc5000.h"
#include "hal_dmr_modem.h"
#if !defined(CONFIG_DMR) || CONFIG_DMR
#include "dmr/ambe/ambeFec.h"
#define HRC5000_TICK_RX	1	// received voice read at the ticks (4.9); the AMBE code is only built with DMR
#else
#define HRC5000_TICK_RX	0
#endif

enum {
	REG_RESET = 0x00,	// active-low resets, bit 7 DMR protocol .. bit 0 CodeC
	REG_IQ = 0x01,		// bit 7 TX I/Q mode, bit 6 RX I/Q mode (0 = IQ, 1 = QI)
	REG_REF_FINE = 0x04,
	REG_VOCODER = 0x06,
	REG_IF_HI = 0x07,
	REG_IF_MID = 0x08,
	REG_IF_LO = 0x09,
	REG_CLK_OUT = 0x0A,	// CLKOutDiv
	REG_PLL_M = 0x0B,
	REG_PLL_CTL = 0x0C,	// bit 7 PLL bypass, bits 5-4 PLLDO, bits 3-0 PLLN
	REG_CODEC_CTL = 0x0D,
	REG_HP_OUT = 0x0E,
	REG_AD_VOL = 0x0F,
	REG_MODE = 0x10,	// bit 7 modulator (0 DMR, 1 FM), bit 2 repeater
	REG_COLOR_CODE = 0x1F,
	REG_RX_PATH_A = 0x25,
	REG_RX_PATH_B = 0x26,
	REG_DEV_FACTOR = 0x35,
	REG_DAC_GAIN = 0x37,
	REG_FM_FILTER = 0x34,
	REG_I2S_CTL = 0x36,	// not in the manual; bit 7 set turns the external-codec I2S clocks off (as on the HR_C6000)
	REG_FM_RX_DEV = 0x3E,
	REG_TX_LIMIT = 0x3F,
	REG_LAYER2 = 0x40,
	REG_SLOT = 0x41,
	REG_VOCODER_CTL = 0x22,	// bit 5 DecodeStart, bit 4 DecodeStop (bits 7-6 encode); bits 2-0 the 0x11 block's 0x07
	REG_REF_A = 0x47,
	REG_REF_B = 0x48,
	REG_VOICE_TX = 0x60,
	REG_INT_MASK = 0x81,
	REG_INT_CLEAR = 0x83,
	REG_SEND_MASK = 0x87,
	REG_CLK_B9 = 0xB9,
	REG_CLK_BA = 0xBA,
	REG_CLK_BB = 0xBB,
};

#define MODE_FM		0x80
#define MODE_DMR	0x6B	// Tier II, layer 2, aligned, timeslot mode (direct)
#define MODE_DMR_REP	0x4F	// the same on a repeater channel: relay and continuous; the engine picks the slot (0x52)
// The value the set-ups end with. DMR frames go out with DMR protocol and PHY at 0 here, so a 0 bit is a running block on
// this chip, not the manual's reset (the manual's polarity looks inverted)
#define RESET_RUN	0x28
// 0x36 as the 0x11 block leaves it (0xF0, clocks off) and while a PCM stream runs. The chip's reset value has the
// clocks running, so only the block's 0xF0 stops them. Clearing bit 7 alone (0x70) left them stopped; 0x00 runs them
// (issue 66, on air: DMR transmit voice decodes on another radio). -DHRC5000_I2S_CTL_ON=<v> tries another value
#define I2S_CTL_OFF	0xF0
#ifndef HRC5000_I2S_CTL_ON
#define HRC5000_I2S_CTL_ON	0x00
#endif
#define IQ_UHF		0xB0
#define IQ_VHF		0xF0

// What the set-ups write to the codec and the receive path registers, in FM and DMR
#define CODEC_CTL	0x8C
#define HP_OUT		0x44
// While a received DMR call plays the codec takes 0x0D = 0x9C (Switch2 on) and 0x0E = 0x4C (LineOutEn on, HPoutEn still
// off), and the idle values back at the end: the speaker amplifier is fed from LINEOUT, not HPOUT (hardware-md380.md 4.10).
// -DHRC5000_SPK_VOICE=0 leaves them out, to compare
#define CODEC_CTL_VOICE	0x9C
#define HP_OUT_VOICE	0x4C
#ifndef HRC5000_SPK_VOICE
#define HRC5000_SPK_VOICE	1
#endif
#define DAC_GAIN	0xC2
#define RX_PATH_A	0x0E
#define RX_PATH_B_DMR_RX	0xFD
#define RX_PATH_B_OFF	0xFE	// receiver IF and audio disconnected: transmitting, and the FM modes

// Deviation factor (4:0x35) for DMR and for FM at each channel width
#define DEV_DMR		0x1E
#define DEV_FM_NARROW	0x30
#define DEV_FM_WIDE	0x3C

// --- Chip description for the shared code ---

static uint8_t dmr_mode(bool repeater)
{
	return repeater ? MODE_DMR_REP : MODE_DMR;
}

// The chip adds the CRC to these bursts from their first 10 bytes
static uint8_t crc_len(uint8_t kind)
{
	switch (kind) {
	case DMR_BURST_CSBK:
	case DMR_BURST_MBC_HEADER:
	case DMR_BURST_DATA_HEADER:
		return 10;
	default:
		return 0;
	}
}

// The embedded LC fragment code of voice bursts A..F (the MCU supplies it): none, first, continuation x2, last, none
static uint8_t voice_lcss(uint8_t voice_index)
{
	static const uint8_t lcss[6] = { 0, 1, 3, 3, 2, 0 };
	return lcss[voice_index % 6u];
}

// The microphone gain is register 0x0F as it is
static bool set_mic_gain(hrc_t *dev, uint8_t gain)
{
	return hrc_write(dev, HRC_PAGE_SYS, REG_AD_VOL, gain);
}

static bool dmr_enter(hrc_t *dev, uint8_t mod2, uint8_t mod1, uint8_t mic_gain)
{
	return hrc5000_set_mode_dmr((hrc5000_t *)dev, mod2, mod1, mic_gain);
}

#if HRC5000_TICK_RX
// The voice RAM (32 bytes, a longer read wraps) holds a received burst as its three 72-bit AMBE+2 air frames
// (hardware-md380.md 4.8), once the decoder runs (rx_decode). The port changes at every tick, the other slot's
// included, so the C0 words say which reads are a call's bursts
static int rx_voice_read(hrc_t *dev, uint8_t out[27])
{
	if (dev->bus->voice == NULL || !dev->bus->voice(dev->bus->ctx, false, out, 27)) {
		return -1;
	}
	int good = 0;
	for (int f = 0; f < 3; f++) {
		uint32_t c[4];
		uint16_t data;
		ambeDeinterleave(out + 9 * f, c);
		int err = ambeGolay24Decode(c[0], &data);
		good += err >= 0 && err <= 1;
	}
	return good;
}

// The CPU controls the vocoder (0x06 bit 0), so the decoder runs only when started: without it the voice RAM never
// holds received voice
static void rx_decode(hrc_t *dev, bool on)
{
	(void)hrc_write(dev, HRC_PAGE_SYS, REG_VOCODER_CTL, on ? 0x27 : 0x17);
}
#endif

static const hrc_chip_t chip = {
	.cfg_page = 0,
	.id_low_first = true,
	.mcu_crc = false,
	.sync_reg = false,
	.mode_per_channel = true,
	.tc_last_rx = true,
	.dmr_mode = dmr_mode,
	.crc_len = crc_len,
	.voice_lcss = voice_lcss,
	.set_mic_gain = set_mic_gain,
	.dmr_enter = dmr_enter,
#if HRC5000_TICK_RX
	.rx_voice_read = rx_voice_read,
	.rx_decode = rx_decode,
#endif
};

void hrc5000_attach(hrc5000_t *dev, const hrc_bus_t *bus, bool vhf)
{
	hrc_attach(&dev->hrc, bus, &chip);
	dev->vhf = vhf;
	dev->mode = HRC5000_MODE_NONE;
	dev->have_ref = false;
	dev->i2s_on = false;
	dev->spk_voice = false;
}

// --- Tables ---

// Page 0 from 0x00 (6 bytes). 0x01 is the UHF I/Q setting; VHF overrides it afterwards
static const uint8_t block_reset[6] = { 0xFF, IQ_UHF, 0x00, 0x00, 0x00, 0x00 };

// Page 0 from 0x11 (44 bytes): channel, addresses, receive masks, access policy (0x21 = 0, 0x22 = 0x07) and the rest
static const uint8_t block_sys[44] = {
	0x80, 0x0A, 0x22, 0x01, 0x00, 0x00, 0x33, 0xEF, 0x00, 0xFF, 0xFF, 0xFF, 0xF0, 0xF0, 0x10, 0x00,
	0x00, 0x07, 0x3B, 0xF8, 0x0E, 0xFD, 0x40, 0xFF, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x04, 0x0B, 0x00,
	0x17, 0x02, 0xFF, 0xE0, 0x28, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Page 1 from 0x10 (20 bytes): the CRC masks
static const uint8_t aux_masks[20] = {
	0x69, 0x69, 0x96, 0x96, 0x96, 0x99, 0x99, 0x99, 0xA5, 0xA5, 0xAA, 0xAA, 0xCC, 0xCC, 0x00, 0xF0,
	0x01, 0xFF, 0x01, 0x0F,
};

// Page 1 from 0x30, 0x40 and 0x50
static const uint8_t aux_30[10] = { 0x30, 0x4E, 0x14, 0x1E, 0x1A, 0x30, 0x3D, 0x50, 0x07, 0x60 };
static const uint8_t aux_40[7] = { 0x90, 0x03, 0x01, 0x02, 0x05, 0x07, 0xF0 };
static const uint8_t aux_50[5] = { 0, 0, 0, 0, 0 };

typedef struct {
	uint8_t page;
	uint8_t reg;
	uint8_t val;
} row_t;

static bool put_rows(hrc5000_t *dev, const row_t *rows, uint32_t n)
{
	bool ok = true;
	for (uint32_t i = 0; i < n; i++) {
		ok &= hrc_write(&dev->hrc, rows[i].page, rows[i].reg, rows[i].val);
	}
	return ok;
}

#define ROWS(a)	(a), (sizeof(a) / sizeof((a)[0]))

// --- Set-ups ---

// System clock: 49.152 MHz = XTAL 29.4912 MHz x PLLM 40 / PLLN 3 / 2^PLLDO 3 (manual, section 6). The PLL is
// bypassed at reset; clearing 0x0C bit 7 selects it. 0xB9-0xBB are not in the manual
static bool set_clock(hrc5000_t *dev)
{
	hrc_t *h = &dev->hrc;
	bool ok = hrc_write(h, HRC_PAGE_SYS, REG_CLK_OUT, 0x80);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_PLL_M, 40);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_PLL_CTL, 0x33);
	h->bus->delay_ms(1);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_CLK_OUT, 0x00);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_CLK_BA, 0x22);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_CLK_BB, 0x11);
	return ok;
}

// The part of both full set-ups that sets the mode, the intermediate frequency and the register blocks. The mode
// differs in 0xB9, 0x10, the IF and 0x06
static bool set_core(hrc5000_t *dev, uint8_t clk_b9, uint8_t mode, uint8_t if_hi, uint8_t if_mid, uint8_t vocoder)
{
	hrc_t *h = &dev->hrc;
	bool ok = set_clock(dev);	// every full set-up re-runs the clock step
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_CLK_B9, clk_b9);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_MODE, mode);
	if (mode != MODE_FM) {
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_LAYER2, 0x43);	// DMR layer 2: transmit and receive, timing from reception
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_SLOT, 0x40);
	}
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_IF_HI, if_hi);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_IF_MID, if_mid);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_IF_LO, 0x00);
	// DMR sets the vocoder / mux word while the chip is still held in reset and then releases it; FM sets it after
	if (mode != MODE_FM) {
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_VOCODER, vocoder);
	}
	ok &= hrc_write_burst(h, HRC_PAGE_SYS, REG_RESET, block_reset, sizeof(block_reset));
	if (dev->vhf) {
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_IQ, IQ_VHF);
	}
	if (mode == MODE_FM) {
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_VOCODER, vocoder);
	}
	ok &= hrc_write_burst(h, HRC_PAGE_SYS, 0x11, block_sys, sizeof(block_sys));
	if (dev->have_ref) {
		ok &= hrc5000_set_ref(dev, dev->ref_tune);	// the block above zeroes 0x04
	}
	if (dev->i2s_on) {
		ok &= hrc5000_i2s_clock(dev, true);		// and puts 0x36 back to clocks off
	}
	return ok;
}

// The codec and interrupt writes both full set-ups end with
static bool set_codec_and_int(hrc5000_t *dev, uint8_t rx_path_b)
{
	hrc_t *h = &dev->hrc;
	bool ok = hrc_write(h, HRC_PAGE_SYS, REG_CODEC_CTL, CODEC_CTL);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_HP_OUT, HP_OUT);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_AD_VOL, 0xC8);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_RX_PATH_A, RX_PATH_A);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_RX_PATH_B, rx_path_b);
	return ok;
}

bool hrc5000_set_mode_fm_rx(hrc5000_t *dev)
{
	hrc_t *h = &dev->hrc;
	bool ok = true;
	if (dev->mode == HRC5000_MODE_FM_TX) {
		// Back from transmit: the codec registers are as FM left them; only the voice transmit path goes off
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_HP_OUT, HP_OUT);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_VOICE_TX, 0x00);
	} else {
		ok &= set_core(dev, 0x33, MODE_FM, 0x0E, 0x10, 0x00);
		dev->spk_voice = false;
		ok &= set_codec_and_int(dev, RX_PATH_B_OFF);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_INT_CLEAR, 0xFF);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_SEND_MASK, 0x00);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_INT_MASK, 0x00);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_VOICE_TX, 0x00);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_RESET, RESET_RUN);
		h->bus->delay_ms(1);
	}
	dev->mode = HRC5000_MODE_FM_RX;
	return ok;
}

bool hrc5000_set_mode_fm_tx(hrc5000_t *dev, bool wide, uint8_t mod2, uint8_t mod1)
{
	hrc_t *h = &dev->hrc;
	bool ok = set_codec_and_int(dev, RX_PATH_B_OFF);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_INT_CLEAR, 0xFF);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_SEND_MASK, 0x00);
	ok &= hrc_set_mod_gain(h, mod2, mod1);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_DEV_FACTOR, wide ? DEV_FM_WIDE : DEV_FM_NARROW);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_TX_LIMIT, 0x04);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_FM_FILTER, 0x3C);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_FM_RX_DEV, 0x08);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_DAC_GAIN, DAC_GAIN);
	ok &= hrc_write(h, HRC_PAGE_AUX, 0x50, 0x00);
	ok &= hrc_write(h, HRC_PAGE_AUX, 0x51, 0x00);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_INT_MASK, 0x00);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_VOICE_TX, 0x80);
	dev->mode = HRC5000_MODE_FM_TX;
	return ok;
}

// Registers set once the blocks are in. The radio's own ID and the channel's colour code are the DMR engine's
// (hrc6000_dmr_configure)
static const row_t dmr_page1[] = {
	{ HRC_PAGE_AUX, 0x52, 0x08 }, { HRC_PAGE_AUX, 0x53, 0xEB }, { HRC_PAGE_AUX, 0x54, 0x78 },
	{ HRC_PAGE_AUX, 0x45, 0x1E }, { HRC_PAGE_AUX, 0x37, 0x50 }, { HRC_PAGE_AUX, 0x35, 0xFF },
};
static const row_t dmr_sys[] = {
	{ HRC_PAGE_SYS, 0x39, 0x02 }, { HRC_PAGE_SYS, 0x3D, 0x0A }, { HRC_PAGE_SYS, REG_INT_CLEAR, 0xFF },
	{ HRC_PAGE_SYS, REG_SEND_MASK, 0x00 }, { HRC_PAGE_SYS, 0x65, 0x0A }, { HRC_PAGE_SYS, 0x1D, 0xFF },
	{ HRC_PAGE_SYS, 0x1E, 0xF1 },
	{ HRC_PAGE_SYS, REG_COLOR_CODE, 0x10 },	// colour code 1 until the engine sets the channel's
};
static const row_t dmr_tail[] = {
	{ HRC_PAGE_SYS, 0x64, 0x00 },
};

bool hrc5000_set_mode_dmr(hrc5000_t *dev, uint8_t mod2, uint8_t mod1, uint8_t mic_gain)
{
	hrc_t *h = &dev->hrc;
	bool ok = set_core(dev, 0x32, MODE_DMR, 0x0B, 0xB8, 0x21);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_RESET, RESET_RUN);
	h->bus->delay_ms(1);
	ok &= hrc_write_burst(h, HRC_PAGE_AUX, 0x10, aux_masks, sizeof(aux_masks));
	ok &= hrc_write_burst(h, HRC_PAGE_AUX, 0x30, aux_30, sizeof(aux_30));
	ok &= hrc_write_burst(h, HRC_PAGE_AUX, 0x40, aux_40, sizeof(aux_40));
	ok &= hrc_write_burst(h, HRC_PAGE_AUX, 0x50, aux_50, sizeof(aux_50));
	ok &= put_rows(dev, ROWS(dmr_page1));
	ok &= put_rows(dev, ROWS(dmr_sys));
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_CODEC_CTL, CODEC_CTL);
	ok &= hrc5000_dmr_path(dev, false, mod2, mod1, mic_gain);
	ok &= hrc_set_mod_gain(h, mod2, mod1);
	ok &= put_rows(dev, ROWS(dmr_tail));
	for (uint8_t reg = 0x24; reg <= 0x27; reg++) {
		ok &= hrc_write(h, HRC_PAGE_AUX, reg, 0x00);	// no privacy key
	}
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_INT_MASK, 0x19);	// system interrupt mask
	ok &= hrc_write(h, HRC_PAGE_SYS, 0x85, 0x00);
	dev->mode = HRC5000_MODE_DMR;
	return ok;
}

bool hrc5000_dmr_path(hrc5000_t *dev, bool tx, uint8_t mod2, uint8_t mod1, uint8_t mic_gain)
{
	hrc_t *h = &dev->hrc;
	bool ok = true;
	bool voice = dev->spk_voice && !tx;
	if (tx || voice) {
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_CODEC_CTL, voice ? CODEC_CTL_VOICE : CODEC_CTL);
	}
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_HP_OUT, voice ? HP_OUT_VOICE : HP_OUT);
	ok &= hrc_set_mic_gain(h, mic_gain);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_DAC_GAIN, DAC_GAIN);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_RX_PATH_A, RX_PATH_A);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_RX_PATH_B, tx ? RX_PATH_B_OFF : RX_PATH_B_DMR_RX);
	if (tx) {
		ok &= hrc_set_mod_gain(h, mod2, mod1);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_DEV_FACTOR, DEV_DMR);
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_TX_LIMIT, 0x04);
	}
	return ok;
}

// The codec set-up for a received call that plays (on) and back to the idle values (off). DMR only: the FM receiver's
// audio doesn't pass the codec, and a beep in FM keeps the idle values
bool hrc5000_speaker_voice(hrc5000_t *dev, bool on)
{
	if (!HRC5000_SPK_VOICE || dev->mode != HRC5000_MODE_DMR) {
		return true;
	}
	hrc_t *h = &dev->hrc;
	dev->spk_voice = on;
	bool ok = hrc_write(h, HRC_PAGE_SYS, REG_CODEC_CTL, on ? CODEC_CTL_VOICE : CODEC_CTL);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_HP_OUT, on ? HP_OUT_VOICE : HP_OUT);
	if (on) {
		ok &= hrc_write(h, HRC_PAGE_SYS, REG_DAC_GAIN, DAC_GAIN);
	}
	return ok;
}

bool hrc5000_power_on(hrc5000_t *dev)
{
	bool ok = hrc5000_set_mode_fm_rx(dev);
	return ok;
}

// --- I2S clocks ---

#ifdef HRC5000_I2S_CYCLE
// Bench knob: each PCM stream that starts takes the next 0x36 value of the list, to find the one that runs the I2S
// clocks without passing the receiver's audio through (the board's probe task logs it)
volatile uint8_t hrc5000_i2s_cycle_value;
#endif

bool hrc5000_i2s_clock(hrc5000_t *dev, bool on)
{
	uint8_t v = on ? HRC5000_I2S_CTL_ON : I2S_CTL_OFF;
#ifdef HRC5000_I2S_CYCLE
	static const uint8_t cycle[] = { 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
	static uint8_t next;
	if (on && !dev->i2s_on) {
		hrc5000_i2s_cycle_value = cycle[next];
		next = (uint8_t)((next + 1u) % sizeof(cycle));
	}
	if (on) {
		v = hrc5000_i2s_cycle_value;
	}
#endif
	dev->i2s_on = on;
	return hrc_write(&dev->hrc, HRC_PAGE_SYS, REG_I2S_CTL, v);
}

// --- Reference oscillator ---

uint16_t hrc5000_ref_dac(uint8_t ref_tune)
{
	return (uint16_t)(ref_tune * 4u + 0x600u);
}

bool hrc5000_set_ref(hrc5000_t *dev, uint8_t ref_tune)
{
	hrc_t *h = &dev->hrc;
	dev->ref_tune = ref_tune;
	dev->have_ref = true;
	uint8_t v = (uint8_t)(0x7Fu - ref_tune);
	bool ok = hrc_write(h, HRC_PAGE_SYS, REG_REF_A, v);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_REF_B, ref_tune < 0x80u ? 0x00 : 0x03);
	ok &= hrc_write(h, HRC_PAGE_SYS, REG_REF_FINE, v);
	return ok;
}
