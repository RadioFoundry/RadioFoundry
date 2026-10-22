// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DB25-D board files
#ifndef DB25D_H
#define DB25D_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "flash/nand_sectors/nand_sectors.h"

// Ops each board file provides
extern const hal_display_t db25d_display;
extern const hal_input_t db25d_input;
extern const hal_radio_t db25d_radio;
extern const hal_power_t db25d_power;

// Physical key bits returned by db25d_input.scan(): the front keys (matrix positions found on the radio,
// hardware-db25d.md 3.3) and the hand mic's 16 keys (3.4)
enum {
	KEY_P1, KEY_P2, KEY_P3, KEY_VOL, KEY_TONE, KEY_BE, KEY_ENC_PUSH,
	KEY_MIC_0, KEY_MIC_1, KEY_MIC_2, KEY_MIC_3, KEY_MIC_4, KEY_MIC_5, KEY_MIC_6, KEY_MIC_7, KEY_MIC_8, KEY_MIC_9,
	KEY_MIC_STAR, KEY_MIC_HASH, KEY_MIC_P4, KEY_MIC_P5, KEY_MIC_P6, KEY_MIC_P7,
	KEY_PTT,
	KEY_COUNT
};

// --- Radio (radio_db25d.c) ---
typedef struct {
	uint16_t main_id, lo_id;	// register 0 of each RF chip (0x4819 on the PRO)
	uint16_t rssi_raw, noise_raw;	// main chip 0x67 / 0x65 from the last status poll
	uint16_t css_raw;		// and its sub-audio status, 0x0C
	uint32_t rf_writes;
	uint32_t hz;			// the tuned frequency
	uint16_t pa_low, pa_high;	// the PA drive a transmission there would use, after the drive scale (0: TX refused)
	bool transmitting;
} db25d_radio_diag_t;
void db25d_radio_diag(db25d_radio_diag_t *d);

// --- Factory calibration (calibration_db25d.c, hardware-db25d.md 6) ---
#define DB25D_CAL_LEN		0x800u
#define DB25D_CAL_POINTS	8u
#define DB25D_CAL_FLASH_ADDR	0x031FE000u	// the program-flash copy, memory-mapped
#define DB25D_CAL_NAND_ADDR	0x280000u	// the NAND copy (block 20)
typedef enum { DB25D_CAL_NONE, DB25D_CAL_FLASH, DB25D_CAL_NAND } db25d_cal_source_t;
// Which copy the stock rule picks: "TUNE" and the higher save counter. Either pointer may be NULL; only the first 8
// bytes are read
db25d_cal_source_t db25d_cal_choose(const uint8_t *flash_copy, const uint8_t *nand_copy);
// Reads NAND main-area bytes (db25d_nand_read)
typedef bool (*db25d_cal_read_fn)(uint32_t addr, void *buf, uint32_t len);
// Copy the chosen page (DB25D_CAL_LEN bytes) into RAM. Either argument may be NULL. A NAND copy that wins on its header
// but can't be read whole gives way to the flash copy. False if neither copy is usable
bool db25d_cal_load(const uint8_t *flash_copy, db25d_cal_read_fn nand_read);
bool db25d_cal_valid(void);
db25d_cal_source_t db25d_cal_source(void);
uint32_t db25d_cal_counter(void);
// Per-frequency values, interpolated between the 8 points of the band. 0 when there's no page or the value isn't one
uint16_t db25d_cal_pa(uint32_t hz, bool high);		// DAC B at the top of the ramp
uint16_t db25d_cal_rx_tune(uint32_t hz);		// DAC A while receiving
uint8_t db25d_cal_mod(uint32_t hz);			// modcal for hrc7000_mod_bias_dc
void db25d_cal_tx_delays(uint32_t hz, uint16_t *on_us, uint16_t *off_us);
uint8_t db25d_cal_tx_center(void);			// 128 without a page
// What a transmission at hz needs from the page. high = the stock's power level 1 (table 0x045), else the other table
// (0x056). drive_pct (1..100) scales the PA drive, for a cautious first test. False, with *p untouched: no page, no
// usable PA value for hz, or a bad percentage. The delays are 0 where the page has none
typedef struct {
	uint16_t pa;		// DAC B at the top of the ramp
	uint16_t on_us;		// after tuning, before the PA is selected
	uint16_t off_us;	// at TX end, before the ramp down
} db25d_tx_plan_t;
bool db25d_cal_tx_plan(uint32_t hz, bool high, uint8_t drive_pct, db25d_tx_plan_t *p);
// The PA ramp: DAC B at step 0..DB25D_APC_STEPS of a ramp to target (smooth start and end; the stock's shaping table
// isn't reproduced). Ramping down runs the steps backwards
#define DB25D_APC_STEPS		180u
#define DB25D_APC_STEP_US	4u
uint16_t db25d_apc_level(uint16_t target, uint16_t step);
// Squelch open threshold for level 0..9, in dB below 0 dBm at the main chip (bk4819_squelch_rssi(-v, 2))
uint8_t db25d_cal_squelch_db(uint32_t hz, uint8_t level);

// --- DMR burst end (hardware-db25d.md 4.5) ---
// The stock firmware starts a burst's TX-off at the modem's RF RX interrupt, which RF_CONTROL 0x00041F1A puts 30 ms +
// 3.1 ms - 2.6 ms after the RF TX interrupt that keyed the burst, then waits the page's TX-off delay before the ramp
// down: the ramp ends about 0.2 ms before the next slot. How long to wait still, elapsed_us after the RF TX interrupt
// (0 once that point is past), and by how much it was missed (0 when on time)
#define DB25D_DMR_RFRX_AFTER_RFTX_US	30500u
static inline uint32_t db25d_dmr_hold_us(uint32_t elapsed_us, uint16_t off_us)
{
	uint32_t at = DB25D_DMR_RFRX_AFTER_RFTX_US + off_us;
	return elapsed_us < at ? at - elapsed_us : 0;
}
static inline uint32_t db25d_dmr_late_us(uint32_t elapsed_us, uint16_t off_us)
{
	uint32_t at = DB25D_DMR_RFRX_AFTER_RFTX_US + off_us;
	return elapsed_us > at ? elapsed_us - at : 0;
}

// --- SPI NAND (nand_db25d.c, hardware-db25d.md 6.5) ---
// ODR's own block range, the only blocks it programs or erases (hardware-db25d.md 6.6): 32 blocks (4 MB) from block
// 1920, far above every stock area and below the stock firmware's scratch blocks at the top of the chip
#define DB25D_NAND_ODR_FIRST	1920u
#define DB25D_NAND_ODR_BLOCKS	32u
bool db25d_nand_init(void);
bool db25d_nand_ready(void);
uint16_t db25d_nand_id(void);		// manufacturer << 8 | device, 0 if nothing answered
uint32_t db25d_nand_corrected(void);	// pages read with corrected bit errors
bool db25d_nand_read(uint32_t addr, void *buf, uint32_t len);
// The NAND's mutex (recursive): taken by every function here, and by the storage around each whole operation
void db25d_nand_lock(void);
void db25d_nand_unlock(void);
// CONFIG_NAND_STORAGE: ODR's range for the sector layer, and how many programs and erases the chip reported failed
extern const nand_dev_t db25d_nand_dev;
uint32_t db25d_nand_program_fails(void);

// --- Data flash (flash_db25d.c, hardware-db25d.md 6.6; CONFIG_NAND_STORAGE): the core's storage as 4 KB sectors
// over ODR's NAND range ---
extern hal_flash_t db25d_flash;
// Mount the sector layer (after db25d_nand_init). False leaves db25d_flash with size 0: nothing is stored
bool db25d_flash_init(void);
void db25d_flash_stats(nand_sectors_stats_t *st);

// --- Diagnostics (diag_db25d.c) ---
uint32_t db25d_input_raw(void);		// the last key scan before debouncing
// The mic connector's lines PTA9 and PTC11 as the 10 ms scan sees them: levels (bit 0 PTA9, bit 1 PTC11) and how many
// times each changed, to find out how the hand mic signals its keys
uint8_t db25d_mic_lines(uint16_t *a9_changes, uint16_t *c11_changes);
void db25d_mic_lines_reset(void);
uint32_t db25d_input_ptt_ignored(void);	// key scans with PTA9 low that the shared-line gate took for serial data
// The hand mic's key held now (its key number, 0xFF for none; mic_db25d.c)
uint8_t db25d_mic_key(void);
// The hand mic's frames as the host UART receives them (miccap_db25d.c, CONFIG_DIAG builds)
void db25d_miccap_init(void);
void db25d_miccap_byte(uint8_t b, bool error, uint32_t now_us);
void db25d_miccap_arm(void);
uint8_t db25d_miccap_last(uint8_t *b, uint8_t max);
uint16_t db25d_miccap_count(void);
bool db25d_diag_line(uint8_t i, char *buf, uint8_t len);
void db25d_diag_reset(void);

#endif
