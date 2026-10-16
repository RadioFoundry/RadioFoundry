// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 baseband: power-on set-up, FM, sub-audio, audio paths (docs/spec/modem-hrc7000.md 4, 5.1, 8)
#include <stddef.h>
#include "hrc7000.h"

#define RF_MODE_VALUE		0x03269860u	// AF RX with averaging, gain 2.375, RX IQ invert, dynamic sync, dual RX, two-point TX
#define RF_CONTROL_VALUE	0x00041F1Au	// RF TX interrupt 3.1 ms before the burst (gotchas 2.4)
#define SYS_INT_ENABLE		0x1187Fu	// layer-2 TX/RX points, late entry, deframe, short LC, slot abnormal, PHY
#define CODEC_RESET_SPINS	400u		// > 2 us at 168 MHz

#define FM_MOD_CTCSS		(1u << 3)
#define FM_MOD_CDCSS		(1u << 2)
#define FM_MOD_CDCSS_TX_INV	(1u << 11)
#define FM_MOD_CDCSS_RX_INV	(1u << 10)
#define FM_MOD_DTMF		(1u << 1)
#define TONE_SEND_START		(1u << 7)	// dtmf_send: the digit on
#define TONE_DIGIT_1		1u		// 697 + 1209 Hz: FM_TONE1 and FM_TONE3 high halves
#define FM_TONE1_DEFAULT	0x05930629u
#define FM_TONE3_DEFAULT	0x09AC0AB0u
#define SUBAUDIO_DEV_RESET	0x28u		// SUBVOICE_COEF b15:8 at reset: 300 Hz CTCSS (8.2)

uint16_t hrc7000_mod_dc(uint8_t tx_center)
{
	// (1228 x 128 + 422 c) x 1023 / (3300 x 128), rounded half up
	uint32_t num = (1228u * 128u + 422u * tx_center) * 1023u, den = 3300u * 128u;
	return (uint16_t)((num + den / 2u) / den);
}

void hrc7000_attach(hrc7000_t *d, volatile uint32_t *regs, volatile uint8_t *buf)
{
	d->regs = regs;
	d->buf = buf;
	d->audio_ctl = C7_AUDIO_I2S_SLAVE;
	d->tone_on = false;
}

static void spin(uint32_t n)
{
	for (volatile uint32_t i = 0; i < n; i++) {
	}
}

void hrc7000_set_mic_gain(hrc7000_t *d, uint8_t db)
{
	hrc7000_codec_wr(d, C7_CODEC_GCR_ADCL, (uint8_t)(0x80u | (db > HRC7000_MIC_GAIN_MAX_DB ? HRC7000_MIC_GAIN_MAX_DB : db)));
}

void hrc7000_set_dac_gain(hrc7000_t *d, uint8_t code)
{
	hrc7000_codec_wr(d, C7_CODEC_GCR_DACL, (uint8_t)(0x80u | (code & 0x3Fu)));
}

void hrc7000_power_on_init(hrc7000_t *d, uint8_t tx_center, uint8_t mic_gain_db)
{
	hrc7000_wr(d, C7_FM_BANDWIDTH, 0x5F);
	hrc7000_wr(d, C7_FM_DEV_COEF, 0x33500);

	// Internal audio codec: reset pulse (released by the 0x190 write below), then its set-up (hardware-dm32.md 9.1)
	hrc7000_wr(d, C7_SYS_SOFT_RSTN, hrc7000_rd(d, C7_SYS_SOFT_RSTN) & ~0x10u);
	spin(CODEC_RESET_SPINS);
	hrc7000_codec_wr(d, C7_CODEC_AICR_DAC, 0xC0);
	hrc7000_codec_wr(d, C7_CODEC_AICR_ADC, 0xC0);
	hrc7000_codec_wr(d, C7_CODEC_CR_MIC1, 0x00);
	hrc7000_codec_wr(d, C7_CODEC_CR_MIC2, 0x70);
	hrc7000_codec_wr(d, C7_CODEC_CR_DAC, 0x20);
	hrc7000_codec_wr(d, C7_CODEC_CR_ADC, 0x20);
	hrc7000_codec_wr(d, C7_CODEC_CR_VIC, 0x0C);
	hrc7000_codec_wr(d, C7_CODEC_CR_CK, 0x40);
	hrc7000_codec_wr(d, C7_CODEC_GCR_DACL, C7_GCR_DACL_0DB);
	hrc7000_codec_wr(d, C7_CODEC_GCR_MIC1, 0x00);
	hrc7000_codec_wr(d, C7_CODEC_GCR_ADCL, 0x8B);
	hrc7000_codec_wr(d, C7_CODEC_CR_DAC_AGC, 0x00);
	hrc7000_codec_wr(d, C7_CODEC_CR_ADC_AGC, 0x00);
	hrc7000_codec_wr(d, C7_CODEC_DR_ADC_AGC, 0x34);
	hrc7000_wr(d, C7_LINEOUT_CTRL, 0x01);

	hrc7000_wr(d, C7_SIG_CENTER, hrc7000_sig_center(tx_center));
	hrc7000_wr(d, C7_RF_MOD_BIAS, hrc7000_mod_bias(tx_center, HRC7000_MODCAL_DEFAULT));
	hrc7000_wr(d, C7_RF_MODE, RF_MODE_VALUE);
	hrc7000_wr(d, C7_DEV_LIMITER, 0x0000FF00u);
	hrc7000_wr(d, C7_SYS_SOFT_RSTN, 0x190);
	hrc7000_wr(d, C7_DAC_CONTROL, 0x8000001Bu);
	hrc7000_wr(d, C7_ADC_CONTROL, hrc7000_rd(d, C7_ADC_CONTROL) | 0x41C3u);
	d->audio_ctl = C7_AUDIO_I2S_SLAVE;
	hrc7000_wr(d, C7_AUDIO_CONTROL, d->audio_ctl);
	hrc7000_wr(d, C7_AUDIO_BUFFER_CLR, 0x03);

	// DMR, written once (gotchas 2.2): ETSI sync words, thresholds, CRC / RS masks (2.2, 2.3)
	hrc7000_wr(d, C7_SEND_DATA_SYNC_H, 0xD5D7F7);
	hrc7000_wr(d, C7_SEND_DATA_SYNC_L, 0x7FD757);
	hrc7000_wr(d, C7_SEND_RC_SYNC_H, 0x77D55F);
	hrc7000_wr(d, C7_SEND_RC_SYNC_L, 0x7DFD77);
	hrc7000_wr(d, C7_RECV_MS_SYNC_H, 0xD5D7F7);
	hrc7000_wr(d, C7_RECV_MS_SYNC_L, 0x7FD757);
	hrc7000_wr(d, C7_RECV_BS_SYNC_H, 0xDFF57D);
	hrc7000_wr(d, C7_RECV_BS_SYNC_L, 0x75DF5D);
	hrc7000_wr(d, C7_RECV_TDMA1_SYNC_H, 0xF7FDD5);
	hrc7000_wr(d, C7_RECV_TDMA1_SYNC_L, 0xDDFD55);
	hrc7000_wr(d, C7_RECV_TDMA2_SYNC_H, 0xD7557F);
	hrc7000_wr(d, C7_RECV_TDMA2_SYNC_L, 0x5FF7F5);
	hrc7000_wr(d, C7_RECV_RC_SYNC_H, 0x77D55F);
	hrc7000_wr(d, C7_RECV_RC_SYNC_L, 0x7DFD77);
	hrc7000_wr(d, C7_THRESHOLD, 0x0978786Fu);
	hrc7000_wr(d, C7_PI_CRC_MASK, 0x6969);
	hrc7000_wr(d, C7_RS_H_MASK, 0x969696);
	hrc7000_wr(d, C7_RS_H_INIT, 0);
	hrc7000_wr(d, C7_RS_T_MASK, 0x999999);	// the reset value 0x969696 is wrong for ETSI
	hrc7000_wr(d, C7_RS_T_INIT, 0);
	hrc7000_wr(d, C7_CSBK_CRC_MASK, 0xA5A5);
	hrc7000_wr(d, C7_MBCH_CRC_MASK, 0xAAAA);
	hrc7000_wr(d, C7_DATAH_CRC_MASK, 0xCCCC);
	hrc7000_wr(d, 0x370, 0xF0);		// undocumented, as the working firmware
	hrc7000_wr(d, 0x374, 0x1FF);
	hrc7000_wr(d, 0x378, 0x10F);
	hrc7000_wr(d, 0x37C, 0x00);
	hrc7000_wr(d, C7_CRC8_INIT, 0);
	hrc7000_wr(d, C7_VOICE_EMB_CTRL, 0);
	hrc7000_wr(d, C7_SCRAMBLE_REG, 0);
	hrc7000_wr(d, C7_SYS_INTERP_MASK, SYS_INT_ENABLE);
	hrc7000_wr(d, C7_LAYER2_SLOT_PRE, 0x551010);
	hrc7000_wr(d, C7_REG_X06, 0x14);
	hrc7000_wr(d, C7_RF_RECV_DELAY, 0x36B0);	// 1.4 ms
	hrc7000_wr(d, C7_RX_IF_FREQ, 0x0BB800);
	hrc7000_wr(d, C7_TX_IF_FREQ, 0x0BB800);
	hrc7000_wr(d, C7_RECV_MS_SYNC_H, hrc7000_rd(d, C7_RECV_MS_SYNC_H) | (1u << 31));	// undocumented enables
	hrc7000_wr(d, C7_RECV_BS_SYNC_H, hrc7000_rd(d, C7_RECV_BS_SYNC_H) | (1u << 31));
	// The same enable for the TDMA direct mode syncs: without it they are never detected (bench, 2026-10-16)
	hrc7000_wr(d, C7_RECV_TDMA1_SYNC_H, hrc7000_rd(d, C7_RECV_TDMA1_SYNC_H) | (1u << 31));
	hrc7000_wr(d, C7_RECV_TDMA2_SYNC_H, hrc7000_rd(d, C7_RECV_TDMA2_SYNC_H) | (1u << 31));
	hrc7000_wr(d, C7_RF_CONTROL, RF_CONTROL_VALUE);
	hrc7000_wr(d, C7_WORK_MODE, C7_WORK_MODE_FM);
	hrc7000_wr(d, C7_FM_TONE1, FM_TONE1_DEFAULT);	// DTMF tone steps (8.3)
	hrc7000_wr(d, C7_FM_TONE2, 0x06D10787u);
	hrc7000_wr(d, C7_FM_TONE3, FM_TONE3_DEFAULT);
	hrc7000_wr(d, C7_FM_TONE4, 0x0BD10D10u);
	// The reset value, not what the register holds: a restart that doesn't reset the modem (the boot stub's) finds the
	// last transmission's value there, and at 25 kHz that is twice the narrow one (0xFF on the DM-32UV: 1.9 kHz CTCSS)
	d->subaudio_dev = SUBAUDIO_DEV_RESET;
	hrc7000_wr(d, C7_SUBVOICE_COEF, (hrc7000_rd(d, C7_SUBVOICE_COEF) & ~0xFF00u) | (SUBAUDIO_DEV_RESET << 8));
	hrc7000_set_mic_gain(d, mic_gain_db);
}

void hrc7000_audio_bits(hrc7000_t *d, uint32_t set, uint32_t clear)
{
	d->audio_ctl = (d->audio_ctl & ~clear) | set;
	hrc7000_wr(d, C7_AUDIO_CONTROL, d->audio_ctl);
}

void hrc7000_fm_rx_audio_irq(hrc7000_t *d, bool on)
{
	uint32_t m = hrc7000_rd(d, C7_SYS_INTERP_MASK);
	hrc7000_wr(d, C7_SYS_INTERP_MASK, on ? (m | C7_SYS_FM_RX) : (m & ~C7_SYS_FM_RX));
}

void hrc7000_codec_mute(hrc7000_t *d, bool mic_muted, bool lineout_muted)
{
	uint8_t adc = hrc7000_codec_rd(d, C7_CODEC_CR_ADC), dac = hrc7000_codec_rd(d, C7_CODEC_CR_DAC);
	hrc7000_codec_wr(d, C7_CODEC_CR_ADC, (uint8_t)(mic_muted ? (adc | 0x80u) : (adc & 0x7Fu)));
	hrc7000_codec_wr(d, C7_CODEC_CR_DAC, (uint8_t)(lineout_muted ? (dac | 0x80u) : (dac & 0x7Fu)));
}

void hrc7000_fm_rx(hrc7000_t *d, bool wide)
{
	hrc7000_wr(d, C7_FM_BANDWIDTH, wide ? 0x4F : 0x40);
	hrc7000_wr(d, C7_FM_DEV_COEF, wide ? 0x40A20 : 0x40A25);
	hrc7000_wr(d, C7_FM_PTT, 0);
	// Not the PCM bits: the PCM stream owns them (platform audio_pcm.c). Clearing the mic PCM bit here stopped a beep's
	// stream when its own key retuned the receiver, and no beep played again
	hrc7000_audio_bits(d, C7_AUDIO_FM_PLAY, 0);
	hrc7000_codec_wr(d, C7_CODEC_GCR_DACL, C7_GCR_DACL_0DB);
	hrc7000_wr(d, C7_WORK_MODE, C7_WORK_MODE_FM);
}

void hrc7000_fm_tx(hrc7000_t *d, bool wide)
{
	hrc7000_wr(d, C7_FM_BANDWIDTH, wide ? 0x5F : 0x50);
	hrc7000_wr(d, C7_FM_DEV_COEF, wide ? 0x40A20 : 0x40A25);
	// Sub-audio deviation (8.2): the bandwidth bits don't scale it; twice the narrow value at 25 kHz
	uint32_t sv_dev = wide ? (uint32_t)d->subaudio_dev * 2u : d->subaudio_dev;
	hrc7000_wr(d, C7_SUBVOICE_COEF, (hrc7000_rd(d, C7_SUBVOICE_COEF) & ~0xFF00u) | ((sv_dev > 0xFFu ? 0xFFu : sv_dev) << 8));
	hrc7000_audio_bits(d, 0, C7_AUDIO_FM_PLAY);
	hrc7000_wr(d, C7_WORK_MODE, C7_WORK_MODE_FM);
	hrc7000_wr(d, C7_FM_PTT, 1);
}

void hrc7000_fm_idle(hrc7000_t *d)
{
	hrc7000_tx_tone(d, 0, 0);
	hrc7000_wr(d, C7_FM_PTT, 0);
	hrc7000_audio_bits(d, 0, C7_AUDIO_FM_PLAY);
}

void hrc7000_dmr_enter(hrc7000_t *d, uint32_t mod_bias, uint8_t mic_gain_db)
{
	hrc7000_wr(d, C7_FM_PTT, 0);
	hrc7000_audio_bits(d, 0, C7_AUDIO_FM_PLAY);
	hrc7000_wr(d, C7_RF_MOD_BIAS, mod_bias);
	hrc7000_set_mic_gain(d, mic_gain_db);
	hrc7000_wr(d, C7_WORK_MODE, C7_WORK_MODE_FM);	// through FM: resets the DMR state machine
	hrc7000_wr(d, C7_WORK_MODE, C7_WORK_MODE_DMR);
}

// Table 59 (DS): tone address for each standard tone, Hz x 10. The address order isn't ascending
static const uint16_t ctcss_by_addr[] = {
	0, 670, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
	1318, 1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799, 1862, 1928, 2035, 2107, 2181, 2257, 2336, 2418,
	2503, 693, 625, 1598, 1655, 1713, 1773, 1835, 1899, 1966, 1995, 2065, 2291, 2541,
};

uint8_t hrc7000_ctcss_addr(uint16_t hz_x10)
{
	for (uint8_t a = 1; a < sizeof(ctcss_by_addr) / sizeof(ctcss_by_addr[0]); a++) {
		if (ctcss_by_addr[a] == hz_x10) {
			return a;
		}
	}
	return 0;
}

uint32_t hrc7000_user_ctcss(uint16_t hz_x10)
{
	// f x 65.536 = (Hz x 10) x 6.5536
	return (1u << 31) | ((((uint32_t)hz_x10 * 65536u) + 5000u) / 10000u & 0x1FFFFFu);
}

void hrc7000_set_subtone(hrc7000_t *d, bool tx, hrc7000_tone_t type, uint16_t value)
{
	uint32_t mod = hrc7000_rd(d, C7_FM_MOD) & ~(FM_MOD_CTCSS | FM_MOD_CDCSS | FM_MOD_CDCSS_TX_INV | FM_MOD_CDCSS_RX_INV);
	if (type == HRC7000_TONE_CTCSS) {
		uint8_t addr = hrc7000_ctcss_addr(value);
		uint32_t a = hrc7000_rd(d, C7_CTCSS_ADDR);
		a = tx ? ((a & ~0xFFu) | addr) : ((a & ~0xFF00u) | ((uint32_t)addr << 8));
		hrc7000_wr(d, C7_CTCSS_ADDR, a);
		hrc7000_wr(d, C7_USER_DEFINE_CTC, addr == 0 ? hrc7000_user_ctcss(value) : 0);
		mod |= FM_MOD_CTCSS;
	} else if (type == HRC7000_TONE_DCS || type == HRC7000_TONE_DCS_INV) {
		// The TX code is taken when FM_PTT goes on: written after it, the radio sent the previous code (SDR bench)
		uint32_t c = hrc7000_rd(d, C7_CDCSS_ADDR);
		c = tx ? ((c & ~0x1FFu) | (value & 0x1FFu)) : ((c & ~0x1FF0000u) | ((uint32_t)(value & 0x1FFu) << 16));
		hrc7000_wr(d, C7_CDCSS_ADDR, c);
		mod |= FM_MOD_CDCSS;
		if (type == HRC7000_TONE_DCS_INV) {
			mod |= tx ? FM_MOD_CDCSS_TX_INV : FM_MOD_CDCSS_RX_INV;
		}
	}
	hrc7000_wr(d, C7_FM_MOD, mod);
}

bool hrc7000_subtone_ok(hrc7000_t *d)
{
	uint32_t mod = hrc7000_rd(d, C7_FM_MOD);
	uint32_t flags = hrc7000_rd(d, C7_FM_DECODE_FLAG2);
	if (mod & FM_MOD_CTCSS) {
		return (flags & 0x1u) != 0;
	}
	if (mod & FM_MOD_CDCSS) {
		return (flags & 0x2u) != 0;
	}
	return true;
}

void hrc7000_tx_tone(hrc7000_t *d, uint16_t hz, uint8_t dev)
{
	if (hz != 0) {
		uint32_t step = ((uint32_t)hz * 2048u + 500u) / 1000u;	// round(f x 2.048)
		if (!d->tone_on) {
			d->subvoice = hrc7000_rd(d, C7_SUBVOICE_COEF);
		}
		hrc7000_wr(d, C7_SUBVOICE_COEF, (d->subvoice & ~0xFF00u) | ((uint32_t)dev << 8));
		hrc7000_wr(d, C7_FM_TONE1, (step << 16) | (FM_TONE1_DEFAULT & 0xFFFFu));
		hrc7000_wr(d, C7_FM_TONE3, (step << 16) | (FM_TONE3_DEFAULT & 0xFFFFu));
		hrc7000_wr(d, C7_FM_MOD, hrc7000_rd(d, C7_FM_MOD) | FM_MOD_DTMF);
		hrc7000_wr(d, C7_TONE_SEND_CTRL, TONE_SEND_START | TONE_DIGIT_1);
		d->tone_on = true;
	} else if (d->tone_on) {
		hrc7000_wr(d, C7_TONE_SEND_CTRL, 0);	// the reset value
		hrc7000_wr(d, C7_FM_MOD, hrc7000_rd(d, C7_FM_MOD) & ~FM_MOD_DTMF);
		hrc7000_wr(d, C7_FM_TONE1, FM_TONE1_DEFAULT);
		hrc7000_wr(d, C7_FM_TONE3, FM_TONE3_DEFAULT);
		hrc7000_wr(d, C7_SUBVOICE_COEF, d->subvoice);
		d->tone_on = false;
	}
}

// About 5.8 Hz of deviation per step (UHF, DM-1801 Pro on the SDR bench: 0x60 = 559 Hz, 0xC0 = 1117 Hz). The guide's
// DTMF values (0x12 at UHF 12.5 kHz) gave about 100 Hz, too little to hear. 0xFF (about 1.5 kHz) is the most the
// field holds, so 25 kHz gets less than twice the narrow value. VHF is unmeasured
uint8_t hrc7000_dtmf_dev(uint32_t hz, bool wide)
{
	(void)hz;
	return wide ? 0xFFu : 0xC0u;
}

void hrc7000_tx_tone_key(hrc7000_t *d, bool down)
{
	if (d->tone_on) {
		hrc7000_wr(d, C7_TONE_SEND_CTRL, (down ? TONE_SEND_START : 0u) | TONE_DIGIT_1);
	}
}
