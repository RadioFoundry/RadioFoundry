// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The theme-pack parser and bounded DebugLink transfer on simulated NOR flash.
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/radio.h"
#include "storage/storage.h"
#include "ui/theme_pack.h"
#include "sim/sim_flash.h"

#define FLASH_SIZE 0x28000u
#define THEME_ADDR 0x21000u
#define PACK_HEADER 20u
#define PACK_ENTRY 60u
#define PACK_MAX_SIZE (PACK_HEADER + THEME_PACK_MAX_THEMES * PACK_ENTRY)

static int failures, checks;
static uint8_t output[64];
static uint8_t flash_before[FLASH_SIZE];
static radio_state_t radio_state;
static hal_flash_map_t map = {
	.settings_addr = 0x01000u, .settings_size = 0x02000u, .journal_addr = 0x03000u,
	.openfw_eeprom = 0u, .openfw_flash = 0x10000u, .openfw_end = 0x20000u,
	.calibration_addr = 0x05000u, .calibration_size = 0x00200u,
	.theme_pack_addr = THEME_ADDR, .theme_pack_size = STORAGE_SECTOR,
};

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

const board_t board = { .name = "theme-pack-test", .flash = &sim_flash };

void storage_lock(void) { }
void storage_unlock(void) { }
void storage_quick_lock(void) { }
void storage_quick_unlock(void) { }

radio_state_t radio_get_state(void)
{
	return radio_state;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
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

static uint8_t command_status(const uint8_t *req, uint16_t len, uint8_t expected)
{
	memset(output, 0xEE, sizeof output);
	size_t n = debuglink_cmd_theme_pack(req, len, output, sizeof output);
	CHECK(n == 1u && output[0] == expected, "operation %u returned %u bytes/status %u; expected status %u",
	      len == 0u ? 0xFFu : req[0], (unsigned)n, n != 0u ? output[0] : 0xFFu, expected);
	return n != 0u ? output[0] : 0xFFu;
}

static void setup(void)
{
	map = (hal_flash_map_t){
		.settings_addr = 0x01000u, .settings_size = 0x02000u, .journal_addr = 0x03000u,
		.openfw_eeprom = 0u, .openfw_flash = 0x10000u, .openfw_end = 0x20000u,
		.calibration_addr = 0x05000u, .calibration_size = 0x00200u,
		.theme_pack_addr = THEME_ADDR, .theme_pack_size = STORAGE_SECTOR,
	};
	radio_state = RS_IDLE;
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init(), "storage init");
	CHECK(ui_theme_pack_supported(), "mapped theme-pack sector is supported");
	CHECK(!ui_theme_pack_reload() && ui_theme_pack_count() == 0u, "blank area has no installed pack");
}

// Encode the version 1 wire format described in docs/spec/theme-packs.md.
static uint16_t make_pack(uint8_t *pack, const char *const *names, uint16_t count)
{
	uint16_t payload_len = (uint16_t)(count * PACK_ENTRY);
	uint16_t total = (uint16_t)(PACK_HEADER + payload_len);
	memset(pack, 0, total);
	memcpy(pack, "RFTP", 4u);
	put16(pack + 4u, 1u);
	put16(pack + 6u, PACK_HEADER);
	put32(pack + 8u, payload_len);
	put16(pack + 12u, count);
	for (uint16_t i = 0; i < count; i++) {
		uint8_t *entry = pack + PACK_HEADER + i * PACK_ENTRY;
		uint8_t name_len = (uint8_t)strlen(names[i]);
		entry[0] = name_len;
		memcpy(entry + 1u, names[i], name_len);
		for (uint8_t j = 0; j < THEME_PACK_COLOR_COUNT; j++)
			put16(entry + 24u + 2u * j, (uint16_t)(0x1200u + i * 0x20u + j));
	}
	put32(pack + 16u, storage_crc32(0u, pack + PACK_HEADER, payload_len));
	return total;
}

static uint8_t begin_transfer(uint16_t length)
{
	uint8_t req[3] = { DL_THEME_PACK_BEGIN, 0u, 0u };
	put16(req + 1u, length);
	return command_status(req, sizeof req, DL_ST_OK);
}

static void send_pack_data(const uint8_t *pack, uint16_t length)
{
	uint8_t req[64];
	for (uint16_t offset = 0; offset < length;) {
		uint16_t n = (uint16_t)(length - offset);
		if (n > 61u)
			n = 61u;
		req[0] = DL_THEME_PACK_DATA;
		put16(req + 1u, offset);
		memcpy(req + 3u, pack + offset, n);
		CHECK(command_status(req, (uint16_t)(3u + n), DL_ST_OK) == DL_ST_OK,
		      "sequential chunk at offset %u accepted", offset);
		offset = (uint16_t)(offset + n);
	}
}

static uint8_t commit_transfer(void)
{
	const uint8_t req[] = { DL_THEME_PACK_COMMIT };
	memset(output, 0xEE, sizeof output);
	size_t n = debuglink_cmd_theme_pack(req, sizeof req, output, sizeof output);
	CHECK(n == 1u, "commit reply length is one");
	return n != 0u ? output[0] : 0xFFu;
}

static void expect_rejected_pack(const uint8_t *pack, uint16_t length, const char *case_name)
{
	uint8_t req[3] = { DL_THEME_PACK_BEGIN, 0u, 0u };
	put16(req + 1u, length);
	CHECK(command_status(req, sizeof req, DL_ST_OK) == DL_ST_OK, "%s transfer begins", case_name);
	send_pack_data(pack, length);
	CHECK(commit_transfer() == DL_ST_REFUSED, "%s rejected at commit", case_name);
	CHECK(ui_theme_pack_count() == 0u, "%s exposes no partial themes", case_name);
}

static void snapshot_non_theme_flash(void)
{
	// Sentinels immediately outside the owned sector catch an erase-size or address regression.
	sim_flash_data[THEME_ADDR - 1u] = 0x5Au;
	sim_flash_data[THEME_ADDR + STORAGE_SECTOR] = 0xA5u;
	memcpy(flash_before, sim_flash_data, FLASH_SIZE);
}

static void check_non_theme_flash_unchanged(const char *operation)
{
	unsigned changed = 0u;
	uint32_t first = 0u;
	for (uint32_t i = 0u; i < FLASH_SIZE; i++) {
		if (i >= THEME_ADDR && i < THEME_ADDR + STORAGE_SECTOR)
			continue;
		if (sim_flash_data[i] != flash_before[i]) {
			if (changed == 0u)
				first = i;
			changed++;
		}
	}
	CHECK(changed == 0u, "%s touched %u bytes outside theme sector (first at 0x%X)", operation, changed, first);
}

static void test_protocol_and_valid_pack(void)
{
	const char *names[THEME_PACK_MAX_THEMES] = {
		"Aurora", "Orbit", "M\xC3\xBCnchen", "Buckeye", "Wolverines", "Night",
		"Pack Seven", "Pack Eight", "Pack Nine", "12345678901234567890123",
	};
	uint8_t pack[PACK_MAX_SIZE];
	char name[THEME_PACK_NAME_LEN];
	uint16_t colors[THEME_PACK_COLOR_COUNT];
	uint16_t length = make_pack(pack, names, THEME_PACK_MAX_THEMES);
	CHECK(length == 620u, "ten entries encode to 620 bytes");

	const uint8_t info[] = { DL_THEME_PACK_INFO };
	memset(output, 0xEE, sizeof output);
	size_t n = debuglink_cmd_theme_pack(info, sizeof info, output, sizeof output);
	CHECK(n == 5u && output[0] == DL_ST_OK && output[1] == 1u && get16(output + 2u) == STORAGE_SECTOR &&
	      output[4] == 0u, "INFO reports protocol, maximum size, and no installed themes");
	CHECK(debuglink_cmd_theme_pack(info, sizeof info, output, 0u) == 0u, "zero-capacity reply writes nothing");
	CHECK(debuglink_cmd_theme_pack(info, sizeof info, output, 4u) == 1u && output[0] == DL_ST_BAD_ARG,
	      "short INFO reply is rejected");
	command_status(NULL, 0u, DL_ST_BAD_ARG);
	const uint8_t bad_info[] = { DL_THEME_PACK_INFO, 0u };
	command_status(bad_info, sizeof bad_info, DL_ST_BAD_ARG);
	const uint8_t unknown[] = { 0xFFu };
	command_status(unknown, sizeof unknown, DL_ST_BAD_ARG);

	CHECK(!ui_theme_pack_transfer_begin(PACK_HEADER - 1u), "BEGIN rejects a short header");
	CHECK(!ui_theme_pack_transfer_begin(STORAGE_SECTOR + 1u), "BEGIN rejects a pack larger than the mapped sector");
	radio_state = RS_TX;
	uint8_t begin_req[] = { DL_THEME_PACK_BEGIN, 0u, 0u };
	put16(begin_req + 1u, length);
	command_status(begin_req, sizeof begin_req, DL_ST_BUSY);
	radio_state = RS_IDLE;

	snapshot_non_theme_flash();
	CHECK(begin_transfer(length) == DL_ST_OK, "valid BEGIN accepted");
	CHECK(ui_theme_pack_count() == 0u, "beginning a replacement hides old entries");

	uint8_t data_req[65] = { DL_THEME_PACK_DATA, 1u, 0u, 0x52u };
	command_status(data_req, 4u, DL_ST_REFUSED);	// first offset must be zero
	command_status(data_req, 3u, DL_ST_BAD_ARG);	// no data bytes
	memset(data_req, 0x52, sizeof data_req);
	data_req[0] = DL_THEME_PACK_DATA;
	put16(data_req + 1u, 0u);
	CHECK(debuglink_cmd_theme_pack(data_req, 64u, output, sizeof output) == 1u && output[0] == DL_ST_OK,
	      "maximum 61-byte data chunk is accepted");	// replace the first 61 bytes with the proper pack bytes below
	command_status(data_req, sizeof data_req, DL_ST_BAD_ARG);	// packet over the 64-byte protocol limit
	CHECK(!ui_theme_pack_transfer_commit(), "commit is refused until all bytes arrive");
	// Restart, since the preceding max-sized probe wrote different bytes, then test a real multi-chunk pack.
	CHECK(begin_transfer(length) == DL_ST_OK, "a new BEGIN restarts the transfer");
	send_pack_data(pack, length);
	CHECK(commit_transfer() == DL_ST_OK, "valid CRC and complete payload commit");
	check_non_theme_flash_unchanged("install");
	CHECK(ui_theme_pack_count() == THEME_PACK_MAX_THEMES, "all ten themes become available together");
	for (uint8_t i = 0u; i < THEME_PACK_MAX_THEMES; i++) {
		CHECK(ui_theme_pack_get(i, name, colors), "theme %u is readable", i);
		CHECK(strcmp(name, names[i]) == 0, "theme %u name round-trips", i);
		for (uint8_t j = 0u; j < THEME_PACK_COLOR_COUNT; j++)
			CHECK(colors[j] == (uint16_t)(0x1200u + i * 0x20u + j), "theme %u color %u round-trips", i, j);
	}
	CHECK(!ui_theme_pack_get(THEME_PACK_MAX_THEMES, name, colors), "index after last theme is rejected");
	CHECK(!ui_theme_pack_get(0u, NULL, colors), "null name output is rejected");

	// A fresh reload models boot: entries are visible only after the stored data validates again.
	sim_flash_power_on();
	CHECK(storage_init(), "storage reinitializes after simulated reboot");
	CHECK(ui_theme_pack_reload() && ui_theme_pack_count() == THEME_PACK_MAX_THEMES, "valid pack survives reload");
	CHECK(ui_theme_pack_get(2u, name, colors) && strcmp(name, names[2]) == 0,
	      "UTF-8 name survives reload");

	// Radio TX refuses all mutating protocol operations, without ending an in-progress transfer.
	CHECK(begin_transfer(length) == DL_ST_OK, "start replacement before TX checks");
	uint8_t part_req[8] = { DL_THEME_PACK_DATA, 0u, 0u, 'R', 'F', 'T', 'P', 1u };
	radio_state = RS_TX;
	command_status(part_req, sizeof part_req, DL_ST_BUSY);
	CHECK(commit_transfer() == DL_ST_BUSY, "COMMIT refused during TX");
	const uint8_t remove_req[] = { DL_THEME_PACK_REMOVE };
	command_status(remove_req, sizeof remove_req, DL_ST_BUSY);
	radio_state = RS_IDLE;
	CHECK(begin_transfer(length) == DL_ST_OK, "restart after TX refusal");
	send_pack_data(pack, length);
	CHECK(commit_transfer() == DL_ST_OK, "transfer succeeds after TX ends");

	// Removal clears only the dedicated sector and makes the installed theme IDs unavailable.
	snapshot_non_theme_flash();
	command_status(remove_req, sizeof remove_req, DL_ST_OK);
	CHECK(ui_theme_pack_count() == 0u, "REMOVE clears installed themes");
	for (uint32_t i = 0u; i < STORAGE_SECTOR; i++)
		CHECK(sim_flash_data[THEME_ADDR + i] == 0xFFu, "removed sector erased at byte %u", i);
	check_non_theme_flash_unchanged("REMOVE");
	CHECK(debuglink_cmd_theme_pack(info, sizeof info, output, sizeof output) == 5u && output[4] == 0u,
	      "INFO reports zero themes after removal");
}

static void test_invalid_packs(void)
{
	const char *names[] = { "One" };
	uint8_t pack[PACK_MAX_SIZE];
	uint16_t length = make_pack(pack, names, 1u);

	pack[PACK_HEADER + 24u] ^= 1u;	// payload changes but the CRC doesn't
	expect_rejected_pack(pack, length, "CRC mismatch");
	length = make_pack(pack, names, 1u);
	put16(pack + 4u, 2u);
	expect_rejected_pack(pack, length, "unknown version");
	length = make_pack(pack, names, 1u);
	put16(pack + 14u, 1u);
	expect_rejected_pack(pack, length, "nonzero reserved field");
	length = make_pack(pack, names, 1u);
	put16(pack + 6u, PACK_HEADER - 1u);
	expect_rejected_pack(pack, length, "wrong header size");
	length = make_pack(pack, names, 1u);
	put16(pack + 12u, 0u);
	expect_rejected_pack(pack, length, "zero theme count");
	length = make_pack(pack, names, 1u);
	put16(pack + 12u, THEME_PACK_MAX_THEMES + 1u);
	expect_rejected_pack(pack, length, "too many themes");
	length = make_pack(pack, names, 1u);
	put32(pack + 8u, PACK_ENTRY + 1u);
	expect_rejected_pack(pack, length, "payload length mismatch");
	length = make_pack(pack, names, 1u);
	pack[PACK_HEADER] = 0u;
	put32(pack + 16u, storage_crc32(0u, pack + PACK_HEADER, length - PACK_HEADER));
	expect_rejected_pack(pack, length, "empty name");
	length = make_pack(pack, names, 1u);
	pack[PACK_HEADER] = 24u;
	put32(pack + 16u, storage_crc32(0u, pack + PACK_HEADER, length - PACK_HEADER));
	expect_rejected_pack(pack, length, "name longer than field");
	length = make_pack(pack, names, 1u);
	pack[PACK_HEADER] = 2u;
	pack[PACK_HEADER + 1u] = 0xC0u;
	pack[PACK_HEADER + 2u] = 0xAFu;
	put32(pack + 16u, storage_crc32(0u, pack + PACK_HEADER, length - PACK_HEADER));
	expect_rejected_pack(pack, length, "invalid UTF-8 name");
	length = make_pack(pack, names, 1u);
	pack[PACK_HEADER] = 1u;
	pack[PACK_HEADER + 1u] = 'A';
	pack[PACK_HEADER + 2u] = 'X';	// padding after the one-byte name must be zero
	put32(pack + 16u, storage_crc32(0u, pack + PACK_HEADER, length - PACK_HEADER));
	expect_rejected_pack(pack, length, "nonzero name padding");

	// Extra bytes are not accepted: transfer length must equal header plus all entries.
	length = make_pack(pack, names, 1u);
	pack[length] = 0u;
	CHECK(begin_transfer((uint16_t)(length + 1u)) == DL_ST_OK, "oversized declared payload begins within sector");
	send_pack_data(pack, (uint16_t)(length + 1u));
	CHECK(commit_transfer() == DL_ST_REFUSED, "extra trailing byte is rejected at commit");
	CHECK(ui_theme_pack_count() == 0u, "trailing byte exposes no partial theme");
}

static void test_transfer_bounds_and_failures(void)
{
	const char *names[] = { "Transfer" };
	uint8_t pack[PACK_MAX_SIZE], req[64];
	uint16_t length = make_pack(pack, names, 1u);

	// A failed flash write cancels the transfer; callers must begin again rather than commit partial data.
	CHECK(begin_transfer(length) == DL_ST_OK, "begin transfer before injected write failure");
	sim_flash_bad_range(THEME_ADDR + 20u, 1u);
	req[0] = DL_THEME_PACK_DATA;
	put16(req + 1u, 0u);
	memcpy(req + 3u, pack, length < 61u ? length : 61u);
	command_status(req, (uint16_t)(3u + (length < 61u ? length : 61u)), DL_ST_REFUSED);
	sim_flash_bad_range(0u, 0u);
	CHECK(commit_transfer() == DL_ST_REFUSED, "failed write leaves commit unavailable");

	CHECK(begin_transfer(length) == DL_ST_OK, "begin valid transfer");
	req[0] = DL_THEME_PACK_DATA;
	put16(req + 1u, 0u);
	memcpy(req + 3u, pack, 61u);
	CHECK(debuglink_cmd_theme_pack(req, 64u, output, sizeof output) == 1u && output[0] == DL_ST_OK,
	      "first data chunk accepted");
	put16(req + 1u, 61u);
	memcpy(req + 3u, pack + 61u, length - 61u);
	CHECK(debuglink_cmd_theme_pack(req, (uint16_t)(3u + length - 61u), output, sizeof output) == 1u &&
	      output[0] == DL_ST_OK, "final chunk accepted");
	CHECK(commit_transfer() == DL_ST_OK, "exact transfer length commits");

	// A read error during validation hides the pack; clearing the injected fault permits a clean reload.
	sim_flash_bad_read(THEME_ADDR + PACK_HEADER, 1u);
	CHECK(!ui_theme_pack_reload() && ui_theme_pack_count() == 0u, "read error rejects the whole pack");
	sim_flash_bad_read(0u, 0u);
	CHECK(ui_theme_pack_reload() && ui_theme_pack_count() == 1u, "valid flash contents reload after read recovers");
}

static void test_flash_area_guard(void)
{
	const uint8_t info[] = { DL_THEME_PACK_INFO };
	const uint8_t remove_req[] = { DL_THEME_PACK_REMOVE };
	uint8_t begin_req[] = { DL_THEME_PACK_BEGIN, PACK_HEADER, 0u };

	map.theme_pack_size = 0u;
	CHECK(!ui_theme_pack_supported(), "zero-sized map disables support");
	command_status(info, sizeof info, DL_ST_UNAVAILABLE);
	command_status(begin_req, sizeof begin_req, DL_ST_UNAVAILABLE);
	command_status(remove_req, sizeof remove_req, DL_ST_UNAVAILABLE);

	map.theme_pack_size = STORAGE_SECTOR;
	map.theme_pack_addr = map.settings_addr;
	CHECK(!ui_theme_pack_supported(), "overlap with settings disables support");
	command_status(begin_req, sizeof begin_req, DL_ST_UNAVAILABLE);

	map.theme_pack_addr = THEME_ADDR + 1u;
	CHECK(!ui_theme_pack_supported(), "unaligned map disables support");
	map.theme_pack_addr = FLASH_SIZE;
	CHECK(!ui_theme_pack_supported(), "map beyond flash disables support");

	// Restore the valid map for the final supported check.
	map.theme_pack_addr = THEME_ADDR;
	CHECK(ui_theme_pack_supported(), "restored dedicated sector is supported");
}

int main(void)
{
	setup();
	test_protocol_and_valid_pack();
	setup();
	test_invalid_packs();
	setup();
	test_transfer_bounds_and_failures();
	setup();
	test_flash_area_guard();
	sim_flash_free();
	if (failures != 0) {
		printf("# themePackTest FAILED (%d checks, %d failures)\n", checks, failures);
		return 1;
	}
	printf("# themePackTest PASSED (%d checks, 0 failures)\n", checks);
	return 0;
}
