// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The zone index log (zone_edit.h): zone_edit.c notes each zone it adds ahead of others or deletes, and whoever keeps
// a zone index (the radio core, dual watch, the UI's screens) maps it through the entries since it last looked. Apart
// from zone_edit.c, so the readers link without the editing code. Entries are written under the storage lock; the
// count goes up after its entry is in place, and busy covers the write before it, so another task reads whole entries
#include "codeplug/zone_edit.h"

static struct {
	uint8_t deleted;	// 1: the zone at index was deleted; 0: a new zone is at index
	uint16_t index;
} shift_log[ZONE_SHIFT_LOG];
static volatile uint32_t shift_count;
static volatile bool shift_busy;

void zone_shift_note(bool deleted, uint16_t index)
{
	uint32_t n = shift_count;
	shift_log[n % ZONE_SHIFT_LOG].deleted = deleted ? 1u : 0u;
	shift_log[n % ZONE_SHIFT_LOG].index = index;
	shift_count = n + 1u;
}

void zone_shift_busy(bool on)
{
	shift_busy = on;
}

uint32_t zone_edit_shifts(void)
{
	return shift_count;
}

bool zone_edit_busy(void)
{
	return shift_busy;
}

zone_map_t zone_edit_map(uint32_t from, uint32_t to, uint16_t index, uint16_t *out)
{
	zone_map_t m = ZONE_MAP_KEPT;
	*out = index;
	if (to - from > ZONE_SHIFT_LOG) {
		return ZONE_MAP_LOST;
	}
	for (uint32_t n = from; n != to; n++) {
		uint16_t at = shift_log[n % ZONE_SHIFT_LOG].index;
		if (!shift_log[n % ZONE_SHIFT_LOG].deleted) {
			if (*out >= at) {
				(*out)++;
			}
		} else if (*out > at) {
			(*out)--;
		} else if (*out == at && m == ZONE_MAP_KEPT) {
			m = ZONE_MAP_GONE;	// from here on *out is the place it had: the next zone takes it
		}
	}
	return m;
}
