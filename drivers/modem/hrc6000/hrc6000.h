// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C6000 DMR / FM baseband chip driver (docs/spec/radios.md): its power-on set-up and FM modes
// (docs/spec/modem-hrc6000.md, docs/spec/hardware-dm1701.md 4 and 12). The register access and the DMR slot engine
// are shared with the HR_C5000 (drivers/modem/hrc_common); the HR_C5000 has its own driver (drivers/modem/hrc5000).
#ifndef HRC6000_H
#define HRC6000_H

#include "modem/hrc_common/hrc.h"

// Which tone deviation register 0xA0 gets during FM TX
typedef enum { HRC6000_TONE_NONE, HRC6000_TONE_CTCSS, HRC6000_TONE_DCS } hrc6000_tone_t;

// FM TX deviation coefficients (hardware-dm1701.md 12.5, modem-hrc6000.md 8)
typedef struct {
	uint8_t voice;	// 4:0x35
	uint8_t ctcss;	// 4:0xA0 with CTCSS
	uint8_t dcs;	// 4:0xA0 with DCS
	uint8_t dtmf;	// 4:0xA0 for DTMF
} hrc6000_fm_dev_t;

typedef struct {
	bool uhf;		// above 300 MHz
	bool wide;		// 25 kHz channel
	uint8_t tone;		// hrc6000_tone_t
	uint8_t mod2;		// 4:0x45 (calibration "I", FM pair)
	uint8_t mod1;		// 4:0x46 (calibration "Q", FM pair)
	int8_t ref_offset;	// reference oscillator tune - 128 for the TX band
	uint8_t mic_gain;	// 0..31
} hrc6000_fm_tx_t;

// The PLL and clock dividers for the clock the board feeds XTALI (modem-hrc6000.md 1.2). Both presets are the
// manual's table 4.1 rows: the PLL makes 49.152 MHz, then Sys_clk 9.8304 MHz, the codec 12.288 MHz, CLKOUT 24.576 MHz
typedef struct {
	uint8_t pll_m;		// 4:0x0B
	uint8_t pll_n;		// 4:0x0C (PLLN and the output divider)
	uint8_t sys_div;	// 4:0xB9
	uint8_t codec_div;	// 4:0xBA
	uint8_t clkout_div;	// 4:0xBB
} hrc6000_clock_t;
extern const hrc6000_clock_t hrc6000_clock_29m4912;	// a 29.4912 MHz reference (hardware-dm1701.md)
extern const hrc6000_clock_t hrc6000_clock_12m288;	// a 12.288 MHz reference (hardware-gd77.md 5.2)
// The FM TX signaling generator used for single tones and DTMF (hrc6000_tx_pair): the registers it borrows, kept
// to give back when the tone ends
typedef struct {
	bool on;
	uint8_t dev;		// 4:0xA0 while the tone sounds
	bool single;		// one tone (the 2-tone mode), else a DTMF pair
	uint8_t a0, a1, a3, a4, a5, a6, d1, e0;	// the values before the tone
	uint8_t code[8];	// 4:0xA8-0xAF before the tone (shared with the CTCSS index and DCS code)
	bool timed;		// hrc6000_tx_sig_service has seen this sequence start, at t_ms
	uint32_t t_ms;
} hrc6000_tx_sig_t;

// Bind the HR_C6000's chip description to a bus
void hrc6000_attach(hrc_t *dev, const hrc_bus_t *bus);
// The receive I/Q order, 4:0x01 bit 6 (modem-hrc6000.md 2): swapped (the default) or not. With an IF input it
// inverts the received spectrum, so it must match the radio's mixers; a board whose first LO is above the signal on
// one band and below it on another sets it per band. Kept for every later write of 0x01; dmr: also write it now (the
// chip is in DMR receive)
bool hrc6000_rx_iq_swap(hrc_t *dev, bool swap, bool dmr);

// Power-on initialization, modem-hrc6000.md section 3 steps 1-14. The board has already taken PWD low and waited
// 10 ms. Leaves the chip in its DMR layer 2 receive set-up; call a hrc6000_set_mode_*() next. The plain form uses the
// 29.4912 MHz clock preset
bool hrc6000_power_on_init(hrc_t *dev, uint8_t dmr_mic_gain);
bool hrc6000_power_on_init_clock(hrc_t *dev, uint8_t dmr_mic_gain, const hrc6000_clock_t *clk);

// FM receive (modem-hrc6000.md 8). The audio feed-through is left as it was: see hrc6000_fm_audio()
bool hrc6000_set_mode_fm_rx(hrc_t *dev);
// FM transmit: two-point modulation of the microphone with the deviation for the band and bandwidth
bool hrc6000_set_mode_fm_tx(hrc_t *dev, const hrc6000_fm_tx_t *cfg);
// FM receive audio feed-through to the line out (4:0x36): on, or muted
bool hrc6000_fm_audio(hrc_t *dev, bool on);
// Reference oscillator offset (hardware-dm1701.md 12.4). fm_tx: the FM TX form (0x48 = 0)
bool hrc6000_set_ref_offset(hrc_t *dev, int8_t offset, bool fm_tx);
// Two-point modulation bias and DC offset where a radio's calibration holds them as such: 4:0x47 = the
// bias's low byte, 4:0x48 = the bits above it, 4:0x04 = the offset. The other form of the same registers is
// hrc6000_set_ref_offset()
bool hrc6000_set_mod_bias(hrc_t *dev, uint16_t bias, uint8_t offset);
// 4:0xA1 = 0x00 (modem-hrc6000.md 8). The power-on init leaves 0x80, whose bit 7 the manual marks reserved, must be 0.
// The other boards receive DMR with it; on a board that feeds the chip a 450 kHz IF it stops every DMR sync
bool hrc6000_tones_off(hrc_t *dev);
// FM receive on a board whose FM audio doesn't pass through the chip: the modulator back to DMR (4:0x10 bit 7 clear).
// In FM mode the codec's line out carries only the chip's own FM receive audio, gated by 4:0x36 bit 1, so PCM (beeps,
// prompts) plays silent there; in DMR mode it plays (modem-hrc6000.md 8). After hrc6000_set_mode_fm_rx()
bool hrc6000_fm_rx_pcm(hrc_t *dev);
// RX audio gain, -31..+31 dB (4:0x37 = 0x80 + 0x40 if positive + the low 5 bits of the gain, modem-hrc6000.md 2)
bool hrc6000_set_rx_gain(hrc_t *dev, int8_t db);

// During FM TX: send the pair low_hz + high_hz (a DTMF digit; the same frequency twice is one tone, the 1750 Hz burst
// or a roger beep) from the chip's DTMF generator at deviation coefficient dev (4:0xA0) in place of the microphone,
// until called with 0, 0. The generator sends digit "1" with its 697 and 1209 Hz table entries rewritten, so any
// pair needs no digit codes (modem-hrc6000.md 8; the manual states only some of them). One code lasts at most 510 ms,
// with about 20 ms of silence in it, so a pair is 16 codes: HRC6000_TX_SIG_MS, then the generator stops. A single
// tone (low_hz == high_hz) comes from the 2-tone mode instead, gapless, at least 25.5 s. hrc6000_tx_sig_service
// starts the sequence again before either runs out
#define HRC6000_TX_SIG_MS	8160u	// 16 codes of 510 ms
#define HRC6000_TX_SIG_RENEW_MS	7500u	// hrc6000_tx_sig_service restarts a pair's sequence this old
#define HRC6000_TX_TONE_MS	25500u	// the 2-tone mode's first tone
#define HRC6000_TX_TONE_RENEW_MS	24000u	// and a single tone's this old
bool hrc6000_tx_pair(hrc_t *dev, hrc6000_tx_sig_t *s, uint16_t low_hz, uint16_t high_hz, uint8_t dev_coef);
// While hrc6000_tx_pair's tone runs: the tone (down) or a silent carrier, the microphone still off (Morse keying)
bool hrc6000_tx_sig_key(hrc_t *dev, hrc6000_tx_sig_t *s, bool down);
// While hrc6000_tx_pair's tone runs: change its frequencies only (the table entries: four single-register writes for
// a pair, ten for a single tone, each atomic on the bus), for AFSK symbol by symbol from an interrupt. False when no tone runs
bool hrc6000_tx_sig_retune(hrc_t *dev, hrc6000_tx_sig_t *s, uint16_t low_hz, uint16_t high_hz);
// While a tone runs, from a task-level poll (more often than every 600 ms): the sequence started again before its
// 16 codes run out, so the tone lasts until hrc6000_tx_pair(0, 0). The first call after a start only notes now_ms
bool hrc6000_tx_sig_service(hrc_t *dev, hrc6000_tx_sig_t *s, uint32_t now_ms);
// Pure: the generator's phase step for hz, f x 65536 / 32000 rounded (modem-hrc6000.md 8)
uint16_t hrc6000_tone_step(uint16_t hz);

// Pure: the FM TX deviation coefficients for a band and bandwidth
hrc6000_fm_dev_t hrc6000_fm_deviation(bool uhf, bool wide);

#endif
