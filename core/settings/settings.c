// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The settings store (settings.h, docs/spec/settings-store.md 2). The settings area is a ring of sectors, each split
// into SLOT_SIZE slots:
//   0   u32 magic "OSET"     4  u32 sequence number     8  u16 layout     10  u16 payload length n
//   12  n bytes payload      12+n  u32 CRC-32 of bytes 0 .. 11+n
//   SLOT_SIZE-4  u32 commit word: programmed to 0 last; an erased (0xFFFFFFFF) commit word means "not finished"
// The newest record is the valid one with the highest sequence number. A save goes to the next free slot after it;
// when its sector is full, the next sector of the ring is erased and used (it only holds older records).
#include <string.h>
#include "settings/settings.h"
#include "storage/storage.h"

#define SLOT_SIZE	STORAGE_SLOT_SIZE	// the record format is storage.h's (the foreign region guard knows it)
#define SLOTS		(STORAGE_SECTOR / SLOT_SIZE)
#define HDR		STORAGE_SLOT_HDR
#define COMMIT		STORAGE_SLOT_COMMIT
#define PAYLOAD_MAX	STORAGE_SLOT_PAYLOAD_MAX
#define MAGIC		STORAGE_SLOT_MAGIC
#define RETRIES		4		// slots tried when a write doesn't read back

_Static_assert(sizeof(settings_t) <= PAYLOAD_MAX, "settings_t outgrew a slot");
_Static_assert(sizeof(settings_t) % 4 == 0, "settings_t has tail padding");
_Static_assert(sizeof(settings_t) == 232, "settings_t changed size: update docs/spec/settings-store.md 3 and this");

static settings_t live;			// under the quick lock
static settings_t saved;		// what the newest record holds (or the defaults)
static settings_t before;		// live at settings_begin
static bool have_flash;
static bool dirty;
static uint32_t changed_ms;
static settings_stats_t stats;
static bool inited;			// live holds the defaults or a loaded record
static uint16_t sectors;		// ring length

void settings_defaults(settings_t *s)
{
	memset(s, 0, sizeof(*s));
	s->squelch = SETTINGS_UNSET8;
	s->vfo_power = SETTINGS_UNSET8;
	s->vfo_ts = 1;
	s->vfo_cc = 1;
	s->brightness = 100;
	s->backlight_s = SETTINGS_UNSET8;
	s->beeps = 1;
	s->hotspot_allowed = 1;
	s->master_power = SETTINGS_UNSET8;
	s->rs_reply_s = SETTINGS_UNSET8;
	s->mic_gain = 5;
	s->ui_flags = SETTINGS_UI_HINTS | SETTINGS_UI_MESSAGE_TONE | SETTINGS_UI_ALIAS | SETTINGS_UI_PTT_LOCKED;
	s->low_batt_pct = 15;
	s->roam_beacon_s = 60;		// ROAM_BEACON_DEFAULT_S
	s->roam_beacon_len_s = 4;	// ROAM_BEACON_LEN_DEFAULT
	s->roam_survey = 1;		// ROAM_SURVEY_SYNC
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t slot_addr(uint16_t sector, uint16_t slot)
{
	return storage_map()->settings_addr + (uint32_t)sector * STORAGE_SECTOR + (uint32_t)slot * SLOT_SIZE;
}

typedef enum { SLOT_FREE, SLOT_VALID, SLOT_BAD } slot_state_t;

// Read a slot; for a valid one, its sequence number and payload (r holds the whole slot)
static slot_state_t read_slot(uint16_t sector, uint16_t slot, uint8_t *r)
{
	if (!board.flash->read(slot_addr(sector, slot), r, SLOT_SIZE)) {
		return SLOT_BAD;
	}
	switch (storage_slot_check(r)) {
	case STORAGE_SLOT_FREE:
		return SLOT_FREE;
	case STORAGE_SLOT_VALID:
		return SLOT_VALID;
	default:
		return SLOT_BAD;
	}
}

static bool have_newest;		// stats.sector / stats.slot point at the newest valid record
// Slot buffers, used only under the storage lock (kept off the small housekeeping and start-up task stacks)
static uint8_t slot_buf[SLOT_SIZE];
static uint8_t slot_buf2[SLOT_SIZE];

settings_load_t settings_init(void)
{
	const hal_flash_map_t *m = storage_map();
	uint8_t *r = slot_buf;
	uint8_t *best = slot_buf2;

	memset(&stats, 0, sizeof(stats));
	settings_defaults(&live);
	inited = true;
	have_newest = false;
	have_flash = storage_ready() && m != NULL && m->settings_size >= 2u * STORAGE_SECTOR;
	if (!have_flash) {
		saved = live;
		stats.load = storage_region() == STORAGE_REGION_FOREIGN ? SETTINGS_FOREIGN : SETTINGS_RAM;
		return (settings_load_t)stats.load;
	}
	sectors = (uint16_t)(m->settings_size / STORAGE_SECTOR);
	storage_lock();
	for (uint16_t sec = 0; sec < sectors; sec++) {
		for (uint16_t sl = 0; sl < SLOTS; sl++) {
			slot_state_t st = read_slot(sec, sl, r);
			if (st == SLOT_BAD) {
				stats.skipped++;
			} else if (st == SLOT_VALID && (!have_newest || (int32_t)(get32(&r[4]) - stats.seq) > 0)) {
				have_newest = true;
				stats.seq = get32(&r[4]);
				stats.sector = sec;
				stats.slot = sl;
				memcpy(best, r, SLOT_SIZE);
			}
		}
	}
	storage_unlock();
	stats.load = SETTINGS_DEFAULTS;
	if (have_newest && (uint16_t)(best[8] | (best[9] << 8)) == SETTINGS_LAYOUT) {
		uint16_t n = (uint16_t)(best[10] | (best[11] << 8));
		memcpy(&live, &best[HDR], n < sizeof(live) ? n : sizeof(live));
		stats.load = n == sizeof(live) ? SETTINGS_LOADED : SETTINGS_UPGRADED;
	}
	saved = live;
	dirty = false;
	return (settings_load_t)stats.load;
}

// Before settings_init: the defaults
static void ensure(void)
{
	if (!inited) {
		settings_defaults(&live);
		saved = live;
		inited = true;
	}
}

void settings_read(settings_t *out)
{
	storage_quick_lock();
	ensure();
	*out = live;
	storage_quick_unlock();
}

settings_t *settings_begin(void)
{
	storage_quick_lock();
	ensure();
	before = live;
	return &live;
}

void settings_end(uint32_t now_ms)
{
	if (memcmp(&live, &before, sizeof(live)) != 0) {
		changed_ms = now_ms;	// the save waits until changes rest
	}
	dirty = memcmp(&live, &saved, sizeof(live)) != 0;	// false when changed back to what is stored
	storage_quick_unlock();
}

// Where the next record goes: the first free slot after the newest one in its sector, else slot 0 of the next
// sector (erased first). Returns false if the flash fails
static bool next_slot(uint16_t *sector, uint16_t *slot, bool *erase)
{
	uint8_t *r = slot_buf2;
	uint16_t sec = have_newest ? stats.sector : 0;
	uint16_t sl = have_newest ? (uint16_t)(stats.slot + 1u) : 0;

	for (; sl < SLOTS; sl++) {
		if (read_slot(sec, sl, r) == SLOT_FREE) {
			*sector = sec;
			*slot = sl;
			*erase = false;
			return true;
		}
	}
	*sector = have_newest ? (uint16_t)((sec + 1u) % sectors) : 0;
	*slot = 0;
	*erase = true;
	return true;
}

static bool save(const settings_t *s)
{
	static uint8_t r[SLOT_SIZE];
	uint16_t n = sizeof(*s);
	uint32_t seq = stats.seq + 1u;

	storage_lock();
	memset(r, 0xFF, sizeof(r));
	put32(&r[0], MAGIC);
	put32(&r[4], seq);
	r[8] = (uint8_t)SETTINGS_LAYOUT;
	r[9] = (uint8_t)(SETTINGS_LAYOUT >> 8);
	r[10] = (uint8_t)n;
	r[11] = (uint8_t)(n >> 8);
	memcpy(&r[HDR], s, n);
	put32(&r[HDR + n], storage_crc32(0, r, HDR + n));

	bool ok = false;
	for (int attempt = 0; attempt < RETRIES && !ok; attempt++) {
		uint16_t sec, sl;
		bool erase;
		if (!next_slot(&sec, &sl, &erase)) {
			break;
		}
		uint32_t a = slot_addr(sec, sl);
		if (erase) {
			if (!board.flash->erase(slot_addr(sec, 0))) {
				break;
			}
			stats.erases++;
		}
		// The record, then the commit word: a cut in between leaves an unfinished slot, which load skips
		static const uint8_t commit[4] = { 0, 0, 0, 0 };
		uint8_t *back = slot_buf;
		bool written = board.flash->write(a, r, HDR + n + 4u) && board.flash->write(a + COMMIT, commit, 4) &&
			       read_slot(sec, sl, back) == SLOT_VALID && get32(&back[4]) == seq;
		// Whatever happened, this slot is used now: the next attempt (or save) goes past it
		have_newest = have_newest || written;
		if (written) {
			stats.sector = sec;
			stats.slot = sl;
			stats.seq = seq;
			ok = true;
		} else {
			// Skip the slot that failed: make it look like the newest position so next_slot moves on
			stats.sector = sec;
			stats.slot = sl;
			have_newest = true;
		}
	}
	storage_unlock();
	if (ok) {
		stats.saves++;
	} else {
		stats.failures++;
	}
	return ok;
}

static bool save_now(void)
{
	settings_t copy;

	storage_quick_lock();
	copy = live;
	bool need = dirty;
	storage_quick_unlock();
	if (!need) {
		return true;
	}
	if (!have_flash) {
		return false;
	}
	if (!save(&copy)) {
		return false;
	}
	storage_quick_lock();
	saved = copy;
	dirty = memcmp(&live, &saved, sizeof(live)) != 0;	// changed again meanwhile: saved later
	storage_quick_unlock();
	return true;
}

// After a claim (storage.h): the settings area is ours and blank now; what the radio runs with is saved there
static void attach(uint32_t now_ms)
{
	const hal_flash_map_t *m = storage_map();
	if (!storage_ready() || m == NULL || m->settings_size < 2u * STORAGE_SECTOR) {
		return;
	}
	storage_quick_lock();
	ensure();
	sectors = (uint16_t)(m->settings_size / STORAGE_SECTOR);
	have_newest = false;
	stats.seq = 0;
	stats.sector = 0;
	stats.slot = 0;
	stats.skipped = 0;
	stats.load = SETTINGS_DEFAULTS;
	memset(&saved, 0xFF, sizeof(saved));	// nothing stored: unlike anything live holds, so the first poll saves
	dirty = true;
	changed_ms = now_ms - SETTINGS_SAVE_DELAY_MS;
	have_flash = true;
	storage_quick_unlock();
}

void settings_poll(uint32_t now_ms)
{
	if (storage_claim_pending() && storage_claim()) {
		attach(now_ms);
	}
	if (dirty && have_flash && (now_ms - changed_ms) >= SETTINGS_SAVE_DELAY_MS) {
		save_now();
	}
}

bool settings_flush(void)
{
	return save_now() && !dirty;
}

void settings_get_stats(settings_stats_t *s)
{
	storage_quick_lock();
	*s = stats;
	s->dirty = dirty;
	storage_quick_unlock();
}
