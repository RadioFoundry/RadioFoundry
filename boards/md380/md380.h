// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Internal interfaces between the MD-380 board files (docs/spec/hardware-md380.md)
#ifndef MD380_H
#define MD380_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "spi_flash/spi_flash.h"

// The LCD data bus doubles as the keypad columns: the display and the keypad scan take turns
void md380_lcdbus_lock(void);
void md380_lcdbus_unlock(void);
// Reconnect LCD D0-D7 (the keypad columns) to the FSMC; the display calls it at the start of every burst, lock held
void md380_lcd_pins_fsmc(void);

// The HR_C5000 configuration port and the SKY72310 bus (md380_rfbus_lock and the rest) are in rfbus_md380.h
#include "rfbus_md380.h"

// DMR voice PCM to the speaker (radio_md380.c; audio_md380.c's hal_audio_t.speaker_pcm calls it)
void md380_speaker_pcm(bool on);
// The HR_C5000's I2S clocks on or off (radio_md380.c; audio_md380.c switches them with the PCM stream)
void md380_i2s_clock(bool on);

// Ops each board file provides
extern const hal_display_t md380_display;
extern const hal_input_t md380_input;
extern const hal_radio_t md380_radio;
extern const hal_power_t md380_power;
extern const hal_audio_t md380_audio;

// ADC readings (power_md380.c): raw 12-bit averages
enum { MD380_ADC_VOLUME, MD380_ADC_BATTERY, MD380_ADC_MIC, MD380_ADC_RSSI, MD380_ADC_BUSY, MD380_ADC_TEMP,
#if CONFIG_SUBTONE
	MD380_ADC_QT,	// the sub-audio input (PA2), also streamed to the detector from the DMA interrupt
#endif
	MD380_ADC_COUNT };
uint16_t md380_adc_raw(uint8_t ch);

// Sub-audio (subtone_md380.c, docs/spec/hardware-md380.md 6.4): software CTCSS / DCS decode from the ADC and PWM encode
#if CONFIG_SUBTONE
// Called from the ADC DMA interrupt (power_md380.c) with each raw QT_DQT_IN sample, at md380_adc_scan_rate_hz()
void md380_subtone_sample(uint16_t raw);
uint32_t md380_adc_scan_rate_hz(void);
// Receive: the tone or code to detect (a change or retune restarts the detector), and whether it is heard now (true
// for none)
void md380_subtone_rx_set(const subtone_t *tone);
bool md380_subtone_rx_match(void);
// Transmit: send the tone or code as PWM on PC7 until stopped; a none tone sends nothing
void md380_subtone_tx_start(const subtone_t *tone);
void md380_subtone_tx_stop(void);
// The DCS turn-off tail in place of the code (on) or the code again (off); false when no DCS is sent
bool md380_subtone_tx_tail(bool on);
// Counters for the diagnostics page: ADC samples seen, detector openings
uint32_t md380_subtone_samples(void);
#endif
int16_t md380_temperature_c10(void);


// Physical key bits returned by md380_input.scan() (matrix positions in docs/spec/hardware-md380.md 9.1)
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_GREEN, KEY_RED,
	KEY_SK1, KEY_SK2, KEY_PTT,
	KEY_COUNT
};
// The raw keypad matrix and channel switch, for the diagnostics page (bring-up: which bit is which key)
void md380_input_raw(uint8_t rows[3], uint8_t *channel_code);

// SPI flash (flash_md380.c, then stm32_spi_flash): its security registers hold the factory calibration in 1-2
bool md380_flash_init(void);

// Factory calibration (calibration_md380.c, docs/spec/hardware-md380.md 11)
typedef struct {
	bool valid;
	uint16_t power_dac[2];	// low, high at the requested frequency (12-bit DAC values)
	uint16_t rx_tune_dac;	// front-end tuning voltage for receive (12-bit DAC value)
	uint8_t fm_i, fm_q;	// HR_C5000 0x45 / 0x46 for FM TX
	uint8_t dmr_i, dmr_q;	// for DMR
	uint8_t ref_tune;	// reference oscillator tune byte (calibration offset 9)
} md380_cal_point_t;

bool md380_cal_load(void);
// Calibration values at a frequency (Hz); false if no usable calibration is loaded (safe defaults filled)
bool md380_cal_at(uint32_t hz, md380_cal_point_t *out);
const char *md380_cal_source(void);	// "factory", "factory/grid" (band ends unreadable) or "none"
const char *md380_cal_why(void);	// why the table was refused, for the diagnostics page
uint8_t md380_cal_byte(uint16_t offset);

// GPS (gps_md380.c): the module is listened for at start-up (platform/stm32f4/gps_uart/gps_uart.h has the rest)
void md380_gps_init(void);

// Radio diagnostics (radio_md380.c; cached values, no bus traffic)
typedef struct {
	uint8_t c5000_r10;	// HR_C5000 0x10 read back after init
	bool c5000_ok;
	bool pll_locked;	// lock detect after the last retune
	uint32_t pll_vco_hz;	// the synthesizer's last frequency
	uint32_t pll_fails;	// retunes refused (out of range) or not locked in time
	uint32_t bus_errors;
} md380_radio_diag_t;
void md380_radio_diag(md380_radio_diag_t *d);

// Diagnostics page (diag_md380.c)
bool md380_diag_line(uint8_t i, char *buf, uint8_t len);
void md380_diag_reset(void);

// A DMA stream's memory address must be in main SRAM (0x2000_0000): the DMA controllers can't reach the CCM, where
// RFOUNDRY_CPU_RAM state and every task stack live
#define DMA_RAM_OK(p)	configASSERT(((uintptr_t)(p) >> 24) == 0x20u)

#endif
