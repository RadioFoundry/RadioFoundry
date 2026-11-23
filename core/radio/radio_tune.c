// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio tuning: the record's check and load, and the trims applied at key-up (radio_tune.h, docs/spec/radio-tune.md)
#include <string.h>
#include "board.h"
#include "radio/radio_tune.h"
#include "storage/storage.h"
#include "settings/settings.h"

#if CONFIG_RADIO_TUNE

#define W_KHZ(w)	((int32_t)((w) & 0x3FFFu) * 100)
#define W_KIND(w)	(((w) >> 14) & 3u)
#define W_SEL(w)	(((w) >> 16) & 7u)
#define W_VAL(w)	((int32_t)(w) >> 19)

typedef struct {
	uint8_t band_level;	// band << 4 | level
	char name[RADIO_TUNE_NAME_LEN + 1];
} name_t;

// The debug link's task writes the table while the radio's task reads it. volatile keeps each store in its place:
// the count cut before words change and raised only after, never merged into one store or moved past the words
static volatile uint32_t words[RADIO_TUNE_WORDS];
static volatile uint8_t n_words;
static name_t names[RADIO_TUNE_NAMES];
static volatile uint8_t n_names;

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t level_drive(uint32_t hz, uint8_t level)
{
	return level < board.power_count && board.radio->tx_drive_level != NULL ? board.radio->tx_drive_level(hz, level) : 0u;
}

// The value of kind/sel at hz: interpolated between its words in hz's band, flat past the ends; 0 when there are none
static int32_t value_at(uint32_t kind, uint32_t sel, uint32_t hz)
{
	int band = board_tx_band(hz);
	bool have_lo = false, have_hi = false;
	uint32_t wlo = 0, whi = 0;	// each word is read once: a word that changes meanwhile is either one, never both
	int32_t khz = (int32_t)(hz / 1000u);
	for (uint8_t i = 0; band >= 0 && i < n_words; i++) {
		uint32_t w = words[i];
		int32_t f = W_KHZ(w);
		if (W_KIND(w) != kind || W_SEL(w) != sel || board_tx_band((uint32_t)f * 1000u) != band) {
			continue;
		}
		if (f <= khz && (!have_lo || f > W_KHZ(wlo))) {
			wlo = w;
			have_lo = true;
		}
		if (f >= khz && (!have_hi || f < W_KHZ(whi))) {
			whi = w;
			have_hi = true;
		}
	}
	if (!have_lo || !have_hi) {
		return have_lo ? W_VAL(wlo) : have_hi ? W_VAL(whi) : 0;
	}
	int32_t f0 = W_KHZ(wlo), f1 = W_KHZ(whi), d0 = W_VAL(wlo), d1 = W_VAL(whi);
	if (f1 == f0) {
		return d0;
	}
	int32_t num = (d1 - d0) * (khz - f0), den = f1 - f0;
	return d0 + (num + (num < 0 ? -den / 2 : den / 2)) / den;	// kHz: within 32 bits for any band
}

// Never above the top level's factory drive (the radio's rating), never below the lowest level's (the calibration
// put that above where the PA keeps control)
uint16_t radio_tune_drive(uint32_t hz, uint8_t level, uint16_t drive)
{
	if (drive == 0u || n_words == 0u || level >= 8u) {
		return drive;
	}
	int32_t d = (int32_t)drive + value_at(RADIO_TUNE_DRIVE, level, hz);
	int32_t hi = level_drive(hz, (uint8_t)(board.power_count - 1u)), lo = level_drive(hz, 0);
	if (hi < drive) {
		hi = drive;
	}
	if (lo > drive || lo == 0) {
		lo = drive;
	}
	return (uint16_t)(d > hi ? hi : d < lo ? lo : d);
}

static int16_t clamp(int32_t v, int32_t max)
{
	return (int16_t)(v > max ? max : v < -max ? -max : v);
}

int16_t radio_tune_dev(uint8_t mode, uint32_t hz)
{
	return clamp(value_at(RADIO_TUNE_DEV, mode, hz), RADIO_TUNE_DEV_MAX);
}

// Never past the edge of hz's transmit band: a channel at the edge keys at the edge, not out of band
int16_t radio_tune_freq(uint8_t mode, uint32_t hz)
{
	int32_t v = clamp(value_at(RADIO_TUNE_FREQ, mode, hz), RADIO_TUNE_FREQ_MAX);
	int band = board_tx_band(hz);
	if (band < 0) {
		return 0;
	}
	const band_t *b = &board.bands[band];
	if (v > 0 && (uint32_t)v > b->hi_hz - hz) {
		v = (int32_t)(b->hi_hz - hz);
	}
	if (v < 0 && (uint32_t)-v > hz - b->lo_hz) {
		v = -(int32_t)(hz - b->lo_hz);
	}
	return (int16_t)v;
}

// A battery word is frequency 0, kind RADIO_TUNE_W_BATT and sel: its low 19 bits say all of that
int16_t radio_tune_batt(uint8_t sel)
{
	int32_t v = 0;
	for (uint8_t i = 0; i < n_words; i++) {
		uint32_t w = words[i];
		if ((w & 0x7FFFFu) == RADIO_TUNE_WORD(0u, RADIO_TUNE_W_BATT, sel, 0u)) {
			v = W_VAL(w);
		}
	}
	return clamp(v, sel == RADIO_TUNE_BATT_GAIN ? RADIO_TUNE_GAIN_MAX : RADIO_TUNE_BATT_MAX);
}

const char *radio_tune_name(uint8_t level, uint32_t tx_hz)
{
	int band = board_tx_band(tx_hz);
	for (uint8_t i = 0; band >= 0 && i < n_names; i++) {
		if (names[i].band_level == (uint8_t)(band << 4 | level)) {
			return names[i].name;
		}
	}
	return board_power_name(level, tx_hz);
}

bool radio_tune_set(uint8_t first, uint8_t total, const uint8_t *w, uint8_t n)
{
	// A gap before first would bring back words of an earlier table
	if (total > RADIO_TUNE_WORDS || first + n > total || first > n_words) {
		return false;
	}
	n_words = first;	// a reader meanwhile sees a shorter table, never a torn word
	for (uint8_t i = 0; i < n; i++) {
		words[first + i] = get32(w + 4u * i);
	}
	n_words = (uint8_t)(first + n);	// only words written are live; the next request continues at first + n
	return true;
}

void radio_tune_counts(uint8_t *nw, uint8_t *nn)
{
	*nw = n_words;
	*nn = n_names;
}

// --- the record (radio-tune.md 3): owner u32, then the entries; one slot of the settings ring ---

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

// Who a record belongs to: this board, then this chip's UID (a board that can't tell its UID: the board alone). A ring
// copied to another radio (a flash image restored onto it) brings its settings, never this radio's trims
static uint32_t owner(void)
{
	hal_chip_t c;
	uint32_t crc = storage_crc32(0, board.name, (uint32_t)strlen(board.name));
	memset(&c, 0, sizeof(c));
	if (board.system != NULL && board.system->chip != NULL && board.system->chip(&c)) {
		crc = storage_crc32(crc, c.uid, sizeof(c.uid));
	}
	return crc;
}

// One entry into the table, live at once (a reader sees the table grow, never a torn word). An entry the radio can't
// use is left out: a drive trim measured against another factory drive, a level or band the radio no longer has, a
// full table
static void add(const uint8_t *e)
{
	uint32_t kind = e[0] >> 4, sel = e[0] & 0xFu, khz = (uint32_t)(e[2] | e[3] << 8) * 25u;
	int32_t v = (int16_t)(e[4] | e[5] << 8);
	uint8_t nw = n_words, nn = n_names;
	if (kind == RADIO_TUNE_NAME) {
		if (nn < RADIO_TUNE_NAMES && e[1] < board_band_count() && e[1] < 16u && board.bands[e[1]].tx &&
		    sel < board.power_count && e[2] != 0) {
			names[nn].band_level = (uint8_t)(e[1] << 4 | sel);
			memcpy(names[nn].name, e + 2, RADIO_TUNE_NAME_LEN);
			names[nn].name[RADIO_TUNE_NAME_LEN] = 0;
			__atomic_signal_fence(__ATOMIC_SEQ_CST);	// names are plain memory: filled before the count says so
			n_names = (uint8_t)(nn + 1u);
		}
		return;
	}
	if (nw == RADIO_TUNE_WORDS || v > 4095 || v < -4095) {
		return;
	}
	if (kind == RADIO_TUNE_BATT) {
		if (sel <= RADIO_TUNE_BATT_FULL && board_has_battery()) {
			words[nw] = RADIO_TUNE_WORD(0u, RADIO_TUNE_W_BATT, sel, v);
			n_words = (uint8_t)(nw + 1u);
		}
		return;
	}
	if (kind > RADIO_TUNE_FREQ || sel > 7u || board_tx_band(khz * 1000u) < 0 ||
	    (kind == RADIO_TUNE_DRIVE && (level_drive(khz * 1000u, (uint8_t)sel) != (uint16_t)(e[6] | e[7] << 8) ||
					  level_drive(khz * 1000u, (uint8_t)sel) == 0u))) {
		return;
	}
	words[nw] = RADIO_TUNE_WORD(khz, kind, sel, v);
	n_words = (uint8_t)(nw + 1u);
}

static void empty(void)
{
	n_words = 0;
	n_names = 0;
}

bool radio_tune_put(bool start, const uint8_t *e, uint8_t n)
{
	if (start) {
		empty();
	}
	for (uint8_t i = 0; i < n; i++) {
		add(e + i * RADIO_TUNE_ENTRY);
	}
	return true;
}

uint8_t radio_tune_get(uint8_t i, uint8_t *e)
{
	uint8_t nw = n_words, nn = n_names;
	memset(e, 0, RADIO_TUNE_ENTRY);
	if (i < nw) {
		uint32_t w = words[i];
		uint32_t kind = W_KIND(w), sel = W_SEL(w);
		if (kind == RADIO_TUNE_W_BATT) {
			e[0] = (uint8_t)(RADIO_TUNE_BATT << 4 | sel);
		} else {
			uint32_t khz = (uint32_t)W_KHZ(w);
			e[0] = (uint8_t)(kind << 4 | sel);
			put16(e + 2, (uint16_t)(khz / 25u));
			if (kind == RADIO_TUNE_DRIVE) {
				put16(e + 6, level_drive(khz * 1000u, (uint8_t)sel));	// the factory drive it applies to now
			}
		}
		put16(e + 4, (uint16_t)(int16_t)W_VAL(w));
	} else if (i < nw + nn) {
		const name_t *m = &names[i - nw];
		e[0] = (uint8_t)(RADIO_TUNE_NAME << 4 | (m->band_level & 0xFu));
		e[1] = (uint8_t)(m->band_level >> 4);
		memcpy(e + 2, m->name, RADIO_TUNE_NAME_LEN);
	}
	return (uint8_t)(nw + nn);
}

// The payload from the table: owner, then each entry (radio_tune_get's)
static uint16_t fill(uint8_t *p, uint16_t cap)
{
	uint8_t n = (uint8_t)(n_words + n_names), max = (uint8_t)((cap - 4u) / RADIO_TUNE_ENTRY);
	uint32_t own = owner();
	memcpy(p, &own, 4);	// little endian, as the radio and the host are
	n = n < max ? n : max;
	for (uint8_t i = 0; i < n; i++) {
		radio_tune_get(i, p + 4u + i * RADIO_TUNE_ENTRY);
	}
	return (uint16_t)(4u + n * RADIO_TUNE_ENTRY);
}

static void take(const uint8_t *p, uint16_t n)
{
	uint32_t own;
	if (n < 4u) {
		return;
	}
	memcpy(&own, p, 4);
	if (own != owner()) {
		return;	// another radio's trims: none here
	}
	for (uint16_t off = 4u; off + RADIO_TUNE_ENTRY <= n; off += RADIO_TUNE_ENTRY) {
		add(p + off);
	}
}

void radio_tune_load(void)
{
	empty();
	if (board.radio != NULL) {
		(void)settings_tune_load(take);
	}
}

bool radio_tune_save(void)
{
	return settings_tune_save(fill);
}

bool radio_tune_clear(void)
{
	empty();
	return settings_tune_save(fill);
}

#endif
