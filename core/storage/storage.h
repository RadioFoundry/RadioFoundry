// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Safe writes to the board's data flash (docs/spec/settings-store.md 4): a sector rewrite goes through a journal (a
// copy of the new sector and a header saying where it goes), so a power cut in the middle of the erase or the
// program is finished at the next boot instead of leaving a half-erased codeplug sector. Shared by the codeplug
// editor and the legacy CPS shim. No RTOS here: storage_lock() / storage_unlock() come from storage_rtos.c in the
// firmware and from the test in host tests.
#ifndef ODR_STORAGE_H
#define ODR_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#define STORAGE_SECTOR		4096u	// the erase unit the core works in (boards with another erase size: no writes)

// One recursive lock around every flash transaction sequence of the core (settings, codeplug, CPS)
void storage_lock(void);
void storage_unlock(void);
// A second lock for short RAM-only sections (the settings copy): never held across a flash operation, so the radio
// task never waits for an erase
void storage_quick_lock(void);
void storage_quick_unlock(void);

// After the board's flash works, before the settings and the codeplug are read: check that the settings ring and the
// journal are ours (else the region is foreign, below), then finish a journaled rewrite that a power cut
// interrupted. False if there is no usable flash or the region is foreign (then every write is refused)
bool storage_init(void);
// Flash usable and the region ours: writes allowed
bool storage_ready(void);

// --- Foreign region guard (docs/spec/settings-store.md 5) ---
// A flash that held another firmware may have its own data where our settings ring and journal go. When the region
// holds bytes that are neither erased nor ours, and not one valid record of ours (a settings record, a journal
// header) is anywhere in it, it is foreign: nothing is erased or programmed there (and so no codeplug or CPS write
// goes through the journal) until the debug link claims it (after the host dumped it)
typedef enum {
	STORAGE_REGION_NONE,		// no flash map / unusable flash
	STORAGE_REGION_OURS,		// blank, or holding our records
	STORAGE_REGION_FOREIGN,		// someone else's data: hands off
} storage_region_t;
storage_region_t storage_region(void);
// Ask for a claim (the debug link, behind the UID token): the housekeeping task erases the settings ring and the
// journal (settings_poll -> storage_claim) and the region becomes ours. False if the region isn't foreign
bool storage_claim_request(void);
bool storage_claim_pending(void);
// Erase the region and take it (false if not foreign or the flash failed). Slow: some 10 sector erases
bool storage_claim(void);
// The flash ranges the debug link may read (the settings ring, the journal, the calibration copy): true when the
// whole of [addr, addr + len) is inside one of them
bool storage_debug_read_allowed(uint32_t addr, uint32_t len);

// --- Settings slots (the record format, shared with core/settings so the guard knows our records) ---
#define STORAGE_SLOT_SIZE	256u
#define STORAGE_SLOT_MAGIC	0x5445534Fu	// "OSET"
#define STORAGE_SLOT_HDR	12u		// magic, sequence, layout, payload length
#define STORAGE_SLOT_COMMIT	(STORAGE_SLOT_SIZE - 4u)
#define STORAGE_SLOT_PAYLOAD_MAX (STORAGE_SLOT_COMMIT - STORAGE_SLOT_HDR - 4u)
typedef enum {
	STORAGE_SLOT_FREE,		// all 0xFF
	STORAGE_SLOT_VALID,		// a complete record (magic, length, CRC, commit word)
	STORAGE_SLOT_TORN,		// our magic but not complete (a cut during a save)
	STORAGE_SLOT_OTHER,		// anything else (a torn erase, or someone else's data)
} storage_slot_t;
storage_slot_t storage_slot_check(const uint8_t slot[STORAGE_SLOT_SIZE]);

// Rewrite the erase unit at addr (aligned) with data (STORAGE_SECTOR bytes), through the journal. Skipped when the
// flash already holds exactly that
bool storage_sector_write(uint32_t addr, const uint8_t *data);

// Write any range: each sector it touches is read into the shared buffer, merged and rewritten as above. False if
// the shared buffer is taken (a CPS write in progress) or the flash fails
bool storage_write(uint32_t addr, const void *buf, uint32_t len);

// The shared sector buffer (STORAGE_SECTOR bytes): NULL while someone else has it
uint8_t *storage_buffer_acquire(void);
void storage_buffer_release(void);

// The flash map, NULL without one
const hal_flash_map_t *storage_map(void);

// --- The native codeplug area (docs/spec/codeplug-native.md 4) ---
// Its header sector starts with this magic ("ODRN", little endian). Nothing writes the area yet; the magic is what
// tells our data from another firmware's there
#define STORAGE_NATIVE_MAGIC	0x4E52444Fu
typedef enum {
	STORAGE_NATIVE_BLANK,		// nothing there: nothing to do
	STORAGE_NATIVE_ERASED,		// our native data was there and is erased now
	STORAGE_NATIVE_FOREIGN,		// something that isn't ours (no magic in the header sector): left alone
	STORAGE_NATIVE_FAILED,		// no flash, no area, the region isn't ours, or an erase failed
} storage_native_t;
// Empty the native area (factory reset): only when it holds our data (the magic) or nothing. The data sectors go
// first and the header sector last, so a cut in between leaves the magic and the reset can run again. Slow: one
// erase per sector that isn't blank (the DM-1701's 1 MB area: up to 256)
storage_native_t storage_native_erase(void);

// --- Deferred flash work: a task that must not wait for the flash (the UI) hands a job to the housekeeping task,
// which runs it from its loop (storage_job_poll, every 100 ms). One job at a time, posted from one task ---
typedef void (*storage_job_fn)(void *ctx);
// False while the last job is still pending or running
bool storage_job_post(storage_job_fn fn, void *ctx);
bool storage_job_busy(void);
// The housekeeping task: run the pending job, if any
void storage_job_poll(void);

typedef struct {
	uint32_t sector_writes;		// journaled rewrites done
	uint32_t sector_skips;		// rewrites skipped (same content)
	uint32_t replays;		// interrupted rewrites finished at boot
	uint32_t errors;		// flash operations that failed
	uint32_t foreign_refusals;	// writes refused because the region is foreign
	uint16_t foreign_slots;		// settings slots (and journal sectors) that were neither erased nor ours at boot
	bool claim_pending;
} storage_stats_t;

void storage_get_stats(storage_stats_t *s);

// CRC-32 (IEEE 802.3, reflected, init and final XOR 0xFFFFFFFF); crc = 0 to start
uint32_t storage_crc32(uint32_t crc, const void *buf, uint32_t len);

#endif
