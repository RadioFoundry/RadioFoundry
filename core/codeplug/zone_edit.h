// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone editing on top of the codeplug calls (codeplug.h): add, remove and move a channel in a zone, a new channel
// written and added in one step, a new zone, a zone copied, renamed or deleted. The radio's zone editor (ui.md 2.2)
// runs these from a storage job; anything else that writes zones (an import) calls the same ones.
//
// Each call reads the zone, changes it and writes it back under the storage lock, so calls from different tasks
// don't interleave. A write can take some 100 ms (a sector rewrite): never call from the UI task. The zone_list_*
// helpers only change a zone_t in RAM and are what the calls use.
#ifndef ZONE_EDIT_H
#define ZONE_EDIT_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

typedef enum {
	ZONE_EDIT_OK = 0,
	ZONE_EDIT_FAILED,	// the flash write failed, or no codeplug can be stored
	ZONE_EDIT_NO_ZONE,	// no zone with that index
	ZONE_EDIT_NO_CHANNEL,	// no channel at that position, or the channel number isn't stored
	ZONE_EDIT_FULL,		// the zone holds its most (codeplug_zone_capacity)
	ZONE_EDIT_ZONES_FULL,	// no room for another zone (CODEPLUG_ZONES_MAX)
	ZONE_EDIT_IN_ZONE,	// the channel is in the zone already
	ZONE_EDIT_EDGE,		// a move past the first or last position
	ZONE_EDIT_NO_NAME,	// an empty name
	ZONE_EDIT_NO_FREE,	// no free channel number
} zone_edit_result_t;

// --- A zone_t in RAM ---
// Position of channel number in z, -1 when it isn't there
int zone_list_find(const zone_t *z, uint16_t channel);
// Insert channel at pos (0..count; count appends). cap: the most the zone may hold
zone_edit_result_t zone_list_insert(zone_t *z, uint16_t pos, uint16_t channel, uint16_t cap);
zone_edit_result_t zone_list_remove(zone_t *z, uint16_t pos);
// Move the channel at pos one place: dir -1 toward the first, +1 toward the last
zone_edit_result_t zone_list_move(zone_t *z, uint16_t pos, int dir);
// The name of a copy: the name with " 2" appended, the name cut so both fit CODEPLUG_NAME_LEN
void zone_copy_name(char out[CODEPLUG_NAME_LEN + 1], const char *name);

// --- The codeplug ---
// A new zone holding z's channels. *index (may be NULL): where it landed, which need not be last
zone_edit_result_t zone_edit_create(const zone_t *z, uint16_t *index);
zone_edit_result_t zone_edit_insert(uint16_t zone, uint16_t pos, uint16_t channel);
zone_edit_result_t zone_edit_remove(uint16_t zone, uint16_t pos);
zone_edit_result_t zone_edit_move(uint16_t zone, uint16_t pos, int dir);
// Write ch at channel number (a free one, codeplug_free_channel) and insert it at pos in the zone. Checked before
// anything is written, so a refusal leaves no channel behind
zone_edit_result_t zone_edit_add_new(uint16_t zone, uint16_t pos, uint16_t number, const channel_t *ch);
// A new zone with the same channels, named zone_copy_name of it. *index: where it landed
zone_edit_result_t zone_edit_copy(uint16_t zone, uint16_t *index);
zone_edit_result_t zone_edit_rename(uint16_t zone, const char *name);
zone_edit_result_t zone_edit_delete(uint16_t zone);

// One of the calls above as data, for a caller that hands the work to another task (the storage job)
typedef enum {
	ZONE_OP_INSERT, ZONE_OP_REMOVE, ZONE_OP_MOVE, ZONE_OP_ADD_NEW, ZONE_OP_COPY, ZONE_OP_RENAME, ZONE_OP_DELETE,
} zone_op_kind_t;

typedef struct {
	uint8_t kind;		// zone_op_kind_t
	int8_t dir;		// MOVE
	uint16_t zone;		// the zone's index
	uint16_t pos;		// INSERT, REMOVE, MOVE, ADD_NEW: the position
	uint16_t channel;	// INSERT: the channel; ADD_NEW: the new channel's number
	char name[CODEPLUG_NAME_LEN + 1];	// RENAME
	channel_t ch;		// ADD_NEW
} zone_op_t;

// Run op. *index (may be NULL): COPY, the new zone's index; others, op->zone
zone_edit_result_t zone_edit_apply(const zone_op_t *op, uint16_t *index);
// Calls zone_edit_apply ran since start-up that wrote (OK), and the last one's result (the debug link's zone.edits,
// zone.edit_last)
void zone_edit_stats(uint32_t *applied, uint8_t *last);
// --- Zone indexes that moved ---
// A zone's index is its place in the list, so a new zone (it takes the lowest free slot, which can be ahead of others)
// moves the zones after it one on, and a deleted zone moves them one back. Whoever keeps a zone index (the zone in
// use, dual watch, the roaming zones, an open screen) follows it through this log: note zone_edit_shifts() when the
// index is taken, and later map it with zone_edit_map from that count to the count now.
//
// zone_edit_create, _copy and _delete add an entry. zone_edit_busy() is true from before such a write until its
// entry is in the log: a task that sees codeplug_serial() change waits while it is true, so it never reads the new
// zone list without the entry that explains it. Read codeplug_serial() first, then zone_edit_busy().
#define ZONE_SHIFT_LOG	8	// entries kept: a reader further behind gets ZONE_MAP_LOST

typedef enum {
	ZONE_MAP_KEPT = 0,	// *out: where the zone is now (maybe the same index)
	ZONE_MAP_GONE,		// deleted; *out: the index it had, moved on by later entries (now the next zone, or the
				// count when it was the last)
	ZONE_MAP_LOST,		// more than ZONE_SHIFT_LOG entries since `from`: find the zone again another way
} zone_map_t;

uint32_t zone_edit_shifts(void);
bool zone_edit_busy(void);
zone_map_t zone_edit_map(uint32_t from, uint32_t to, uint16_t index, uint16_t *out);
// zone_edit.c's side of the log (zone_shift.c)
void zone_shift_note(bool deleted, uint16_t index);
void zone_shift_busy(bool on);

// --- Saved zone indexes across a power cut (zone_settle.c) ---
// The settings hold zone indexes too (the zone in use, the roaming zones). zone_edit.c notes an add or delete in the
// settings before its codeplug write (zone_settle_begin) and settles it after (zone_edit_settle): the saved indexes
// are moved in the record that clears the note. Called at power on, zone_edit_settle finishes a note a power cut
// left, from what the codeplug shows. Under the storage lock or before the tasks start
void zone_settle_begin(bool deleted, uint16_t index);
void zone_edit_settle(void);

#endif
