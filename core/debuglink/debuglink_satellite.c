// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Selected satellite TLE inventory on the debug link (docs/spec/satellite.md 6). Three erase sectors keep the active
// inventory intact while a transfer is validated and the next complete generation is written.
#include <string.h>
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "satellite/channels.h"
#include "satellite/store.h"
#include "storage/storage.h"

#if CONFIG_SATELLITE_TLE
#define SAT_MAX_RECORDS		SATELLITE_STORE_MAX_RECORDS
#define SAT_MAX_NAME		48u
#define SAT_TLE_LINE		69u
#define SAT_MAX_RECORD_BYTES	(4u + 1u + SAT_MAX_NAME + 2u * SAT_TLE_LINE)
#define SAT_MAX_DATA		(SAT_MAX_RECORDS * SAT_MAX_RECORD_BYTES)
#define SAT_HEADER_SIZE		24u
#define SAT_MAGIC		0x31544653u
#define SAT_COMMIT		0x54494D43u
#define SAT_PENDING		(SAT_COMMIT | 0x80000000u)
#define SAT_STAGE_MAGIC		0x31475453u
#define SAT_VERSION		2u
#define SAT_VERSION_LEGACY	1u
#define SAT_PROTOCOL_VERSION	2u

#include <builtin_satellite_records.inc>

typedef struct {
	uint32_t catalog_id;
	uint32_t epoch;
	uint16_t offset;
	uint16_t length;
} sat_entry_t;

typedef struct {
	uint32_t generation;
	uint32_t crc;
	uint32_t defaults_hash;
	uint16_t length;
	uint8_t header_size;
	uint8_t count;
} sat_bank_t;

static bool transaction_active;
static uint16_t transaction_id;
static uint16_t next_transaction_id = 1u;
static uint16_t transaction_count;
static uint16_t transaction_length;
static uint16_t transaction_offset;
static uint32_t transaction_crc;
static uint8_t transaction_bank;
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

static bool tle_checksum(const uint8_t *line)
{
	if (line[68] < '0' || line[68] > '9') {
		return false;
	}
	unsigned sum = 0;
	for (uint8_t i = 0; i < 68u; i++) {
		if (line[i] >= '0' && line[i] <= '9') {
			sum += line[i] - '0';
		} else if (line[i] == '-') {
			sum++;
		}
	}
	return sum % 10u == (unsigned)(line[68] - '0');
}

static bool leap_year(uint32_t year)
{
	return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
}

static bool tle_epoch(const uint8_t *line, uint32_t *epoch)
{
	for (uint8_t i = 18u; i <= 22u; i++) {
		if (line[i] < '0' || line[i] > '9') {
			return false;
		}
	}
	if (line[23] != '.') {
		return false;
	}
	uint32_t fraction = 0;
	for (uint8_t i = 24u; i <= 31u; i++) {
		if (line[i] < '0' || line[i] > '9') {
			return false;
		}
		fraction = fraction * 10u + (uint32_t)(line[i] - '0');
	}
	uint32_t short_year = (uint32_t)(line[18] - '0') * 10u + (uint32_t)(line[19] - '0');
	uint32_t year = short_year >= 57u ? 1900u + short_year : 2000u + short_year;
	uint32_t day = (uint32_t)(line[20] - '0') * 100u + (uint32_t)(line[21] - '0') * 10u +
		       (uint32_t)(line[22] - '0');
	uint32_t max_day = leap_year(year) ? 366u : 365u;
	if (day == 0u || day > max_day) {
		return false;
	}
	int64_t days = 0;
	for (uint32_t y = 1970u; y < year; y++) {
		days += leap_year(y) ? 366u : 365u;
	}
	if (year < 1970u) {
		for (uint32_t y = year; y < 1970u; y++) {
			days -= leap_year(y) ? 366u : 365u;
		}
	}
	int64_t seconds = (days + (int64_t)day - 1) * 86400 + (int64_t)((uint64_t)fraction * 86400u / 100000000u);
	if (seconds < 0 || (uint64_t)seconds > 0xFFFFFFFFu) {
		return false;
	}
	*epoch = (uint32_t)seconds;
	return true;
}

static bool digits(const uint8_t *p, uint8_t n)
{
	for (uint8_t i = 0; i < n; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return false;
		}
	}
	return true;
}

static bool angle_field(const uint8_t *p, uint32_t maximum, bool inclusive)
{
	uint8_t i = 0, whole_digits = 0;
	uint32_t whole = 0, fraction = 0;
	while (i < 3u && p[i] == ' ') i++;
	while (i < 3u && p[i] >= '0' && p[i] <= '9') {
		whole = whole * 10u + (uint32_t)(p[i++] - '0');
		whole_digits++;
	}
	if (whole_digits == 0u || i > 3u || p[i++] != '.') {
		return false;
	}
	for (; i < 8u; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return false;
		}
		fraction = fraction * 10u + (uint32_t)(p[i] - '0');
	}
	return whole < maximum || (inclusive && whole == maximum && fraction == 0u);
}

static bool mean_motion_field(const uint8_t *p)
{
	uint32_t whole;
	if (p[0] == ' ' && p[1] >= '0' && p[1] <= '9') {
		whole = (uint32_t)(p[1] - '0');
	} else if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
		whole = (uint32_t)(p[0] - '0') * 10u + (uint32_t)(p[1] - '0');
	} else {
		return false;
	}
	if (p[2] != '.' || !digits(p + 3, 8u)) {
		return false;
	}
	for (uint8_t i = 3u; i < 11u; i++) {
		if (p[i] != '0') return true;
	}
	return whole != 0u;
}

static bool implied_exponent(const uint8_t *p)
{
	return (p[0] == ' ' || p[0] == '+' || p[0] == '-') && digits(p + 1, 5u) &&
	       (p[6] == '+' || p[6] == '-') && digits(p + 7, 1u);
}

static bool valid_fixed_fields(const uint8_t *line1, const uint8_t *line2)
{
	if ((line1[7] != 'U' && line1[7] != 'C' && line1[7] != 'S') || line1[8] != ' ' || line1[17] != ' ' ||
	    line1[32] != ' ' || line1[43] != ' ' || line1[52] != ' ' || line1[61] != ' ' || line1[63] != ' ' ||
	    (line1[33] != ' ' && line1[33] != '+' && line1[33] != '-') || line1[34] != '.' ||
	    !digits(line1 + 35, 8u) || !implied_exponent(line1 + 44) || !implied_exponent(line1 + 53) ||
	    !digits(line1 + 62, 1u)) {
		return false;
	}
	for (uint8_t i = 9u; i < 17u; i++) {
		uint8_t c = line1[i];
		if (c != ' ' && !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z')) {
			return false;
		}
	}
	uint8_t set_digits = 0;
	bool set_started = false;
	for (uint8_t i = 64u; i < 68u; i++) {
		if (line1[i] >= '0' && line1[i] <= '9') {
			set_started = true;
			set_digits++;
		} else if (line1[i] != ' ' || set_started) {
			return false;
		}
	}
	return set_digits != 0u && line2[7] == ' ' && line2[16] == ' ' && line2[25] == ' ' && line2[33] == ' ' &&
	       line2[42] == ' ' && line2[51] == ' ' && digits(line2 + 26, 7u) && digits(line2 + 63, 5u) &&
	       angle_field(line2 + 8, 180u, true) && angle_field(line2 + 17, 360u, false) &&
	       angle_field(line2 + 34, 360u, false) && angle_field(line2 + 43, 360u, false) &&
	       mean_motion_field(line2 + 52);
}

static bool validate_record(const uint8_t *p, uint16_t len, sat_entry_t *entry)
{
	if (len < 4u + 1u + 1u + 2u * SAT_TLE_LINE) {
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
	if (line1[0] != '1' || line1[1] != ' ' || line2[0] != '2' || line2[1] != ' ' ||
	    !valid_fixed_fields(line1, line2)) {
		return false;
	}
	uint32_t line_id1 = 0, line_id2 = 0;
	for (uint8_t i = 2u; i < 7u; i++) {
		if (line1[i] < '0' || line1[i] > '9' || line2[i] < '0' || line2[i] > '9') {
			return false;
		}
		line_id1 = line_id1 * 10u + (uint32_t)(line1[i] - '0');
		line_id2 = line_id2 * 10u + (uint32_t)(line2[i] - '0');
	}
	uint32_t epoch;
	if (line_id1 != id || line_id2 != id || !tle_checksum(line1) || !tle_checksum(line2) || !tle_epoch(line1, &epoch)) {
		return false;
	}
	for (uint8_t i = 0; i < 2u * SAT_TLE_LINE; i++) {
		if (line1[i] < 0x20u || line1[i] > 0x7Eu) {
			return false;
		}
	}
	entry->catalog_id = id;
	entry->epoch = epoch;
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
			      h[4] == SAT_VERSION_LEGACY ? 20u : 0u;
	uint8_t commit_offset = h[4] == SAT_VERSION ? 20u : 16u;
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
	info->defaults_hash = h[4] == SAT_VERSION ? get32(h + 16) : 0u;
	info->length = length;
	info->header_size = header_size;
	info->count = count;
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

static bool marker_prefix(const hal_flash_map_t *m, uint8_t bank, uint32_t offset, uint32_t marker)
{
	uint8_t bytes[4];
	uint8_t expected[4];
	put32(expected, marker);
	if (!flash_read(bank_addr(m, bank) + offset, bytes, sizeof(bytes))) {
		return false;
	}
	for (uint8_t prefix = 1u; prefix <= sizeof(bytes); prefix++) {
		bool matches = true;
		for (uint8_t i = 0; i < prefix; i++) {
			matches = matches && bytes[i] == expected[i];
		}
		for (uint8_t i = prefix; i < sizeof(bytes); i++) {
			matches = matches && bytes[i] == 0xFFu;
		}
		if (matches) {
			return true;
		}
	}
	return false;
}

static bool bank_recoverable(const hal_flash_map_t *m, uint8_t bank)
{
	bool erased;
	return (bank_erased(m, bank, &erased) && erased) ||
	       marker_prefix(m, bank, 0u, SAT_STAGE_MAGIC) ||
	       marker_prefix(m, bank, 20u, SAT_PENDING) ||
	       marker_prefix(m, bank, 16u, SAT_PENDING);
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
	if (!found) {
		for (uint8_t i = 0; i < 3u; i++) {
			if (!bank_recoverable(m, i)) {
				return false;
			}
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
			       uint32_t defaults_hash)
{
	if (!stage_entries(m)) {
		return false;
	}
	uint8_t old_count = active >= 0 ? old_info->count : 0u;
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
	put32(h + 16, defaults_hash);
	static const uint8_t committed[4] = { (uint8_t)SAT_COMMIT, (uint8_t)(SAT_COMMIT >> 8),
					      (uint8_t)(SAT_COMMIT >> 16), (uint8_t)(SAT_COMMIT >> 24) };
	if (!flash_write(target_base, h, sizeof(h)) || !flash_write(target_base + 20u, committed, sizeof(committed))) {
		return false;
	}
	return true;
}
#endif

bool satellite_store_get(uint8_t index, satellite_record_t *record)
{
#if CONFIG_SATELLITE_TLE
	if (record == NULL) {
		return false;
	}
	const hal_flash_map_t *m = sat_map();
	if (m == NULL) {
		return false;
	}
	storage_lock();
	int active;
	sat_bank_t current;
	bool result = false;
	if (!active_bank(m, &active, &current) || active < 0 || index >= current.count) {
		goto done;
	}
	bool valid;
	if (!check_bank(m, (uint8_t)active, &current, active_entries, &valid) || !valid) {
		goto done;
	}
	sat_entry_t entry = active_entries[index];
	uint16_t length;
	if (!read_record(bank_addr(m, (uint8_t)active), current.header_size, entry.offset, current.length,
			  record_buf, &length) ||
	    !validate_record(record_buf, length, &entry)) {
		goto done;
	}
	satellite_record_t copy = { 0 };
	copy.catalog_id = entry.catalog_id;
	copy.epoch = entry.epoch;
	copy.name_length = record_buf[4];
	memcpy(copy.name, record_buf + 5, copy.name_length);
	const uint8_t *line1 = record_buf + 5u + copy.name_length;
	memcpy(copy.line1, line1, SAT_TLE_LINE);
	memcpy(copy.line2, line1 + SAT_TLE_LINE, SAT_TLE_LINE);
	*record = copy;
	result = true;
done:
	storage_unlock();
	return result;
#else
	(void)index;
	(void)record;
	return false;
#endif
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

bool satellite_store_seed_defaults(void)
{
#if CONFIG_SATELLITE_TLE
	bool result = false;
	storage_lock();
	const hal_flash_map_t *m = sat_map();
	int active;
	sat_bank_t current;
	if (m == NULL || transaction_active || !active_bank(m, &active, &current))
		goto done;
	if (active >= 0 && current.defaults_hash == SATELLITE_BUILTIN_BUILD_HASH) {
		result = true;
		goto done;
	}
	uint16_t built_in_count = sizeof(built_in_records) / sizeof(built_in_records[0]);
	uint16_t missing_count = 0u;
	for (uint16_t i = 0u; i < built_in_count; i++) {
		bool found = false;
		for (uint8_t j = 0u; active >= 0 && j < current.count; j++)
			found = found || active_entries[j].catalog_id == built_in_records[i].catalog_id;
		missing_count += !found;
	}
	if (active >= 0 && (uint16_t)current.count + missing_count > SAT_MAX_RECORDS)
		goto done;
	if (active < 0) {
		for (uint8_t i = 0u; i < 3u; i++) {
			bool erased;
			if (!bank_erased(m, i, &erased) || !erased)
				goto done;
		}
	}
	transaction_bank = 0xFFu;
	for (uint8_t i = 0u; i < 3u; i++) {
		if ((int)i != active) {
			transaction_bank = i;
			break;
		}
	}
	if (transaction_bank == 0xFFu || !flash_erase(bank_addr(m, transaction_bank)))
		goto done;
	transaction_count = built_in_count;
	transaction_length = 0u;
	for (uint16_t i = 0u; i < built_in_count; i++)
		transaction_length = (uint16_t)(transaction_length + 5u + built_in_records[i].name_length +
						2u * SAT_TLE_LINE);
	transaction_offset = transaction_length;
	transaction_crc = 0u;
	transaction_active = true;
	uint8_t marker[4];
	put32(marker, SAT_STAGE_MAGIC);
	if (!flash_write(bank_addr(m, transaction_bank), marker, sizeof(marker)))
		goto finish;
	uint16_t offset = 0u;
	for (uint16_t i = 0u; i < built_in_count; i++) {
		const satellite_record_t *record = &built_in_records[i];
		uint16_t length = (uint16_t)(5u + record->name_length + 2u * SAT_TLE_LINE);
		put32(record_buf, record->catalog_id);
		record_buf[4] = record->name_length;
		memcpy(record_buf + 5u, record->name, record->name_length);
		memcpy(record_buf + 5u + record->name_length, record->line1, SAT_TLE_LINE);
		memcpy(record_buf + 5u + record->name_length + SAT_TLE_LINE, record->line2, SAT_TLE_LINE);
		sat_entry_t entry;
		if (!validate_record(record_buf, length, &entry) ||
		    !flash_write(bank_addr(m, transaction_bank) + SAT_HEADER_SIZE + offset, record_buf, length))
			goto finish;
		transaction_crc = storage_crc32(transaction_crc, record_buf, length);
		offset = (uint16_t)(offset + length);
	}
	result = offset == transaction_length &&
		commit_transaction(m, active, &current, SATELLITE_BUILTIN_BUILD_HASH);
finish:
	transaction_active = false;
done:
	storage_unlock();
	return result;
#else
	return false;
#endif
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
		commit_transaction(m, active, &current, current.defaults_hash);
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

static size_t status(uint8_t *out, uint8_t value)
{
	out[0] = value;
	return 1u;
}

static size_t debuglink_cmd_satellite_locked(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
#if CONFIG_SATELLITE_TLE
	const hal_flash_map_t *m = sat_map();
	if (cap == 0u) {
		return 0u;
	}
	if (len == 0u) return status(out, DL_ST_BAD_ARG);
	if (m == NULL) {
		return status(out, DL_ST_UNAVAILABLE);
	}
	int active;
	sat_bank_t current;
	if (!active_bank(m, &active, &current)) {
		return status(out, DL_ST_REFUSED);
	}
	switch (req[0]) {
	case DL_SAT_OP_INFO:
		if (len != 1u || cap < 6u) {
			return status(out, DL_ST_BAD_ARG);
		}
		out[0] = DL_ST_OK;
		out[1] = SAT_PROTOCOL_VERSION;
		put16(out + 2, SAT_MAX_RECORDS);
		put16(out + 4, active >= 0 ? current.count : 0u);
		return 6u;
	case DL_SAT_OP_LIST: {
		if (len != 3u || cap < 2u) {
			return status(out, DL_ST_BAD_ARG);
		}
		uint16_t start = get16(req + 1);
		uint8_t count = start < (active >= 0 ? current.count : 0u) ?
			(uint8_t)((active >= 0 ? current.count : 0u) - start) : 0u;
		if (count > SAT_MAX_RECORDS) count = SAT_MAX_RECORDS;
		bool valid;
		if (cap < 2u + (size_t)count * 8u ||
		    (active >= 0 && (!check_bank(m, (uint8_t)active, &current, active_entries, &valid) || !valid))) {
			return status(out, DL_ST_REFUSED);
		}
		out[0] = DL_ST_OK;
		out[1] = count;
		for (uint8_t i = 0; i < count; i++) {
			put32(out + 2u + (size_t)i * 8u, active_entries[start + i].catalog_id);
			put32(out + 6u + (size_t)i * 8u, active_entries[start + i].epoch);
		}
		return 2u + (size_t)count * 8u;
	}
	case DL_SAT_OP_BEGIN: {
		uint16_t count = len == 9u ? get16(req + 1) : 0u;
		uint16_t bytes = len == 9u ? get16(req + 3) : 0u;
		if (len != 9u || count == 0u || count > SAT_MAX_RECORDS || bytes < count * 144u ||
		    bytes > count * SAT_MAX_RECORD_BYTES || bytes > SAT_MAX_DATA || cap < 3u) {
			return status(out, DL_ST_BAD_ARG);
		}
		if (transaction_active) {
			return status(out, DL_ST_BUSY);
		}
		uint8_t bank = 0xFFu;
		if (active < 0) {
			for (uint8_t i = 0; i < 3u; i++) {
				bool erased;
				if (bank_erased(m, i, &erased) && erased) {
					bank = i;
					break;
				}
			}
		}
		for (uint8_t i = 0; bank == 0xFFu && i < 3u; i++) {
			if ((int)i != active) {
				bank = i;
			}
		}
		if (bank == 0xFFu || !flash_erase(bank_addr(m, bank))) {
			return status(out, DL_ST_REFUSED);
		}
		transaction_id = next_transaction_id++;
		if (transaction_id == 0u) transaction_id = next_transaction_id++;
		transaction_count = count;
		transaction_length = bytes;
		transaction_crc = get32(req + 5);
		transaction_offset = 0;
		transaction_bank = bank;
		uint8_t marker[4];
		put32(marker, SAT_STAGE_MAGIC);
		if (!flash_write(bank_addr(m, bank), marker, sizeof(marker))) {
			return status(out, DL_ST_REFUSED);
		}
		transaction_active = true;
		out[0] = DL_ST_OK;
		put16(out + 1, transaction_id);
		return 3u;
	}
	case DL_SAT_OP_DATA: {
		if (len < 6u || len > 64u || cap < 3u) return status(out, DL_ST_BAD_ARG);
		uint16_t id = get16(req + 1), offset = get16(req + 3);
		uint16_t n = (uint16_t)(len - 5u);
		if (!transaction_active || id != transaction_id) return status(out, DL_ST_REFUSED);
		if (n > 59u || n == 0u || offset != transaction_offset || n > transaction_length - transaction_offset) {
			return status(out, DL_ST_BAD_ARG);
		}
		if (!flash_write(bank_addr(m, transaction_bank) + SAT_HEADER_SIZE + offset, req + 5, n)) {
			return status(out, DL_ST_REFUSED);
		}
		transaction_offset = (uint16_t)(transaction_offset + n);
		out[0] = DL_ST_OK;
		put16(out + 1, transaction_offset);
		return 3u;
	}
	case DL_SAT_OP_COMMIT: {
		if (len != 7u) return status(out, DL_ST_BAD_ARG);
		if (!transaction_active || get16(req + 1) != transaction_id) return status(out, DL_ST_REFUSED);
		uint32_t crc = 0;
		for (uint16_t off = 0; off < transaction_length;) {
			uint16_t remaining = (uint16_t)(transaction_length - off);
			uint16_t n = remaining > sizeof(io_buf) ? (uint16_t)sizeof(io_buf) : remaining;
			if (!flash_read(bank_addr(m, transaction_bank) + SAT_HEADER_SIZE + off, io_buf, n)) {
				return status(out, DL_ST_REFUSED);
			}
			crc = storage_crc32(crc, io_buf, n);
			off = (uint16_t)(off + n);
		}
		if (transaction_offset != transaction_length || crc != transaction_crc || get32(req + 3) != transaction_crc ||
		    !stage_entries(m)) {
			return status(out, DL_ST_REFUSED);
		}
		uint8_t old_count = active >= 0 ? current.count : 0u;
		uint8_t merged = old_count;
		for (uint8_t i = 0; i < transaction_count; i++) {
			bool found = false;
			for (uint8_t j = 0; j < old_count; j++) found = found || transaction_entries[i].catalog_id == active_entries[j].catalog_id;
			if (!found) merged++;
		}
	uint32_t defaults_hash = active >= 0 ? current.defaults_hash : 0u;
		if (merged > SAT_MAX_RECORDS || !commit_transaction(m, active, &current, defaults_hash)) {
			return status(out, DL_ST_REFUSED);
		}
		transaction_active = false;
		return status(out, DL_ST_OK);
	}
	case DL_SAT_OP_ABORT:
		if (len != 3u) return status(out, DL_ST_BAD_ARG);
		if (!transaction_active || get16(req + 1) != transaction_id) return status(out, DL_ST_REFUSED);
		transaction_active = false;
		return status(out, DL_ST_OK);
	case DL_SAT_OP_SYNC_CHANNELS:
		if (len != 1u) return status(out, DL_ST_BAD_ARG);
		return status(out, satellite_channels_sync() ? DL_ST_OK : DL_ST_REFUSED);
	default:
		return status(out, DL_ST_BAD_ARG);
	}
#else
	(void)req;
	(void)len;
	(void)cap;
	return status(out, DL_ST_UNAVAILABLE);
#endif
}

size_t debuglink_cmd_satellite(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	storage_lock();
	size_t result = debuglink_cmd_satellite_locked(req, len, out, cap);
	storage_unlock();
	return result;
}