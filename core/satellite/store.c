// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Persisted satellite TLE inventory (docs/spec/satellite.md 6). Three erase sectors keep the active inventory intact
// while a transfer is validated and the next complete generation is written.
#include <string.h>
#include "config.h"
#include "board.h"
#include "satellite/frequency.h"
#include "satellite/orbit.h"
#include "satellite/store.h"
#include "storage/storage.h"

#if CONFIG_SATELLITE_TLE
#define SAT_MAX_RECORDS		SATELLITE_STORE_MAX_RECORDS
#define SAT_MAX_NAME		48u
#define SAT_TLE_LINE		69u
#define SAT_MAX_RECORD_BYTES	(4u + 1u + SAT_MAX_NAME + 2u * SAT_TLE_LINE)
#define SAT_MAX_DATA		(SAT_MAX_RECORDS * SAT_MAX_RECORD_BYTES)
#define SAT_HEADER_SIZE		172u
#define SAT_PROFILE_OFFSET	24u
#define SAT_PROFILE_BYTES	141u
#define SAT_PROFILE_CRC_OFFSET	165u
#define SAT_MAGIC		0x31544653u
#define SAT_COMMIT		0x54494D43u
#define SAT_PENDING		(SAT_COMMIT | 0x80000000u)
#define SAT_STAGE_MAGIC		0x31475453u
#define SAT_VERSION		4u
#define SAT_VERSION_V3		3u
#define SAT_VERSION_V2		2u
#define SAT_VERSION_LEGACY	1u
typedef struct {
	uint32_t catalog_id;
	uint32_t epoch;
	uint16_t offset;
	uint16_t length;
} sat_entry_t;

typedef struct {
	uint32_t generation;
	uint32_t crc;
	uint32_t profile_ids[SATELLITE_PROFILE_MAX];
	satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX];
	uint16_t length;
	uint8_t header_size;
	uint8_t count;
	uint8_t profile_count;
} sat_bank_t;

static const uint32_t default_profile_ids[] = { 25544u, 27607u, 43017u };

static bool transaction_active;
static uint16_t transaction_id;
static uint16_t next_transaction_id = 1u;
static uint16_t transaction_count;
static uint16_t transaction_length;
static uint16_t transaction_offset;
static uint32_t transaction_crc;
static uint8_t transaction_bank;
static bool transaction_replace;
static bool transaction_profiles_staged;
static bool transaction_profile_chunk_active;
static uint8_t transaction_profile_count;
static uint8_t transaction_profile_staged_count;
static satellite_frequency_profile_t transaction_profiles[SATELLITE_PROFILE_MAX];
static sat_entry_t transaction_entries[SAT_MAX_RECORDS];
static sat_entry_t active_entries[SAT_MAX_RECORDS];
static uint8_t record_buf[SAT_MAX_RECORD_BYTES];
static uint8_t io_buf[128];
static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

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
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static const hal_flash_map_t *sat_map(void)
{
	const hal_flash_map_t *m = storage_map();
	if (!storage_ready() || m == NULL || m->satellite_size < 3u * STORAGE_SECTOR ||
	    (m->satellite_addr % STORAGE_SECTOR) != 0u || (m->satellite_size % STORAGE_SECTOR) != 0u ||
	    board.flash == NULL || board.flash->read == NULL || board.flash->write == NULL || board.flash->erase == NULL ||
	    board.flash->erase_size != STORAGE_SECTOR || m->satellite_addr > board.flash->size ||
	    m->satellite_size > board.flash->size - m->satellite_addr) {
		return NULL;
	}
	return m;
}

static uint32_t bank_addr(const hal_flash_map_t *m, uint8_t bank)
{
	return m->satellite_addr + (uint32_t)bank * STORAGE_SECTOR;
}

static bool flash_read(uint32_t addr, void *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->read(addr, buf, len);
	storage_unlock();
	return ok;
}

static bool flash_write(uint32_t addr, const void *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->write(addr, buf, len);
	storage_unlock();
	return ok;
}

static bool flash_erase(uint32_t addr)
{
	storage_lock();
	bool ok = board.flash->erase(addr);
	storage_unlock();
	return ok;
}

static bool utf8_name(const uint8_t *p, uint8_t n)
{
	for (uint8_t i = 0; i < n;) {
		uint8_t c = p[i++];
		if (c < 0x20u || c == 0x7Fu) {
			return false;
		}
		if (c < 0x80u) {
			continue;
		}
		uint8_t more = c >= 0xC2u && c <= 0xDFu ? 1u : c >= 0xE0u && c <= 0xEFu ? 2u :
			       c >= 0xF0u && c <= 0xF4u ? 3u : 0u;
		if (more == 0u || more > n - i) {
			return false;
		}
		uint8_t first = p[i];
		if ((c == 0xE0u && first < 0xA0u) || (c == 0xEDu && first >= 0xA0u) ||
		    (c == 0xF0u && first < 0x90u) || (c == 0xF4u && first >= 0x90u)) {
			return false;
		}
		for (uint8_t j = 0; j < more; j++) {
			if (p[i + j] < 0x80u || p[i + j] > 0xBFu) {
				return false;
			}
		}
		i = (uint8_t)(i + more);
	}
	return true;
}

static bool validate_record(const uint8_t *p, uint16_t len, sat_entry_t *entry)
{
	if (len < 5u + 1u + 2u * SAT_TLE_LINE) {
		return false;
	}
	uint32_t id = get32(p);
	uint8_t name_len = p[4];
	uint16_t expected = (uint16_t)(5u + name_len + 2u * SAT_TLE_LINE);
	if (id == 0u || id > 99999u || name_len == 0u || name_len > SAT_MAX_NAME || len != expected ||
	    !utf8_name(p + 5, name_len)) {
		return false;
	}
	const uint8_t *line1 = p + 5u + name_len;
	const uint8_t *line2 = line1 + SAT_TLE_LINE;
	if (line1[7] != 'U' && line1[7] != 'C' && line1[7] != 'S') {
		return false;
	}
	for (uint8_t i = 0u; i < 2u * SAT_TLE_LINE; i++) {
		if (line1[i] < 0x20u || line1[i] > 0x7Eu) {
			return false;
		}
	}
	satellite_record_t record = { 0 };
	record.catalog_id = id;
	record.name_length = name_len;
	memcpy(record.name, p + 5u, name_len);
	memcpy(record.line1, line1, SAT_TLE_LINE);
	memcpy(record.line2, line2, SAT_TLE_LINE);
	int64_t epoch_s;
	uint16_t fraction_q16;
	if (!satellite_tle_epoch(record.line1, &epoch_s, &fraction_q16) || epoch_s < 0 || epoch_s > UINT32_MAX) {
		return false;
	}
	record.epoch = (uint32_t)epoch_s;
	satellite_orbit_t orbit;
	if (!satellite_orbit_init(&orbit, &record)) {
		return false;
	}
	entry->catalog_id = id;
	entry->epoch = (uint32_t)orbit.epoch_s;
	return true;
}

static bool profile_id_supported(uint32_t catalog_id)
{
	for (uint8_t i = 0u; i < satellite_frequency_profile_catalog_count(); i++) {
		satellite_frequency_profile_t profile;
		if (satellite_frequency_profile_catalog_get(i, &profile) && profile.catalog_id == catalog_id)
			return true;
	}
	return false;
}

static bool catalog_profile(uint32_t catalog_id, satellite_frequency_profile_t *profile)
{
	for (uint8_t i = 0u; i < satellite_frequency_profile_catalog_count(); i++) {
		satellite_frequency_profile_t candidate;
		if (satellite_frequency_profile_catalog_get(i, &candidate) && candidate.catalog_id == catalog_id) {
			if (profile != NULL)
				*profile = candidate;
			return true;
		}
	}
	return false;
}

static bool default_frequency_profiles(satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX])
{
	for (uint8_t i = 0u; i < (uint8_t)(sizeof(default_profile_ids) / sizeof(default_profile_ids[0])); i++)
		if (!catalog_profile(default_profile_ids[i], &profiles[i]))
			return false;
	return true;
}

static bool valid_profiles(uint8_t count, const uint32_t *ids)
{
	if (count > SATELLITE_PROFILE_MAX || (count != 0u && ids == NULL))
		return false;
	for (uint8_t i = 0u; i < count; i++) {
		if (!profile_id_supported(ids[i]))
			return false;
		for (uint8_t j = 0u; j < i; j++)
			if (ids[i] == ids[j])
				return false;
	}
	return true;
}

static bool valid_frequency_profiles(uint8_t count, const satellite_frequency_profile_t *profiles)
{
	if (count > SATELLITE_PROFILE_MAX || (count != 0u && profiles == NULL))
		return false;
	for (uint8_t i = 0u; i < count; i++) {
		const satellite_frequency_profile_t *profile = &profiles[i];
		if (profile->catalog_id == 0u || profile->rx_hz == 0u || profile->rx_hz >= 1000000000u ||
		    profile->tx_hz == 0u || profile->tx_hz >= 1000000000u ||
		    (profile->tx_tone_tenths != 0u &&
		     (profile->tx_tone_tenths < 600u || profile->tx_tone_tenths > 2600u)) ||
		    (board_band_count() != 0u &&
		     (!board_in_band(profile->rx_hz, false) || !board_in_band(profile->tx_hz, true))))
			return false;
		for (uint8_t j = 0u; j < i; j++)
			if (profile->catalog_id == profiles[j].catalog_id)
				return false;
	}
	return true;
}

static bool read_record(uint32_t base, uint16_t header_size, uint16_t offset, uint16_t total,
			uint8_t *buf, uint16_t *length)
{
	uint8_t prefix[5];
	if (offset > total || (uint32_t)(total - offset) < (uint32_t)sizeof(prefix) ||
	    !flash_read(base + header_size + offset, prefix, sizeof(prefix))) {
		return false;
	}
	uint8_t name_len = prefix[4];
	uint16_t n = (uint16_t)(5u + name_len + 2u * SAT_TLE_LINE);
	if (name_len == 0u || name_len > SAT_MAX_NAME || n > sizeof(record_buf) || n > total - offset ||
	    !flash_read(base + header_size + offset, buf, n)) {
		return false;
	}
	*length = n;
	return true;
}

static bool check_bank(const hal_flash_map_t *m, uint8_t bank, sat_bank_t *info, sat_entry_t *entries, bool *valid)
{
	uint8_t h[SAT_HEADER_SIZE];
	uint32_t base = bank_addr(m, bank);
	*valid = false;
	if (!flash_read(base, h, sizeof(h))) {
		return false;
	}
	uint8_t header_size = h[4] == SAT_VERSION ? SAT_HEADER_SIZE :
			      h[4] == SAT_VERSION_V3 ? 72u :
			      h[4] == SAT_VERSION_V2 ? 24u : h[4] == SAT_VERSION_LEGACY ? 20u : 0u;
	uint8_t commit_offset = h[4] == SAT_VERSION_LEGACY ? 16u : 20u;
	if (get32(h) != SAT_MAGIC || header_size == 0u || get32(h + commit_offset) != SAT_COMMIT) {
		return true;
	}
	uint8_t count = h[5];
	uint16_t length = get16(h + 6);
	if (count == 0u || count > SAT_MAX_RECORDS || length == 0u || length > SAT_MAX_DATA ||
	    length > STORAGE_SECTOR - header_size) {
		return true;
	}
	uint32_t crc = 0;
	for (uint16_t off = 0; off < length;) {
		uint16_t remaining = (uint16_t)(length - off);
		uint16_t n = remaining > sizeof(io_buf) ? (uint16_t)sizeof(io_buf) : remaining;
		if (!flash_read(base + header_size + off, io_buf, n)) {
			return false;
		}
		crc = storage_crc32(crc, io_buf, n);
		off = (uint16_t)(off + n);
	}
	if (crc != get32(h + 12)) {
		return true;
	}
	uint16_t offset = 0;
	uint32_t previous = 0;
	for (uint8_t i = 0; i < count; i++) {
		uint16_t n;
		if (!read_record(base, header_size, offset, length, record_buf, &n) ||
		    !validate_record(record_buf, n, &entries[i]) ||
		    entries[i].catalog_id <= previous) {
			return true;
		}
		entries[i].offset = offset;
		entries[i].length = n;
		previous = entries[i].catalog_id;
		offset = (uint16_t)(offset + n);
	}
	if (offset != length) {
		return true;
	}
	info->generation = get32(h + 8);
	info->crc = crc;
	info->length = length;
	info->header_size = header_size;
	info->count = count;
	if (h[4] == SAT_VERSION) {
		if (storage_crc32(0u, h + SAT_PROFILE_OFFSET, SAT_PROFILE_BYTES) != get32(h + SAT_PROFILE_CRC_OFFSET))
			return true;
		info->profile_count = h[SAT_PROFILE_OFFSET];
		if (info->profile_count > SATELLITE_PROFILE_MAX)
			return true;
		for (uint8_t i = 0u; i < SATELLITE_PROFILE_MAX; i++)
		{
			const uint8_t *p = h + SAT_PROFILE_OFFSET + 1u + (uint16_t)i * 14u;
			info->profiles[i].catalog_id = get32(p);
			info->profiles[i].rx_hz = get32(p + 4u);
			info->profiles[i].tx_hz = get32(p + 8u);
			info->profiles[i].tx_tone_tenths = get16(p + 12u);
			info->profiles[i].name = NULL;
			info->profile_ids[i] = info->profiles[i].catalog_id;
			if (info->profiles[i].catalog_id != 0u) {
				satellite_frequency_profile_t catalog;
				if (catalog_profile(info->profiles[i].catalog_id, &catalog))
					info->profiles[i].name = catalog.name;
			}
		}
		if (!valid_frequency_profiles(info->profile_count, info->profiles))
			return true;
	} else {
		if (h[4] == SAT_VERSION_V3) {
			if (storage_crc32(0u, h + SAT_PROFILE_OFFSET, 41u) != get32(h + 65u))
				return true;
			info->profile_count = h[SAT_PROFILE_OFFSET];
			if (info->profile_count > SATELLITE_PROFILE_MAX)
				return true;
			for (uint8_t i = 0u; i < SATELLITE_PROFILE_MAX; i++)
				info->profile_ids[i] = get32(h + SAT_PROFILE_OFFSET + 1u + (uint16_t)i * 4u);
			if (!valid_profiles(info->profile_count, info->profile_ids))
				return true;
		} else {
			info->profile_count = (uint8_t)(sizeof(default_profile_ids) / sizeof(default_profile_ids[0]));
			memcpy(info->profile_ids, default_profile_ids, sizeof(default_profile_ids));
		}
		for (uint8_t i = 0u; i < info->profile_count; i++)
			if (!catalog_profile(info->profile_ids[i], &info->profiles[i]))
				return true;
	}
	*valid = true;
	return true;
}

static bool newer(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

static bool bank_erased(const hal_flash_map_t *m, uint8_t bank, bool *erased)
{
	*erased = true;
	for (uint32_t off = 0; off < STORAGE_SECTOR; off += sizeof(io_buf)) {
		uint32_t n = STORAGE_SECTOR - off < sizeof(io_buf) ? STORAGE_SECTOR - off : sizeof(io_buf);
		if (!flash_read(bank_addr(m, bank) + off, io_buf, n)) {
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (io_buf[i] != 0xFFu) {
				*erased = false;
				return true;
			}
		}
	}
	return true;
}

static bool active_bank(const hal_flash_map_t *m, int *active, sat_bank_t *info)
{
	bool found = false;
	*active = -1;
	memset(info, 0, sizeof(*info));
	for (uint8_t i = 0; i < 3u; i++) {
		sat_bank_t candidate;
		bool valid;
		if (!check_bank(m, i, &candidate, active_entries, &valid)) {
			return false;
		}
		if (valid && (!found || newer(candidate.generation, info->generation))) {
			*active = i;
			*info = candidate;
			found = true;
		}
	}
	if (*active >= 0) {
		bool valid;
		if (!check_bank(m, (uint8_t)*active, info, active_entries, &valid) || !valid) {
			return false;
		}
	}
	return true;
}

static bool stage_entries(const hal_flash_map_t *m)
{
	uint16_t offset = 0;
	uint32_t previous = 0;
	for (uint16_t i = 0; i < transaction_count; i++) {
		uint16_t n;
		if (!read_record(bank_addr(m, transaction_bank), SAT_HEADER_SIZE, offset, transaction_length,
				 record_buf, &n) ||
		    !validate_record(record_buf, n, &transaction_entries[i]) ||
		    transaction_entries[i].catalog_id <= previous) {
			return false;
		}
		transaction_entries[i].offset = offset;
		transaction_entries[i].length = n;
		previous = transaction_entries[i].catalog_id;
		offset = (uint16_t)(offset + n);
	}
	return offset == transaction_length;
}

static bool copy_record(uint32_t src, uint16_t src_header_size, uint16_t src_off, uint32_t dst,
			uint16_t dst_off, uint16_t len, uint32_t *crc)
{
	if (len > sizeof(record_buf) || !flash_read(src + src_header_size + src_off, record_buf, len) ||
	    !flash_write(dst + SAT_HEADER_SIZE + dst_off, record_buf, len)) {
		return false;
	}
	*crc = storage_crc32(*crc, record_buf, len);
	return true;
}

static bool commit_transaction(const hal_flash_map_t *m, int active, const sat_bank_t *old_info,
			       uint8_t profile_count, const satellite_frequency_profile_t *profiles,
			       bool replace_records)
{
	if (!stage_entries(m) || !valid_frequency_profiles(profile_count, profiles)) {
		return false;
	}
	uint8_t old_count = active >= 0 && !replace_records ? old_info->count : 0u;
	uint8_t old_i = 0, new_i = 0, result_count = 0;
	uint32_t out_crc = 0;
	uint16_t out_len = 0;
	uint8_t target = 0xFFu;
	for (uint8_t i = 0; i < 3u; i++) {
		if ((int)i != active && i != transaction_bank) {
			target = i;
			break;
		}
	}
	if (target == 0xFFu || !flash_erase(bank_addr(m, target))) {
		return false;
	}
	uint32_t old_base = active >= 0 ? bank_addr(m, (uint8_t)active) : 0;
	uint32_t new_base = bank_addr(m, transaction_bank);
	uint32_t target_base = bank_addr(m, target);
	uint8_t pending[4];
	put32(pending, SAT_PENDING);
	if (!flash_write(target_base + 20u, pending, sizeof(pending))) {
		return false;
	}
	while (old_i < old_count || new_i < transaction_count) {
		bool take_new = new_i < transaction_count &&
			(old_i == old_count || transaction_entries[new_i].catalog_id <= active_entries[old_i].catalog_id);
		const sat_entry_t *entry = take_new ? &transaction_entries[new_i] : &active_entries[old_i];
		if (!take_new && new_i < transaction_count && transaction_entries[new_i].catalog_id == entry->catalog_id) {
			old_i++;
			continue;
		}
		if (result_count >= SAT_MAX_RECORDS || out_len > SAT_MAX_DATA - entry->length) {
			return false;
		}
		uint32_t src = take_new ? new_base : old_base;
		uint16_t src_header_size = take_new ? SAT_HEADER_SIZE : old_info->header_size;
		if (!copy_record(src, src_header_size, entry->offset, target_base, out_len, entry->length, &out_crc)) {
			return false;
		}
		out_len = (uint16_t)(out_len + entry->length);
		result_count++;
		if (take_new) {
			if (old_i < old_count && active_entries[old_i].catalog_id == entry->catalog_id) {
				old_i++;
			}
			new_i++;
		} else {
			old_i++;
		}
	}
	uint8_t h[20];
	put32(h, SAT_MAGIC);
	h[4] = SAT_VERSION;
	h[5] = result_count;
	put16(h + 6, out_len);
	put32(h + 8, active >= 0 ? old_info->generation + 1u : 1u);
	put32(h + 12, out_crc);
	put32(h + 16, 0u);
	uint8_t profile_data[SAT_PROFILE_BYTES] = { 0 };
	profile_data[0] = profile_count;
	for (uint8_t i = 0u; i < profile_count; i++) {
		uint8_t *p = profile_data + 1u + (uint16_t)i * 14u;
		put32(p, profiles[i].catalog_id);
		put32(p + 4u, profiles[i].rx_hz);
		put32(p + 8u, profiles[i].tx_hz);
		put16(p + 12u, profiles[i].tx_tone_tenths);
	}
	uint32_t profile_crc = storage_crc32(0u, profile_data, sizeof(profile_data));
	static const uint8_t committed[4] = { (uint8_t)SAT_COMMIT, (uint8_t)(SAT_COMMIT >> 8),
					      (uint8_t)(SAT_COMMIT >> 16), (uint8_t)(SAT_COMMIT >> 24) };
	uint8_t profile_crc_bytes[4];
	put32(profile_crc_bytes, profile_crc);
	if (!flash_write(target_base, h, sizeof(h)) ||
	    !flash_write(target_base + SAT_PROFILE_OFFSET, profile_data, sizeof(profile_data)) ||
	    !flash_write(target_base + SAT_PROFILE_CRC_OFFSET, profile_crc_bytes, sizeof(profile_crc_bytes)) ||
	    !flash_write(target_base + 20u, committed, sizeof(committed))) {
		return false;
	}
	return true;
}
#endif

static bool satellite_store_read(uint8_t index, satellite_record_t *record, satellite_record_summary_t *summary)
{
#if CONFIG_SATELLITE_TLE
	if (record == NULL && summary == NULL) {
		return false;
	}
	const hal_flash_map_t *m = sat_map();
	if (m == NULL) {
		return false;
	}
	storage_lock();
	int active;
	static sat_bank_t current;
	bool result = false;
	if (!active_bank(m, &active, &current) || active < 0 || index >= current.count) {
		goto done;
	}
	bool valid;
	if (!check_bank(m, (uint8_t)active, &current, active_entries, &valid) || !valid) {
		goto done;
	}
	sat_entry_t *entry = &active_entries[index];
	uint16_t length;
	if (!read_record(bank_addr(m, (uint8_t)active), current.header_size, entry->offset, current.length,
			  record_buf, &length) ||
	    !validate_record(record_buf, length, entry)) {
		goto done;
	}
	uint8_t name_length = record_buf[4];
	if (record != NULL) {
		memset(record, 0, sizeof(*record));
		record->catalog_id = entry->catalog_id;
		record->epoch = entry->epoch;
		record->name_length = name_length;
		memcpy(record->name, record_buf + 5u, name_length);
		const uint8_t *line1 = record_buf + 5u + name_length;
		memcpy(record->line1, line1, SAT_TLE_LINE);
		memcpy(record->line2, line1 + SAT_TLE_LINE, SAT_TLE_LINE);
	}
	if (summary != NULL) {
		memset(summary, 0, sizeof(*summary));
		summary->catalog_id = entry->catalog_id;
		summary->epoch = entry->epoch;
		summary->name_length = name_length;
		memcpy(summary->name, record_buf + 5u, name_length);
	}
	result = true;
done:
	storage_unlock();
	return result;
#else
	(void)index;
	(void)record;
	(void)summary;
	return false;
#endif
}

bool satellite_store_get(uint8_t index, satellite_record_t *record)
{
	return satellite_store_read(index, record, NULL);
}

bool satellite_store_summary_get(uint8_t index, satellite_record_summary_t *summary)
{
	return satellite_store_read(index, NULL, summary);
}

uint8_t satellite_store_count(void)
{
#if CONFIG_SATELLITE_TLE
	const hal_flash_map_t *m = sat_map();
	if (m == NULL)
		return 0;
	storage_lock();
	int active;
	sat_bank_t current;
	uint8_t count = active_bank(m, &active, &current) && active >= 0 ? current.count : 0u;
	storage_unlock();
	return count;
#else
	return 0;
#endif
}

static bool satellite_store_profiles_get_locked(uint8_t *count, uint32_t ids[SATELLITE_PROFILE_MAX])
{
#if CONFIG_SATELLITE_TLE
	if (count == NULL || ids == NULL)
		return false;
	const hal_flash_map_t *m = sat_map();
	int active;
	sat_bank_t current;
	if (m == NULL || !active_bank(m, &active, &current))
		return false;
	if (active < 0) {
		*count = (uint8_t)(sizeof(default_profile_ids) / sizeof(default_profile_ids[0]));
		memset(ids, 0, SATELLITE_PROFILE_MAX * sizeof(ids[0]));
		memcpy(ids, default_profile_ids, sizeof(default_profile_ids));
		return true;
	}
	*count = current.profile_count;
	memcpy(ids, current.profile_ids, sizeof(current.profile_ids));
	return true;
#else
	(void)count;
	(void)ids;
	return false;
#endif
}

static bool satellite_store_frequency_profiles_get_locked(uint8_t *count,
							   satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX])
{
#if CONFIG_SATELLITE_TLE
	if (count == NULL || profiles == NULL)
		return false;
	const hal_flash_map_t *m = sat_map();
	int active;
	static sat_bank_t current;
	if (m == NULL || !active_bank(m, &active, &current))
		return false;
	memset(profiles, 0, SATELLITE_PROFILE_MAX * sizeof(profiles[0]));
	if (active < 0) {
		*count = (uint8_t)(sizeof(default_profile_ids) / sizeof(default_profile_ids[0]));
		return default_frequency_profiles(profiles);
	}
	*count = current.profile_count;
	memcpy(profiles, current.profiles, sizeof(current.profiles));
	return true;
#else
	(void)count;
	(void)profiles;
	return false;
#endif
}

bool satellite_store_profiles_get(uint8_t *count, uint32_t ids[SATELLITE_PROFILE_MAX])
{
	storage_lock();
	bool result = satellite_store_profiles_get_locked(count, ids);
	storage_unlock();
	return result;
}

static bool satellite_store_frequency_profiles_set_locked(uint8_t count,
							   const satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX]);

static bool satellite_store_profiles_set_locked(uint8_t count, const uint32_t ids[SATELLITE_PROFILE_MAX])
{
#if CONFIG_SATELLITE_TLE
	if (!valid_profiles(count, ids) || transaction_active)
		return false;
	satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX] = { 0 };
	for (uint8_t i = 0u; i < count; i++)
		if (!catalog_profile(ids[i], &profiles[i]))
			return false;
	return satellite_store_frequency_profiles_set_locked(count, profiles);
#else
	(void)count;
	(void)ids;
	return false;
#endif
}

static bool satellite_store_frequency_profiles_set_locked(uint8_t count,
							   const satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX])
{
#if CONFIG_SATELLITE_TLE
	if (!valid_frequency_profiles(count, profiles) || transaction_active)
		return false;
	const hal_flash_map_t *m = sat_map();
	int active;
	sat_bank_t current;
	if (m == NULL || !active_bank(m, &active, &current) || active < 0)
		return false;
	transaction_bank = 0xFFu;
	for (uint8_t i = 0u; i < 3u; i++) {
		if ((int)i != active) {
			transaction_bank = i;
			break;
		}
	}
	if (transaction_bank == 0xFFu || !flash_erase(bank_addr(m, transaction_bank)))
		return false;
	uint8_t marker[4];
	put32(marker, SAT_STAGE_MAGIC);
	if (!flash_write(bank_addr(m, transaction_bank), marker, sizeof(marker)))
		return false;
	transaction_count = current.count;
	transaction_length = current.length;
	transaction_crc = 0u;
	for (uint16_t offset = 0u; offset < current.length;) {
		uint16_t remaining = (uint16_t)(current.length - offset);
		uint16_t n = remaining > sizeof(io_buf) ? (uint16_t)sizeof(io_buf) : remaining;
		if (!flash_read(bank_addr(m, (uint8_t)active) + current.header_size + offset, io_buf, n) ||
		    !flash_write(bank_addr(m, transaction_bank) + SAT_HEADER_SIZE + offset, io_buf, n))
			return false;
		transaction_crc = storage_crc32(transaction_crc, io_buf, n);
		offset = (uint16_t)(offset + n);
	}
	transaction_offset = transaction_length;
	transaction_active = true;
	bool result = commit_transaction(m, active, &current, count, profiles, false);
	transaction_active = false;
	return result;
#else
	(void)count;
	(void)profiles;
	return false;
#endif
}

bool satellite_store_profiles_set(uint8_t count, const uint32_t ids[SATELLITE_PROFILE_MAX])
{
	storage_lock();
	bool result = satellite_store_profiles_set_locked(count, ids);
	storage_unlock();
	return result;
}

bool satellite_store_frequency_profiles_get(uint8_t *count,
						    satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX])
{
	storage_lock();
	bool result = satellite_store_frequency_profiles_get_locked(count, profiles);
	storage_unlock();
	return result;
}

bool satellite_store_frequency_profiles_set(uint8_t count,
						    const satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX])
{
	storage_lock();
	bool result = satellite_store_frequency_profiles_set_locked(count, profiles);
	storage_unlock();
	return result;
}

bool satellite_store_profile_get(uint32_t catalog_id, satellite_frequency_profile_t *profile)
{
	if (profile == NULL)
		return false;
	uint8_t count;
	satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX];
	if (!satellite_store_frequency_profiles_get(&count, profiles))
		return false;
	for (uint8_t i = 0u; i < count; i++) {
		if (profiles[i].catalog_id == catalog_id) {
			*profile = profiles[i];
			return true;
		}
	}
	return false;
}

bool satellite_store_profile_selected(uint32_t catalog_id)
{
	uint8_t count;
	uint32_t ids[SATELLITE_PROFILE_MAX];
	storage_lock();
	bool result = satellite_store_profiles_get_locked(&count, ids);
	storage_unlock();
	if (!result)
		return false;
	for (uint8_t i = 0u; i < count; i++)
		if (ids[i] == catalog_id)
			return true;
	return false;
}

static bool satellite_store_replace_locked(uint8_t index, const char *line1, const char *line2)
{
#if CONFIG_SATELLITE_TLE
	if (line1 == NULL || line2 == NULL || line1[SAT_TLE_LINE] != '\0' || line2[SAT_TLE_LINE] != '\0' ||
	    transaction_active) {
		return false;
	}
	const hal_flash_map_t *m = sat_map();
	if (m == NULL)
		return false;
	int active;
	sat_bank_t current;
	if (!active_bank(m, &active, &current) || active < 0 || index >= current.count) {
		return false;
	}
	uint16_t length;
	const sat_entry_t *old = &active_entries[index];
	if (!read_record(bank_addr(m, (uint8_t)active), current.header_size, old->offset, current.length,
			  record_buf, &length))
		return false;
	uint8_t name_length = record_buf[4];
	memcpy(record_buf + 5u + name_length, line1, SAT_TLE_LINE);
	memcpy(record_buf + 5u + name_length + SAT_TLE_LINE, line2, SAT_TLE_LINE);
	sat_entry_t entry;
	if (!validate_record(record_buf, length, &entry))
		return false;
	uint8_t bank = 0xFFu;
	for (uint8_t i = 0; i < 3u; i++) {
		if ((int)i != active) {
			bank = i;
			break;
		}
	}
	if (bank == 0xFFu)
		return false;
	transaction_active = true;
	if (!flash_erase(bank_addr(m, bank))) {
		transaction_active = false;
		return false;
	}
	transaction_bank = bank;
	transaction_count = 1u;
	transaction_length = length;
	transaction_offset = length;
	transaction_crc = storage_crc32(0u, record_buf, length);
	uint8_t marker[4];
	put32(marker, SAT_STAGE_MAGIC);
	bool result = flash_write(bank_addr(m, bank), marker, sizeof(marker)) &&
		flash_write(bank_addr(m, bank) + SAT_HEADER_SIZE, record_buf, length) &&
		commit_transaction(m, active, &current, current.profile_count, current.profiles, false);
	transaction_active = false;
	return result;
#else
	(void)index;
	(void)line1;
	(void)line2;
	return false;
#endif
}

bool satellite_store_replace(uint8_t index, const char *line1, const char *line2)
{
	storage_lock();
	bool result = satellite_store_replace_locked(index, line1, line2);
	storage_unlock();
	return result;
}

bool satellite_store_list(uint16_t start, uint8_t limit, satellite_record_summary_t *records, uint8_t *count)
{
#if CONFIG_SATELLITE_TLE
	if (count == NULL || limit > SAT_MAX_RECORDS || (limit != 0u && records == NULL))
		return false;
	*count = 0u;
	const hal_flash_map_t *m = sat_map();
	if (m == NULL)
		return false;
	storage_lock();
	int active;
	sat_bank_t current;
	bool result = active_bank(m, &active, &current);
	if (result && active >= 0 && start < current.count) {
		uint16_t remaining = (uint16_t)(current.count - start);
		uint8_t number = remaining < limit ? (uint8_t)remaining : limit;
		for (uint8_t i = 0u; i < number; i++) {
			sat_entry_t *entry = &active_entries[start + i];
			uint16_t length;
			if (!read_record(bank_addr(m, (uint8_t)active), current.header_size, entry->offset,
					 current.length, record_buf, &length) ||
			    !validate_record(record_buf, length, entry)) {
				result = false;
				break;
			}
			satellite_record_summary_t *summary = &records[i];
			memset(summary, 0, sizeof(*summary));
			summary->catalog_id = entry->catalog_id;
			summary->epoch = entry->epoch;
			summary->name_length = record_buf[4];
			memcpy(summary->name, record_buf + 5u, summary->name_length);
			(*count)++;
		}
	}
	storage_unlock();
	return result;
#else
	(void)start;
	(void)limit;
	(void)records;
	(void)count;
	return false;
#endif
}

bool satellite_store_available(void)
{
#if CONFIG_SATELLITE_TLE
	return sat_map() != NULL;
#else
	return false;
#endif
}

static bool transfer_profiles_match_records(uint8_t count, const satellite_frequency_profile_t *profiles)
{
	for (uint8_t i = 0u; i < transaction_count; i++) {
		bool found = false;
		for (uint8_t j = 0u; j < count; j++)
			found = found || transaction_entries[i].catalog_id == profiles[j].catalog_id;
		if (!found)
			return false;
	}
	return true;
}

satellite_store_transfer_status_t satellite_store_transfer_begin(uint16_t count, uint16_t bytes, uint32_t crc,
								 bool replace, uint16_t *id)
{
#if CONFIG_SATELLITE_TLE
	if (id == NULL || count == 0u || count > SAT_MAX_RECORDS ||
	    bytes < count * 144u || bytes > count * SAT_MAX_RECORD_BYTES || bytes > SAT_MAX_DATA)
		return SATELLITE_STORE_TRANSFER_BAD_ARG;
	storage_lock();
	const hal_flash_map_t *m = sat_map();
	int active;
	sat_bank_t current;
	satellite_store_transfer_status_t result = SATELLITE_STORE_TRANSFER_REFUSED;
	if (transaction_active) {
		result = SATELLITE_STORE_TRANSFER_BUSY;
		goto done;
	}
	if (m == NULL || !active_bank(m, &active, &current))
		goto done;
	uint8_t bank = 0xFFu;
	if (active < 0) {
		for (uint8_t i = 0u; i < 3u; i++) {
			bool erased;
			if (bank_erased(m, i, &erased) && erased) {
				bank = i;
				break;
			}
		}
	}
	for (uint8_t i = 0u; bank == 0xFFu && i < 3u; i++) {
		if ((int)i != active)
			bank = i;
	}
	if (bank == 0xFFu || !flash_erase(bank_addr(m, bank)))
		goto done;
	transaction_id = next_transaction_id++;
	if (transaction_id == 0u)
		transaction_id = next_transaction_id++;
	transaction_count = count;
	transaction_length = bytes;
	transaction_crc = crc;
	transaction_offset = 0u;
	transaction_bank = bank;
	transaction_replace = replace;
	transaction_profiles_staged = false;
	transaction_profile_chunk_active = false;
	transaction_profile_count = 0u;
	transaction_profile_staged_count = 0u;
	uint8_t marker[4];
	put32(marker, SAT_STAGE_MAGIC);
	if (!flash_write(bank_addr(m, bank), marker, sizeof(marker)))
		goto done;
	transaction_active = true;
	*id = transaction_id;
	result = SATELLITE_STORE_TRANSFER_OK;
done:
	storage_unlock();
	return result;
#else
	(void)count;
	(void)bytes;
	(void)crc;
	(void)replace;
	(void)id;
	return SATELLITE_STORE_TRANSFER_REFUSED;
#endif
}

satellite_store_transfer_status_t satellite_store_transfer_data(uint16_t id, uint16_t offset, const uint8_t *data,
								uint16_t length, uint16_t *next_offset)
{
#if CONFIG_SATELLITE_TLE
	if (data == NULL || next_offset == NULL || length == 0u || length > 59u)
		return SATELLITE_STORE_TRANSFER_BAD_ARG;
	storage_lock();
	const hal_flash_map_t *m = sat_map();
	satellite_store_transfer_status_t result = SATELLITE_STORE_TRANSFER_REFUSED;
	if (!transaction_active || id != transaction_id || m == NULL)
		goto done;
	if (offset != transaction_offset || offset > transaction_length || length > transaction_length - offset) {
		result = SATELLITE_STORE_TRANSFER_BAD_ARG;
		goto done;
	}
	if (!flash_write(bank_addr(m, transaction_bank) + SAT_HEADER_SIZE + offset, data, length))
		goto done;
	transaction_offset = (uint16_t)(transaction_offset + length);
	*next_offset = transaction_offset;
	result = SATELLITE_STORE_TRANSFER_OK;
done:
	storage_unlock();
	return result;
#else
	(void)id;
	(void)offset;
	(void)data;
	(void)length;
	(void)next_offset;
	return SATELLITE_STORE_TRANSFER_REFUSED;
#endif
}

satellite_store_transfer_status_t satellite_store_transfer_profiles(
	uint16_t id, uint8_t count, const satellite_frequency_profile_t *profiles)
{
#if CONFIG_SATELLITE_TLE
	if (count > SATELLITE_PROFILE_MAX || (count != 0u && profiles == NULL))
		return SATELLITE_STORE_TRANSFER_BAD_ARG;
	storage_lock();
	const hal_flash_map_t *m = sat_map();
	satellite_store_transfer_status_t result = SATELLITE_STORE_TRANSFER_REFUSED;
	if (!transaction_active || !transaction_replace || transaction_profile_chunk_active ||
	    transaction_profiles_staged || id != transaction_id || m == NULL || !stage_entries(m))
		goto done;
	if (count != transaction_count || !valid_frequency_profiles(count, profiles) ||
	    !transfer_profiles_match_records(count, profiles)) {
		result = SATELLITE_STORE_TRANSFER_BAD_ARG;
		goto done;
	}
	memset(transaction_profiles, 0, sizeof(transaction_profiles));
	memcpy(transaction_profiles, profiles, (size_t)count * sizeof(profiles[0]));
	transaction_profile_count = count;
	transaction_profiles_staged = true;
	result = SATELLITE_STORE_TRANSFER_OK;
done:
	storage_unlock();
	return result;
#else
	(void)id;
	(void)count;
	(void)profiles;
	return SATELLITE_STORE_TRANSFER_REFUSED;
#endif
}

satellite_store_transfer_status_t satellite_store_transfer_profiles_chunk(
	uint16_t id, uint8_t total, uint8_t start, uint8_t count, const satellite_frequency_profile_t *profiles)
{
#if CONFIG_SATELLITE_TLE
	if (profiles == NULL || total == 0u || total > SATELLITE_PROFILE_MAX || count == 0u ||
	    start > total || count > total - start)
		return SATELLITE_STORE_TRANSFER_BAD_ARG;
	storage_lock();
	const hal_flash_map_t *m = sat_map();
	if (!transaction_active || !transaction_replace || transaction_profiles_staged || id != transaction_id ||
	    m == NULL || !stage_entries(m)) {
		storage_unlock();
		return SATELLITE_STORE_TRANSFER_REFUSED;
	}
	if (total != transaction_count || (!transaction_profile_chunk_active && start != 0u) ||
	    (transaction_profile_chunk_active &&
	     (total != transaction_profile_count || start != transaction_profile_staged_count))) {
		storage_unlock();
		return SATELLITE_STORE_TRANSFER_BAD_ARG;
	}
	if (!transaction_profile_chunk_active) {
		transaction_profile_chunk_active = true;
		transaction_profile_count = total;
		transaction_profile_staged_count = 0u;
	}
	memcpy(transaction_profiles + start, profiles, (size_t)count * sizeof(profiles[0]));
	transaction_profile_staged_count = (uint8_t)(transaction_profile_staged_count + count);
	satellite_store_transfer_status_t result = SATELLITE_STORE_TRANSFER_OK;
	if (transaction_profile_staged_count == transaction_profile_count) {
		if (!valid_frequency_profiles(transaction_profile_count, transaction_profiles) ||
		    !transfer_profiles_match_records(transaction_profile_count, transaction_profiles))
			result = SATELLITE_STORE_TRANSFER_BAD_ARG;
		else
			transaction_profiles_staged = true;
	}
	storage_unlock();
	return result;
#else
	(void)id;
	(void)total;
	(void)start;
	(void)count;
	(void)profiles;
	return SATELLITE_STORE_TRANSFER_REFUSED;
#endif
}

satellite_store_transfer_status_t satellite_store_transfer_commit(uint16_t id, uint32_t crc)
{
#if CONFIG_SATELLITE_TLE
	storage_lock();
	const hal_flash_map_t *m = sat_map();
	int active;
	sat_bank_t current;
	satellite_store_transfer_status_t result = SATELLITE_STORE_TRANSFER_REFUSED;
	if (!transaction_active || id != transaction_id || m == NULL ||
	    (transaction_profile_chunk_active && !transaction_profiles_staged) ||
	    !active_bank(m, &active, &current))
		goto done;
	uint32_t actual_crc = 0u;
	for (uint16_t offset = 0u; offset < transaction_length;) {
		uint16_t remaining = (uint16_t)(transaction_length - offset);
		uint16_t n = remaining > sizeof(io_buf) ? (uint16_t)sizeof(io_buf) : remaining;
		if (!flash_read(bank_addr(m, transaction_bank) + SAT_HEADER_SIZE + offset, io_buf, n))
			goto done;
		actual_crc = storage_crc32(actual_crc, io_buf, n);
		offset = (uint16_t)(offset + n);
	}
	if (transaction_offset != transaction_length || actual_crc != transaction_crc || crc != transaction_crc ||
	    !stage_entries(m))
		goto done;
	uint8_t old_count = active >= 0 ? current.count : 0u;
	uint8_t merged = transaction_replace ? (uint8_t)transaction_count : old_count;
	if (!transaction_replace) {
		for (uint8_t i = 0u; i < transaction_count; i++) {
			bool found = false;
			for (uint8_t j = 0u; j < old_count; j++)
				found = found || transaction_entries[i].catalog_id == active_entries[j].catalog_id;
			if (!found)
				merged++;
		}
	}
	satellite_frequency_profile_t defaults[SATELLITE_PROFILE_MAX] = { 0 };
	if (active < 0 && !default_frequency_profiles(defaults))
		goto done;
	uint8_t profile_count = transaction_profiles_staged ? transaction_profile_count : active >= 0 ?
		current.profile_count : (uint8_t)(sizeof(default_profile_ids) / sizeof(default_profile_ids[0]));
	const satellite_frequency_profile_t *profiles = transaction_profiles_staged ? transaction_profiles :
		active >= 0 ? current.profiles : defaults;
	if (merged > SAT_MAX_RECORDS ||
	    !commit_transaction(m, active, &current, profile_count, profiles, transaction_replace))
		goto done;
	transaction_active = false;
	transaction_replace = false;
	transaction_profiles_staged = false;
	transaction_profile_chunk_active = false;
	result = SATELLITE_STORE_TRANSFER_OK;
done:
	storage_unlock();
	return result;
#else
	(void)id;
	(void)crc;
	return SATELLITE_STORE_TRANSFER_REFUSED;
#endif
}

satellite_store_transfer_status_t satellite_store_transfer_abort(uint16_t id)
{
#if CONFIG_SATELLITE_TLE
	storage_lock();
	satellite_store_transfer_status_t result = SATELLITE_STORE_TRANSFER_REFUSED;
	if (transaction_active && id == transaction_id) {
		transaction_active = false;
		transaction_replace = false;
		transaction_profiles_staged = false;
		transaction_profile_chunk_active = false;
		result = SATELLITE_STORE_TRANSFER_OK;
	}
	storage_unlock();
	return result;
#else
	(void)id;
	return SATELLITE_STORE_TRANSFER_REFUSED;
#endif
}

