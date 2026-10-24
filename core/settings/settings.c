// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The settings store (settings.h, docs/spec/settings-store.md 2). The settings area is a ring of sectors, each split
// into SLOT_SIZE slots:
//   0   u32 magic "OSET"     4  u32 sequence number     8  u16 layout     10  u16 payload length n
//   12  n bytes payload      12+n  u32 CRC-32 of bytes 0 .. 11+n
//   SLOT_SIZE-4  u32 commit word: programmed to 0 last; an erased (0xFFFFFFFF) commit word means "not finished"
// The layout field holds the page in bits 12-15: settings_t is stored as SETTINGS_PAGES records of up to
// SETTINGS_PAGE_BYTES. A save writes every page, the last pages first and page 0 last, each with the next sequence
// number. The set in force is the newest valid page 0 record and, for each other page, the newest valid record of that
// page older than it (the one its save wrote just before). A save goes to the next free slot after the newest record;
// when its sector is full, the next sector of the ring is erased and used (it only holds older records).
#include <stddef.h>
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

_Static_assert(SETTINGS_PAGE_BYTES == PAYLOAD_MAX, "a page is a slot's payload");
_Static_assert(SETTINGS_PAGES <= SETTINGS_PAGES_MAX, "settings_t outgrew its pages");
_Static_assert(sizeof(settings_t) % 4 == 0, "settings_t has tail padding");
_Static_assert(offsetof(settings_t, dual_b) == SETTINGS_PAGE_BYTES, "page 1 starts at its first field");
_Static_assert(sizeof(settings_t) == 272,"settings_t changed size: update docs/spec/settings-store.md 3 and this");

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
	s->gps = 1;			// powered only on radios where a module was heard (board_t.gps_enable)
	s->vfo2_power = SETTINGS_UNSET8;
	s->vfo2_cc = 1;
	s->vfo2_ts = 1;
}

// Page k of a settings_t: its offset and length
static uint16_t page_len(unsigned k)
{
	size_t off = k * SETTINGS_PAGE_BYTES;
	size_t n = sizeof(settings_t) - off;
	return (uint16_t)(n < SETTINGS_PAGE_BYTES ? n : SETTINGS_PAGE_BYTES);
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

static bool have_newest;		// stats.sector / stats.slot point at the newest valid record (or a slot past it that failed)
// Sectors no save may erase, a bit each: those holding a page of the set in force (valid_mask), and those this save
// wrote a page to so far (save_mask)
static uint32_t valid_mask, save_mask;
#define SECTORS_MAX	32u
static uint32_t next_seq;		// the next record's sequence number: past every record in the ring
// Slot buffers, used only under the storage lock (kept off the small housekeeping and start-up task stacks)
static uint8_t slot_buf[SLOT_SIZE];
static uint8_t slot_buf2[SLOT_SIZE];

static uint16_t rec_page(const uint8_t *r) { return (uint16_t)((r[8] | (r[9] << 8)) >> SETTINGS_PAGE_SHIFT); }
static uint16_t rec_layout(const uint8_t *r) { return (uint16_t)((r[8] | (r[9] << 8)) & 0x0FFFu); }

settings_load_t settings_init(void)
{
	const hal_flash_map_t *m = storage_map();
	uint8_t *r = slot_buf;

	memset(&stats, 0, sizeof(stats));
	settings_defaults(&live);
	inited = true;
	have_newest = false;
	valid_mask = 0;
	have_flash = storage_ready() && m != NULL && m->settings_size >= 2u * STORAGE_SECTOR;
	if (!have_flash) {
		saved = live;
		stats.load = storage_region() == STORAGE_REGION_FOREIGN ? SETTINGS_FOREIGN : SETTINGS_RAM;
		return (settings_load_t)stats.load;
	}
	sectors = (uint16_t)(m->settings_size / STORAGE_SECTOR);
	if (sectors > SECTORS_MAX) {
		sectors = SECTORS_MAX;	// the masks' width; a larger area leaves the rest unused
	}
	// First pass: the newest record of any page (where the next save follows) and the newest page 0 (the set)
	bool have0 = false;
	uint32_t seq0 = 0, seq_any = 0;
	uint16_t sec0 = 0, sl0 = 0;
	storage_lock();
	for (uint16_t sec = 0; sec < sectors; sec++) {
		for (uint16_t sl = 0; sl < SLOTS; sl++) {
			slot_state_t st = read_slot(sec, sl, r);
			if (st == SLOT_BAD) {
				stats.skipped++;
				continue;
			}
			if (st != SLOT_VALID) {
				continue;
			}
			uint32_t q = get32(&r[4]);
			if (!have_newest || (int32_t)(q - seq_any) > 0) {
				have_newest = true;
				seq_any = q;
				stats.sector = sec;
				stats.slot = sl;
			}
			if (rec_page(r) == 0 && (!have0 || (int32_t)(q - seq0) > 0)) {
				have0 = true;
				seq0 = q;
				sec0 = sec;
				sl0 = sl;
			}
		}
	}
	stats.load = SETTINGS_DEFAULTS;
	stats.seq = seq0;
	next_seq = seq_any + 1u;
	if (have0) {
		valid_mask = 1u << sec0;
	}
	if (have0 && read_slot(sec0, sl0, r) == SLOT_VALID && rec_layout(r) == SETTINGS_LAYOUT) {
		uint16_t n = (uint16_t)(r[10] | (r[11] << 8));
		memcpy(&live, &r[HDR], n < page_len(0) ? n : page_len(0));
		bool whole = n == page_len(0);
		// Second pass: each other page, the newest of it older than page 0 (an older firmware's save wrote
		// page 0 alone: the page from before it, else the defaults)
		for (unsigned k = 1; k < SETTINGS_PAGES; k++) {
			bool found = false;
			uint32_t qk = 0;
			uint16_t sk = 0, lk = 0;
			for (uint16_t sec = 0; sec < sectors; sec++) {
				for (uint16_t sl = 0; sl < SLOTS; sl++) {
					if (read_slot(sec, sl, r) != SLOT_VALID || rec_page(r) != k ||
					    rec_layout(r) != SETTINGS_LAYOUT) {
						continue;
					}
					uint32_t q = get32(&r[4]);
					if ((int32_t)(seq0 - q) > 0 && (!found || (int32_t)(q - qk) > 0)) {
						found = true;
						qk = q;
						sk = sec;
						lk = sl;
					}
				}
			}
			if (found && read_slot(sk, lk, r) == SLOT_VALID) {
				uint16_t nk = (uint16_t)(r[10] | (r[11] << 8));
				memcpy((uint8_t *)&live + k * SETTINGS_PAGE_BYTES, &r[HDR], nk < page_len(k) ? nk : page_len(k));
				whole = whole && nk == page_len(k) && qk == seq0 - k;
				valid_mask |= 1u << sk;	// part of the set in force: saves must not erase it either
			} else {
				whole = false;
			}
		}
		stats.load = whole ? SETTINGS_LOADED : SETTINGS_UPGRADED;
	}
	storage_unlock();
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

// Where the next record goes: the first free slot after the newest one in its sector, else slot 0 of the next sector
// that holds no page of the set in force or of this save (erased first). False when every sector does
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
	*slot = 0;
	*erase = true;
	for (uint16_t i = 0; i < sectors; i++) {
		*sector = have_newest ? (uint16_t)((sec + 1u + i) % sectors) : i;
		if (((valid_mask | save_mask) & (1u << *sector)) == 0) {
			return true;
		}
	}
	return false;
}

// One record (page `page`, n bytes of payload) to the next free slot, retried past slots that don't read back
static bool put_record(uint16_t page, const uint8_t *payload, uint16_t n)
{
	static uint8_t r[SLOT_SIZE];
	uint32_t seq = next_seq;
	uint16_t layout = (uint16_t)(SETTINGS_LAYOUT | (page << SETTINGS_PAGE_SHIFT));

	memset(r, 0xFF, sizeof(r));
	put32(&r[0], MAGIC);
	put32(&r[4], seq);
	r[8] = (uint8_t)layout;
	r[9] = (uint8_t)(layout >> 8);
	r[10] = (uint8_t)n;
	r[11] = (uint8_t)(n >> 8);
	memcpy(&r[HDR], payload, n);
	put32(&r[HDR + n], storage_crc32(0, r, HDR + n));

	for (int attempt = 0; attempt < RETRIES; attempt++) {
		uint16_t sec, sl;
		bool erase;
		if (!next_slot(&sec, &sl, &erase)) {
			return false;
		}
		uint32_t a = slot_addr(sec, sl);
		if (erase) {
			if (!board.flash->erase(slot_addr(sec, 0))) {
				return false;
			}
			stats.erases++;
		}
		// The record, then the commit word: a cut in between leaves an unfinished slot, which load skips
		static const uint8_t commit[4] = { 0, 0, 0, 0 };
		uint8_t *back = slot_buf;
		bool written = board.flash->write(a, r, HDR + n + 4u) && board.flash->write(a + COMMIT, commit, 4) &&
			       read_slot(sec, sl, back) == SLOT_VALID && get32(&back[4]) == seq;
		// Whatever happened, this slot is used now: the next attempt (or record) goes past it
		stats.sector = sec;
		stats.slot = sl;
		have_newest = true;
		if (written) {
			next_seq = seq + 1u;
			save_mask |= 1u << sec;
			return true;
		}
	}
	return false;
}

// Every page, the last first and page 0 last: page 0's commit makes the set the one in force
static bool save(const settings_t *s)
{
	bool ok = true;

	storage_lock();
	save_mask = 0;
	for (unsigned k = SETTINGS_PAGES; ok && k-- > 0;) {
		ok = put_record((uint16_t)k, (const uint8_t *)s + k * SETTINGS_PAGE_BYTES, page_len(k));
	}
	if (ok) {
		valid_mask = save_mask;
		stats.seq = next_seq - 1u;
	}
	save_mask = 0;
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
	if (sectors > SECTORS_MAX) {
		sectors = SECTORS_MAX;
	}
	have_newest = false;
	valid_mask = 0;
	next_seq = 1;
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
