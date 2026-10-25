// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 factory calibration (calibration_gd88.c, docs/spec/hardware-gd88.md 7), shared by both SoCs' builds. The page
// has the layout of hardware-db25d.md 6: two halves (UHF, VHF at +0x400), 8 points a band.
#ifndef GD88_CAL_H
#define GD88_CAL_H

#include <stdint.h>
#include <stdbool.h>

#define GD88_CAL_LEN		0x800u
#define GD88_CAL_POINTS		8u
#define GD88_CAL_FLASH_ADDR	0x031FE000u	// the program-flash copy, memory-mapped
#define GD88_CAL_NAND_ADDR	0x280000u	// the NAND copy (block 20)
typedef enum { GD88_CAL_NONE, GD88_CAL_FLASH, GD88_CAL_NAND } gd88_cal_source_t;
// Which copy wins: "TUNE" and the higher save counter. Either pointer may be NULL; only the first 8 bytes are read
gd88_cal_source_t gd88_cal_choose(const uint8_t *flash_copy, const uint8_t *nand_copy);
// Reads NAND main-area bytes (gd88_nand_read)
typedef bool (*gd88_cal_read_fn)(uint32_t addr, void *buf, uint32_t len);
// Copy the chosen page (GD88_CAL_LEN bytes) into RAM. Either argument may be NULL. A NAND copy that wins on its header
// but can't be read whole gives way to the flash copy. False if neither copy is usable
bool gd88_cal_load(const uint8_t *flash_copy, gd88_cal_read_fn nand_read);
bool gd88_cal_valid(void);
gd88_cal_source_t gd88_cal_source(void);
uint32_t gd88_cal_counter(void);
// Per-frequency values, interpolated between the 8 points of the band. 0 when there's no page or the value isn't one
uint16_t gd88_cal_pa(uint32_t hz, bool high);		// DAC B at the top of the ramp
uint16_t gd88_cal_rx_tune(uint32_t hz);			// DAC A while receiving
uint8_t gd88_cal_mod(uint32_t hz);			// modcal for hrc7000_mod_bias_dc
void gd88_cal_tx_delays(uint32_t hz, uint16_t *on_us, uint16_t *off_us);
uint8_t gd88_cal_tx_center(void);			// 128 without a page
// What a transmission at hz needs from the page. high = power level 1 (table 0x045), else the other table (0x056).
// drive_pct (1..100) scales the PA drive. False, with *p untouched: no page, no usable PA value for hz, or a bad
// percentage. The delays are 0 where the page has none
typedef struct {
	uint16_t pa;		// DAC B at the top of the ramp
	uint16_t on_us;		// after tuning, before the PA is selected
	uint16_t off_us;	// at TX end, before the ramp down
} gd88_tx_plan_t;
bool gd88_cal_tx_plan(uint32_t hz, bool high, uint8_t drive_pct, gd88_tx_plan_t *p);
// The PA ramp: DAC B at step 0..GD88_APC_STEPS of a ramp to target (smooth start and end). Ramping down runs the
// steps backwards
#define GD88_APC_STEPS		180u
#define GD88_APC_STEP_US	4u
uint16_t gd88_apc_level(uint16_t target, uint16_t step);
// Squelch open threshold for level 0..9, in dB below 0 dBm at the main chip (bk4819_squelch_rssi(-v, 2))
uint8_t gd88_cal_squelch_db(uint32_t hz, uint8_t level);
// TODO(gd88): unknown, needs a radio: the page's items at 0x602 (4 bytes), 0x606, 0x607 (1 each), 0x608 and 0x60B
// (3 each) set modem fields at 0x100 / 0x104; their meaning isn't known, so they aren't used
#define GD88_CAL_OFF_EXTRA	0x602u

// DMR burst end: the TX-off starts at the modem's RF RX interrupt, 30.5 ms after the RF TX interrupt that keyed the
// burst, plus the page's TX-off delay, as in hardware-db25d.md 4.5. How long to wait still, elapsed_us after
// the RF TX interrupt (0 once that point is past), and by how much it was missed (0 when on time)
#define GD88_DMR_RFRX_AFTER_RFTX_US	30500u
static inline uint32_t gd88_dmr_hold_us(uint32_t elapsed_us, uint16_t off_us)
{
	uint32_t at = GD88_DMR_RFRX_AFTER_RFTX_US + off_us;
	return elapsed_us < at ? at - elapsed_us : 0;
}
static inline uint32_t gd88_dmr_late_us(uint32_t elapsed_us, uint16_t off_us)
{
	uint32_t at = GD88_DMR_RFRX_AFTER_RFTX_US + off_us;
	return elapsed_us > at ? elapsed_us - at : 0;
}

#endif
