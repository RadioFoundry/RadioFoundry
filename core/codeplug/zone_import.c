// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone sharing's codeplug side (zone_import.h, docs/spec/zone-share.md 4 and 5)
#include "config.h"
#if CONFIG_ZONE_SHARE	// the whole file
#include <stdio.h>
#include <string.h>
#include "codeplug/zone_import.h"
#include "board.h"

// The import's working state: static, as the housekeeping task's stack is small. One import or export at a time (the
// storage job queue runs one job at a time)
static zone_t old_zone, new_zone;
static uint16_t added_ch[CODEPLUG_ZONE_MAX];		// channel numbers this import wrote, for the undo
static uint8_t added_ch_count;
static uint16_t contact_index[ZS_CONTACTS_MAX];		// the record's contacts here (1-based codeplug index)
static uint16_t added_ct[ZS_CONTACTS_MAX];			// contacts this import added
static uint8_t added_ct_count;
static zsRecord_t rec;

static uint32_t fnv1a(uint32_t h, const uint8_t *p, int n)
{
	for (int i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

uint32_t zone_import_key(uint32_t sender, uint32_t zone_id)
{
	uint8_t b[8] = {
		(uint8_t)(sender >> 24), (uint8_t)(sender >> 16), (uint8_t)(sender >> 8), (uint8_t)sender,
		(uint8_t)(zone_id >> 24), (uint8_t)(zone_id >> 16), (uint8_t)(zone_id >> 8), (uint8_t)zone_id,
	};
	uint32_t h = fnv1a(2166136261u, b, 8);

	return (h != 0) ? h : 1;
}

bool zone_import_in_band(const zsChannel_t *ch)
{
	return board_in_band(ch->rxHz, false);
}

static bool name_eq(const char *a, const char *b)
{
	return strncmp(a, b, CODEPLUG_NAME_LEN) == 0;
}

// --- Undo ---

static void undo(void)
{
	while (added_ch_count > 0) {
		codeplug_delete_channel(added_ch[--added_ch_count]);
	}
	while (added_ct_count > 0) {
		codeplug_delete_contact(added_ct[--added_ct_count]);
	}
}

// --- Contacts ---

// The record's contacts that channels the radio can receive talk to (needed[k], 1-based references from 1)
static bool needed[ZS_CONTACTS_MAX];

static int mark_needed(void)
{
	zsChannel_t z;
	int n = 0;

	memset(needed, 0, sizeof(needed));
	for (int i = 0; i < rec.channels; i++) {
		if (zsChannelAt(&rec, i, &z) && z.dmr && z.contact != 0 && zone_import_in_band(&z) && !needed[z.contact - 1]) {
			needed[z.contact - 1] = true;
			n++;
		}
	}
	return n;
}

// Every contact the imported channels need mapped to one here: matched by ID and type (the user's name kept), else
// added in the lowest free slots. One pass over the contact records (up to 24 KB of flash)
static zone_import_status_t map_contacts(zone_import_result_t *res)
{
	memset(contact_index, 0, sizeof(contact_index));
	if (rec.contacts == 0 || mark_needed() == 0) {
		return ZI_OK;
	}
	uint16_t free_slots[ZS_CONTACTS_MAX];
	int free_count = 0;
	zsContact_t c;

	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		contact_t have;

		if (!codeplug_contact(i, &have)) {
			if (free_count < ZS_CONTACTS_MAX) {
				free_slots[free_count++] = i;
			}
			continue;
		}
		for (int k = 0; k < rec.contacts; k++) {
			if (needed[k] && (contact_index[k] == 0) && zsContactAt(&rec, k, &c) && (c.id == have.id) && (c.type == have.type)) {
				contact_index[k] = i;
			}
		}
	}
	int next_free = 0;

	for (int k = 0; k < rec.contacts; k++) {
		if (!needed[k]) {
			continue;
		}
		if (contact_index[k] != 0) {
			res->contacts_matched++;
			continue;
		}
		if (!zsContactAt(&rec, k, &c)) {
			return ZI_BAD_RECORD;
		}
		// The same contact twice in the record: the first one's
		for (int j = 0; j < k; j++) {
			zsContact_t d;

			if (needed[j] && zsContactAt(&rec, j, &d) && (d.id == c.id) && (d.type == c.type)) {
				contact_index[k] = contact_index[j];
				break;
			}
		}
		if (contact_index[k] != 0) {
			continue;
		}
		if (next_free >= free_count) {
			return ZI_NO_CONTACTS;
		}
		contact_t nc;

		memset(&nc, 0, sizeof(nc));
		if (c.name[0] != '\0') {
			strncpy(nc.name, c.name, CODEPLUG_NAME_LEN);
		} else {
			snprintf(nc.name, sizeof(nc.name), "%s %lu", (c.type == ZS_CONTACT_PRIVATE) ? "ID" : "TG",
				 (unsigned long)c.id);
		}
		nc.id = c.id;
		nc.type = c.type;
		nc.timeslot = c.timeslot;
		uint16_t slot = free_slots[next_free++];

		if (!codeplug_set_contact(slot, &nc)) {
			return ZI_FLASH;
		}
		added_ct[added_ct_count++] = slot;
		contact_index[k] = slot;
		res->contacts_added++;
	}
	return ZI_OK;
}

// --- Channels ---

static uint8_t power_here(uint8_t p)
{
	uint8_t n = (board.power_count > 0) ? board.power_count : 1;

	switch (p) {
	case ZS_POWER_LOW:
		return 0;
	case ZS_POWER_MID:
		return (uint8_t)(n / 2);
	case ZS_POWER_HIGH:
		return (uint8_t)(n - 1);
	default:
		return CODEPLUG_POWER_DEFAULT;
	}
}

static uint8_t power_on_air(uint8_t p)
{
	if (p == CODEPLUG_POWER_DEFAULT || board.power_count == 0) {
		return ZS_POWER_DEFAULT;
	}
	if (p == 0) {
		return ZS_POWER_LOW;
	}
	return (p + 1u >= board.power_count) ? ZS_POWER_HIGH : ZS_POWER_MID;
}

static void channel_here(const zsChannel_t *z, int position, channel_t *ch)
{
	memset(ch, 0, sizeof(*ch));
	if (z->name[0] != '\0') {
		strncpy(ch->name, z->name, CODEPLUG_NAME_LEN);
	} else {
		snprintf(ch->name, sizeof(ch->name), "Channel %d", position + 1);
	}
	ch->rx_hz = z->rxHz;
	ch->tx_hz = (uint32_t)((int64_t)z->rxHz + z->txOffsetHz);
	ch->mode = z->dmr ? RADIO_MODE_DMR : RADIO_MODE_FM;
	ch->bw = z->wide ? RADIO_BW_25K : RADIO_BW_12K5;
	ch->power = power_here(z->power);
	ch->rx_only = true;// always: the user turns transmit on (zone-share.md 5.3)
	ch->color_code = z->dmr ? z->colorCode : 1;
	ch->timeslot = z->dmr ? z->timeslot : 1;
	ch->dmo = z->dmr && z->dmo;
	if (!z->dmr) {
		ch->rx_tone = (subtone_t){ z->rxToneType, z->rxTone };
		ch->tx_tone = (subtone_t){ z->txToneType, z->txTone };
	}
	ch->contact = (z->dmr && z->contact != 0) ? contact_index[z->contact - 1] : 0;
}

// The record's channels the radio can receive, written to free channel numbers and listed in new_zone
static zone_import_status_t write_channels(zone_import_result_t *res)
{
	zsChannel_t z;
	channel_t ch;

	for (int i = 0; i < rec.channels; i++) {
		if (!zsChannelAt(&rec, i, &z)) {
			return ZI_BAD_RECORD;
		}
		if (!zone_import_in_band(&z)) {
			res->out_of_band++;
			continue;
		}
		channel_here(&z, i, &ch);
		if (new_zone.count >= CODEPLUG_ZONE_MAX) {
			return ZI_ZONE_FULL;
		}
		uint16_t n = codeplug_free_channel();

		if (n == 0) {
			return ZI_NO_CHANNELS;
		}
		if (!codeplug_set_channel(n, &ch)) {
			return ZI_FLASH;
		}
		added_ch[added_ch_count++] = n;
		new_zone.channels[new_zone.count++] = n;
		res->channels++;
	}
	return (res->channels > 0) ? ZI_OK : ZI_NONE_IN_BAND;
}

// --- Zones ---

static int find_zone(const char *name, int skip)
{
	zone_t z;
	uint16_t n = codeplug_zone_count();

	for (uint16_t i = 0; i < n; i++) {
		if ((int)i != skip && codeplug_zone(i, &z) && name_eq(z.name, name)) {
			return i;
		}
	}
	return -1;
}

// A name no other zone has: the record's, else with " 2" ... " 99"
static void unique_name(const char *want, int skip, char out[CODEPLUG_NAME_LEN + 1])
{
	char base[CODEPLUG_NAME_LEN + 1];

	strncpy(base, (want[0] != '\0') ? want : "Shared", CODEPLUG_NAME_LEN);
	base[CODEPLUG_NAME_LEN] = '\0';
	strcpy(out, base);
	for (int n = 2; n < 100 && find_zone(out, skip) >= 0; n++) {
		char suffix[4];
		int sl = snprintf(suffix, sizeof(suffix), " %d", n);
		int bl = (int)strlen(base);

		if (bl + sl > CODEPLUG_NAME_LEN) {
			bl = CODEPLUG_NAME_LEN - sl;
		}
		while (bl > 0 && base[bl - 1] == ' ') {
			bl--;// "Sixteen chars 2", not "Sixteen chars  2"
		}
		memcpy(out, base, (size_t)bl);
		memcpy(out + bl, suffix, (size_t)sl + 1);
	}
}

// Channel n is in a zone other than skip
static bool in_other_zone(uint16_t n, int skip)
{
	uint16_t count = codeplug_zone_count();

	for (uint16_t i = 0; i < count; i++) {
		if ((int)i == skip || !codeplug_zone(i, &new_zone)) {
			continue;
		}
		for (uint16_t k = 0; k < new_zone.count; k++) {
			if (new_zone.channels[k] == n) {
				return true;
			}
		}
	}
	return false;
}

// --- Updates: which of the earlier copy's channels are the import's, untouched ---

// A channel's hash: its number (0: content only) and every field, so any change the user makes shows
static uint32_t channel_hash(uint16_t number, const channel_t *ch)
{
	uint8_t b[40];
	int n = 0;

	b[n++] = (uint8_t)(number >> 8);
	b[n++] = (uint8_t)number;
	for (int i = 0; i < 4; i++) {
		b[n++] = (uint8_t)(ch->rx_hz >> (8 * i));
		b[n++] = (uint8_t)(ch->tx_hz >> (8 * i));
		b[n++] = (uint8_t)(ch->dmr_id >> (8 * i));
	}
	b[n++] = ch->mode;
	b[n++] = ch->bw;
	b[n++] = ch->rx_tone.type;
	b[n++] = (uint8_t)ch->rx_tone.value;
	b[n++] = (uint8_t)(ch->rx_tone.value >> 8);
	b[n++] = ch->tx_tone.type;
	b[n++] = (uint8_t)ch->tx_tone.value;
	b[n++] = (uint8_t)(ch->tx_tone.value >> 8);
	b[n++] = ch->power;
	b[n++] = ch->color_code;
	b[n++] = ch->timeslot;
	b[n++] = (uint8_t)ch->contact;
	b[n++] = (uint8_t)(ch->contact >> 8);
	b[n++] = (uint8_t)ch->listen_list;
	b[n++] = (uint8_t)(ch->listen_list >> 8);
	b[n++] = (uint8_t)ch->tot_s;
	b[n++] = (uint8_t)(ch->tot_s >> 8);
	b[n++] = ch->alias_tx;
	b[n++] = (uint8_t)((ch->rx_only ? 1 : 0) | (ch->dmo ? 2 : 0) | (ch->scan_skip ? 4 : 0) | (ch->vox ? 8 : 0) |
			   (ch->no_beep ? 16 : 0) | (ch->no_eco ? 32 : 0) | (ch->roam ? 64 : 0));
	uint32_t h = fnv1a(2166136261u, b, n);

	return fnv1a(h, (const uint8_t *)ch->name, (int)strnlen(ch->name, CODEPLUG_NAME_LEN));
}

// The XOR of channel_hash over list[0..n): the import's signature (order doesn't matter)
static uint32_t list_sig(const uint16_t *list, uint16_t n)
{
	uint32_t sig = 0;
	channel_t ch;

	for (uint16_t k = 0; k < n; k++) {
		if (codeplug_channel(list[k], &ch)) {
			sig ^= channel_hash(list[k], &ch);
		}
	}
	return sig;
}

static bool replaced[CODEPLUG_ZONE_MAX];	// old_zone.channels[k] is the import's, untouched: the update replaces it
static uint32_t new_hash[CODEPLUG_ZONE_MAX];	// the new channels' content hashes

// Which of the earlier copy's channels the update replaces. Only the import's channels the user left as they were:
// - the zone still starts with the channels the import wrote, unchanged (their signature matches): those, and
//   everything after them is the user's;
// - otherwise (the user moved, changed or removed one): only the channels identical to one the new copy brings, so
//   nothing the user added or changed is lost, at worst an outdated channel stays
// The rest stay in the zone, after the new channels
static void mark_replaced(const settings_zs_import_t *earlier)
{
	channel_t ch;
	uint16_t imported = new_zone.count;

	memset(replaced, 0, sizeof(replaced));
	if (earlier->count > 0 && earlier->count <= old_zone.count &&
	    list_sig(old_zone.channels, earlier->count) == earlier->sig) {
		for (uint16_t k = 0; k < earlier->count; k++) {
			replaced[k] = true;
		}
		return;
	}
	for (uint16_t i = 0; i < imported; i++) {
		new_hash[i] = codeplug_channel(new_zone.channels[i], &ch) ? channel_hash(0, &ch) : 0;
	}
	for (uint16_t k = 0; k < old_zone.count; k++) {
		if (!codeplug_channel(old_zone.channels[k], &ch)) {
			continue;
		}
		uint32_t h = channel_hash(0, &ch);

		for (uint16_t i = 0; i < imported && !replaced[k]; i++) {
			replaced[k] = new_hash[i] == h;
		}
	}
}

// The earlier copy's channels the update doesn't replace, after the new ones (as many as fit)
static void keep_users(void)
{
	for (uint16_t k = 0; k < old_zone.count && new_zone.count < CODEPLUG_ZONE_MAX; k++) {
		if (!replaced[k]) {
			new_zone.channels[new_zone.count++] = old_zone.channels[k];
		}
	}
}

static void remember(settings_zs_import_t imports[SETTINGS_ZS_IMPORTS], uint32_t key, const char *name, uint32_t sig,
		     uint8_t count)
{
	int at = SETTINGS_ZS_IMPORTS - 1;// the oldest goes when the list is full

	for (int i = 0; i < SETTINGS_ZS_IMPORTS; i++) {
		if (imports[i].key == key) {
			at = i;
			break;
		}
	}
	// Newest first
	for (int i = at; i > 0; i--) {
		imports[i] = imports[i - 1];
	}
	memset(&imports[0], 0, sizeof(imports[0]));
	imports[0].key = key;
	imports[0].sig = sig;
	imports[0].count = count;
	memcpy(imports[0].zone, name, strnlen(name, sizeof(imports[0].zone)));
}

static void begin(zone_import_result_t *res)
{
	memset(res, 0, sizeof(*res));
	added_ch_count = 0;
	added_ct_count = 0;
	memset(&new_zone, 0, sizeof(new_zone));
}

zone_import_status_t zone_import_zone(const uint8_t *record, int length, uint32_t sender,
				      settings_zs_import_t imports[SETTINGS_ZS_IMPORTS], zone_import_result_t *res)
{
	begin(res);
	if (!zsParse(record, length, &rec) || (rec.flags & ZS_FLAG_CHANNEL)) {
		return ZI_BAD_RECORD;
	}
	uint32_t key = zone_import_key(sender, rec.zoneId);
	int target = -1;
	int earlier = -1;

	// An earlier import of this zone, if its zone is still there under the name it got
	for (int i = 0; i < SETTINGS_ZS_IMPORTS && target < 0; i++) {
		if (imports[i].key == key) {
			char name[CODEPLUG_NAME_LEN + 1];

			memcpy(name, imports[i].zone, CODEPLUG_NAME_LEN);
			name[CODEPLUG_NAME_LEN] = '\0';
			target = codeplug_zones_stored() ? find_zone(name, -1) : -1;
			earlier = i;
		}
	}
	bool stored = codeplug_zones_stored();

	if (target < 0 && stored && codeplug_zone_count() >= CODEPLUG_ZONES_MAX) {
		return ZI_NO_ZONE;
	}
	// No zones stored yet: the virtual "All channels" zone (the first CODEPLUG_ZONE_MAX channels, as the user sees it)
	// is stored as zone 0 first, so adding ours doesn't replace it
	if (!stored) {
		if (!codeplug_zone(0, &old_zone)) {
			return ZI_NO_ZONE;
		}
	} else if (target >= 0 && !codeplug_zone(target, &old_zone)) {
		return ZI_FLASH;
	}
	zone_import_status_t st = map_contacts(res);

	if (st == ZI_OK) {
		st = write_channels(res);
	}
	if (st != ZI_OK) {
		undo();
		return st;
	}
	// What a later update recognizes as this import's own channels (they lead the zone)
	uint32_t sig = list_sig(new_zone.channels, new_zone.count);
	uint8_t count = (uint8_t)new_zone.count;

	if (target >= 0) {
		strncpy(new_zone.name, old_zone.name, CODEPLUG_NAME_LEN);
		mark_replaced(&imports[earlier]);
		keep_users();
	} else {
		unique_name(rec.name, -1, new_zone.name);
	}
	if (!stored) {
		// old_zone holds the virtual zone as read before ours were written
		if (!codeplug_set_zone(0, &old_zone)) {
			undo();
			return ZI_FLASH;
		}
	}
	uint16_t index = (target >= 0) ? (uint16_t)target : codeplug_zone_count();

	strcpy(res->name, new_zone.name);
	if (!codeplug_set_zone(index, &new_zone)) {
		if (!stored) {
			codeplug_delete_zone(0);
		}
		undo();
		return ZI_FLASH;
	}
	res->zone = index;
	res->updated = target >= 0;
	// The replaced channels that no zone holds now go (in_other_zone reuses new_zone: it is written)
	if (target >= 0) {
		for (uint16_t k = 0; k < old_zone.count; k++) {
			if (replaced[k] && !in_other_zone(old_zone.channels[k], -1)) {
				codeplug_delete_channel(old_zone.channels[k]);
			}
		}
	}
	remember(imports, key, res->name, sig, count);
	return ZI_OK;
}

zone_import_status_t zone_import_channel(const uint8_t *record, int length, uint16_t zone, zone_import_result_t *res)
{
	begin(res);
	if (!zsParse(record, length, &rec) || !(rec.flags & ZS_FLAG_CHANNEL) || rec.channels != 1) {
		return ZI_BAD_RECORD;
	}
	bool stored = codeplug_zones_stored();

	// Without stored zones the only zone is the virtual one, which lists every channel by itself
	if ((!stored && zone != 0) || !codeplug_zone(zone, &old_zone)) {
		return ZI_NO_ZONE;
	}
	if (stored && old_zone.count >= CODEPLUG_ZONE_MAX) {
		return ZI_ZONE_FULL;
	}
	zone_import_status_t st = map_contacts(res);

	if (st == ZI_OK) {
		st = write_channels(res);
	}
	if (st != ZI_OK) {
		undo();
		return st;
	}
	if (stored) {
		old_zone.channels[old_zone.count++] = new_zone.channels[0];
		if (!codeplug_set_zone(zone, &old_zone)) {
			undo();
			return ZI_FLASH;
		}
	}
	res->zone = zone;
	strncpy(res->name, old_zone.name, CODEPLUG_NAME_LEN);
	return ZI_OK;
}

// --- Export ---

static void channel_on_air(const channel_t *ch, zsChannel_t *z)
{
	memset(z, 0, sizeof(*z));
	strncpy(z->name, ch->name, ZS_NAME_MAX);
	z->dmr = ch->mode == RADIO_MODE_DMR;
	z->wide = ch->bw == RADIO_BW_25K;
	z->rxOnly = ch->rx_only;
	z->power = power_on_air(ch->power);
	z->rxHz = ch->rx_hz;
	z->txOffsetHz = (int32_t)((int64_t)ch->tx_hz - ch->rx_hz);
	if (ch->tx_hz == 0) {
		z->txOffsetHz = 0;
	}
	if (z->dmr) {
		z->colorCode = ch->color_code & 0x0F;
		z->timeslot = (ch->timeslot == 2) ? 2 : 1;
		z->dmo = ch->dmo;
	} else {
		if (zsToneValid(ch->rx_tone.type, ch->rx_tone.value)) {
			z->rxToneType = ch->rx_tone.type;
			z->rxTone = ch->rx_tone.value;
		}
		if (zsToneValid(ch->tx_tone.type, ch->tx_tone.value)) {
			z->txToneType = ch->tx_tone.type;
			z->txTone = ch->tx_tone.value;
		}
	}
}

// The channel's talk contact on air: false when it has none (or it can't be read)
static bool contact_on_air(const channel_t *ch, zsContact_t *c, uint16_t *index)
{
	contact_t have;

	*index = (ch->contact != 0) ? ch->contact : codeplug_channel_talk_contact(ch);
	if (*index == 0 || !codeplug_contact(*index, &have) || have.id == 0 || have.id > 0xFFFFFF ||
	    have.type > CONTACT_ALL_CALL) {
		return false;
	}
	memset(c, 0, sizeof(*c));
	c->type = have.type;
	c->timeslot = (have.timeslot <= 2) ? have.timeslot : 0;
	c->id = have.id;
	strncpy(c->name, have.name, ZS_NAME_MAX);
	return true;
}

// A stable zone ID from the name: the same zone sent again updates the receiver's import
static uint32_t zone_id_of(const char *name, uint8_t flags)
{
	uint32_t h = fnv1a(2166136261u, (const uint8_t *)name, (int)strnlen(name, CODEPLUG_NAME_LEN));

	h = fnv1a(h, &flags, 1);
	return (h != 0) ? h : 1;
}

// Channels list[0..n) as a record: contacts first, then channels in order while they fit. Returns the length
static int export_list(const char *name, uint8_t flags, const uint16_t *list, uint16_t n, uint8_t *out, int size,
		       uint8_t *included)
{
	static uint16_t ct_src[ZS_CONTACTS_MAX];	// codeplug index of each record contact
	static uint8_t ch_ct[CODEPLUG_ZONE_MAX];	// each included channel's record contact (1-based), 0 none
	static uint16_t ch_num[CODEPLUG_ZONE_MAX];
	uint8_t contacts = 0, channels = 0;
	int budget = (size < ZS_RECORD_MAX) ? size : ZS_RECORD_MAX;
	int used = zsHeaderSize(name);
	channel_t ch;
	zsChannel_t z;
	zsContact_t c;

	// Choose: each channel in order with its contact, while the whole fits
	for (uint16_t i = 0; i < n && channels < ZS_CHANNELS_MAX; i++) {
		if (!codeplug_channel(list[i], &ch)) {
			continue;
		}
		channel_on_air(&ch, &z);
		int need = 0;
		uint8_t ref = 0;
		uint16_t idx = 0;

		if (z.dmr && contact_on_air(&ch, &c, &idx)) {
			for (uint8_t k = 0; k < contacts; k++) {
				if (ct_src[k] == idx) {
					ref = (uint8_t)(k + 1);
				}
			}
			if (ref == 0) {
				if (contacts >= ZS_CONTACTS_MAX) {
					break;
				}
				need += zsContactSize(&c);
				ref = (uint8_t)(contacts + 1);
			}
			z.contact = ref;
		}
		need += zsChannelSize(&z);
		if (used + need > budget) {
			break;
		}
		used += need;
		if (ref == contacts + 1) {
			ct_src[contacts++] = idx;
		}
		ch_ct[channels] = z.dmr ? ref : 0;
		ch_num[channels++] = list[i];
	}
	if (included != NULL) {
		*included = channels;
	}
	if (channels == 0) {
		return 0;
	}
	zsWriter_t w;

	zsWriterBegin(&w, out, budget, zone_id_of(name, flags), flags, name);
	for (uint8_t k = 0; k < contacts; k++) {
		channel_t probe;

		memset(&probe, 0, sizeof(probe));
		probe.contact = ct_src[k];
		uint16_t idx;

		if (!contact_on_air(&probe, &c, &idx)) {
			return 0;
		}
		zsWriterContact(&w, &c);
	}
	for (uint8_t i = 0; i < channels; i++) {
		if (!codeplug_channel(ch_num[i], &ch)) {
			return 0;
		}
		channel_on_air(&ch, &z);
		z.contact = ch_ct[i];
		zsWriterChannel(&w, &z);
	}
	return zsWriterEnd(&w);
}

int zone_export_zone(uint16_t zone, uint8_t *out, int size, uint8_t *included)
{
	if (included != NULL) {
		*included = 0;
	}
	// Its own copy: an export (the UI task) may run while an import job holds old_zone and new_zone
	static zone_t ex_zone;

	if (!codeplug_zone(zone, &ex_zone)) {
		return 0;
	}
	return export_list(ex_zone.name, 0, ex_zone.channels, ex_zone.count, out, size, included);
}

int zone_export_channel(uint16_t number, uint8_t *out, int size)
{
	channel_t ch;

	if (!codeplug_channel(number, &ch)) {
		return 0;
	}
	return export_list(ch.name, ZS_FLAG_CHANNEL, &number, 1, out, size, NULL);
}

#endif	// CONFIG_ZONE_SHARE
