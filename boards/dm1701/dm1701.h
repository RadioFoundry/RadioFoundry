// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DM-1701 board files
#ifndef DM1701_H
#define DM1701_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

// The LCD data bus doubles as the keypad columns: the display and the keypad scan take turns
void dm1701_lcdbus_lock(void);
void dm1701_lcdbus_unlock(void);
// Reconnect LCD D0-D7 (the keypad columns) to the FSMC; the display calls it at the start of every burst, lock held
void dm1701_lcd_pins_fsmc(void);

// DMR voice PCM to the speaker (radio_dm1701.c; audio_dm1701.c's hal_audio_t.speaker_pcm calls it): combined with the
// FM squelch request, FM feed-through off while PCM plays. Callable from the audio and radio tasks
void dm1701_speaker_pcm(bool on);

// Extra analog readings (power_dm1701.c)
uint16_t dm1701_mic_level_raw(void);
int16_t dm1701_temperature_c10(void);

// Ops each board file provides (display_dm1701.c, input_dm1701.c, radio_dm1701.c, power_dm1701.c, flash_dm1701.c)
extern const hal_display_t dm1701_display;
extern const hal_input_t dm1701_input;
extern const hal_radio_t dm1701_radio;
extern const hal_power_t dm1701_power;
extern hal_flash_t dm1701_flash;
extern const hal_audio_t dm1701_audio;
extern const hal_vocoder_t dm1701_vocoder;

#if CONFIG_AUDIO_PROBE || CONFIG_NET_USRP || CONFIG_APRS_RX
// The audio tap: FM receive audio on the PCM stream for APRS (CONFIG_APRS_RX) and USRP, FM transmit audio for USRP
// (CONFIG_NET_USRP), the probe (CONFIG_AUDIO_PROBE, docs/spec/hardware-dm1701.md
// 21.3). audio_dm1701.c; register access and the transmit source switch in radio_dm1701.c
extern const hal_audio_tap_t dm1701_audio_tap;
#endif
#if CONFIG_NET_USRP
bool dm1701_fm_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t mode);
void dm1701_pwm_audio(audio_tap_pull_t pull);	// PWM audio on PC8: start (pull) or stop (NULL)
#endif
#if CONFIG_AUDIO_PROBE
bool dm1701_modem_reg(uint8_t page, uint8_t reg, uint8_t *val, bool write);
bool dm1701_modem_ram(uint8_t page, uint16_t addr, uint8_t *buf, uint16_t len, bool write);
bool dm1701_modem_burst(uint8_t page, uint8_t reg, const uint8_t *buf, uint16_t len);
#endif

// Physical key bits returned by dm1701_input.scan() (matrix positions in docs/spec/hardware-dm1701.md 10.1)
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_GREEN, KEY_RED,
	KEY_FN_LEFT, KEY_FN_RIGHT, KEY_TOP, KEY_SK1, KEY_SK2, KEY_PTT,
	KEY_COUNT
};

// SPI flash security registers (1..3, 256 bytes each): factory calibration in 1-2, panel type and model flags in 3
bool dm1701_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len);
bool dm1701_cps_security_read(uint32_t addr, void *buf, uint32_t len);

// Diagnostics (diag_dm1701.c and the accessors it reads; all return cached values, no bus traffic)
typedef struct {
	uint8_t rssi, noise;	// AT1846S raw readings from the last status poll
	uint16_t flags;		// AT1846S reg 0x1C from the last status poll
	uint8_t c6000_r10;	// HR_C6000 4:0x10 read back after init (0x80 = FM)
	bool c6000_ok;		// that read worked
	uint32_t bus_errors;
} dm1701_radio_diag_t;
void dm1701_radio_diag(dm1701_radio_diag_t *d);
uint8_t dm1701_display_panel_type(void);
uint16_t dm1701_adc_raw(uint8_t ch);	// 0 volume, 1 battery, 2 mic, 3 temperature
const char *dm1701_cal_source(void);	// "working", "factory" or "none"
void dm1701_cal_why(char *out, uint8_t len, uint8_t marker[8]);
uint8_t dm1701_cal_byte(uint16_t offset);
bool dm1701_diag_line(uint8_t i, char *buf, uint8_t len);
// Zero the counters behind the diagnostics lines (board.diag_reset)
void dm1701_diag_reset(void);

// Factory calibration (calibration_dm1701.c, docs/spec/hardware-dm1701.md section 12)
typedef struct {
	bool valid;
	uint16_t power_dac[4];	// very low, low, mid, high at the requested frequency (12-bit DAC values)
	uint8_t fm_i, fm_q;	// HR_C6000 4:0x45 / 4:0x46 for FM TX
	uint8_t dmr_i, dmr_q;	// for DMR
	int8_t ref_offset;	// reference oscillator tune - 128
} dm1701_cal_point_t;

// APRS AFSK through the FM transmitter (radio_dm1701.c for afsk_dm1701.c): begin and end at task level with FM TX
// keyed, tone from the TIM6 interrupt
bool dm1701_afsk_begin(uint16_t hz, uint8_t level);
bool dm1701_afsk_tone(uint16_t hz);
void dm1701_afsk_end(void);
extern const hal_afsk_t dm1701_afsk;

bool dm1701_cal_load(void);
// Calibration values interpolated at a frequency (Hz); returns false if no calibration is loaded (safe defaults filled)
bool dm1701_cal_at(uint32_t hz, dm1701_cal_point_t *out);

// A DMA stream's memory address must be in main SRAM (0x2000_0000): the DMA controllers can't reach the CCM, where
// ODR_CPU_RAM state and every task stack live. Stops here, at set-up, instead of a silent transfer error later
#define DMA_RAM_OK(p)	configASSERT(((uintptr_t)(p) >> 24) == 0x20u)

#endif
