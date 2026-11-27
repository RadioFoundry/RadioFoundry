// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio tuning (docs/spec/radio-tune.md): the owner's own trims on top of the factory calibration, which they never
// change: each power level's PA drive, each mode's deviation and carrier frequency, the power levels' names, and the
// battery reading's gain and the voltages the charge percentage runs between. The RAM table below is the only copy in
// RAM; saved, it is one record of the settings ring (settings_tune_save, docs/spec/settings-store.md 2.2), bound to
// this radio's board and chip UID, kept by a factory reset. Nothing here is in the codeplug, so a shared codeplug never
// carries it to another radio.
//
// The site's Radio Tuning page puts entries in the table and saves it through the debug link's STORAGE command
// (TUNE_PUT, TUNE_SAVE, TUNE_GET, TUNE_LOAD, TUNE_CLEAR), and tries values out live through the RADIO command (TUNE_SET
// puts words in the RAM table, TX_DRIVE moves the drive of a transmission in progress). The menu's Clear tuning
// removes the saved trims.
//
// A trim is a value at a frequency for one level or mode. The value at a transmit frequency is interpolated between
// that level's or mode's trims in its band, and held flat past the outer ones. A drive trim keeps the factory drive it
// was measured against: one whose factory drive is no longer the same (new tables, new calibration, levels added or
// removed) is stale and not loaded. Every value is clamped where it is used, so no word, stored or live, takes the
// radio past its limits.
//
// Threading: the getters read a small RAM table that the debug link's task (and the menu's Clear tuning, through the
// storage job) changes, one aligned word per entry.
#ifndef RADIO_RADIO_TUNE_H
#define RADIO_RADIO_TUNE_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "board.h"

#define RADIO_TUNE_WORDS	20u	// trims the radio applies (RAM: 4 bytes each)
#define RADIO_TUNE_NAMES	6u	// level names (RAM: 8 bytes each)
#define RADIO_TUNE_NAME_LEN	6u	// characters in a level name (the status line's power field)
#define RADIO_TUNE_ENTRIES	(RADIO_TUNE_WORDS + RADIO_TUNE_NAMES)	// entries the table, so a record, holds (26)
#define RADIO_TUNE_DEV_MAX	250	// a deviation trim, permille either way
#define RADIO_TUNE_FREQ_MAX	2000	// a frequency trim, Hz either way
#define RADIO_TUNE_GAIN_MAX	150	// the battery reading's gain, permille either way
#define RADIO_TUNE_BATT_MAX	1000	// the empty or full voltage, mV either way

// The record's payload (little endian): owner u32, then entries of 8 bytes. One settings slot holds 29
#define RADIO_TUNE_ENTRY	8u
_Static_assert(4u + RADIO_TUNE_ENTRIES * RADIO_TUNE_ENTRY <= 236u, "a tune record is one settings slot's payload");

// An entry's kind (high nibble of its first byte; the low nibble is the level or the radio_mode_t)
enum {
	RADIO_TUNE_DRIVE = 0,	// kHz/25 u16, drive offset s16, factory drive u16
	RADIO_TUNE_DEV = 1,	// kHz/25 u16, deviation permille s16, 0
	RADIO_TUNE_FREQ = 2,	// kHz/25 u16, carrier offset Hz s16, 0
	RADIO_TUNE_NAME = 3,	// board.bands index u8, name char[6] (NUL padded)
	RADIO_TUNE_BATT = 4,	// 0 u16, value s16, 0; low nibble RADIO_TUNE_BATT_*
};
// A battery entry's low nibble: the reading's gain (permille), the empty and full voltages' offsets (mV)
enum { RADIO_TUNE_BATT_GAIN, RADIO_TUNE_BATT_EMPTY, RADIO_TUNE_BATT_FULL };
// A battery entry in the RAM table: word kind 3 (the name kind's number, names never being words), frequency 0
#define RADIO_TUNE_W_BATT	3u

// A RAM word (RADIO TUNE_SET): frequency in 100 kHz units (bits 0-13), kind (14-15), level or mode (16-18), value
// (19-31, signed)
#define RADIO_TUNE_WORD(khz, kind, sel, v) \
	((uint32_t)((khz) / 100u) | (uint32_t)(kind) << 14 | (uint32_t)(sel) << 16 | (uint32_t)(v) << 19)

// What a board does with the trims: board_t.tune bit 1 << mode, it scales that mode's deviation by radio_tune_dev.
// Drive trims need hal_radio_t.tx_drive_level; frequency trims are the radio core's own (key_tx) and need nothing

#if CONFIG_RADIO_TUNE
// The saved record into the RAM table, replacing it (at start, after the calibration the drive trims are checked
// against; and when the debug link puts live tries back). A record of another radio, or none: an empty table
void radio_tune_load(void);
// The RAM table as a record into the settings ring. False without a ring or when the write failed (the record before
// stays in force)
bool radio_tune_save(void);
// No trims: the table emptied and an empty record saved (false as radio_tune_save; the table stays empty)
bool radio_tune_clear(void);
// n entries (RADIO_TUNE_ENTRY bytes each, the record's) into the RAM table, live at once; start empties it first.
// Entries the radio can't use are left out (radio-tune.md 3)
void radio_tune_put(bool start, const uint8_t *entries, uint8_t n);
// Entry i of the RAM table as the record keeps it (zeros past the end); returns the entries in the table
uint8_t radio_tune_get(uint8_t i, uint8_t *entry);
// The drive for a transmission: the board's drive for level at hz with the trim applied, clamped. 0 stays 0
uint16_t radio_tune_drive(uint32_t hz, uint8_t level, uint16_t drive);
// The deviation trim of mode at hz, permille of the board's own (0: none)
int16_t radio_tune_dev(uint8_t mode, uint32_t hz);
// The carrier trim of mode at hz, Hz (0: none)
int16_t radio_tune_freq(uint8_t mode, uint32_t hz);
// The level's name at a transmit frequency: the owner's name, else the board's (board_power_name)
const char *radio_tune_name(uint8_t level, uint32_t tx_hz);
// A battery trim (RADIO_TUNE_BATT_*), clamped (0: none)
int16_t radio_tune_batt(uint8_t sel);
// The RAM table from the debug link: words first.. of a table of total (0: empty), live from the next use
bool radio_tune_set(uint8_t first, uint8_t total, const uint8_t *words, uint8_t n);
// The table's fill, for the debug link's view
void radio_tune_counts(uint8_t *words, uint8_t *names);
#else
static inline void radio_tune_load(void) { }
static inline uint16_t radio_tune_drive(uint32_t hz, uint8_t level, uint16_t drive)
{
	(void)hz;
	(void)level;
	return drive;
}
static inline int16_t radio_tune_dev(uint8_t mode, uint32_t hz)
{
	(void)mode;
	(void)hz;
	return 0;
}
static inline int16_t radio_tune_freq(uint8_t mode, uint32_t hz)
{
	(void)mode;
	(void)hz;
	return 0;
}
static inline int16_t radio_tune_batt(uint8_t sel)
{
	(void)sel;
	return 0;
}
static inline const char *radio_tune_name(uint8_t level, uint32_t tx_hz)
{
	return board_power_name(level, tx_hz);
}
#endif

#endif
