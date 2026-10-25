// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT1846S RF transceiver: chip driver, independent of the board (docs/spec/hardware-dm1701.md section 5).
// The board supplies a bus (one 16-bit register write / read, I2C on the STM32 radios); this driver handles the
// register page (reg 0x7F) and keeps a per-page cache of the values written so unchanged writes are skipped.
// A driver instance is not reentrant: call it from one task (or guard it). The pure helpers have no state.
#ifndef AT1846S_H
#define AT1846S_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	bool (*write)(void *ctx, uint8_t reg, uint16_t val);
	bool (*read)(void *ctx, uint8_t reg, uint16_t *val);
	void (*delay_ms)(uint32_t ms);
	void *ctx;
} at1846s_bus_t;

typedef enum { AT1846S_BW_12K5, AT1846S_BW_25K } at1846s_bw_t;
typedef enum { AT1846S_MODE_FM, AT1846S_MODE_DMR } at1846s_mode_t;

// Reg 0x30 states (5.3)
typedef enum {
	AT1846S_STATE_OFF,	// receiver off at power-down
	AT1846S_STATE_IDLE,	// RX and TX off, the state for a frequency write
	AT1846S_STATE_RX,
	AT1846S_STATE_FM_TX,
	AT1846S_STATE_DMR_TX,
} at1846s_state_t;

typedef enum { AT1846S_TONE_NONE, AT1846S_TONE_CTCSS, AT1846S_TONE_DCS, AT1846S_TONE_DCS_INV } at1846s_tone_t;

// Reg 0x1C flag bits (5.7)
#define AT1846S_FLAG_SQ		0x0001u
#define AT1846S_FLAG_DCS_INV	0x0040u
#define AT1846S_FLAG_DCS	0x0080u
#define AT1846S_FLAG_CTCSS2	0x0100u
#define AT1846S_FLAG_CTCSS1	0x0200u

// The CTCSS register value used to send DCS (134.4 Hz, the DCS bit rate)
#define AT1846S_DCS_TONE_REG	13440u

#define AT1846S_STATE_COUNT	5

// A radio's own register values for the chip, where its design differs from the defaults (hardware-dm1701.md 5.2-5.5):
// the init list, the bandwidth table, reg 0x30 per state and the reg 0x05 rule. Board data; the driver only walks it
typedef struct {
	uint8_t reg;		// page 0
	uint16_t val;
	uint16_t wait_ms;	// after the write
} at1846s_step_t;

typedef struct {
	uint8_t page;
	uint8_t reg;
	uint16_t val[2];	// 12.5 kHz, 25 kHz
} at1846s_bw_row_t;

typedef struct {
	const at1846s_step_t *init;	// after the soft reset and its 50 ms, in order
	uint8_t init_len;
	const at1846s_bw_row_t *bw;	// written in order on a bandwidth change, then reg 0x30 idle
	uint8_t bw_len;
	uint16_t ctrl[AT1846S_STATE_COUNT][2];	// reg 0x30 per at1846s_state_t, 12.5 / 25 kHz
	bool no_mode_tables;		// set_mode writes nothing: the init and bandwidth values serve FM and DMR
	uint16_t (*reg05)(uint32_t hz);	// reg 0x05 for a frequency; NULL: 0x8763
} at1846s_profile_t;

typedef struct {
	const at1846s_bus_t *bus;
	const at1846s_profile_t *prof;	// NULL: the driver's defaults
	uint16_t cache[2][128];
	uint32_t valid[2][4];	// one bit per cached register
	int8_t page;		// selected page, -1 unknown
	int8_t bw;		// at1846s_bw_t applied, -1 none yet
	int8_t mode;		// at1846s_mode_t applied, -1 none yet
	uint8_t state;		// at1846s_state_t last set
	uint8_t tx_tone;	// at1846s_tone_t of the last at1846s_set_tx_tone()
} at1846s_t;

// --- Pure helpers (no chip access) ---

// Synthesizer word for a frequency: 1/16 kHz units, rounded (5.6). 0x29 takes bits 29:16, 0x2A bits 15:0
uint32_t at1846s_freq_word(uint32_t hz);
// Reg 0x30 value for a state at a bandwidth (5.3)
uint16_t at1846s_state_value(at1846s_bw_t bw, at1846s_state_t state);
// Reg 0x5B match / unmatch threshold byte for a CTCSS register value (Hz x 100)
uint8_t at1846s_ctcss_threshold(uint16_t tone_reg);
// CTCSS register value (Hz x 100) from Hz x 10
static inline uint16_t at1846s_ctcss_reg(uint16_t hz_x10) { return (uint16_t)(hz_x10 * 10u); }
// Whether reg 0x1C flags show the RX tone (true when none is set). Carrier (sq_cmp) is not part of this
bool at1846s_tone_match(uint16_t flags, at1846s_tone_t type);

// --- Register access ---

void at1846s_attach(at1846s_t *dev, const at1846s_bus_t *bus);
// A radio's own values (before at1846s_init); NULL goes back to the defaults
void at1846s_set_profile(at1846s_t *dev, const at1846s_profile_t *prof);
// Reg 0x30 for a state at a bandwidth with this device's profile (at1846s_state_value() is the defaults')
uint16_t at1846s_ctrl_value(const at1846s_t *dev, at1846s_bw_t bw, at1846s_state_t state);
// Reg 0x05 for a frequency with this device's profile
uint16_t at1846s_reg05_value(const at1846s_t *dev, uint32_t hz);
// Cached write: skipped when the cache already holds the value
bool at1846s_write(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t val);
// Always written
bool at1846s_write_force(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t val);
// Read from the chip (not the cache)
bool at1846s_read(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t *val);
// val = (val & keep) | set, starting from the cache when it holds the register
bool at1846s_modify(at1846s_t *dev, uint8_t page, uint8_t reg, uint16_t keep, uint16_t set);
// Forget every cached value (after a reset, or when the chip may have lost them)
void at1846s_cache_clear(at1846s_t *dev);

// --- Chip operations ---

// Soft reset, power on, vendor values, calibration, 12.5 kHz (5.2 steps 1-35). Sleeps about 360 ms
bool at1846s_init(at1846s_t *dev);
// The last init step, run after the modem chip has been initialized (5.2 step 36)
bool at1846s_init_finish(at1846s_t *dev);
// Bandwidth table (5.4); no-op when already applied. Leaves the chip idle (call at1846s_set_state after)
bool at1846s_set_bandwidth(at1846s_t *dev, at1846s_bw_t bw);
// FM or DMR mode table (5.5); no-op when already applied. Bandwidth and mode tables both reset reg 0x3A, so set the
// RX tone again after either
bool at1846s_set_mode(at1846s_t *dev, at1846s_mode_t mode);
// Tune with the receiver off (5.6 steps 1-5). Leaves the chip idle; set the state after
bool at1846s_set_freq(at1846s_t *dev, uint32_t hz);
bool at1846s_set_state(at1846s_t *dev, at1846s_state_t state);
// Sub-audio (5.7). value: CTCSS Hz x 10, DCS the 9-bit code (octal digits as 3-bit groups, 023 -> 0x13)
bool at1846s_set_tx_tone(at1846s_t *dev, at1846s_tone_t type, uint16_t value);
// The DCS turn-off tail (5.7), while transmitting DCS: on switches TX sub-audio from CDCSS to CTCSS, which sends 0x4A's
// 134.4 Hz (the DCS bit rate) as a tone; off goes back to the code. False with no DCS set (nothing written)
bool at1846s_tx_tail(at1846s_t *dev, bool on);
bool at1846s_set_rx_tone(at1846s_t *dev, at1846s_tone_t type, uint16_t value);
// A radio's calibrated TX values, with the register map's field names. Each goes into its bit field only; the other
// bits of the register keep their value
typedef struct {
	uint8_t pa_drv;		// reg 0x0A bits 14:11, PA driver current (padrv_ibit)
	uint8_t mic_pga;	// reg 0x0A bits 10:6, microphone PGA gain (pga_gain)
	uint8_t voice_gain;	// reg 0x41 bits 6:0, digital TX voice gain (voice_gain_tx)
	uint8_t gain_tx;	// reg 0x44 bits 11:8, digital gain after the TX ADC (gain_tx)
	uint16_t fm_dev;	// reg 0x59 bits 15:6, FM deviation (xmitter_dev)
	uint8_t tone_dev;	// reg 0x59 bits 5:0, CTCSS / DCS deviation (c_dev)
} at1846s_tx_cal_t;

bool at1846s_set_tx_cal(at1846s_t *dev, const at1846s_tx_cal_t *cal);
// RX audio levels: reg 0x44 bits 7:4 (DAC analog gain) and 3:0 (digital volume)
bool at1846s_set_rx_volume(at1846s_t *dev, uint8_t dac_gain, uint8_t volume);
// Reg 0x1B: rssi_db (high byte) and noise_db (low byte), 1 dB units, chip scale (5.9)
bool at1846s_read_signal(at1846s_t *dev, uint8_t *rssi_db, uint8_t *noise_db);
// Reg 0x1C flags (AT1846S_FLAG_*)
bool at1846s_read_flags(at1846s_t *dev, uint16_t *flags);

#endif
