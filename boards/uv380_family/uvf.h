// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UV380 board family's shared code: STM32F405 + HR_C6000 + one AT1846S for both bands, 160x128 color LCD on the
// FSMC, keypad on the LCD data lines, 16 MB SPI NOR. The DM-1701 / RT84 (boards/dm1701) and the MD-UV380 / MD-UV390 /
// RT3S (boards/mduv380) are this board with different data: those differences are uvf_variant_t below, and everything
// else is in this directory (docs/spec/hardware-dm1701.md, docs/spec/hardware-mduv380.md 1). A board lists
// boards/uv380_family in BOARD_DIRS and BOARD_INC, and its own board.c defines board, uvf_variant and uvf_key_matrix.
// uvf_ = UV380 family
#ifndef UVF_H
#define UVF_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "spi_flash/spi_flash.h"

// AT1846S chips: one, or two where a second receiver has its own (board.mk MCU_DEFS -DUVF_RF_CHIPS=2). The first
// receives and transmits on both bands; the second rests
#ifndef UVF_RF_CHIPS
#define UVF_RF_CHIPS	1
#endif

// What the family's radios differ in (hardware-mduv380.md 1), one const table in each board's board.c
typedef struct {
	// AT1846S 7-bit I2C addresses: the main chip, the second receiver's (UVF_RF_CHIPS 2)
	uint8_t rf_addr[2];
	// Factory panel type (SPI flash security register 3 byte 0x1D, low two bits) -> the ST7735 type the driver runs
	// and whether it is driven in RGB order (hardware-dm1701.md 9.2). The visible rows are UVF_LCD_HIDDEN_ROWS (board.mk)
	uint8_t panel_type[4];
	bool panel_rgb[4];
	// Battery sense (hardware-dm1701.md 11.1): mV = raw * bat_mul / bat_div
	uint16_t bat_mul, bat_div;
	// Volume pot (11.2): raw at the bottom stop, at the taper's break (50 %), at the top stop
	uint16_t vol_min, vol_break, vol_max;
	// TX power interpolation fractions (7), per mille, f0..f3, [VHF, UHF]
	uint16_t frac[2][4];
	// Factory to working copy power conversion (12.1), per mille, k0..k3 (very low, low, mid, high), [VHF, UHF]
	uint16_t cal_k[2][4];
	// The channel knob counts the other way: the same pins, the A and B phases swapped on the board (10.3)
	bool enc_reverse;
	// Board start-up after the shared one (board_init), or NULL: GPS and the radio's own checks
	void (*init)(void);
	// Why the transmitter must stay off on this radio, or NULL to allow it (asked on every tx()): a build for the
	// wrong hardware version
	const char *(*tx_block)(void);
} uvf_variant_t;
extern const uvf_variant_t uvf_variant;
// The key matrix, [row PA6, PD2, PD3][column D0..D7], with the key bits below (input_uvf.c)
extern const uint8_t uvf_key_matrix[3][8];

// The LCD data bus doubles as the keypad columns: the display and the keypad scan take turns
void uvf_lcdbus_lock(void);
void uvf_lcdbus_unlock(void);
// Reconnect LCD D0-D7 (the keypad columns) to the FSMC; the display calls it at the start of every burst, lock held
void uvf_lcd_pins_fsmc(void);

// DMR voice PCM to the speaker (radio_uvf.c; audio_uvf.c's hal_audio_t.speaker_pcm calls it): combined with the
// FM squelch request, FM feed-through off while PCM plays. Callable from the audio and radio tasks
void uvf_speaker_pcm(bool on);

// Extra analog readings (power_uvf.c)
uint16_t uvf_mic_level_raw(void);
int16_t uvf_temperature_c10(void);

// Ops each board file provides (display_uvf.c, input_uvf.c, radio_uvf.c, power_uvf.c)
extern const hal_display_t uvf_display;
extern const hal_input_t uvf_input;
extern const hal_radio_t uvf_radio;
extern const hal_power_t uvf_power;
extern const hal_audio_t uvf_audio;

#if CONFIG_AUDIO_PROBE || CONFIG_NET_USRP || CONFIG_APRS_RX
// The audio tap: FM receive audio on the PCM stream for APRS (CONFIG_APRS_RX) and USRP, FM transmit audio for USRP
// (CONFIG_NET_USRP), the probe (CONFIG_AUDIO_PROBE, docs/spec/hardware-dm1701.md
// 21.3). audio_uvf.c; register access and the transmit source switch in radio_uvf.c
extern const hal_audio_tap_t uvf_audio_tap;
#endif
#if CONFIG_NET_USRP
bool uvf_fm_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t mode);
void uvf_pwm_audio(audio_tap_pull_t pull);	// PWM audio on PC8: start (pull) or stop (NULL)
#endif
#if CONFIG_AUDIO_PROBE
bool uvf_modem_reg(uint8_t page, uint8_t reg, uint8_t *val, bool write);
bool uvf_modem_ram(uint8_t page, uint16_t addr, uint8_t *buf, uint16_t len, bool write);
bool uvf_modem_burst(uint8_t page, uint8_t reg, const uint8_t *buf, uint16_t len);
#endif

// Physical key bits returned by uvf_input.scan(), the family's keys: a radio's matrix (uvf_key_matrix, hardware-dm1701.md
// 10.1) uses the ones it has (the MD-UV380 has no P1/P2 and no SK3). The MD-2017's ring adds left and right (and drives
// up and down), its ring center is OK and its orange top key TOP (hardware-md2017.md 5)
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_GREEN, KEY_RED,
	KEY_FN_LEFT, KEY_FN_RIGHT, KEY_SK1, KEY_SK2, KEY_SK3, KEY_PTT,	// side keys top to bottom
	KEY_LEFT, KEY_RIGHT, KEY_OK, KEY_TOP,
	KEY_COUNT
};

// SPI flash (flash_uvf.c, then stm32_spi_flash): its security registers hold the factory calibration in 1-2 and
// the panel type and model flags in 3
bool uvf_flash_init(void);

#if CONFIG_GPS
// The GPS module on USART1 / PA9, detected at run time (gps_uvf.c): from the variant's init. board_t.gps_present is
// stm32_gps_present
void uvf_gps_init(void);
#endif

// Diagnostics (diag_uvf.c and the accessors it reads; all return cached values, no bus traffic)
typedef struct {
	uint8_t rssi, noise;	// AT1846S raw readings from the last status poll
	uint16_t flags;		// AT1846S reg 0x1C from the last status poll
	uint8_t c6000_r10;	// HR_C6000 4:0x10 read back after init (0x80 = FM)
	bool c6000_ok;		// that read worked
	uint32_t bus_errors;
} uvf_radio_diag_t;
void uvf_radio_diag(uvf_radio_diag_t *d);
uint8_t uvf_display_panel_type(void);
// How the clock started (system_uvf.c): the HSE as an external clock, as a crystal, or neither (HSI)
enum { UVF_HSE_NONE, UVF_HSE_BYPASS, UVF_HSE_CRYSTAL };
uint8_t uvf_hse_mode(void);
uint16_t uvf_adc_raw(uint8_t ch);	// 0 volume, 1 battery, 2 mic, 3 temperature
const char *uvf_cal_source(void);	// "working", "factory" or "none"
void uvf_cal_why(char *out, uint8_t len, uint8_t marker[8]);
uint8_t uvf_cal_byte(uint16_t offset);
bool uvf_diag_line(uint8_t i, char *buf, uint8_t len);
// Zero the counters behind the diagnostics lines (board.diag_reset)
void uvf_diag_reset(void);

// Factory calibration (calibration_uvf.c, docs/spec/hardware-dm1701.md section 12)
typedef struct {
	bool valid;
	uint16_t power_dac[4];	// very low, low, mid, high at the requested frequency (12-bit DAC values)
	uint8_t fm_i, fm_q;	// HR_C6000 4:0x45 / 4:0x46 for FM TX
	uint8_t dmr_i, dmr_q;	// for DMR
	int8_t ref_offset;	// reference oscillator tune - 128
} uvf_cal_point_t;

// APRS AFSK through the FM transmitter (radio_uvf.c for afsk_uvf.c): begin and end at task level with FM TX
// keyed, tone from the TIM6 interrupt
bool uvf_afsk_begin(uint16_t hz, uint8_t level);
bool uvf_afsk_tone(uint16_t hz);
void uvf_afsk_end(void);
extern const hal_afsk_t uvf_afsk;

bool uvf_cal_load(void);
// Calibration values interpolated at a frequency (Hz); returns false if no calibration is loaded (safe defaults filled)
bool uvf_cal_at(uint32_t hz, uvf_cal_point_t *out);

// Direct DMR keying (hardware-dm1701.md 6): the AT1846S switched from RX straight to DMR TX, no idle step, puts the
// carrier about 3.07 kHz low, and a frequency write can't fix it (it applies only coming out of idle). The HR_C6000
// MOD1 reference offset (4:0x04, about 75 Hz a step at 461 MHz) is raised for the burst instead. Measured on a DM-1701
// at 461.1375 MHz (SDR bench, 2026-10-19): 41 steps -57..-15 Hz, 42 steps +16..+60 Hz, 43 steps +94..+132 Hz; the
// MD-UV380 has the same two chips, unchecked on its own
#define UVF_DIRECT_TX_REF_STEPS	42
// 4:0x04 for a direct burst at hz from the calibrated offset; false where direct keying doesn't apply (VHF) or the
// value would overflow the register, and the burst then goes through idle
bool uvf_direct_tx_ref(uint32_t hz, int8_t ref_offset, int8_t *out);

// The user's reference trim (hal_radio_t.ref_trim) in MOD1 offset steps, added to the calibrated offset. The offset
// pulls the reference, so a step is the same share of any carrier: about 74 Hz at 461.1375 MHz from the direct keying
// measurement above (41 and 43 steps 149 Hz apart), 160 ppb. Clamped to the register's -128..127
#define UVF_REF_TRIM_PPB_PER_STEP	160
int8_t uvf_ref_trimmed(int8_t ref_offset, int32_t ppb);

// A DMA stream's memory address must be in main SRAM (0x2000_0000): the DMA controllers can't reach the CCM, where
// RFOUNDRY_CPU_RAM state and every task stack live. Stops here, at set-up, instead of a silent transfer error later
#define DMA_RAM_OK(p)	configASSERT(((uintptr_t)(p) >> 24) == 0x20u)

#endif
