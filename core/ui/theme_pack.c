// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 RadioFoundry Theme Pack contributors
#include <string.h>
#include "config.h"
#include "theme_pack.h"
#include "storage/storage.h"
#include "radio/radio.h"

#define PACK_HEADER_SIZE 20u
#define PACK_ENTRY_SIZE 60u
#define PACK_VERSION 1u

typedef struct {
	char name[THEME_PACK_NAME_LEN];
	uint16_t colors[THEME_PACK_COLOR_COUNT];
} pack_theme_t;

static uint8_t theme_count;
#if CONFIG_THEME_PACKS
static pack_theme_t themes[THEME_PACK_MAX_THEMES];
static uint16_t transfer_length;
static uint16_t transfer_received;
static bool transfer_active;
#endif

#if CONFIG_THEME_PACKS
static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return alen != 0u && blen != 0u && a < b + blen && b < a + alen;
}

static bool theme_area(const hal_flash_map_t **map)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || board.flash == NULL || board.flash->read == NULL || board.flash->write == NULL ||
	    board.flash->erase == NULL || board.flash->erase_size != STORAGE_SECTOR || m->theme_pack_size != STORAGE_SECTOR ||
	    m->theme_pack_addr % STORAGE_SECTOR != 0u || m->theme_pack_addr > board.flash->size ||
	    STORAGE_SECTOR > board.flash->size - m->theme_pack_addr)
		return false;
	uint32_t a = m->theme_pack_addr, n = m->theme_pack_size;
	if (overlaps(a, n, m->settings_addr, m->settings_size) || overlaps(a, n, m->journal_addr, 2u * STORAGE_SECTOR) ||
	    overlaps(a, n, m->openfw_eeprom, 0x10000u) ||
	    overlaps(a, n, m->openfw_flash, m->openfw_end > m->openfw_flash ? m->openfw_end - m->openfw_flash : 0u) ||
	    overlaps(a, n, m->calibration_addr, m->calibration_size) || overlaps(a, n, m->native_addr, m->native_size) ||
	    overlaps(a, n, m->staging_addr, m->staging_size) ||
	    overlaps(a, n, m->satellite_addr, m->satellite_size))
		return false;
	*map = m;
	return true;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool utf8_name(const uint8_t *s, uint8_t n)
{
	for (uint8_t i = 0; i < n;) {
		uint8_t c = s[i++];
		if (c >= 0x20u && c <= 0x7Eu)
			continue;
		if (c < 0xC2u || c > 0xF4u)
			return false;
		uint8_t need = c < 0xE0u ? 1u : c < 0xF0u ? 2u : 3u;
		if ((uint16_t)i + need > n)
			return false;
		uint8_t first = s[i];
		if ((first & 0xC0u) != 0x80u || (c == 0xE0u && first < 0xA0u) ||
		    (c == 0xEDu && first >= 0xA0u) || (c == 0xF0u && first < 0x90u) ||
		    (c == 0xF4u && first >= 0x90u))
			return false;
		for (uint8_t j = 1; j < need; j++) {
			if ((s[i + j] & 0xC0u) != 0x80u)
				return false;
		}
		i = (uint8_t)(i + need);
	}
	return true;
}
#endif

bool ui_theme_pack_reload(void)
{
	theme_count = 0;
#if CONFIG_THEME_PACKS
	const hal_flash_map_t *m;
	if (!storage_ready() || !theme_area(&m))
		return false;
	uint8_t h[PACK_HEADER_SIZE], entry[PACK_ENTRY_SIZE], chunk[64];
	uint32_t addr = m->theme_pack_addr;
	storage_lock();
	bool ok = board.flash->read(addr, h, sizeof h) && memcmp(h, "RFTP", 4) == 0 &&
		  get16(&h[4]) == PACK_VERSION && get16(&h[6]) == PACK_HEADER_SIZE && get16(&h[14]) == 0u;
	uint32_t payload_len = ok ? get32(&h[8]) : 0;
	uint16_t count = ok ? get16(&h[12]) : 0;
	if (!ok || count == 0u || count > THEME_PACK_MAX_THEMES || payload_len != (uint32_t)count * PACK_ENTRY_SIZE ||
	    PACK_HEADER_SIZE + payload_len > m->theme_pack_size) {
		storage_unlock();
		return false;
	}
	uint32_t crc = 0;
	for (uint32_t off = 0; ok && off < payload_len;) {
		uint32_t n = payload_len - off < sizeof chunk ? payload_len - off : sizeof chunk;
		ok = board.flash->read(addr + PACK_HEADER_SIZE + off, chunk, n);
		if (ok)
			crc = storage_crc32(crc, chunk, n);
		off += n;
	}
	ok = ok && crc == get32(&h[16]);
	for (uint8_t i = 0; ok && i < count; i++) {
		ok = board.flash->read(addr + PACK_HEADER_SIZE + (uint32_t)i * PACK_ENTRY_SIZE, entry, sizeof entry);
		uint8_t n = ok ? entry[0] : 0u;
		if (!ok || n == 0u || n > 23u || !utf8_name(&entry[1], n)) {
			ok = false;
			break;
		}
		for (uint8_t j = n; j < 23u; j++) {
			if (entry[1u + j] != 0u) {
				ok = false;
				break;
			}
		}
		if (!ok)
			break;
		memcpy(themes[i].name, &entry[1], n);
		themes[i].name[n] = '\0';
		for (uint8_t j = 0; j < THEME_PACK_COLOR_COUNT; j++)
			themes[i].colors[j] = get16(&entry[24u + 2u * j]);
	}
	storage_unlock();
	if (ok)
		theme_count = (uint8_t)count;
	return ok;
#else
	return false;
#endif
}

void ui_theme_pack_init(void)
{
	(void)ui_theme_pack_reload();
}

uint8_t ui_theme_pack_count(void)
{
	return theme_count;
}

bool ui_theme_pack_supported(void)
{
#if CONFIG_THEME_PACKS
	const hal_flash_map_t *m;
	return storage_ready() && theme_area(&m);
#else
	return false;
#endif
}

bool ui_theme_pack_get(uint8_t index, char name[THEME_PACK_NAME_LEN], uint16_t colors[THEME_PACK_COLOR_COUNT])
{
#if CONFIG_THEME_PACKS
	if (index >= theme_count || name == NULL || colors == NULL)
		return false;
	strcpy(name, themes[index].name);
	memcpy(colors, themes[index].colors, sizeof themes[index].colors);
	return true;
#else
	(void)index;
	(void)name;
	(void)colors;
	return false;
#endif
}

bool ui_theme_pack_transfer_begin(uint16_t length)
{
#if CONFIG_THEME_PACKS
	const hal_flash_map_t *m;
	if (radio_get_state() == RS_TX || !storage_ready() || !theme_area(&m) || length < PACK_HEADER_SIZE ||
	    length > m->theme_pack_size)
		return false;
	storage_lock();
	bool ok = board.flash->erase(m->theme_pack_addr);
	storage_unlock();
	if (ok)
		theme_count = 0;
	transfer_length = length;
	transfer_received = 0;
	transfer_active = ok;
	return ok;
#else
	(void)length;
	return false;
#endif
}

bool ui_theme_pack_transfer_data(uint16_t offset, const uint8_t *data, uint16_t length)
{
#if CONFIG_THEME_PACKS
	const hal_flash_map_t *m;
	if (!transfer_active || transfer_received > transfer_length || data == NULL || length == 0u || offset != transfer_received ||
	    length > transfer_length - transfer_received || radio_get_state() == RS_TX || !storage_ready() || !theme_area(&m))
		return false;
	storage_lock();
	bool ok = board.flash->write(m->theme_pack_addr + offset, data, length);
	storage_unlock();
	if (ok)
		transfer_received = (uint16_t)(transfer_received + length);
	else
		transfer_active = false;
	return ok;
#else
	(void)offset;
	(void)data;
	(void)length;
	return false;
#endif
}

bool ui_theme_pack_transfer_commit(void)
{
#if CONFIG_THEME_PACKS
	if (!transfer_active || transfer_received != transfer_length || radio_get_state() == RS_TX)
		return false;
	transfer_active = false;
	bool ok = ui_theme_pack_reload() && transfer_length == PACK_HEADER_SIZE + (uint16_t)theme_count * PACK_ENTRY_SIZE;
	if (!ok)
		theme_count = 0;
	return ok;
#else
	return false;
#endif
}

bool ui_theme_pack_remove(void)
{
#if CONFIG_THEME_PACKS
	const hal_flash_map_t *m;
	if (radio_get_state() == RS_TX || !storage_ready() || !theme_area(&m))
		return false;
	storage_lock();
	bool ok = board.flash->erase(m->theme_pack_addr);
	storage_unlock();
	transfer_active = false;
	transfer_length = transfer_received = 0;
	theme_count = 0;
	return ok;
#else
	return false;
#endif
}
