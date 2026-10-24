// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Zone sharing's codeplug side (CONFIG_ZONE_SHARE, docs/spec/zone-share.md 4 and 5): a zone or one channel made into a
 * share record, and a received record written into the codeplug. Every codeplug write of zone sharing is here, so the
 * import can move to the zone edit calls in one place.
 *
 * Runs where codeplug writes run (the housekeeping task through storage_job_post); never on the radio task. The
 * caller saves the import list (settings_t.zs_import) it passes in.
 *
 * Import rules:
 *   - channels whose receive frequency is outside the radio's bands are left out; none left: refused
 *   - every imported channel is receive only, whatever the sender's was; the user turns transmit on per channel
 *   - contacts are matched by ID and type, and an existing contact keeps its name; missing ones are added with the
 *     sender's name; a record naming one contact twice gets one
 *   - the same zone from the same sender again (same zone ID) replaces the earlier import: its zone is rewritten in
 *     place and the channels it no longer uses are deleted, unless another zone holds them
 *   - a new zone whose name is taken gets " 2", " 3" ... (shortened to fit 16 characters)
 *   - anything that fails part way is undone: channels and contacts added by this import are deleted again
 */

#ifndef RFOUNDRY_ZONE_IMPORT_H
#define RFOUNDRY_ZONE_IMPORT_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug/codeplug.h"
#include "settings/settings.h"
#include "dmr/zoneShare.h"

typedef enum
{
	ZI_OK,
	ZI_BAD_RECORD,						// not a valid record
	ZI_NONE_IN_BAND,					// no channel the radio can receive
	ZI_NO_ZONE,							// no room for another zone, or zones aren't stored and can't be
	ZI_ZONE_FULL,						// a channel record: the chosen zone has CODEPLUG_ZONE_MAX channels
	ZI_NO_CHANNELS,						// not enough free channel numbers
	ZI_NO_CONTACTS,						// not enough free contacts
	ZI_FLASH,							// a codeplug write failed (what this import wrote was undone)
} zone_import_status_t;

typedef struct
{
	uint8_t  channels;					// written
	uint8_t  out_of_band;				// left out
	uint8_t  contacts_added;
	uint8_t  contacts_matched;			// already there (names kept)
	bool     updated;					// replaced an earlier import of this zone
	uint16_t zone;						// the zone's index
	char     name[CODEPLUG_NAME_LEN + 1];	// the zone's name here
} zone_import_result_t;

// The import list key of a zone from a sender: never 0
uint32_t zone_import_key(uint32_t sender, uint32_t zone_id);
// A receive frequency the radio can tune (board_in_band without transmit)
bool zone_import_in_band(const zsChannel_t *ch);

// A zone record into the codeplug. imports: settings_t.zs_import (SETTINGS_ZS_IMPORTS entries), updated on success:
// the caller saves it
zone_import_status_t zone_import_zone(const uint8_t *record, int length, uint32_t sender,
				      settings_zs_import_t imports[SETTINGS_ZS_IMPORTS], zone_import_result_t *res);
// A channel record into the codeplug, added to the end of zone `zone` (0..codeplug_zone_count()-1)
zone_import_status_t zone_import_channel(const uint8_t *record, int length, uint16_t zone, zone_import_result_t *res);

// The share record of zone `zone`: its channels in order while the record has room (ZS_RECORD_MAX), each with its
// talk contact. The record's length (0: none, or no channel fits); *included, when not NULL, the channels in it
int zone_export_zone(uint16_t zone, uint8_t *out, int size, uint8_t *included);
// The share record of channel `number` (ZS_FLAG_CHANNEL)
int zone_export_channel(uint16_t number, uint8_t *out, int size);

#endif
