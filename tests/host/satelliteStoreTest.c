// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug-link satellite TLE store on simulated NOR: bounded capacity, merged commits, input validation and aborts.
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "satellite/channels.h"
#include "satellite/frequency.h"
#include "satellite/store.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

void storage_lock(void) { }
void storage_unlock(void) { }
void storage_quick_lock(void) { }
void storage_quick_unlock(void) { }

#define FLASH_SIZE 0x13000u
#define TEST_RECORD_MAX (4u + 1u + 48u + 2u * 69u)
static band_t test_band = { .lo_hz = 10000000u, .hi_hz = 999999999u, .tx = true };
static const hal_flash_map_t map = {
	.settings_addr = 0x01000u, .settings_size = 0x02000u, .journal_addr = 0x03000u,
	.openfw_eeprom = 0, .openfw_flash = 0x08000u, .openfw_end = 0x10000u,
	.satellite_addr = 0x10000u, .satellite_size = 0x03000u,
};
const board_t board = { .name = "satellite-test", .bands = &test_band, .band_count = 1u, .flash = &sim_flash };

static uint8_t output[256];
static bool channel_sync_result = true;
static satellite_channels_sync_status_t channel_sync_status;

bool satellite_channels_sync(void)
{
	return channel_sync_result;
}

bool satellite_channels_sync_schedule(void)
{
	if (channel_sync_status == SATELLITE_CHANNELS_SYNC_RUNNING)
		return false;
	channel_sync_status = SATELLITE_CHANNELS_SYNC_RUNNING;
	return true;
}

satellite_channels_sync_status_t satellite_channels_sync_status(void)
{
	return channel_sync_status;
}

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

static size_t command(const uint8_t *req, uint16_t len)
{
	memset(output, 0xEE, sizeof(output));
	return debuglink_cmd_satellite(req, len, output, sizeof(output));
}

static void make_line(uint8_t *line, char number, uint32_t id, const char *epoch)
{
	memset(line, ' ', 69);
	line[0] = (uint8_t)number;
	line[1] = ' ';
	line[2] = (uint8_t)('0' + id / 10000u);
	line[3] = (uint8_t)('0' + id / 1000u % 10u);
	line[4] = (uint8_t)('0' + id / 100u % 10u);
	line[5] = (uint8_t)('0' + id / 10u % 10u);
	line[6] = (uint8_t)('0' + id % 10u);
	if (number == '1') {
		line[7] = 'U';
		memcpy(line + 9, "98067A  ", 8u);
		line[17] = ' ';
		memcpy(line + 18, epoch, 14);
		line[32] = ' ';
		memcpy(line + 33, " .00016717", 10u);
		line[43] = ' ';
		memcpy(line + 44, " 00000-0", 8u);
		line[52] = ' ';
		memcpy(line + 53, " 00000-0", 8u);
		line[61] = ' ';
		line[62] = '0';
		line[63] = ' ';
		memcpy(line + 64, "0001", 4u);
	} else {
		memcpy(line + 8, " 51.6400", 8u);
		memcpy(line + 17, " 15.0000", 8u);
		memcpy(line + 26, "0005000", 7u);
		memcpy(line + 34, " 20.0000", 8u);
		memcpy(line + 43, " 30.0000", 8u);
		memcpy(line + 52, "15.50000000", 11u);
		memcpy(line + 63, "00001", 5u);
	}
	unsigned sum = 0;
	for (uint8_t i = 0; i < 68u; i++) {
		if (line[i] >= '0' && line[i] <= '9') sum += line[i] - '0';
		else if (line[i] == '-') sum++;
	}
	line[68] = (uint8_t)('0' + sum % 10u);
}

static void set_revolution_field(uint8_t *line, const char field[5])
{
	memcpy(line + 63u, field, 5u);
	unsigned sum = 0u;
	for (uint8_t i = 0u; i < 68u; i++) {
		if (line[i] >= '0' && line[i] <= '9') sum += line[i] - '0';
		else if (line[i] == '-') sum++;
	}
	line[68] = (uint8_t)('0' + sum % 10u);
}

static uint16_t make_record(uint8_t *out, uint32_t id, const char *name, const char *epoch)
{
	uint8_t name_len = (uint8_t)strlen(name);
	put32(out, id);
	out[4] = name_len;
	memcpy(out + 5, name, name_len);
	uint8_t *line1 = out + 5 + name_len;
	uint8_t *line2 = line1 + 69;
	make_line(line1, '1', id, epoch);
	make_line(line2, '2', id, "00000.00000000");
	return (uint16_t)(5u + name_len + 138u);
}

static bool install_operation(const uint8_t *data, uint16_t data_len, uint16_t count, bool bad_crc, bool bad_tle,
			     bool bad_orbit, uint8_t begin_operation)
{
	uint8_t transfer_data[10 * TEST_RECORD_MAX];
	memcpy(transfer_data, data, data_len);
	if (bad_tle) transfer_data[5u + transfer_data[4] + 68u] ^= 1u;
	if (bad_orbit) {
		uint8_t *line2 = transfer_data + 5u + transfer_data[4] + 69u;
		memcpy(line2 + 52u, "00.00000000", 11u);
		unsigned sum = 0;
		for (uint8_t i = 0; i < 68u; i++) {
			if (line2[i] >= '0' && line2[i] <= '9') sum += line2[i] - '0';
			else if (line2[i] == '-') sum++;
		}
		line2[68] = (uint8_t)('0' + sum % 10u);
	}
	uint32_t crc = storage_crc32(0, transfer_data, data_len);
	uint8_t req[64];
	req[0] = begin_operation;
	put16(req + 1, count);
	put16(req + 3, data_len);
	put32(req + 5, crc);
	CHECK(command(req, 9) == 3 && output[0] == DL_ST_OK);
	if (output[0] != DL_ST_OK) return false;
	uint16_t transaction = get16(output + 1);
	for (uint16_t off = 0; off < data_len;) {
		uint16_t remaining = (uint16_t)(data_len - off);
		uint16_t n = remaining > 59u ? 59u : remaining;
		req[0] = DL_SAT_OP_DATA;
		put16(req + 1, transaction);
		put16(req + 3, off);
		memcpy(req + 5, transfer_data + off, n);
		CHECK(command(req, (uint16_t)(5u + n)) == 3 && output[0] == DL_ST_OK && get16(output + 1) == off + n);
		off = (uint16_t)(off + n);
	}
	req[0] = DL_SAT_OP_COMMIT;
	put16(req + 1, transaction);
	put32(req + 3, crc ^ (bad_crc ? 1u : 0u));
	command(req, 7);
	if (bad_crc || bad_tle || bad_orbit) {
		CHECK(output[0] == DL_ST_REFUSED);
		req[0] = DL_SAT_OP_ABORT;
		put16(req + 1, transaction);
		CHECK(command(req, 3) == 1 && output[0] == DL_ST_OK);
		return false;
	}
	CHECK(output[0] == DL_ST_OK);
	return output[0] == DL_ST_OK;
}

static bool install(const uint8_t *data, uint16_t data_len, uint16_t count, bool bad_crc, bool bad_tle,
		    bool bad_orbit)
{
	return install_operation(data, data_len, count, bad_crc, bad_tle, bad_orbit, DL_SAT_OP_BEGIN);
}

static bool install_replace(const uint8_t *data, uint16_t data_len, uint16_t count)
{
	return install_operation(data, data_len, count, false, false, false, DL_SAT_OP_REPLACE_BEGIN);
}

static bool install_replace_profile(const uint8_t *data, uint16_t data_len, satellite_frequency_profile_t profile)
{
	uint8_t req[64];
	uint32_t crc = storage_crc32(0u, data, data_len);
	req[0] = DL_SAT_OP_REPLACE_BEGIN;
	put16(req + 1u, 1u);
	put16(req + 3u, data_len);
	put32(req + 5u, crc);
	CHECK(command(req, 9u) == 3u && output[0] == DL_ST_OK);
	if (output[0] != DL_ST_OK)
		return false;
	uint16_t transaction = get16(output + 1u);
	for (uint16_t offset = 0u; offset < data_len;) {
		uint16_t remaining = (uint16_t)(data_len - offset);
		uint16_t n = remaining > 59u ? 59u : remaining;
		req[0] = DL_SAT_OP_DATA;
		put16(req + 1u, transaction);
		put16(req + 3u, offset);
		memcpy(req + 5u, data + offset, n);
		CHECK(command(req, (uint16_t)(5u + n)) == 3u && output[0] == DL_ST_OK);
		if (output[0] != DL_ST_OK)
			return false;
		offset = (uint16_t)(offset + n);
	}
	uint8_t stage[18] = { DL_SAT_OP_PROFILE_STAGE };
	put16(stage + 1u, transaction);
	stage[3] = 1u;
	put32(stage + 4u, profile.catalog_id);
	put32(stage + 8u, profile.rx_hz);
	put32(stage + 12u, profile.tx_hz);
	put16(stage + 16u, profile.tx_tone_tenths);
	CHECK(command(stage, sizeof(stage)) == 1u && output[0] == DL_ST_OK);
	if (output[0] != DL_ST_OK)
		return false;
	req[0] = DL_SAT_OP_COMMIT;
	put16(req + 1u, transaction);
	put32(req + 3u, crc);
	CHECK(command(req, 7u) == 1u && output[0] == DL_ST_OK);
	return output[0] == DL_ST_OK;
}

static bool install_replace_profile_chunks(const uint8_t *data, uint16_t data_len, uint8_t count,
					  const satellite_frequency_profile_t *profiles)
{
	uint8_t req[64] = { DL_SAT_OP_REPLACE_BEGIN };
	uint32_t crc = storage_crc32(0u, data, data_len);
	put16(req + 1u, count);
	put16(req + 3u, data_len);
	put32(req + 5u, crc);
	CHECK(command(req, 9u) == 3u && output[0] == DL_ST_OK);
	if (output[0] != DL_ST_OK)
		return false;
	uint16_t transaction = get16(output + 1u);
	for (uint16_t offset = 0u; offset < data_len;) {
		uint16_t remaining = (uint16_t)(data_len - offset);
		uint16_t n = remaining > 59u ? 59u : remaining;
		req[0] = DL_SAT_OP_DATA;
		put16(req + 1u, transaction);
		put16(req + 3u, offset);
		memcpy(req + 5u, data + offset, n);
		CHECK(command(req, (uint16_t)(5u + n)) == 3u && output[0] == DL_ST_OK);
		offset = (uint16_t)(offset + n);
	}
	for (uint8_t start = 0u; start < count; start = (uint8_t)(start + 4u)) {
		uint8_t chunk = (uint8_t)(count - start);
		if (chunk > 4u)
			chunk = 4u;
		req[0] = DL_SAT_OP_PROFILE_STAGE_CHUNK;
		put16(req + 1u, transaction);
		req[3] = count;
		req[4] = start;
		req[5] = chunk;
		for (uint8_t i = 0u; i < chunk; i++) {
			uint8_t *p = req + 6u + (uint16_t)i * 14u;
			put32(p, profiles[start + i].catalog_id);
			put32(p + 4u, profiles[start + i].rx_hz);
			put32(p + 8u, profiles[start + i].tx_hz);
			put16(p + 12u, profiles[start + i].tx_tone_tenths);
		}
		uint16_t request_len = (uint16_t)(6u + (uint16_t)chunk * 14u);
		uint32_t reads_before = sim_flash_reads();
		CHECK(request_len <= 64u && command(req, request_len) == 1u && output[0] == DL_ST_OK);
		if (start + chunk == count)
			CHECK(sim_flash_reads() - reads_before == 4u * count + 1u);
		if (start == 0u) {
			req[0] = DL_SAT_OP_COMMIT;
			put16(req + 1u, transaction);
			put32(req + 3u, crc);
			CHECK(command(req, 7u) == 1u && output[0] == DL_ST_REFUSED);
		}
	}
	req[0] = DL_SAT_OP_COMMIT;
	put16(req + 1u, transaction);
	put32(req + 3u, crc);
	CHECK(command(req, 7u) == 1u && output[0] == DL_ST_OK);
	return output[0] == DL_ST_OK;
}

static void test_inventory_transactions(void)
{
	uint8_t req[9] = { DL_SAT_OP_INFO };
	CHECK(command(req, 1) == 6 && output[0] == DL_ST_OK && output[1] == 7 && get16(output + 2) == 10 &&
	      get16(output + 4) == 0);
	uint8_t profile_req = DL_SAT_OP_PROFILE_GET;
	CHECK(command(&profile_req, 1u) == 14u && output[0] == DL_ST_OK && output[1] == 3u &&
	      get32(output + 2u) == 25544u && get32(output + 6u) == 27607u && get32(output + 10u) == 43017u);
	CHECK(satellite_store_count() == 0u);
	uint8_t sync_req = DL_SAT_OP_SYNC_CHANNELS;
	CHECK(command(&sync_req, 1u) == 1u && output[0] == DL_ST_OK);
	channel_sync_result = false;
	uint8_t sync_status_req = DL_SAT_OP_SYNC_CHANNELS_STATUS;
	CHECK(command(&sync_status_req, 1u) == 2u && output[0] == DL_ST_OK &&
	      output[1] == SATELLITE_CHANNELS_SYNC_RUNNING);
	channel_sync_status = channel_sync_result ? SATELLITE_CHANNELS_SYNC_COMPLETE :
		SATELLITE_CHANNELS_SYNC_FAILED;
	CHECK(command(&sync_status_req, 1u) == 2u && output[0] == DL_ST_OK &&
	      output[1] == SATELLITE_CHANNELS_SYNC_FAILED);
	channel_sync_result = true;

	uint8_t data[10 * TEST_RECORD_MAX];
	uint16_t first_len = make_record(data, 25544u, "ISS", "26315.50000000");
	CHECK(install(data, first_len, 1, false, false, false));
	uint16_t second_len = make_record(data + first_len, 20580u, "HST", "26315.50000000");
	CHECK(install(data + first_len, second_len, 1, false, false, false));
	satellite_record_t satellite;
	CHECK(satellite_store_count() == 2u);
	CHECK(satellite_store_get(0, &satellite));
	CHECK(satellite.catalog_id == 20580u && satellite.name_length == 3u && strcmp(satellite.name, "HST") == 0);
	CHECK(memcmp(satellite.line1, data + first_len + 8u, 69u) == 0);
	CHECK(memcmp(satellite.line2, data + first_len + 8u + 69u, 69u) == 0);
	satellite_record_summary_t summary;
	CHECK(satellite_store_summary_get(0u, &summary));
	CHECK(summary.catalog_id == satellite.catalog_id && summary.name_length == satellite.name_length &&
	      strcmp(summary.name, satellite.name) == 0);
	CHECK(!satellite_store_summary_get(2u, &summary));
	CHECK(!satellite_store_get(2, &satellite));
	uint32_t selected_profiles[SATELLITE_PROFILE_MAX] = {
		40908u, 40931u, 43678u, 61781u, 63213u, 63214u, 63215u, 67291u, 63217u, 63218u
	};
	CHECK(satellite_store_profiles_set(SATELLITE_PROFILE_MAX, selected_profiles));
	uint8_t selected_count = 0u;
	uint32_t selected_readback[SATELLITE_PROFILE_MAX] = { 0 };
	CHECK(satellite_store_profiles_get(&selected_count, selected_readback) &&
	      selected_count == SATELLITE_PROFILE_MAX && memcmp(selected_readback, selected_profiles, sizeof(selected_profiles)) == 0);
	satellite_frequency_profile_t frequency_profile;
	CHECK(satellite_frequency_profile(40908u, &frequency_profile) && frequency_profile.rx_hz == 437200000u);
	CHECK(!satellite_frequency_profile(25544u, &frequency_profile));
	uint32_t invalid_profiles[SATELLITE_PROFILE_MAX] = { 25544u, 25544u };
	CHECK(!satellite_store_profiles_set(2u, invalid_profiles));
	invalid_profiles[1] = 20580u;
	CHECK(!satellite_store_profiles_set(2u, invalid_profiles));

	char replacement_line1[70] = {0}, replacement_line2[70] = {0};
	make_line((uint8_t *)replacement_line1, '1', satellite.catalog_id, "26316.50000000");
	make_line((uint8_t *)replacement_line2, '2', satellite.catalog_id, "00000.00000000");
	CHECK(satellite_store_replace(0u, replacement_line1, replacement_line2));
	CHECK(satellite_store_count() == 2u && satellite_store_get(0u, &satellite));
	CHECK(satellite.catalog_id == 20580u && strcmp(satellite.name, "HST") == 0 &&
	      memcmp(satellite.line1, replacement_line1, 69u) == 0);
	CHECK(satellite_store_profiles_get(&selected_count, selected_readback) &&
	      selected_count == SATELLITE_PROFILE_MAX && memcmp(selected_readback, selected_profiles, sizeof(selected_profiles)) == 0);
	replacement_line1[68] = replacement_line1[68] == '9' ? '0' : (char)(replacement_line1[68] + 1);
	CHECK(!satellite_store_replace(0u, replacement_line1, replacement_line2));
	CHECK(satellite_store_get(0u, &satellite) && memcmp(satellite.line1, replacement_line1, 68u) == 0 &&
	      satellite.line1[68] != replacement_line1[68]);
	make_line((uint8_t *)replacement_line1, '1', 99999u, "26316.50000000");
	CHECK(!satellite_store_replace(0u, replacement_line1, replacement_line2));

	req[0] = DL_SAT_OP_INFO;
	CHECK(command(req, 1) == 6 && output[0] == DL_ST_OK && get16(output + 4) == 2);
	uint8_t list_req[3] = { DL_SAT_OP_LIST, 0, 0 };
	CHECK(command(list_req, sizeof(list_req)) == 18 && output[0] == DL_ST_OK && output[1] == 2);
	CHECK(get32(output + 2) == 20580u && get32(output + 10) == 25544u && get32(output + 6) != 0u);

	uint16_t replacement_len = make_record(data, 25544u, "ISS UPDATED", "26316.50000000");
	CHECK(!install(data, replacement_len, 1, true, false, false));
	CHECK(command(req, 1) == 6 && output[0] == DL_ST_OK && get16(output + 4) == 2);
	CHECK(command(list_req, sizeof(list_req)) == 18 && output[0] == DL_ST_OK && output[1] == 2);
	CHECK(get32(output + 2) == 20580u && get32(output + 10) == 25544u);
	uint32_t old_epoch = get32(output + 14);
	CHECK(install(data, replacement_len, 1, false, false, false));
	CHECK(command(list_req, sizeof(list_req)) == 18 && output[0] == DL_ST_OK && output[1] == 2);
	CHECK(get32(output + 2) == 20580u && get32(output + 10) == 25544u && get32(output + 14) != old_epoch);
	uint32_t committed_epoch = get32(output + 14);
	CHECK(!install(data, replacement_len, 1, false, true, false));
	CHECK(!install(data, replacement_len, 1, false, false, true));
	CHECK(command(req, 1) == 6 && output[0] == DL_ST_OK && get16(output + 4) == 2);

	uint16_t interrupted_len = make_record(data, 25544u, "ISS INTERRUPTED", "26317.50000000");
	uint32_t interrupted_crc = storage_crc32(0, data, interrupted_len);
	uint8_t transfer[64] = { DL_SAT_OP_BEGIN };
	put16(transfer + 1, 1);
	put16(transfer + 3, interrupted_len);
	put32(transfer + 5, interrupted_crc);
	CHECK(command(transfer, 9) == 3 && output[0] == DL_ST_OK);
	uint16_t transaction = get16(output + 1);
	for (uint16_t off = 0; off < interrupted_len;) {
		uint16_t remaining = (uint16_t)(interrupted_len - off);
		uint16_t n = remaining > 59u ? 59u : remaining;
		transfer[0] = DL_SAT_OP_DATA;
		put16(transfer + 1, transaction);
		put16(transfer + 3, off);
		memcpy(transfer + 5, data + off, n);
		CHECK(command(transfer, (uint16_t)(5u + n)) == 3 && output[0] == DL_ST_OK);
		off = (uint16_t)(off + n);
	}
	sim_flash_cut_after(1);
	transfer[0] = DL_SAT_OP_COMMIT;
	put16(transfer + 1, transaction);
	put32(transfer + 3, interrupted_crc);
	CHECK(command(transfer, 7) == 1 && output[0] == DL_ST_REFUSED);
	sim_flash_power_on();
	transfer[0] = DL_SAT_OP_ABORT;
	put16(transfer + 1, transaction);
	CHECK(command(transfer, 3) == 1 && output[0] == DL_ST_OK);
	CHECK(command(req, 1) == 6 && output[0] == DL_ST_OK && get16(output + 4) == 2);
	CHECK(command(list_req, sizeof(list_req)) == 18 && output[0] == DL_ST_OK && get32(output + 14) == committed_epoch);
	list_req[1] = 2;
	CHECK(command(list_req, sizeof(list_req)) == 2 && output[0] == DL_ST_OK && output[1] == 0);
	uint8_t begin[9] = { DL_SAT_OP_BEGIN };
	put16(begin + 1, 11);
	put16(begin + 3, 11u * 144u);
	CHECK(command(begin, sizeof(begin)) == 1 && output[0] == DL_ST_BAD_ARG);
	uint16_t keep_len = make_record(data, 25544u, "ISS", "26318.50000000");
	CHECK(install_replace(data, keep_len, 1u));
	CHECK(command(req, 1u) == 6u && output[0] == DL_ST_OK && get16(output + 4u) == 1u);
	list_req[1] = 0u;
	CHECK(command(list_req, sizeof(list_req)) == 10u && output[0] == DL_ST_OK && output[1] == 1u &&
	      get32(output + 2u) == 25544u);
	CHECK(satellite_store_profiles_get(&selected_count, selected_readback) &&
	      selected_count == SATELLITE_PROFILE_MAX && memcmp(selected_readback, selected_profiles, sizeof(selected_profiles)) == 0);
	static const uint32_t chunked_ids[SATELLITE_PROFILE_MAX] = {
		25544u, 27607u, 40908u, 40931u, 43017u, 43678u, 61781u, 63213u, 63214u, 63215u
	};
	satellite_frequency_profile_t chunked_profiles[SATELLITE_PROFILE_MAX] = { 0 };
	uint16_t chunked_len = 0u, five_record_len = 0u;
	for (uint8_t i = 0u; i < SATELLITE_PROFILE_MAX; i++) {
		chunked_profiles[i].catalog_id = chunked_ids[i];
		chunked_profiles[i].rx_hz = 145900000u;
		chunked_profiles[i].tx_hz = 435100000u;
		chunked_profiles[i].tx_tone_tenths = 885u;
		uint16_t record_offset = chunked_len;
		uint16_t record_length = make_record(data + record_offset, chunked_ids[i], "SAT", "26318.50000000");
		if (i >= 5u)
			set_revolution_field(data + record_offset + 5u + 3u + 69u, " 1234");
		chunked_len = (uint16_t)(chunked_len + record_length);
		if (i == 4u)
			five_record_len = chunked_len;
	}
	CHECK(install_replace_profile_chunks(data, five_record_len, 5u, chunked_profiles));
	CHECK(install_replace_profile_chunks(data, chunked_len, SATELLITE_PROFILE_MAX, chunked_profiles));
	CHECK(satellite_store_count() == SATELLITE_PROFILE_MAX);
	CHECK(satellite_frequency_profile(63215u, &frequency_profile) && frequency_profile.rx_hz == 145900000u &&
	      frequency_profile.tx_hz == 435100000u && frequency_profile.tx_tone_tenths == 885u);
}

static void test_corrupt_empty_area_recovers(void)
{
	uint8_t req = DL_SAT_OP_INFO;
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	for (uint8_t bank = 0u; bank < 3u; bank++)
		sim_flash_data[map.satellite_addr + (uint32_t)bank * STORAGE_SECTOR] = 0u;
	CHECK(command(&req, 1) == 6u && output[0] == DL_ST_OK && get16(output + 4u) == 0u);
	uint8_t record[TEST_RECORD_MAX];
	uint16_t length = make_record(record, 25544u, "ISS", "26315.50000000");
	CHECK(install(record, length, 1u, false, false, false));
	CHECK(satellite_store_count() == 1u);
}

static void test_commit_power_cuts(void)
{
	static const uint32_t cuts[] = { 1u, 100u, 170u, 174u };
	for (unsigned cut = 0; cut < sizeof(cuts) / sizeof(cuts[0]); cut++) {
		sim_flash_create(FLASH_SIZE, &map);
		CHECK(storage_init());
		uint8_t record[TEST_RECORD_MAX];
		uint16_t length = make_record(record, 25544u, "ISS", "26315.50000000");
		CHECK(install(record, length, 1, false, false, false));
		length = make_record(record, 25544u, "ISS UPDATED", "26316.50000000");
		uint32_t crc = storage_crc32(0, record, length);
		uint8_t req[64] = { DL_SAT_OP_BEGIN };
		put16(req + 1, 1u);
		put16(req + 3, length);
		put32(req + 5, crc);
		CHECK(command(req, 9u) == 3u && output[0] == DL_ST_OK);
		uint16_t transaction = get16(output + 1);
		for (uint16_t offset = 0; offset < length;) {
			uint16_t remaining = (uint16_t)(length - offset);
			uint16_t n = remaining > 59u ? 59u : remaining;
			req[0] = DL_SAT_OP_DATA;
			put16(req + 1, transaction);
			put16(req + 3, offset);
			memcpy(req + 5, record + offset, n);
			CHECK(command(req, (uint16_t)(5u + n)) == 3u && output[0] == DL_ST_OK);
			offset = (uint16_t)(offset + n);
		}
		sim_flash_cut_after(cuts[cut]);
		req[0] = DL_SAT_OP_COMMIT;
		put16(req + 1, transaction);
		put32(req + 3, crc);
		command(req, 7u);
		CHECK(output[0] == DL_ST_REFUSED);
		sim_flash_power_on();
		CHECK(storage_init());
		uint8_t info = DL_SAT_OP_INFO;
		CHECK(command(&info, 1u) == 6u && output[0] == DL_ST_OK && get16(output + 4) == 1u);
		uint8_t list[3] = { DL_SAT_OP_LIST, 0, 0 };
		CHECK(command(list, sizeof(list)) == 10u && output[0] == DL_ST_OK && output[1] == 1u);
		CHECK(get32(output + 2) == 25544u &&
		      (get32(output + 6) == 1794398400u || get32(output + 6) == 1794484800u));
		req[0] = DL_SAT_OP_ABORT;
		put16(req + 1, transaction);
		command(req, 3u);
	}
}

static void test_replace_power_cuts(void)
{
	static const uint32_t cuts[] = { 1u, 100u, 170u, 174u };
	for (unsigned cut = 0; cut < sizeof(cuts) / sizeof(cuts[0]); cut++) {
		sim_flash_create(FLASH_SIZE, &map);
		CHECK(storage_init());
		uint8_t initial[TEST_RECORD_MAX];
		uint16_t initial_len = make_record(initial, 25544u, "ISS", "26315.50000000");
		CHECK(install(initial, initial_len, 1u, false, false, false));
		char line1[70], line2[70];
		make_line((uint8_t *)line1, '1', 25544u, "26316.50000000");
		make_line((uint8_t *)line2, '2', 25544u, "00000.00000000");
		line1[69] = '\0';
		line2[69] = '\0';
		sim_flash_cut_after(cuts[cut]);
		CHECK(!satellite_store_replace(0u, line1, line2));
		sim_flash_power_on();
		CHECK(storage_init());
		satellite_record_t satellite;
		CHECK(satellite_store_count() == 1u && satellite_store_get(0u, &satellite));
		CHECK(satellite.catalog_id == 25544u && satellite.epoch == 1794398400u &&
		      memcmp(satellite.line1, initial + 8u, 69u) == 0);
	}
}

static void test_custom_frequency_profiles(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	uint8_t record[TEST_RECORD_MAX];
	uint16_t length = make_record(record, 99001u, "MANUAL SAT", "26315.50000000");
	satellite_frequency_profile_t profile = {
		.catalog_id = 99001u, .name = "MANUAL SAT", .rx_hz = 145900000u, .tx_hz = 435100000u,
		.tx_tone_tenths = 885u,
	};
	CHECK(install_replace_profile(record, length, profile));
	satellite_frequency_profile_t readback;
	CHECK(satellite_frequency_profile(99001u, &readback) && readback.rx_hz == profile.rx_hz &&
	      readback.tx_hz == profile.tx_hz && readback.tx_tone_tenths == profile.tx_tone_tenths);
	uint8_t get_profiles = DL_SAT_OP_PROFILE_VALUES_GET;
	CHECK(command(&get_profiles, 1u) == 16u && output[0] == DL_ST_OK && output[1] == 1u &&
	      get32(output + 2u) == profile.catalog_id && get32(output + 6u) == profile.rx_hz &&
	      get32(output + 10u) == profile.tx_hz && get16(output + 14u) == profile.tx_tone_tenths);
	length = make_record(record, 99001u, "MANUAL SAT 2", "26316.50000000");
	CHECK(install_replace_profile(record, length, profile));
	satellite_record_t replacement;
	CHECK(satellite_store_count() == 1u && satellite_store_get(0u, &replacement) &&
	      replacement.catalog_id == 99001u && strcmp(replacement.name, "MANUAL SAT 2") == 0);
	CHECK(satellite_frequency_profile(99001u, &readback) && readback.rx_hz == profile.rx_hz &&
	      readback.tx_hz == profile.tx_hz && readback.tx_tone_tenths == profile.tx_tone_tenths);

	uint8_t bad_begin[9] = { DL_SAT_OP_REPLACE_BEGIN };
	put16(bad_begin + 1u, 1u);
	put16(bad_begin + 3u, length);
	uint32_t crc = storage_crc32(0u, record, length);
	put32(bad_begin + 5u, crc);
	CHECK(command(bad_begin, sizeof(bad_begin)) == 3u && output[0] == DL_ST_OK);
	uint16_t transaction = get16(output + 1u);
	uint8_t transfer[64];
	for (uint16_t offset = 0u; offset < length;) {
		uint16_t remaining = (uint16_t)(length - offset);
		uint16_t n = remaining > 59u ? 59u : remaining;
		transfer[0] = DL_SAT_OP_DATA;
		put16(transfer + 1u, transaction);
		put16(transfer + 3u, offset);
		memcpy(transfer + 5u, record + offset, n);
		CHECK(command(transfer, (uint16_t)(5u + n)) == 3u && output[0] == DL_ST_OK);
		offset = (uint16_t)(offset + n);
	}
	uint8_t invalid[18] = { DL_SAT_OP_PROFILE_STAGE };
	put16(invalid + 1u, transaction);
	invalid[3] = 1u;
	put32(invalid + 4u, profile.catalog_id);
	put32(invalid + 8u, 1000000000u);
	put32(invalid + 12u, profile.tx_hz);
	put16(invalid + 16u, profile.tx_tone_tenths);
	CHECK(command(invalid, sizeof(invalid)) == 1u && output[0] == DL_ST_BAD_ARG);
	invalid[3] = 1u;
	put32(invalid + 4u, 99002u);
	put32(invalid + 8u, profile.rx_hz);
	CHECK(command(invalid, sizeof(invalid)) == 1u && output[0] == DL_ST_BAD_ARG);
	uint8_t abort[3] = { DL_SAT_OP_ABORT };
	put16(abort + 1u, transaction);
	CHECK(command(abort, sizeof(abort)) == 1u && output[0] == DL_ST_OK);
	CHECK(satellite_frequency_profile(99001u, &readback) && readback.rx_hz == profile.rx_hz);
}

// The command's working arrays are one static buffer (debuglink_satellite.c: one debug link command runs at a time): what
// an earlier command left in it, longer or of another kind, never reaches the next
static void test_working_buffer_reuse(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	static const uint32_t ten[SATELLITE_PROFILE_MAX] = {
		40908u, 40931u, 43678u, 61781u, 63213u, 63214u, 63215u, 67291u, 63217u, 63218u
	};
	uint8_t set[2u + 4u * SATELLITE_PROFILE_MAX] = { DL_SAT_OP_PROFILE_SET };
	uint8_t get_ids = DL_SAT_OP_PROFILE_GET, get_values = DL_SAT_OP_PROFILE_VALUES_GET;
	uint8_t list[3] = { DL_SAT_OP_LIST };
	uint8_t record[TEST_RECORD_MAX];

	CHECK(install(record, make_record(record, 25544u, "ISS", "26315.50000000"), 1, false, false, false));
	CHECK(command(list, 3u) == 2u + 8u && output[0] == DL_ST_OK && output[1] == 1u);	// the list buffer holds a record
	set[1] = SATELLITE_PROFILE_MAX;
	for (uint8_t i = 0u; i < SATELLITE_PROFILE_MAX; i++)
		put32(set + 2u + 4u * i, ten[i]);
	CHECK(command(set, sizeof(set)) == 1u && output[0] == DL_ST_OK);
	CHECK(command(&get_values, 1u) == 2u + 14u * SATELLITE_PROFILE_MAX && output[0] == DL_ST_OK &&
	      output[1] == SATELLITE_PROFILE_MAX && get32(output + 2u) == ten[0] &&
	      get32(output + 2u + 14u * 9u) == ten[9]);		// the buffer now holds ten profiles
	set[1] = 2u;
	CHECK(command(set, 2u + 4u * 2u) == 1u && output[0] == DL_ST_OK);
	CHECK(command(&get_ids, 1u) == 2u + 4u * 2u && output[1] == 2u && get32(output + 2u) == ten[0] &&
	      get32(output + 6u) == ten[1]);			// two ids, none of the ten profiles' bytes
	CHECK(command(&get_values, 1u) == 2u + 14u * 2u && output[1] == 2u && get32(output + 2u) == ten[0] &&
	      get32(output + 16u) == ten[1] && get32(output + 6u) != 0u);
	CHECK(command(list, 3u) == 2u + 8u && output[0] == DL_ST_OK && output[1] == 1u && get32(output + 2u) == 25544u);
	CHECK(command(&get_ids, 1u) == 2u + 4u * 2u && output[1] == 2u && get32(output + 2u) == ten[0] &&
	      get32(output + 6u) == ten[1]);
	set[1] = SATELLITE_PROFILE_MAX;
	CHECK(command(set, sizeof(set)) == 1u && output[0] == DL_ST_OK);
	CHECK(command(&get_ids, 1u) == 2u + 4u * SATELLITE_PROFILE_MAX && output[1] == SATELLITE_PROFILE_MAX &&
	      get32(output + 2u + 4u * 9u) == ten[9]);
}

static void test_arm_capability(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	uint8_t data[2 * TEST_RECORD_MAX];
	uint16_t first = make_record(data, 25544u, "ISS", "26315.50000000");
	uint16_t second = make_record(data + first, 27607u, "CUSTOM NAME", "26315.50000000");
	CHECK(install(data, (uint16_t)(first + second), 2u, false, false, false));
	satellite_record_t record;
	satellite_record_summary_t summaries[2];
	uint8_t count = 0u;
	CHECK(satellite_store_list(0u, 2u, summaries, &count) && count == 2u);
	CHECK(summaries[0].flags == 0u && summaries[1].flags == SATELLITE_RECORD_NEEDS_ARM);
	CHECK(satellite_store_get(1u, &record) && record.flags == SATELLITE_RECORD_NEEDS_ARM &&
	      strcmp(record.name, "CUSTOM NAME") == 0);
	sim_flash_power_on();
	CHECK(storage_init());
	CHECK(satellite_store_summary_get(1u, &summaries[1]) &&
	      summaries[1].flags == SATELLITE_RECORD_NEEDS_ARM);
	CHECK(satellite_store_get(0u, &record) && record.flags == 0u);
}

static void test_first_install_recovery(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	uint8_t record[TEST_RECORD_MAX];
	uint16_t length = make_record(record, 25544u, "ISS", "26315.50000000");
	uint32_t crc = storage_crc32(0, record, length);
	uint8_t req[64] = { DL_SAT_OP_BEGIN };
	put16(req + 1, 1u);
	put16(req + 3, length);
	put32(req + 5, crc);
	CHECK(command(req, 9u) == 3u && output[0] == DL_ST_OK);
	uint16_t transaction = get16(output + 1);
	for (uint16_t offset = 0; offset < length;) {
			uint16_t remaining = (uint16_t)(length - offset);
			uint16_t n = remaining > 59u ? 59u : remaining;
		req[0] = DL_SAT_OP_DATA;
		put16(req + 1, transaction);
		put16(req + 3, offset);
		memcpy(req + 5, record + offset, n);
		CHECK(command(req, (uint16_t)(5u + n)) == 3u && output[0] == DL_ST_OK);
		offset = (uint16_t)(offset + n);
	}
	sim_flash_cut_after(100u);
	req[0] = DL_SAT_OP_COMMIT;
	put16(req + 1, transaction);
	put32(req + 3, crc);
	CHECK(command(req, 7u) == 1u && output[0] == DL_ST_REFUSED);
	sim_flash_power_on();
	CHECK(storage_init());
	uint8_t info = DL_SAT_OP_INFO;
	CHECK(command(&info, 1u) == 6u && output[0] == DL_ST_OK && get16(output + 4) == 0u);
	req[0] = DL_SAT_OP_ABORT;
	put16(req + 1, transaction);
	CHECK(command(req, 3u) == 1u && output[0] == DL_ST_OK);
	CHECK(install(record, length, 1u, false, false, false));
	CHECK(command(&info, 1u) == 6u && output[0] == DL_ST_OK && get16(output + 4) == 1u);
}

static void test_cross_band_profiles_install(void)
{
	static const satellite_frequency_profile_t profiles[SATELLITE_PROFILE_MAX] = {
		{ 25544u, "ISS", 437800000u, 145990000u, 670u },
		{ 27607u, "SO-50", 436795000u, 145850000u, 670u },
		{ 43017u, "AO-91", 145960000u, 435250000u, 0u },
	};
	static const uint32_t band_limits[][2] = {
		{ 136000000u, 174000000u },
		{ 400000000u, 480000000u },
	};
	sim_flash_create(FLASH_SIZE, &map);
	test_band.lo_hz = 10000000u;
	test_band.hi_hz = 999999999u;
	CHECK(storage_init());
	for (uint8_t band = 0u; band < sizeof(band_limits) / sizeof(band_limits[0]); band++) {
		test_band.lo_hz = band_limits[band][0];
		test_band.hi_hz = band_limits[band][1];
		CHECK(!satellite_store_frequency_profiles_set(3u, profiles));
	}
	test_band.lo_hz = 10000000u;
	test_band.hi_hz = 999999999u;
}

int main(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	test_inventory_transactions();
	CHECK(sim_flash_violations() == 0);
	test_corrupt_empty_area_recovers();
	test_commit_power_cuts();
	test_replace_power_cuts();
	test_custom_frequency_profiles();
	test_working_buffer_reuse();
	test_arm_capability();
	test_first_install_recovery();
	test_cross_band_profiles_install();
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
	if (failures != 0) {
		printf("# satelliteStoreTest FAILED (%d checks, %d failures)\n", checks, failures);
		return 1;
	}
	printf("# satelliteStoreTest PASSED (%d checks, 0 failures)\n", checks);
	return 0;
}