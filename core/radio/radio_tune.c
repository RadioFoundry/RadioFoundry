// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio tuning: the record's check and load, and the trims applied at key-up (radio_tune.h, docs/spec/radio-tune.md)
#include <string.h>
#include "board.h"
#include "radio/radio_tune.h"
#include "storage/storage.h"

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

// --- the record ---

// Who a record belongs to: this board, then this chip's UID (a board that can't tell its UID: the board alone)
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

// The copy at addr into r: its seq when it is whole and ours, else 0 (a record never has seq 0)
static uint32_t read_copy(uint32_t addr, uint8_t *r)
{
	if (!board.flash->read(addr, r, RADIO_TUNE_HDR) || get32(r) != RADIO_TUNE_MAGIC || r[8] != RADIO_TUNE_VERSION ||
	    r[9] > RADIO_TUNE_ENTRIES || get32(r + 4) == 0u || get32(r + 12) != owner()) {
		return 0;
	}
	uint32_t n = RADIO_TUNE_HDR + r[9] * RADIO_TUNE_ENTRY;
	if (!board.flash->read(addr + RADIO_TUNE_HDR, r + RADIO_TUNE_HDR, n + 8u - RADIO_TUNE_HDR) ||
	    storage_crc32(0, r, n) != get32(r + n) || get32(r + n + 4u) != 0u) {
		return 0;
	}
	return get32(r + 4);
}

static void take(const uint8_t *r)
{
	uint8_t nw = 0, nn = 0;
	// Readers on other tasks see an empty table while it fills, never old and new words mixed
	n_words = 0;
	n_names = 0;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);	// names are plain memory: kept between the two counts' stores
	for (uint8_t i = 0; r != NULL && i < r[9]; i++) {
		const uint8_t *e = r + RADIO_TUNE_HDR + i * RADIO_TUNE_ENTRY;
		uint32_t kind = e[0] >> 4, sel = e[0] & 0xFu, khz = (uint32_t)(e[2] | e[3] << 8) * 25u;
		int32_t v = (int16_t)(e[4] | e[5] << 8);
		if (kind == RADIO_TUNE_NAME) {
			if (nn < RADIO_TUNE_NAMES && e[1] < board_band_count() && e[1] < 16u && board.bands[e[1]].tx &&
			    sel < board.power_count && e[2] != 0) {
				names[nn].band_level = (uint8_t)(e[1] << 4 | sel);
				memcpy(names[nn].name, e + 2, RADIO_TUNE_NAME_LEN);
				names[nn].name[RADIO_TUNE_NAME_LEN] = 0;
				nn++;
			}
			continue;
		}
		if (kind == RADIO_TUNE_BATT) {
			if (nw < RADIO_TUNE_WORDS && sel <= RADIO_TUNE_BATT_FULL && v <= 4095 && v >= -4095 && board_has_battery()) {
				words[nw++] = RADIO_TUNE_WORD(0u, RADIO_TUNE_W_BATT, sel, v);
			}
			continue;
		}
		// A drive trim measured against another factory drive, or a level or band the radio no longer has, is left out
		if (nw == RADIO_TUNE_WORDS || sel > 7u || v > 4095 || v < -4095 || board_tx_band(khz * 1000u) < 0 ||
		    (kind == RADIO_TUNE_DRIVE && (level_drive(khz * 1000u, (uint8_t)sel) != (uint16_t)(e[6] | e[7] << 8) ||
						  level_drive(khz * 1000u, (uint8_t)sel) == 0u))) {
			continue;
		}
		words[nw++] = RADIO_TUNE_WORD(khz, kind, sel, v);
	}
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	n_names = nn;
	n_words = nw;
}

// Each unit blank, or a tune copy: a record's magic at its start, whole or cut short by a lost save, from this radio
// or not. Anything else may be another firmware's data, and a save would erase it. Only for a tune area the board
// doesn't own: in its own flash every byte is ours
bool radio_tune_area_ours(uint8_t *buf, uint32_t cap)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || m->tune_addr == 0u || board.flash == NULL || cap < 4u) {
		return false;
	}
	for (uint32_t u = 0; u < HAL_FLASH_TUNE_UNITS; u++) {
		uint32_t a = m->tune_addr + u * board.flash->erase_size;
		if (!board.flash->read(a, buf, 4u)) {
			return false;
		}
		if (get32(buf) == RADIO_TUNE_MAGIC) {
			continue;
		}
		for (uint32_t off = 0; off < board.flash->erase_size; off += cap) {
			uint32_t n = board.flash->erase_size - off < cap ? board.flash->erase_size - off : cap;
			if (!board.flash->read(a + off, buf, n)) {
				return false;
			}
			for (uint32_t i = 0; i < n; i++) {
				if (buf[i] != 0xFFu) {
					return false;
				}
			}
		}
	}
	return true;
}

uint8_t radio_tune_load(void)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || m->tune_addr == 0u || board.flash == NULL || board.radio == NULL) {
		return 0;
	}
	uint8_t *r = storage_buffer_acquire();
	if (r == NULL) {
		return RADIO_TUNE_LOAD_BUSY;	// a save or a CPS write holds the buffer: the table stays, the host tries again
	}
	// Copy A at the buffer's start, copy B after it (the shared buffer is a sector; a record is under 300 bytes)
	uint8_t *rb = r + RADIO_TUNE_HDR + RADIO_TUNE_ENTRIES * RADIO_TUNE_ENTRY + 8u;
	storage_lock();
	uint32_t sa = read_copy(m->tune_addr, r), sb = read_copy(m->tune_addr + board.flash->erase_size, rb);
	storage_unlock();
	take(sb != 0u && (sa == 0u || (int32_t)(sb - sa) > 0) ? rb : sa != 0u ? r : NULL);
	storage_buffer_release();
	return (uint8_t)((sa != 0u ? 1u : 0u) | (sb != 0u ? 2u : 0u));
}

#endif
