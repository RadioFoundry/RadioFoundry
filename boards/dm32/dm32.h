// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DM-32UV board files
#ifndef DM32_H
#define DM32_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

// Ops each board file provides
extern const hal_display_t dm32_display;
extern const hal_input_t dm32_input;
extern const hal_radio_t dm32_radio;
extern const hal_power_t dm32_power;
extern hal_flash_t dm32_flash;
extern const hal_dmr_modem_t dm32_dmr;
extern const hal_audio_t dm32_audio;
extern const hal_vocoder_t dm32_vocoder;

// Physical key bits returned by dm32_input.scan() (matrix in docs/spec/hardware-dm32.md 6.1, 6.2)
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_GREEN, KEY_RED,
	KEY_SK1, KEY_SK2, KEY_PTT,
	KEY_COUNT
};

// The LCD data lines DB0-DB4 are also the keypad rows: the display and the key scan take turns (board.c)
void dm32_lcdbus_lock(void);
void dm32_lcdbus_unlock(void);

// --- Factory calibration (calibration_dm32.c, hardware-dm32.md 11) ---

typedef struct {
	uint16_t very_low, low, mid, high;	// PA drive DAC values (0..4095) at a frequency
} dm32_power_points_t;

// Find and read the factory calibration (never writes). Needs the data flash
bool dm32_cal_load(void);
bool dm32_cal_valid(void);
const char *dm32_cal_source(void);	// "factory", "copy" (OpenDM32's copy at 0x710000) or "none"
uint8_t dm32_cal_tx_center(void);	// 128 without calibration
// Modulation balance (RF_MOD_BIAS_CTRL low half) for a TX frequency in Hz; 0x4D4D without calibration
uint16_t dm32_cal_modcal(uint32_t hz);
// PA drive points for a TX frequency in Hz; false without calibration
bool dm32_cal_power(uint32_t hz, dm32_power_points_t *p);
// DAC value for a power level 0..8 (hardware-dm32.md 11.3), clamped to 4095; the uncalibrated fallback when false
uint16_t dm32_cal_power_level(uint32_t hz, uint8_t level);
// Raw byte of the 0x175-byte working image (diagnostics)
uint8_t dm32_cal_byte(uint16_t offset);

// Pure part for host tests: decode the three pieces of a factory sector into the working image and derive K values
#define DM32_CAL_LEN		0x175u
bool dm32_cal_parse(const uint8_t *sector, uint32_t sector_len, uint8_t image[DM32_CAL_LEN]);
void dm32_cal_set_image(const uint8_t image[DM32_CAL_LEN], bool valid);

// --- Radio <-> DMR glue (radio_dm32.c, dmr_dm32.c) ---

#include "config.h"
#include "modem/hrc7000/hrc7000.h"
#if CONFIG_DMR
void dm32_dmr_board_init(hrc7000_t *modem);	// once, from the radio init: interrupts, PA timer, task
void dm32_dmr_enter(void);			// the modem has just entered DMR: engine on, interrupts on
void dm32_dmr_leave(void);
void dm32_dmr_rx_restart(void);
bool dm32_dmr_tx_active(void);
void dm32_dmr_tx_drop(void);
#else
static inline void dm32_dmr_board_init(hrc7000_t *modem) { (void)modem; }
static inline void dm32_dmr_enter(void) {}
static inline void dm32_dmr_leave(void) {}
static inline void dm32_dmr_rx_restart(void) {}
static inline bool dm32_dmr_tx_active(void) { return false; }
static inline void dm32_dmr_tx_drop(void) {}
#endif

// From the modem interrupts (ISR-safe, radio_dm32.c): per-burst RF switching and the PA one-shot expiry
void dm32_rf_dmr_switch(bool tx);
void dm32_rf_dmr_pa_on(void);
// The "DMR PA" delay in microseconds (1..35 ms, default 28 ms; hardware-dm32.md 8.6)
uint32_t dm32_dmr_pa_delay_us(void);

// The modem driver instance (radio_dm32.c owns it; audio and DMR use it)
hrc7000_t *dm32_modem(void);
// DMR PCM to the speaker instead of the FM path (radio_dm32.c; hal_audio_t.speaker_pcm)
void dm32_speaker_pcm(bool on);

// --- Diagnostics (diag_dm32.c) ---
typedef struct {
	uint16_t rssi_raw, noise_raw;	// FD6818 0x67 / 0x65 from the last status poll
	uint16_t fm_noise;		// HR_C7000 FM_SQL
	uint32_t fd_writes;
} dm32_radio_diag_t;
void dm32_radio_diag(dm32_radio_diag_t *d);
uint16_t dm32_adc_raw(uint8_t ch);	// 1 = ADC channel B, 2 = channel C (battery)
bool dm32_diag_line(uint8_t i, char *buf, uint8_t len);
void dm32_diag_reset(void);
#define DM32_DMR_DIAG_LINES	9
bool dm32_dmr_stats_line(uint8_t i, char *buf, uint8_t len);
void dm32_dmr_stats_reset(void);
void dm32_audio_stats(uint32_t *blocks, uint32_t *overruns, uint32_t *underruns);
void dm32_audio_reset_stats(void);
uint8_t dm32_vocoder_tag(void);	// 49 or 46: the stock codec in the slot, 0 none
uint32_t dm32_flash_jedec(void);

#endif
