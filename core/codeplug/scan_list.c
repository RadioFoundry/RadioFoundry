// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored scan lists (scan_list.h)
#include <string.h>
#include "codeplug/scan_list.h"
#include "codeplug/codeplug_kv.h"
#include "config.h"

#if CONFIG_SCAN_LIST

_Static_assert(CODEPLUG_NAME_LEN == 16, "the record's name field is 16 bytes");
_Static_assert(SCAN_LIST_SLOTS <= 256, "the key family reserves 256 slots");

// Record offsets (docs/spec/scan-list.md 3)
enum { OFF_FLAGS = 0, OFF_COUNT = 1, OFF_NAME = 2, OFF_CHANNELS = SCAN_LIST_HEAD };

static volatile uint32_t writes;

uint32_t scan_list_generation(void)
{
	return (writes << 8) | codeplug_kv_serial();
}

// A list the store accepts: a name, at most SCAN_LIST_MAX channels, each a channel number (1 and up) once
bool scan_list_valid(const scan_list_t *l)
{
	if (l->name[0] == '\0' || l->count > SCAN_LIST_MAX)
		return false;
	for (uint16_t i = 0; i < l->count; i++) {
		if (l->channels[i] == 0)
			return false;
		for (uint16_t j = 0; j < i; j++)
			if (l->channels[j] == l->channels[i])
				return false;
	}
	return true;
}

unsigned scan_list_encode(const scan_list_t *l, uint8_t out[SCAN_LIST_REC_MAX])
{
	if (!scan_list_valid(l))
		return 0;
	memset(out, 0, SCAN_LIST_HEAD);
	out[OFF_COUNT] = (uint8_t)l->count;
	memcpy(out + OFF_NAME, l->name, CODEPLUG_NAME_LEN);
	for (uint16_t i = 0; i < l->count; i++) {
		out[OFF_CHANNELS + 2u * i] = (uint8_t)l->channels[i];
		out[OFF_CHANNELS + 2u * i + 1u] = (uint8_t)(l->channels[i] >> 8);
	}
	return SCAN_LIST_HEAD + 2u * l->count;
}

bool scan_list_decode(const uint8_t *in, unsigned len, scan_list_t *out)
{
	if (len < SCAN_LIST_HEAD || in[OFF_COUNT] > SCAN_LIST_MAX || len < SCAN_LIST_HEAD + 2u * in[OFF_COUNT])
		return false;
	scan_list_t l;
	memset(&l, 0, sizeof l);
	memcpy(l.name, in + OFF_NAME, CODEPLUG_NAME_LEN);
	l.count = in[OFF_COUNT];
	for (uint16_t i = 0; i < l.count; i++)
		l.channels[i] = (uint16_t)(in[OFF_CHANNELS + 2u * i] | in[OFF_CHANNELS + 2u * i + 1u] << 8);
	if (!scan_list_valid(&l))
		return false;
	*out = l;
	return true;
}

bool scan_list_usable(void)
{
	return codeplug_kv_writable();
}

uint16_t scan_list_slots(void)
{
	return scan_list_usable() ? SCAN_LIST_SLOTS : 0u;
}

bool scan_list_read(uint16_t slot, scan_list_t *out)
{
	uint8_t b[SCAN_LIST_REC_MAX];
	if (slot >= SCAN_LIST_SLOTS)
		return false;
	int n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_SCAN_LIST + slot), b, sizeof b);
	return n >= 0 && scan_list_decode(b, (unsigned)n < sizeof b ? (unsigned)n : sizeof b, out);
}

// The slots that hold a list, as a bit mask: one walk, so the area is checked once
static uint32_t stored(void)
{
	uint8_t b[SCAN_LIST_REC_MAX];
	scan_list_t l;
	uint32_t mask = 0;
	uint16_t key;
	codeplug_kv_cursor_t c;
	codeplug_kv_begin(&c, CODEPLUG_KEY_SCAN_LIST, (uint16_t)(CODEPLUG_KEY_SCAN_LIST + SCAN_LIST_SLOTS - 1u));
	for (int n; (n = codeplug_kv_next(&c, &key, b, sizeof b)) >= 0;)
		if (scan_list_decode(b, (unsigned)n < sizeof b ? (unsigned)n : sizeof b, &l))
			mask |= 1u << (key - CODEPLUG_KEY_SCAN_LIST);
	return mask;
}

uint16_t scan_list_count(void)
{
	uint32_t m = stored();
	uint16_t c = 0;
	for (; m != 0; m &= m - 1u)
		c++;
	return c;
}

bool scan_list_write(uint16_t slot, const scan_list_t *l)
{
	uint8_t rec[SCAN_LIST_REC_MAX];
	unsigned n = scan_list_encode(l, rec);
	if (slot >= SCAN_LIST_SLOTS || n == 0)
		return false;
	bool ok = codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SCAN_LIST + slot), rec, (uint16_t)n);
	writes++;		// even a failed write may have changed the sector
	return ok;
}

int scan_list_add(const scan_list_t *l)
{
	if (!scan_list_valid(l))
		return -1;
	uint32_t m = stored();
	for (uint16_t i = 0; i < SCAN_LIST_SLOTS; i++)
		if (!(m & 1u << i))
			return scan_list_write(i, l) ? (int)i : -1;
	return -1;
}

bool scan_list_delete(uint16_t slot)
{
	if (slot >= SCAN_LIST_SLOTS || !(stored() & 1u << slot))
		return false;
	bool ok = codeplug_kv_remove((uint16_t)(CODEPLUG_KEY_SCAN_LIST + slot));
	writes++;
	return ok;
}

#endif // CONFIG_SCAN_LIST
