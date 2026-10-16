// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 baseband (the modem inside the DM-32's SoC): register map, power-on set-up, FM modes, sub-audio, audio
// paths and the internal audio codec (docs/spec/modem-hrc7000.md 2, 4, 5.1, 8; hardware-dm32.md 9).
//
// The modem is memory mapped: 32-bit registers at 0x11000000, byte buffers at 0x16000000. The driver reaches both
// through the base pointers given to hrc7000_attach, so host tests can run it on plain arrays. Peripherals outside
// the modem (the DAC for the PA drive, the ADC, the IO mux) belong to the board.
#ifndef HRC7000_H
#define HRC7000_H

#include <stdint.h>
#include <stdbool.h>

#define HRC7000_REG_BASE	0x11000000u
#define HRC7000_BUF_BASE	0x16000000u

// --- Registers (byte offsets from HRC7000_REG_BASE) ---
#define C7_SYS_SOFT_RSTN	0x000u
#define C7_DAC_CONTROL		0x070u
#define C7_ADC_CONTROL		0x074u
#define C7_AUDIO_CONTROL	0x080u
#define C7_AUDIO_BUFFER_CLR	0x084u
#define C7_LINEOUT_CTRL		0x088u
#define C7_WORK_MODE		0x100u
#define C7_RF_MODE		0x104u
#define C7_SIG_CENTER		0x108u
#define C7_TX_IF_FREQ		0x10Cu
#define C7_RF_CONTROL		0x110u
#define C7_RF_MOD_BIAS		0x114u
#define C7_DEV_LIMITER		0x118u
#define C7_THRESHOLD		0x120u
#define C7_SEND_DATA_SYNC_H	0x12Cu
#define C7_SEND_DATA_SYNC_L	0x130u
#define C7_SEND_RC_SYNC_H	0x134u
#define C7_SEND_RC_SYNC_L	0x138u
#define C7_RECV_MS_SYNC_H	0x13Cu
#define C7_RECV_MS_SYNC_L	0x140u
#define C7_RECV_BS_SYNC_H	0x144u
#define C7_RECV_BS_SYNC_L	0x148u
#define C7_RECV_TDMA1_SYNC_H	0x14Cu
#define C7_RECV_TDMA1_SYNC_L	0x150u
#define C7_RECV_TDMA2_SYNC_H	0x154u
#define C7_RECV_TDMA2_SYNC_L	0x158u
#define C7_RECV_RC_SYNC_H	0x15Cu
#define C7_RECV_RC_SYNC_L	0x160u
#define C7_RECV_INFO_COUNT	0x164u
#define C7_REG_X06		0x168u
#define C7_RX_IF_FREQ		0x1B0u
#define C7_RF_RECV_DELAY	0x1B4u
#define C7_PI_CRC_MASK		0x350u
#define C7_RS_H_MASK		0x354u
#define C7_RS_H_INIT		0x358u
#define C7_RS_T_MASK		0x35Cu
#define C7_RS_T_INIT		0x360u
#define C7_CSBK_CRC_MASK	0x364u
#define C7_MBCH_CRC_MASK	0x368u
#define C7_DATAH_CRC_MASK	0x36Cu
#define C7_CRC8_INIT		0x380u
#define C7_LOCAL_CC		0x384u
#define C7_VOICE_EMB_CTRL	0x388u
#define C7_SCRAMBLE_REG		0x38Cu
#define C7_RX_TYPE_INFO		0x390u
#define C7_RX_INFO		0x394u
#define C7_SYS_INTERP_LIST	0x398u
#define C7_SYS_INTERP_MASK	0x39Cu	// 1 = enabled
#define C7_SYS_INTERP_CLEAR	0x3A0u
#define C7_INTERP_CLEAR		0x3B0u
#define C7_LAYER2_CONTROL	0x400u
#define C7_LAYER2_SLOTON	0x404u
#define C7_LAYER2_TXRX_CTRL	0x408u
#define C7_LAYER2_SEND_TYPE	0x418u
#define C7_LAYER2_STATUS	0x41Cu
#define C7_LAYER2_SLOT_PRE	0x424u
#define C7_FM_BANDWIDTH		0x500u
#define C7_FM_DEV_COEF		0x504u
#define C7_FM_SQL		0x508u
#define C7_SUBVOICE_COEF	0x50Cu
#define C7_FM_MOD		0x510u
#define C7_CTCSS_ADDR		0x518u
#define C7_USER_DEFINE_CTC	0x51Cu
#define C7_CDCSS_ADDR		0x520u
#define C7_TONE_SEND_CTRL	0x528u
#define C7_FM_TONE1		0x550u
#define C7_FM_TONE2		0x554u
#define C7_FM_TONE3		0x558u
#define C7_FM_TONE4		0x55Cu
#define C7_FM_PTT		0x560u
#define C7_FM_DECODE_FLAG2	0x568u
#define C7_FM_ADDR_SW		0x56Cu	// FM audio RAM half for the CPU: 1 = the first, 2 = the second (8.5)

// --- Buffers (byte offsets from HRC7000_BUF_BASE) ---
#define C7_TX_RAM		0x000u
#define C7_RX_RAM		0x4B0u
#define C7_FM_RX_AUDIO		(C7_RX_RAM + 0x30u)	// FM receive audio, two 512-byte halves (8.5)
#define C7_VOICE_RAM		0x960u	// 27 bytes, TX and RX
#define C7_PROMPT_FIFO		0x980u
#define C7_CODEC		0x9C0u	// internal audio codec, byte registers

// Codec registers (offsets from C7_CODEC)
#define C7_CODEC_AICR_DAC	0x08u
#define C7_CODEC_AICR_ADC	0x09u
#define C7_CODEC_CR_MIC1	0x0Bu
#define C7_CODEC_CR_MIC2	0x0Cu
#define C7_CODEC_CR_DAC		0x0Du	// b7 = mute line out
#define C7_CODEC_CR_ADC		0x0Fu	// b7 = mute the mic
#define C7_CODEC_CR_MIX		0x10u	// mixer: b7 enable, b6 load (1 = write), b5:0 the MIX_n address (DS 6.5.2.10)
#define C7_CODEC_DR_MIX		0x11u	// mixer data (DS 6.5.2.11)
#define C7_CODEC_CR_VIC		0x12u
#define C7_CODEC_CR_CK		0x13u
#define C7_CODEC_GCR_DACL	0x1Fu
#define C7_GCR_DACL_0DB		0x80u	// left shared, 0 dB
#define C7_CODEC_GCR_MIC1	0x23u
#define C7_CODEC_GCR_ADCL	0x25u
#define C7_CODEC_CR_DAC_AGC	0x2Bu
#define C7_CODEC_CR_ADC_AGC	0x2Fu
#define C7_CODEC_DR_ADC_AGC	0x30u

// WORK_MODE values: the only two the working firmware writes (2.1)
#define C7_WORK_MODE_FM		0x80u
#define C7_WORK_MODE_DMR	0x6Eu

// AUDIO_CONTROL bits (8.1)
#define C7_AUDIO_RING_FULL	(1u << 31)
#define C7_AUDIO_RING_PLAY	(1u << 7)
#define C7_AUDIO_FM_PLAY	(1u << 6)
#define C7_AUDIO_PCM_SPK	(1u << 5)	// CPU-fed speaker PCM
#define C7_AUDIO_PCM_MIC	(1u << 4)	// mic PCM to the CPU
#define C7_AUDIO_I2S_SLAVE	(1u << 1)

// INTERP_CLEAR bits (2.4)
#define C7_INT_TS_TX		(1u << 0)
#define C7_INT_TS_RX		(1u << 1)
#define C7_INT_RF_TX		(1u << 2)
#define C7_INT_RF_RX		(1u << 3)
#define C7_INT_PCM_WR		(1u << 4)
#define C7_INT_PCM_RD		(1u << 5)

// PCM buffers in the system SRAM shared with the modem (160 bytes = 80 samples each, 10 ms)
#define C7_PCM_MIC_ADDR		0x18000000u
#define C7_PCM_SPK_ADDR		0x180000A0u
#define C7_PCM_SAMPLES		80u
// Prompt FIFO (speaker samples, 8 kHz, low byte first): 64 bytes per push while AUDIO_CONTROL b31 is clear
#define C7_PROMPT_FIFO_ADDR	0x16000980u
#define C7_PROMPT_CHUNK		32u

typedef struct {
	volatile uint32_t *regs;	// HRC7000_REG_BASE
	volatile uint8_t *buf;		// HRC7000_BUF_BASE
	uint32_t audio_ctl;		// shadow of AUDIO_CONTROL (the CPU's bits)
	uint32_t subvoice;		// SUBVOICE_COEF before a TX tone (hrc7000_tx_tone)
	bool tone_on;
	uint8_t subaudio_dev;		// SUBVOICE_COEF b15:8 at 12.5 kHz (the chip's reset value, set by power_on_init)
	// FM transmit limiter, FM_DEV_COEF b23:16, at 12.5 and 25 kHz: the peak deviation of audio through the codec ADC
	// (the microphone, the codec mixer). The board sets it; 0 = HRC7000_FM_TX_LIMIT_DEF (8.1)
	uint8_t fm_tx_limit[2];
} hrc7000_t;

static inline void hrc7000_wr(hrc7000_t *d, uint32_t off, uint32_t v) { d->regs[off / 4u] = v; }
static inline uint32_t hrc7000_rd(hrc7000_t *d, uint32_t off) { return d->regs[off / 4u]; }
static inline void hrc7000_codec_wr(hrc7000_t *d, uint8_t reg, uint8_t v) { d->buf[C7_CODEC + reg] = v; }
static inline uint8_t hrc7000_codec_rd(hrc7000_t *d, uint8_t reg) { return d->buf[C7_CODEC + reg]; }

void hrc7000_attach(hrc7000_t *d, volatile uint32_t *regs, volatile uint8_t *buf);

// --- Calibration values (hardware-dm32.md 11.3) ---
// SIG_CENTER: TX I and Q center = the factory TX center byte (128 without calibration)
static inline uint32_t hrc7000_sig_center(uint8_t tx_center) { return ((uint32_t)tx_center << 24) | ((uint32_t)tx_center << 16); }
// RF_MOD_BIAS_CTRL: two-point bias from the TX center, modulation balance (b x 0x0101) from the frequency
static inline uint32_t hrc7000_mod_bias(uint8_t tx_center, uint16_t modcal) { return (((uint32_t)tx_center + 384u) << 16) | modcal; }
#define HRC7000_MODCAL_DEFAULT	0x4D4Du	// before a TX frequency is known
// The two-point modulation's DC level from the TX center byte in its exact form, as the DB25-D stock firmware computes
// it: round((1.228 + c x 0.422 / 128) x 1023 / 3.3), 381 at 0, 512 at 128 (hardware-db25d.md 6.4). The DM-32's
// c + 384 above is its straight-line fit
uint16_t hrc7000_mod_dc(uint8_t tx_center);
// SIG_CENTER and RF_MOD_BIAS_CTRL with that level: its low byte in both low bytes of SIG_CENTER, all 10 bits in the
// bias
static inline uint32_t hrc7000_sig_center_dc(uint8_t tx_center)
{
	return hrc7000_sig_center(tx_center) | (uint32_t)(hrc7000_mod_dc(tx_center) & 0xFFu) * 0x0101u;
}
static inline uint32_t hrc7000_mod_bias_dc(uint8_t tx_center, uint16_t modcal)
{
	return (uint32_t)(hrc7000_mod_dc(tx_center) & 0x3FFu) << 16 | modcal;
}

// --- Sequences ---

// Power-on set-up (modem-hrc7000.md 4 steps 4-32; the board does steps 1-3: ADC, IO mux and DAC). Leaves the modem in
// FM (WORK_MODE 0x80) with every DMR register written once, the modem system interrupts enabled at the chip and
// mic_gain (0..42 dB) in GCR_ADCL
void hrc7000_power_on_init(hrc7000_t *d, uint8_t tx_center, uint8_t mic_gain_db);
void hrc7000_set_mic_gain(hrc7000_t *d, uint8_t db);
// Speaker line out gain, GCR_DACL bits 5:0: 0..31 = 0..-31 dB, 32..63 = +32..+1 dB (hardware-dm32.md 9.1; the
// direction checked on the DM-32 at bring-up: code 0 and code 48 both louder than code 7)
void hrc7000_set_dac_gain(hrc7000_t *d, uint8_t code);

// FM receive / transmit (8.2). wide = 25 kHz. FM receive turns the FM audio path to the speaker on
void hrc7000_fm_rx(hrc7000_t *d, bool wide);
#define HRC7000_FM_TX_LIMIT_DEF	4u	// the KP value; about 1.65 kHz peak at 25 kHz (DM-1801 Pro BENCH)
void hrc7000_fm_tx(hrc7000_t *d, bool wide);
// FM_DEV_COEF for a transmission: the limiter for the bandwidth, the RX and TX deviation coefficients
uint32_t hrc7000_fm_tx_dev_coef(const hrc7000_t *d, bool wide);
// Back to the FM idle state: FM_PTT off, FM audio off
void hrc7000_fm_idle(hrc7000_t *d);

// Enter DMR (5.1): modulation balance for the TX frequency, mic gain, WORK_MODE 0x80 then 0x6E. The board turns the
// PA drive DAC back on first (FM receive powers it down)
void hrc7000_dmr_enter(hrc7000_t *d, uint32_t mod_bias, uint8_t mic_gain_db);

// Audio paths (AUDIO_CONTROL bits, the shadow keeps the CPU's view)
void hrc7000_audio_bits(hrc7000_t *d, uint32_t set, uint32_t clear);
void hrc7000_codec_mute(hrc7000_t *d, bool mic_muted, bool lineout_muted);
// Codec mixer: the ADC's output to the modem becomes the DAC's audio (the prompt FIFO) alone, the ADC's own input
// zeroed in the mix (MIX_2 mix_rec, MIX_3 mixadc_sel = 3). Off clears both and disables the mixer. The FM transmit
// source for audio the CPU makes (APRS AFSK) where ring play doesn't reach the modulator by itself (8.4)
void hrc7000_codec_dac_to_tx(hrc7000_t *d, bool on);

// --- Sub-audio by the modem's own engine (8.1, 8.2) ---

// CTCSS tone address for Hz x 10 (table 59), 0 if the tone isn't a standard one
uint8_t hrc7000_ctcss_addr(uint16_t hz_x10);
// USER_DEFINE_CTC step for a non-standard tone: round(f x 65.536), with the enable bit
uint32_t hrc7000_user_ctcss(uint16_t hz_x10);

typedef enum { HRC7000_TONE_NONE, HRC7000_TONE_CTCSS, HRC7000_TONE_DCS, HRC7000_TONE_DCS_INV } hrc7000_tone_t;
// TX and RX sub-audio; value = Hz x 10 for CTCSS, the 9-bit code for DCS (octal digits as written: 023 -> 19). Set the
// TX tone before hrc7000_fm_tx(): the modem takes the DCS code when FM_PTT goes on
void hrc7000_set_subtone(hrc7000_t *d, bool tx, hrc7000_tone_t type, uint16_t value);
// The received tone or code matches (FM_DECODE_FLAG2 b0 CTCSS / b1 CDCSS); true when no RX tone is set
bool hrc7000_subtone_ok(hrc7000_t *d);
// A single FM TX tone instead of the microphone (8.3): DTMF digit "1" with both of its tones at hz, at the DTMF
// deviation dev (SUBVOICE_COEF b15:8, which the sub-audio shares while the tone plays). hz = 0: the microphone, the
// sub-audio deviation and the default DTMF steps back. tx_tone_key: the tone off (a silent carrier) and on again
void hrc7000_tx_tone(hrc7000_t *d, uint16_t hz, uint8_t dev);
void hrc7000_tx_tone_key(hrc7000_t *d, bool down);
// TX tone deviation (SUBVOICE_COEF b15:8) for a TX frequency and bandwidth (8.3)
uint8_t hrc7000_dtmf_dev(uint32_t hz, bool wide);
// --- Prompt FIFO (8.4) ---

// Push one chunk (C7_PROMPT_CHUNK samples, low byte first) when the FIFO has room; false when it is full. Inline: the
// AFSK symbol interrupt uses it
static inline bool hrc7000_prompt_push(hrc7000_t *d, const int16_t *s)
{
	if ((hrc7000_rd(d, C7_AUDIO_CONTROL) & C7_AUDIO_RING_FULL) != 0) {
		return false;
	}
	volatile uint8_t *f = &d->buf[C7_PROMPT_FIFO];
	for (uint32_t i = 0; i < C7_PROMPT_CHUNK; i++) {
		uint16_t v = (uint16_t)s[i];
		f[2u * i] = (uint8_t)v;
		f[2u * i + 1u] = (uint8_t)(v >> 8);
	}
	return true;
}

// --- FM receive audio to the CPU (8.5) ---

// In FM the modem writes its demodulated receive audio, 8 kHz, into two 256-sample halves of the RX RAM in turn (high
// byte first) and raises SYS_INTERP bit 14 as each one fills; FM_ADDR_SW names the half the CPU may read
#define C7_SYS_FM_RX		(1u << 14)
#define HRC7000_FM_RX_SAMPLES	256u
#define HRC7000_FM_RX_RATE	8000u

// The SYS_INTERP_MASK bit for it (the power-on mask leaves it off)
void hrc7000_fm_rx_audio_irq(hrc7000_t *d, bool on);
// Copy the half FM_ADDR_SW names into out (HRC7000_FM_RX_SAMPLES); 0 when it names neither. Inline: it runs in the
// modem's system interrupt, which runs from RAM
static inline uint16_t hrc7000_fm_rx_audio(hrc7000_t *d, int16_t *out)
{
	uint32_t sw = hrc7000_rd(d, C7_FM_ADDR_SW) & 3u;
	if (sw != 1u && sw != 2u) {
		return 0;
	}
	const volatile uint8_t *p = &d->buf[C7_FM_RX_AUDIO + (sw - 1u) * HRC7000_FM_RX_SAMPLES * 2u];
	for (uint16_t i = 0; i < HRC7000_FM_RX_SAMPLES; i++) {
		out[i] = (int16_t)(uint16_t)((uint16_t)p[2u * i] << 8 | p[2u * i + 1u]);
	}
	return HRC7000_FM_RX_SAMPLES;
}

// FM noise measure (FM_SQL bits 15:0, higher = less signal)
static inline uint16_t hrc7000_fm_noise(hrc7000_t *d) { return (uint16_t)hrc7000_rd(d, C7_FM_SQL); }

#endif
