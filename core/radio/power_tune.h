// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning (docs/spec/power-tune.md): the owner's own adjustments to the PA drive of each power level, measured with
// a wattmeter on a dummy load, and the names they give the levels. They sit on top of the factory calibration, which
// they never change, in the board's tune area (hal_flash_map_t.tune_addr): two copies of one record, each with its own
// CRC and commit word, bound to this radio's chip UID and board. Nothing here is in the codeplug, so a shared codeplug
// never carries it to another radio.
//
// A tuning point is (frequency, level, drive offset). The offset at a transmit frequency is interpolated between the
// points of that level in its band, and held flat past the outer ones. Each point keeps the factory drive it was tuned
// against: a point whose factory drive is no longer the same (new firmware tables, new calibration, levels added or
// removed) is stale and not applied, so an offset never lands on a level it wasn't measured on.
//
// Threading: power_tune_drive and power_tune_name read a small RAM table that only the debug link's task changes, and
// each entry is one aligned word. Flash work (load, edits) runs under storage_lock.
#ifndef RADIO_POWER_TUNE_H
#define RADIO_POWER_TUNE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "board.h"

#define POWER_TUNE_POINTS	20u	// tuning points the radio applies (RAM: 4 bytes each)
#define POWER_TUNE_LABELS	8u	// level names (RAM: 8 bytes each)
#define POWER_TUNE_LABEL_LEN	6u	// characters in a level name (the status line's power field)
#define POWER_TUNE_DELTA_MAX	4095	// the largest offset, in drive units, either way
#define POWER_TUNE_PREVIEW_MS	120000u	// a preview lapses this long after it was set
#define POWER_TUNE_LEVELS_MAX	8u	// levels a point can name (3 bits)

// The stored record (little endian), one per copy, each copy at the start of its own erase unit
#define POWER_TUNE_MAGIC	0x54504652u	// "RFPT"
#define POWER_TUNE_VERSION	1u
#define POWER_TUNE_HDR		20u	// magic u32, seq u32, version u8, points u8, labels u8, levels u8, board u32, uid u32
#define POWER_TUNE_POINT_SIZE	16u	// khz u32, level u8, state u8, factory u16, delta s16, reserved u16, mw u32
#define POWER_TUNE_LABEL_SIZE	20u	// khz u32, level u8, state u8, factory u16, target mw u32, name char[8]
#define POWER_TUNE_RECORD_MAX	(POWER_TUNE_HDR + POWER_TUNE_POINTS * POWER_TUNE_POINT_SIZE + \
				 POWER_TUNE_LABELS * POWER_TUNE_LABEL_SIZE + 8u)	// + CRC-32, commit word

// An entry's state (the state byte of a READ reply; 0 in flash)
enum {
	POWER_TUNE_APPLIED = 0,		// in force
	POWER_TUNE_STALE = 1,		// the factory drive changed since it was tuned
	POWER_TUNE_GONE = 2,		// no such level, or the frequency is in no transmit band now
};

// The edits (power_tune_edit); each writes a new record to both copies and reloads the table
typedef struct {
	uint8_t op;		// POWER_TUNE_PUT_POINT ...
	uint8_t level;
	int16_t delta;		// drive units
	uint32_t khz;
	uint32_t mw;		// a point: the reading; a label: the target (0 none)
	char name[POWER_TUNE_LABEL_LEN + 1];	// a label: "" = the factory name again
} power_tune_edit_t;
enum {
	POWER_TUNE_PUT_POINT = 0,	// add, or replace the point of this level at this frequency
	POWER_TUNE_DEL_POINT = 1,
	POWER_TUNE_PUT_LABEL = 2,	// the name and target of this level in the band of khz ("" and 0: removed)
	POWER_TUNE_CLEAR = 3,		// every point and name
};
typedef enum {
	POWER_TUNE_OK,
	POWER_TUNE_BAD,		// no such level or band, an offset past the limits, a name too long
	POWER_TUNE_FULL,	// no room for another point or name
	POWER_TUNE_NO_STORE,	// no tune area, the flash region isn't ours, or the board can't tell its drive
	POWER_TUNE_FAILED,	// the flash failed (the copy that was in force still is)
} power_tune_result_t;

// What a debug link INFO reports
typedef struct {
	uint32_t seq;		// the record in force (0: none)
	uint8_t copies;		// bit 0 copy A valid, bit 1 copy B valid
	uint8_t points;		// points applied
	uint8_t labels;		// names applied
	bool store;		// the tune area can be written
	bool preview;		// a preview is in force
} power_tune_info_t;

// The limits of one level at a transmit frequency, in drive units (all 0: the level doesn't transmit there)
typedef struct {
	uint16_t factory;	// the board's drive (hal_radio_t.tx_drive_level)
	uint16_t tuned;		// what a transmission uses now (factory with the applied offset)
	uint16_t lo, hi;	// the range an offset may reach
} power_tune_limits_t;

#if CONFIG_POWER_TUNE
// Read the record (after storage_init and board.radio->init, which loads the calibration the points are checked against)
void power_tune_init(void);
// The drive for a transmission: the board's drive for level at hz with the tuning applied, clamped to the limits.
// drive 0 (the level doesn't transmit there) stays 0
uint16_t power_tune_drive(uint32_t hz, uint8_t level, uint16_t drive);
// The level's name at a transmit frequency: the owner's name, else the board's (board_power_name)
const char *power_tune_name(uint8_t level, uint32_t tx_hz);
void power_tune_limits(uint32_t hz, uint8_t level, power_tune_limits_t *l);
// A try-out offset for one level, RAM only, used in place of the stored ones until cleared (level 0xFF) or it lapses
// (POWER_TUNE_PREVIEW_MS)
bool power_tune_preview(uint8_t level, int16_t delta, uint32_t now_ms);
power_tune_result_t power_tune_edit(const power_tune_edit_t *e);
// For power_tune_store.c: the table from a record (n 0: empty), an entry's state, the table's size, a preview in force
void power_tune_take(const uint8_t *r, size_t n);
uint8_t power_tune_state(const uint8_t *entry);
void power_tune_counts(uint8_t *points, uint8_t *labels);
bool power_tune_previewing(uint32_t now_ms);
void power_tune_info(power_tune_info_t *i, uint32_t now_ms);
// The record in force with each entry's state, for the debug link: header, points, labels as stored, the state bytes
// filled in (cap at least POWER_TUNE_RECORD_MAX). Returns its length, 0 when there is none
size_t power_tune_read(uint8_t *out, size_t cap);
#else
static inline void power_tune_init(void) { }
static inline uint16_t power_tune_drive(uint32_t hz, uint8_t level, uint16_t drive)
{
	(void)hz;
	(void)level;
	return drive;
}
static inline const char *power_tune_name(uint8_t level, uint32_t tx_hz)
{
	return board_power_name(level, tx_hz);
}
#endif

#endif
