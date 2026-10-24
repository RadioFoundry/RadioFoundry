// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone editing (zone_edit.h): read the zone, change it in RAM, write it back, all under the storage lock (recursive:
// the codeplug calls take it too). The codeplug format is unchanged: these are codeplug_set_zone / _delete_zone.
#include <string.h>
#include "codeplug/zone_edit.h"
#include "storage/storage.h"

int zone_list_find(const zone_t *z, uint16_t channel)
{
	for (uint16_t i = 0; i < z->count; i++) {
		if (z->channels[i] == channel) {
			return i;
		}
	}
	return -1;
}

zone_edit_result_t zone_list_insert(zone_t *z, uint16_t pos, uint16_t channel, uint16_t cap)
{
	if (pos > z->count || channel == 0) {
		return ZONE_EDIT_NO_CHANNEL;
	}
	if (zone_list_find(z, channel) >= 0) {
		return ZONE_EDIT_IN_ZONE;
	}
	if (z->count >= cap || z->count >= CODEPLUG_ZONE_MAX) {
		return ZONE_EDIT_FULL;
	}
	memmove(&z->channels[pos + 1u], &z->channels[pos], (size_t)(z->count - pos) * sizeof(z->channels[0]));
	z->channels[pos] = channel;
	z->count++;
	return ZONE_EDIT_OK;
}

zone_edit_result_t zone_list_remove(zone_t *z, uint16_t pos)
{
	if (pos >= z->count) {
		return ZONE_EDIT_NO_CHANNEL;
	}
	z->count--;
	memmove(&z->channels[pos], &z->channels[pos + 1u], (size_t)(z->count - pos) * sizeof(z->channels[0]));
	return ZONE_EDIT_OK;
}

zone_edit_result_t zone_list_move(zone_t *z, uint16_t pos, int dir)
{
	if (pos >= z->count) {
		return ZONE_EDIT_NO_CHANNEL;
	}
	int to = (int)pos + (dir < 0 ? -1 : 1);
	if (to < 0 || to >= (int)z->count) {
		return ZONE_EDIT_EDGE;
	}
	uint16_t c = z->channels[pos];
	z->channels[pos] = z->channels[to];
	z->channels[to] = c;
	return ZONE_EDIT_OK;
}

void zone_copy_name(char out[CODEPLUG_NAME_LEN + 1], const char *name)
{
	size_t n = strlen(name);
	if (n > CODEPLUG_NAME_LEN - 2u) {
		n = CODEPLUG_NAME_LEN - 2u;
	}
	memcpy(out, name, n);
	memcpy(out + n, " 2", 3);
}

// The zone every call works on: one at a time (the storage lock), off the callers' stacks
static zone_t work;

static zone_edit_result_t write_back(uint16_t zone)
{
	return codeplug_set_zone(zone, &work) ? ZONE_EDIT_OK : ZONE_EDIT_FAILED;
}

// Read zone into work; the caller holds the storage lock
static zone_edit_result_t load(uint16_t zone)
{
	if (zone >= codeplug_zone_count() || !codeplug_zone(zone, &work)) {
		return ZONE_EDIT_NO_ZONE;
	}
	return ZONE_EDIT_OK;
}

// Room for one more zone. Without stored zones the radio shows one virtual zone ("All channels"): it is stored first,
// so the user keeps the zone they saw and the new one comes after it
static zone_edit_result_t make_room(void)
{
	if (!codeplug_zones_stored()) {
		if (load(0) != ZONE_EDIT_OK || work.count == 0) {
			return ZONE_EDIT_OK;	// nothing to keep (no channels): the new zone is the first
		}
		if (write_back(0) != ZONE_EDIT_OK) {
			return ZONE_EDIT_FAILED;
		}
	}
	return codeplug_zone_count() >= CODEPLUG_ZONES_MAX ? ZONE_EDIT_ZONES_FULL : ZONE_EDIT_OK;
}

// work, written as a new zone after make_room
static zone_edit_result_t create_locked(uint16_t *index)
{
	if (work.name[0] == '\0') {
		return ZONE_EDIT_NO_NAME;
	}
	if (work.count > codeplug_zone_capacity()) {
		return ZONE_EDIT_FULL;
	}
	zone_edit_result_t r = write_back(codeplug_zones_stored() ? codeplug_zone_count() : 0);
	if (r == ZONE_EDIT_OK && index != NULL) {
		*index = codeplug_zone_written();
	}
	return r;
}

zone_edit_result_t zone_edit_create(const zone_t *z, uint16_t *index)
{
	storage_lock();
	zone_edit_result_t r = make_room();
	if (r == ZONE_EDIT_OK) {
		work = *z;
		work.name[CODEPLUG_NAME_LEN] = '\0';
		r = create_locked(index);
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_insert(uint16_t zone, uint16_t pos, uint16_t channel)
{
	channel_t ch;
	storage_lock();
	zone_edit_result_t r = load(zone);
	if (r == ZONE_EDIT_OK) {
		r = codeplug_channel(channel, &ch) ? zone_list_insert(&work, pos, channel, codeplug_zone_capacity()) :
						     ZONE_EDIT_NO_CHANNEL;
	}
	if (r == ZONE_EDIT_OK) {
		r = write_back(zone);
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_remove(uint16_t zone, uint16_t pos)
{
	storage_lock();
	zone_edit_result_t r = load(zone);
	if (r == ZONE_EDIT_OK) {
		r = zone_list_remove(&work, pos);
	}
	if (r == ZONE_EDIT_OK) {
		r = write_back(zone);
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_move(uint16_t zone, uint16_t pos, int dir)
{
	storage_lock();
	zone_edit_result_t r = load(zone);
	if (r == ZONE_EDIT_OK) {
		r = zone_list_move(&work, pos, dir);
	}
	if (r == ZONE_EDIT_OK) {
		r = write_back(zone);
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_add_new(uint16_t zone, uint16_t pos, uint16_t number, const channel_t *ch)
{
	channel_t old;
	storage_lock();
	zone_edit_result_t r = load(zone);
	if (r == ZONE_EDIT_OK && (number == 0 || codeplug_channel(number, &old))) {
		r = ZONE_EDIT_NO_FREE;		// not a free number: never overwrite a channel
	}
	if (r == ZONE_EDIT_OK) {
		// The zone's checks first, so a full zone leaves no channel behind (a free number is in no zone)
		r = pos > work.count ? ZONE_EDIT_NO_CHANNEL : work.count >= codeplug_zone_capacity() ? ZONE_EDIT_FULL :
										      ZONE_EDIT_OK;
	}
	if (r == ZONE_EDIT_OK && !codeplug_set_channel(number, ch)) {
		r = ZONE_EDIT_FAILED;
	}
	if (r == ZONE_EDIT_OK) {
		// Read the zone again: writing the channel can turn the built-in set into a stored codeplug, and the virtual
		// zone (every channel) holds the new one already, at the end
		r = load(zone);
		int had = zone_list_find(&work, number);
		if (r == ZONE_EDIT_OK && had >= 0) {
			zone_list_remove(&work, (uint16_t)had);
		}
	}
	if (r == ZONE_EDIT_OK) {
		r = zone_list_insert(&work, pos <= work.count ? pos : work.count, number, codeplug_zone_capacity());
	}
	if (r == ZONE_EDIT_OK) {
		r = write_back(zone);
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_copy(uint16_t zone, uint16_t *index)
{
	storage_lock();
	zone_edit_result_t r = make_room();	// first: it can use work
	if (r == ZONE_EDIT_OK) {
		r = load(zone);
	}
	if (r == ZONE_EDIT_OK) {
		char name[CODEPLUG_NAME_LEN + 1];
		zone_copy_name(name, work.name);
		strcpy(work.name, name);
		r = create_locked(index);
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_rename(uint16_t zone, const char *name)
{
	if (name[0] == '\0') {
		return ZONE_EDIT_NO_NAME;
	}
	storage_lock();
	zone_edit_result_t r = load(zone);
	if (r == ZONE_EDIT_OK) {
		strncpy(work.name, name, CODEPLUG_NAME_LEN);
		work.name[CODEPLUG_NAME_LEN] = '\0';
		r = write_back(zone);
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_delete(uint16_t zone)
{
	storage_lock();
	zone_edit_result_t r = zone < codeplug_zone_count() && codeplug_zones_stored() ? ZONE_EDIT_OK : ZONE_EDIT_NO_ZONE;
	if (r == ZONE_EDIT_OK && !codeplug_delete_zone(zone)) {
		r = ZONE_EDIT_FAILED;
	}
	storage_unlock();
	return r;
}

zone_edit_result_t zone_edit_apply(const zone_op_t *op, uint16_t *index)
{
	if (index != NULL) {
		*index = op->zone;
	}
	switch (op->kind) {
	case ZONE_OP_INSERT: return zone_edit_insert(op->zone, op->pos, op->channel);
	case ZONE_OP_REMOVE: return zone_edit_remove(op->zone, op->pos);
	case ZONE_OP_MOVE: return zone_edit_move(op->zone, op->pos, op->dir);
	case ZONE_OP_ADD_NEW: return zone_edit_add_new(op->zone, op->pos, op->channel, &op->ch);
	case ZONE_OP_COPY: return zone_edit_copy(op->zone, index);
	case ZONE_OP_RENAME: return zone_edit_rename(op->zone, op->name);
	case ZONE_OP_DELETE: return zone_edit_delete(op->zone);
	default: return ZONE_EDIT_FAILED;
	}
}

uint16_t zone_edit_follow(const zone_op_t *op, uint16_t added, uint16_t index)
{
	if (op->kind == ZONE_OP_COPY && index >= added) {
		return (uint16_t)(index + 1u);
	}
	if (op->kind == ZONE_OP_DELETE && index > op->zone) {
		return (uint16_t)(index - 1u);
	}
	return index;
}
