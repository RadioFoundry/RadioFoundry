// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning's record: the two copies in the board's tune area, read at start and rewritten by each edit
// (docs/spec/power-tune.md 3)
#include <string.h>
#include "board.h"
#include "radio/power_tune.h"
#include "storage/storage.h"

#if CONFIG_POWER_TUNE

static uint32_t seq_in_force;
static uint8_t copies;

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static bool drives(void)
{
	return board.radio != NULL && board.radio->tx_drive_level != NULL && board.power_count != 0u;
}

static const hal_flash_map_t *area(void)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || m->tune_addr == 0u || m->tune_addr % STORAGE_SECTOR != 0u || board.flash->erase_size != STORAGE_SECTOR ||
	    board.flash->size < HAL_FLASH_TUNE_UNITS * STORAGE_SECTOR ||
	    m->tune_addr > board.flash->size - HAL_FLASH_TUNE_UNITS * STORAGE_SECTOR) {
		return NULL;
	}
	return m;
}

// Who the record belongs to: this board and this chip (0 when the board can't tell its UID)
static uint32_t board_id(void)
{
	return storage_crc32(0, board.name, (uint32_t)strlen(board.name));
}

static uint32_t chip_id(void)
{
	hal_chip_t c;
	memset(&c, 0, sizeof(c));
	if (board.system == NULL || board.system->chip == NULL || !board.system->chip(&c)) {
		return 0u;
	}
	return storage_crc32(0, c.uid, sizeof(c.uid));
}

static size_t body_len(const uint8_t *r)
{
	return POWER_TUNE_HDR + (size_t)r[9] * POWER_TUNE_POINT_SIZE + (size_t)r[10] * POWER_TUNE_LABEL_SIZE;
}

// Read the copy at addr into r (POWER_TUNE_RECORD_MAX bytes): its body length when it is whole and ours, else 0
static size_t read_copy(uint32_t addr, uint8_t *r)
{
	if (!board.flash->read(addr, r, POWER_TUNE_HDR) || get32(r) != POWER_TUNE_MAGIC || r[8] != POWER_TUNE_VERSION ||
	    r[9] > POWER_TUNE_POINTS || r[10] > POWER_TUNE_LABELS) {
		return 0;
	}
	size_t n = body_len(r);
	if (!board.flash->read(addr + POWER_TUNE_HDR, r + POWER_TUNE_HDR, (uint32_t)(n + 8u - POWER_TUNE_HDR)) ||
	    storage_crc32(0, r, (uint32_t)n) != get32(r + n) || get32(r + n + 4u) != 0u ||
	    get32(r + 12) != board_id() || get32(r + 16) != chip_id()) {
		return 0;
	}
	return n;
}

static bool newer(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

// The copy in force into r: its body length (0 none); *which the copy (0 A, 1 B), copies the valid ones
static size_t read_newest(const hal_flash_map_t *m, uint8_t *r, uint8_t *which)
{
	size_t na = read_copy(m->tune_addr, r);
	uint32_t sa = na != 0u ? get32(r + 4) : 0u;
	size_t nb = read_copy(m->tune_addr + STORAGE_SECTOR, r);
	uint32_t sb = nb != 0u ? get32(r + 4) : 0u;
	copies = (uint8_t)((na != 0u ? 1u : 0u) | (nb != 0u ? 2u : 0u));
	if (nb != 0u && (na == 0u || !newer(sa, sb))) {
		*which = 1;
		return nb;		// still in r
	}
	*which = 0;
	return na != 0u ? read_copy(m->tune_addr, r) : 0u;
}

void power_tune_init(void)
{
	const hal_flash_map_t *m = area();
	uint8_t *r = m != NULL ? storage_buffer_acquire() : NULL;
	if (r == NULL) {
		return;
	}
	uint8_t which;
	storage_lock();
	size_t n = read_newest(m, r, &which);
	seq_in_force = n != 0u ? get32(r + 4) : 0u;
	power_tune_take(r, n);
	storage_unlock();
	storage_buffer_release();
}

// Program one copy: erase, the body and its CRC, then the commit word, then read back
static bool write_copy(uint32_t addr, const uint8_t *r, size_t n, uint8_t *check)
{
	static const uint8_t commit[4] = { 0, 0, 0, 0 };
	if (!board.flash->erase(addr) || !board.flash->write(addr, r, (uint32_t)(n + 4u)) ||
	    !board.flash->write(addr + n + 4u, commit, 4u)) {
		return false;
	}
	return read_copy(addr, check) == n && memcmp(check, r, n) == 0;
}

static bool entry_in(const uint8_t *p, uint8_t level, int band)
{
	return p[4] == level && board_tx_band(get32(p) * 1000u) == band;
}

static power_tune_result_t apply_edit(uint8_t *r, const power_tune_edit_t *e)
{
	uint8_t *pts = r + POWER_TUNE_HDR, *lbl = pts + r[9] * POWER_TUNE_POINT_SIZE;
	size_t lbl_bytes = (size_t)r[10] * POWER_TUNE_LABEL_SIZE;
	uint32_t hz = e->khz * 1000u;
	int band = board_tx_band(hz);
	power_tune_limits_t l;
	if (e->op == POWER_TUNE_CLEAR) {
		r[9] = 0;
		r[10] = 0;
		return POWER_TUNE_OK;
	}
	if (e->khz == 0u || e->khz > 0xFFFFu * 25u || e->level >= POWER_TUNE_LEVELS_MAX || e->level >= board.power_count) {
		return POWER_TUNE_BAD;
	}
	if (e->op == POWER_TUNE_DEL_POINT) {
		for (uint8_t i = 0; i < r[9]; i++) {
			uint8_t *p = pts + i * POWER_TUNE_POINT_SIZE;
			if (p[4] == e->level && get32(p) == e->khz) {
				memmove(p, p + POWER_TUNE_POINT_SIZE, (size_t)(r[9] - i - 1u) * POWER_TUNE_POINT_SIZE + lbl_bytes);
				r[9]--;
				return POWER_TUNE_OK;
			}
		}
		return POWER_TUNE_OK;
	}
	power_tune_limits(hz, e->level, &l);
	if (band < 0 || band >= 16 || l.factory == 0u) {
		return POWER_TUNE_BAD;
	}
	if (e->op == POWER_TUNE_PUT_POINT) {
		int32_t d = (int32_t)l.factory + e->delta;
		if (e->delta > POWER_TUNE_DELTA_MAX || e->delta < -POWER_TUNE_DELTA_MAX || d < l.lo || d > l.hi) {
			return POWER_TUNE_BAD;
		}
		uint8_t i = 0;
		while (i < r[9] && !(pts[i * POWER_TUNE_POINT_SIZE + 4] == e->level && get32(pts + i * POWER_TUNE_POINT_SIZE) == e->khz)) {
			i++;
		}
		if (i == r[9]) {
			if (r[9] >= POWER_TUNE_POINTS) {
				return POWER_TUNE_FULL;
			}
			memmove(lbl + POWER_TUNE_POINT_SIZE, lbl, lbl_bytes);
			r[9]++;
		}
		uint8_t *p = pts + i * POWER_TUNE_POINT_SIZE;
		memset(p, 0, POWER_TUNE_POINT_SIZE);
		put32(p, e->khz);
		p[4] = e->level;
		put16(p + 6, l.factory);
		put16(p + 8, (uint16_t)e->delta);
		put32(p + 12, e->mw);
		return POWER_TUNE_OK;
	}
	if (e->op != POWER_TUNE_PUT_LABEL || strlen(e->name) > POWER_TUNE_LABEL_LEN) {
		return POWER_TUNE_BAD;
	}
	uint8_t i = 0;
	while (i < r[10] && !entry_in(lbl + i * POWER_TUNE_LABEL_SIZE, e->level, band)) {
		i++;
	}
	uint8_t *p = lbl + i * POWER_TUNE_LABEL_SIZE;
	if (e->name[0] == 0 && e->mw == 0u) {
		if (i < r[10]) {
			memmove(p, p + POWER_TUNE_LABEL_SIZE, (size_t)(r[10] - i - 1u) * POWER_TUNE_LABEL_SIZE);
			r[10]--;
		}
		return POWER_TUNE_OK;
	}
	if (i == r[10]) {
		if (r[10] >= POWER_TUNE_LABELS) {
			return POWER_TUNE_FULL;
		}
		r[10]++;
	}
	memset(p, 0, POWER_TUNE_LABEL_SIZE);
	put32(p, e->khz);
	p[4] = e->level;
	put16(p + 6, l.factory);
	put32(p + 8, e->mw);
	memcpy(p + 12, e->name, strlen(e->name));
	return POWER_TUNE_OK;
}

power_tune_result_t power_tune_edit(const power_tune_edit_t *e)
{
	const hal_flash_map_t *m = area();
	if (m == NULL || !storage_ready() || !drives()) {
		return POWER_TUNE_NO_STORE;
	}
	uint8_t *r = storage_buffer_acquire();
	if (r == NULL) {
		return POWER_TUNE_FAILED;
	}
	uint8_t *check = r + POWER_TUNE_RECORD_MAX;	// the shared buffer holds both (STORAGE_SECTOR bytes)
	uint8_t which;
	power_tune_result_t res;
	storage_lock();
	size_t n = read_newest(m, r, &which);
	uint32_t seq = n != 0u ? get32(r + 4) + 1u : 1u;
	if (n == 0u) {
		memset(r, 0, POWER_TUNE_HDR);
		which = 1;		// none in force: A first
	}
	res = apply_edit(r, e);
	if (res == POWER_TUNE_OK) {
		put32(r, POWER_TUNE_MAGIC);
		put32(r + 4, seq != 0u ? seq : 1u);
		r[8] = POWER_TUNE_VERSION;
		r[11] = board.power_count;
		put32(r + 12, board_id());
		put32(r + 16, chip_id());
		n = body_len(r);
		put32(r + n, storage_crc32(0, r, (uint32_t)n));
		// The copy not in force first: a cut leaves the one in force whole, then the second follows the first
		uint32_t first = m->tune_addr + (which == 0u ? STORAGE_SECTOR : 0u);
		uint32_t second = m->tune_addr + (which == 0u ? 0u : STORAGE_SECTOR);
		if (!write_copy(first, r, n, check)) {
			res = POWER_TUNE_FAILED;
		} else {
			bool both = write_copy(second, r, n, check);
			copies = both ? 3u : (which == 0u ? 2u : 1u);
			seq_in_force = get32(r + 4);
			power_tune_take(r, n);
		}
	}
	storage_unlock();
	storage_buffer_release();
	return res;
}

void power_tune_info(power_tune_info_t *i, uint32_t now_ms)
{
	i->seq = seq_in_force;
	i->copies = copies;
	power_tune_counts(&i->points, &i->labels);
	i->store = area() != NULL && storage_ready() && drives();
	i->preview = power_tune_previewing(now_ms);
}

size_t power_tune_read(uint8_t *out, size_t cap)
{
	const hal_flash_map_t *m = area();
	uint8_t which;
	if (m == NULL || cap < POWER_TUNE_RECORD_MAX) {
		return 0;
	}
	storage_lock();
	size_t n = read_newest(m, out, &which);
	storage_unlock();
	for (uint8_t i = 0; n != 0u && i < out[9] + out[10]; i++) {
		uint8_t *p = out + POWER_TUNE_HDR + (i < out[9] ? i * POWER_TUNE_POINT_SIZE
			: out[9] * POWER_TUNE_POINT_SIZE + (i - out[9]) * POWER_TUNE_LABEL_SIZE);
		p[5] = power_tune_state(p);
	}
	return n;
}

#endif
