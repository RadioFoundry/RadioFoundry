// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Internal interfaces between the MD-9600 board files (docs/spec/hardware-md9600.md)
#ifndef MD9600_H
#define MD9600_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "config.h"
#include "rfplan_md9600.h"

// Ops each board file provides
extern const hal_display_t md9600_display;
extern const hal_input_t md9600_input;
extern const hal_radio_t md9600_radio;
extern const hal_power_t md9600_power;

// SPI2 carries the data flash and the LCD: one lock for both (board.c). Recursive, a no-op before the scheduler
void md9600_spi2_lock(void);
void md9600_spi2_unlock(void);
void md9600_spi2_init(void);

// ADC1 scan ranks (power_md9600.c): raw 12-bit averages
enum { MD9600_ADC_NOISE_U, MD9600_ADC_NOISE_V, MD9600_ADC_RSSI_U, MD9600_ADC_RSSI_V, MD9600_ADC_MIC,
	MD9600_ADC_SUPPLY, MD9600_ADC_MICKEY_ROW, MD9600_ADC_MICKEY_COL, MD9600_ADC_COUNT };
uint16_t md9600_adc_raw(uint8_t ch);
// Supply voltage in mV (unverified scale)
uint16_t md9600_supply_mv(void);

// "Off" is a standby the board keeps itself in (power_md9600.c): the power key's state as the key scan sees it, and
// the start-up check that waits in standby when the radio was switched off
void md9600_power_key(bool down);
void md9600_standby_check(void);

// Backlight on TIM8 CH1 (display_md9600.c), 0..100 %
void md9600_backlight(uint8_t percent);

// Physical key bits returned by md9600_input.scan() (matrix positions in docs/spec/hardware-md9600.md 9.1)
enum {
	KEY_P1, KEY_P2, KEY_P3, KEY_P4, KEY_ALARM, KEY_BACK, KEY_ENTER, KEY_UP, KEY_DOWN, KEY_POWER, KEY_PTT,
	// The hand mic's keypad (9.3, input_md9600.c)
	KEY_MIC_0, KEY_MIC_1, KEY_MIC_2, KEY_MIC_3, KEY_MIC_4, KEY_MIC_5, KEY_MIC_6, KEY_MIC_7, KEY_MIC_8, KEY_MIC_9,
	KEY_MIC_STAR, KEY_MIC_HASH, KEY_MIC_A, KEY_MIC_B, KEY_MIC_C, KEY_MIC_D, KEY_MIC_AB, KEY_MIC_UP, KEY_MIC_DOWN,
	KEY_COUNT
};
_Static_assert(KEY_COUNT <= 32, "key bits are a 32-bit mask");
// The raw matrix (columns low per row, bits 0-2 = K4-K6), for the diagnostics page
void md9600_input_raw(uint8_t rows[4]);

// Data flash (flash_md9600.c, on platform/stm32f4/spi_flash's stm32_spi_flash): probe it from board_init, and its
// security registers 1..3, 256 bytes each (calibration in 1-2, options in 3)
bool md9600_flash_init(void);
bool md9600_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len);
// The board's own record sector: the stored board revision
md9600_rev_t md9600_flash_rev_load(void);
bool md9600_flash_rev_store(md9600_rev_t rev);

// Factory calibration and the hardware option block: cal_md9600.h (docs/spec/hardware-md9600.md 11)
#include "cal_md9600.h"
// What the RF code takes from it (rfcal_md9600.c): PA drive per power level and the squelch thresholds, each used
// only when its rows look plausible (md9600_cal_rf_rows)
#define MD9600_POWER_LEVELS	4u		// 5 W, 10 W, 25 W, 40 W (labels, unverified)
enum { MD9600_CALOK_RX = 1u << 0, MD9600_CALOK_PA = 1u << 1, MD9600_CALOK_SQL = 1u << 2 };
bool md9600_cal_apc(uint32_t hz, uint8_t power, uint16_t *dac);	// false: no plausible PA rows
bool md9600_cal_squelch(uint32_t hz, uint16_t *l1_open, uint16_t *l9_open);	// 12-bit noise readings
uint8_t md9600_cal_rf_rows(uint8_t band);	// MD9600_CALOK_* bits for a band (md9600_band_t)

// Board revision in use and the RF state (radio_md9600.c, rfseq_md9600.c)
md9600_rev_state_t md9600_rev_state(void);
typedef struct {
	uint32_t lo_hz;		// the synthesizer's last frequency
	uint8_t band;
	uint8_t state;		// md9600_rf_state_t
	uint32_t retunes, refused, errors;
	uint8_t last_err;	// md9600_rf_err_t
	bool tx_armed;
	int16_t probe;		// receiver probe: -1 not run, else the md9600_rev_t it picked (0 = no verdict)
	uint16_t apc;		// the APC DAC value of the last transmission
	uint32_t rx_hz;		// the receive frequency (the calibration lookups' point)
} md9600_radio_diag_t;
void md9600_radio_diag(md9600_radio_diag_t *d);
void md9600_radio_early(void);	// board_init: the revision and the power-on synthesizer set-up
// Transmit arm for this power cycle (hwopt_md9600.c); false when refused (rfseq_md9600.h)
bool md9600_radio_tx_arm(bool on);
// Hardware options (hwopt_md9600.c, hal_hwopt.h): the board revision, and the transmit arm with CONFIG_MD9600_TX
#define MD9600_HWOPT_COUNT	(CONFIG_MD9600_TX ? 2u : 1u)	// needs config.h
extern const hal_hwopt_t md9600_hwopts[];
// TIM8 PWM outputs (pwm_md9600.c): CH2 PC7 the sub-audio tone, CH3 PC8 the beep. Duty in counts of 256 at about
// 28 kHz; 0 = off (the pin a GPIO driven low). Needs the backlight's TIM8 set-up (display_md9600.c) first
enum { MD9600_PWM_TONE = 2, MD9600_PWM_BEEP = 3 };
void md9600_pwm_out(uint8_t ch, uint8_t duty);
// Audio as PWM on PC8 into the HR_C6000's microphone input (pwm_md9600.c): FM transmit audio from a host
// (MD9600_FM_TX_INJECT, c6000_md9600.c) and the audio probe's PWM tone. pull non-NULL starts it, with amp counts either
// side of the middle for a full-scale sample (at most 127); NULL stops it. Radio task (needs config.h)
#define MD9600_FM_TX_INJECT	(CONFIG_DMR && CONFIG_NET_USRP && CONFIG_MD9600_TX)
#define MD9600_PWM_AUDIO	(MD9600_FM_TX_INJECT || (CONFIG_DMR && CONFIG_AUDIO_PROBE))
#if MD9600_PWM_AUDIO
void md9600_pwm_audio(audio_tap_pull_t pull, int16_t amp);
#endif
// The other analog inputs: the sub-audio input on ADC3 (PA2), 12-bit, one conversion
uint16_t md9600_adc3_qt(void);
// APRS (afsk_md9600.c): transmit through the HR_C6000's tone generator (CONFIG_MD9600_TX), receive audio from the
// discriminator capture (CONFIG_APRS_RX)
extern const hal_afsk_t md9600_afsk;

// FM receive audio on the PCM stream's input (c6000_md9600.c): a host audio session hears the receiver (NET_USRP)
#define MD9600_FM_RX_TAP	(CONFIG_DMR && CONFIG_NET_USRP)
// The discriminator capture (disc_md9600.c): PC3 on ADC3 at MD9600_DISC_RATE_HZ into a ring each reader keeps its own
// tail on, running while a reader wants it; ADC3 is the capture's meanwhile. Radio task
#define MD9600_DISC		(MD9600_FM_RX_TAP || (CONFIG_APRS && CONFIG_APRS_RX))
#define MD9600_DISC_RATE_HZ	((72000000u / 8u / 492u + 1u) / 2u)	// 9146 Hz
#define MD9600_DISC_RING	1024u					// 112 ms, a power of two
enum { MD9600_DISC_APRS = 1u << 0, MD9600_DISC_STREAM = 1u << 1 };
void md9600_disc_start(uint8_t user);
void md9600_disc_stop(uint8_t user);
bool md9600_disc_running(void);
uint32_t md9600_disc_head(void);		// samples written since power-up (wraps)
const volatile int16_t *md9600_disc_ring(void);	// sample k is at [k % MD9600_DISC_RING]

// Sub-audio receive (subtone_md9600.c, CONFIG_SUBTONE): software CTCSS / DCS decode from PA2. rx_set starts the
// sampling on first use and restarts the detector for a tone (TONE_NONE matches always)
void md9600_subtone_rx_set(const subtone_t *tone);
bool md9600_subtone_rx_match(void);
typedef struct {
	uint32_t samples;	// fed to the detector since start
	uint16_t fs_hz;		// its sample rate
	uint16_t rms, mean;	// the input's AC level and mean over the last 0.11 s, 12-bit counts
	uint8_t kind;		// subaudio_kind_t
	bool match, fault;
} md9600_subtone_stats_t;
void md9600_subtone_stats(md9600_subtone_stats_t *s);
// Sub-audio transmit (CONFIG_MD9600_TX): the tone as PWM on PC7 for an FM over; tail: the DCS turn-off tail
void md9600_subtone_tx_start(const subtone_t *tone, bool wide, bool vhf);
void md9600_subtone_tx_stop(void);
bool md9600_subtone_tx_tail(bool on);

// The debug link's board view keys (view_md9600.c)
bool md9600_view_line(uint8_t i, char *buf, uint8_t len);

// Diagnostics page (diag_md9600.c)
bool md9600_diag_line(uint8_t i, char *buf, uint8_t len);
void md9600_diag_reset(void);

// HR_C6000, PCM audio and the speaker routing (c6000_md9600.c). Radio task, after md9600_radio_early
void md9600_c6000_init(void);
#if CONFIG_DMR && (CONFIG_AUDIO_PROBE || MD9600_FM_RX_TAP)
// The audio tap: FM receive audio on the stream (MD9600_FM_RX_TAP); the audio probe's HR_C6000 register access
// (debuglink.md 3.14)
extern const hal_audio_tap_t md9600_audio_tap;
#endif
void md9600_c6000_mode(bool dmr, uint32_t hz);	// set_mode: into or out of DMR at the receive frequency
void md9600_c6000_retune(uint32_t hz);		// after every receiver retune: reference offset, DMR receiver restart
void md9600_speaker_fm(bool on, bool vhf);	// the core's FM squelch (hal_radio_t.audio)
void md9600_speaker_poll(uint32_t now_ms);	// every status poll: ends the amplifier's hush after a squelch close
// FM transmit: the chip in FM TX (the microphone, the band's FM modulation pair) before the PA keys, and back to the
// receive set-up after the over (a no-op when not in FM TX)
void md9600_c6000_fm_tx(uint32_t hz, bool wide);
void md9600_c6000_fm_tx_end(void);
// The chip's tone generator in FM TX: the 1750 Hz burst, the roger beep, DTMF and APRS
#define MD9600_TX_SIG	(CONFIG_MD9600_TX && (CONFIG_FM_TX_TONES || CONFIG_DTMF || CONFIG_APRS))
// During FM TX: the chip's tone generator sends low_hz + high_hz in place of the microphone (0, 0: the microphone
// again); tx_key keys that tone on and off (CONFIG_FM_TX_TONES, CONFIG_DTMF)
bool md9600_c6000_tx_pair(uint16_t low_hz, uint16_t high_hz);
bool md9600_c6000_tx_key(bool down);
// From the radio task's poll (radio status): keeps a tone from tx_pair going past the generator's 16 codes
void md9600_c6000_tx_service(uint32_t now_ms);
// APRS transmit (CONFIG_APRS): the tone generator at level (hal_afsk.h), then retuned symbol by symbol from an
// interrupt; md9600_c6000_tx_pair(0, 0) or the end of FM TX stops it
bool md9600_c6000_afsk_begin(uint16_t hz, uint8_t level);
bool md9600_c6000_afsk_tone(uint16_t hz);
typedef struct {
	bool up;		// the chip answered after its power-on set-up
	bool dmr;		// in DMR
	uint8_t r10;		// 4:0x10 read after the set-up
	uint8_t speaker;	// bit 0 amplifier on, bit 1 FM audio through, bit 2 codec line out
	uint32_t errors;	// bus transfers that failed
	uint32_t tx_refused;	// bursts the RF switch refused to key
} md9600_c6000_diag_t;
void md9600_c6000_diag(md9600_c6000_diag_t *d);
extern const hal_audio_t md9600_audio;
#if CONFIG_MD9600_DMR_TX
// The run-time gate on DMR transmit (off at start-up): follows the transmit arm (md9600_radio_tx_arm)
void md9600_dmr_tx_allow(bool on);
// The RF chain's DMR switching (radio_md9600.c): the per-burst keying from the modem interrupts, the transmit side
// tuned ahead of the first burst (never blocks) and the task-level wait for it, the receiver back at a split channel
// transmission's end (task level), the counters for the glue
bool md9600_rf_dmr_key(bool tx);
void md9600_c6000_tx_lead(uint8_t steps);
void md9600_rf_dmr_prepare(void);
void md9600_rf_dmr_wait(void);
void md9600_rf_dmr_done(void);
void md9600_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx);
#endif

// GPS (gps_md9600.c): the module's NMEA on USART1, detected at run time (13)
void md9600_gps_init(void);

// The hand mic's keypad, for diagnostics: the key decoded on the last scan (ADCKEYS_NONE for none)
uint8_t md9600_mic_key(void);

// A DMA stream's memory address must be in main SRAM (0x2000_0000): the DMA controllers can't reach the CCM
#define DMA_RAM_OK(p)	configASSERT(((uintptr_t)(p) >> 24) == 0x20u)

#endif
