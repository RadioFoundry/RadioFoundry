// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The saved zone indexes across a power cut (zone_edit.h): a zone add or delete that moves zone indexes is noted in
// the settings before the codeplug is written, and the saved indexes (the zone in use, the roaming zones) are moved in
// the same settings record that clears the note. A power cut anywhere in between leaves the note, and the next power
// on finishes it from what the codeplug shows. Each settings save and the codeplug's zone write are atomic, so the
// saved indexes always match the zone list.
#include "codeplug/zone_edit.h"
#include "settings/settings.h"

// The zone list's state the note compares: the stored zone count, 0 with only the built-in zone. An add makes it one
// more, a delete one less (the last stored zone deleted: 1 to 0)
static uint16_t zone_mark(void)
{
	return codeplug_zones_stored() ? codeplug_zone_count() : 0;
}

void zone_settle_begin(bool deleted, uint16_t index)
{
	settings_t *s = settings_begin();
	s->zone_jrn_kind = deleted ? SETTINGS_ZONE_JRN_DELETE : SETTINGS_ZONE_JRN_ADD;
	s->zone_jrn_index = index;
	s->zone_jrn_count = zone_mark();
	settings_end(0);
	settings_flush();
}

// One add or delete applied to an index, as zone_edit_map does for one log entry
static zone_map_t map_one(bool deleted, uint16_t at, uint16_t index, uint16_t *out)
{
	*out = index;
	if (!deleted) {
		*out = index >= at ? (uint16_t)(index + 1u) : index;
		return ZONE_MAP_KEPT;
	}
	if (index > at) {
		*out = (uint16_t)(index - 1u);
	}
	return index == at ? ZONE_MAP_GONE : ZONE_MAP_KEPT;
}

// Finish a note in s (inside settings_begin / _end): when the codeplug shows the change, move the saved indexes with
// the rules of a reload (ui.md 2.2); either way the note is cleared. True: s changed
static bool settle(settings_t *s)
{
	if (s->zone_jrn_kind == SETTINGS_ZONE_JRN_NONE) {
		return false;
	}
	bool deleted = s->zone_jrn_kind == SETTINGS_ZONE_JRN_DELETE;
	uint16_t at = s->zone_jrn_index;
	bool applied = zone_mark() != s->zone_jrn_count;
	s->zone_jrn_kind = SETTINGS_ZONE_JRN_NONE;
	s->zone_jrn_index = 0;
	s->zone_jrn_count = 0;
	if (!applied) {
		return true;
	}
	uint16_t out, n = codeplug_zone_count();
	if (map_one(deleted, at, s->zone_index, &out) == ZONE_MAP_GONE) {
		s->zone_pos = 0;	// the zone that took its place: on the same channel if it holds it, else its first
	}
	s->zone_index = n > 0 && out >= n ? (uint16_t)(n - 1u) : out;
	uint8_t bits[SETTINGS_ROAM_ZONE_BYTES] = { 0 };
	for (uint16_t z = 0; z < SETTINGS_ROAM_ZONE_BYTES * 8u; z++) {
		if ((s->roam_zones[z / 8u] & (1u << (z % 8u))) != 0 && map_one(deleted, at, z, &out) == ZONE_MAP_KEPT &&
		    out < SETTINGS_ROAM_ZONE_BYTES * 8u) {
			bits[out / 8u] |= (uint8_t)(1u << (out % 8u));	// a deleted roaming zone's flag is dropped
		}
	}
	for (uint16_t i = 0; i < SETTINGS_ROAM_ZONE_BYTES; i++) {
		s->roam_zones[i] = bits[i];
	}
	return true;
}

void zone_edit_settle(void)
{
	settings_t *s = settings_begin();
	bool changed = settle(s);
	settings_end(0);
	if (changed) {
		settings_flush();
	}
}
