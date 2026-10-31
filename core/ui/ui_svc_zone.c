// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls for zone sharing (docs/spec/zone-share.md): the record is made from the codeplug on the UI task
// (reads only) and handed to zone_share_send, which queues its fragments; the offer is read in place
// (zone_share_offer) and imported by a storage job on the housekeeping task, like every codeplug write of the UI.
#include "config.h"

#if CONFIG_ZONE_SHARE
#include <string.h>
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "codeplug/codeplug.h"
#include "codeplug/zone_import.h"
#include "dmr/zone_share_api.h"
#include "dmr/zoneShare.h"
#include "dmr/dmrDataService.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "hal_time.h"
#include "FreeRTOS.h"
static bool sent_group;			// the last send's destination is a talkgroup (for its result's name)

static void contact_name(uint32_t id, bool group, char *out, size_t len)
{
	contact_t c;
	out[0] = 0;
	if (codeplug_contact_find(id, group, &c)) {
		strncpy(out, c.name, len - 1);
		out[len - 1] = 0;
	}
}

uint8_t ui_svc_zone_send(uint16_t zone, uint16_t pos, bool one, uint32_t dst, bool group, uint8_t *total)
{
	static zone_t z;
	uint8_t included = 0;
	int len = 0;

	uint16_t num = pos;	// the channel, when one
	if (total != NULL)
		*total = 0;
	if (zone == UI_ZONE_NONE) {
		if (!one || num == 0)
			return 0;
	} else {
		if (!codeplug_zone(zone, &z))
			return 0;
		if (one && pos >= z.count)
			return 0;
		if (one)
			num = z.channels[pos];
	}
	if (total != NULL)
		*total = one ? 1 : (uint8_t)z.count;
	// The record being sent, from the RTOS heap for the moment it is made: zone_share_send copies it into the air
	// queue. Not static: a kilobyte of main RAM is too much on some radios for a buffer used this briefly
	uint8_t *record = pvPortMalloc(ZS_RECORD_MAX);
	if (record == NULL)
		return 0;	// "Can't send now"
	if (one) {
		len = zone_export_channel(num, record, ZS_RECORD_MAX);
		included = len > 0;
	} else {
		len = zone_export_zone(zone, record, ZS_RECORD_MAX, &included);
	}
	bool ok = len > 0 && zone_share_send(dst, group, record, len);
	vPortFree(record);
	if (!ok)
		return 0;
	sent_group = group;
	return included;
}

// The offer's record, checked again here (it was checked when it came in; this is the copy the import reads)
static bool offer_record(uint32_t serial, zone_share_offer_t *o, zsRecord_t *rec)
{
	const uint8_t *data;
	if (!zone_share_offer(o, &data) || (serial != 0 && o->serial != serial))
		return false;
	return zsParse(data, o->length, rec);
}

static zsRecord_t rec;	// static: too big for the UI task's stack

bool ui_svc_zone_offer_pending(void)
{
	return zone_share_offer(NULL, NULL);
}

bool ui_svc_zone_offer(ui_zone_offer_t *out)
{
	zone_share_offer_t o;
	zsChannel_t ch;
	if (!offer_record(0, &o, &rec))
		return false;
	memset(out, 0, sizeof *out);
	out->channel = (rec.flags & ZS_FLAG_CHANNEL) != 0;
	out->serial = o.serial;
	out->src = o.src;
	out->channels = rec.channels;
	for (int i = 0; i < rec.channels; i++)
		if (zsChannelAt(&rec, i, &ch) && zone_import_in_band(&ch))
			out->in_band++;
	if (out->channel && zsChannelAt(&rec, 0, &ch))
		strcpy(out->name, ch.name);
	else
		strcpy(out->name, rec.name);
	contact_name(o.src, false, out->from, sizeof out->from);
	return true;
}

void ui_svc_zone_offer_open(uint32_t serial)
{
	zone_share_offer_take(serial);
}

bool ui_svc_zone_offer_channel(uint32_t serial, uint16_t i, ui_zone_offer_ch_t *out)
{
	zone_share_offer_t o;
	zsChannel_t ch;
	if (!offer_record(serial, &o, &rec) || i >= rec.channels || !zsChannelAt(&rec, i, &ch))
		return false;
	memset(out, 0, sizeof *out);
	strcpy(out->name, ch.name);
	out->rx_hz = ch.rxHz;
	out->dmr = ch.dmr;
	out->in_band = zone_import_in_band(&ch);
	return true;
}

void ui_svc_zone_offer_reject(uint32_t serial)
{
	zone_share_offer_done(serial);
}

// --- The import, a storage job ---

static struct {
	uint32_t serial;
	uint16_t zone;
} job;
// The import list's copy: the settings aren't held while the codeplug is written
static settings_zs_import_t job_imports[SETTINGS_ZS_IMPORTS];

// ok: name the zone's name here, index the channels written, id 1 when it updated an earlier import; else text why, in
// English (TRN): the UI translates it when it shows it, so a longer translation isn't cut to the event's 40 bytes
static void import_done(bool ok, const char *text, const zone_import_result_t *r)
{
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = UI_EV_ZONE_IMPORTED;
	ev.ok = ok;
	if (r != NULL) {
		memcpy(ev.name, r->name, sizeof ev.name - 1);
		ev.index = r->channels;
		ev.id = r->updated;
	}
	if (text != NULL)
		strncpy(ev.text, text, sizeof ev.text - 1);
	ui_notify(&ev);
}

static const char *import_failure(zone_import_status_t st)
{
	switch (st) {
	case ZI_NONE_IN_BAND: return TRN("No channel in this radio's bands");
	case ZI_NO_ZONE: return TRN("No room for another zone");
	case ZI_ZONE_FULL: return TRN("That zone is full");
	case ZI_NO_CHANNELS: return TRN("Not enough free channels");
	case ZI_NO_CONTACTS: return TRN("Not enough free contacts");
	case ZI_FLASH: return TRN("Couldn't save: try again");
	default: return TRN("Not a zone this radio reads");
	}
}

static void import_job(void *ctx)
{
	(void)ctx;
	static zone_import_result_t r;
	zone_share_offer_t o;
	const uint8_t *data;
	if (!zone_share_offer(&o, &data) || o.serial != job.serial) {
		import_done(false, TRN("The offer is gone"), NULL);
		return;
	}
	zone_import_status_t st;
	// The record's flags octet (zone-share.md 4.1); the import checks the whole record again
	bool channel = o.length > 7 && (data[7] & ZS_FLAG_CHANNEL);
	if (channel) {
		st = zone_import_channel(data, o.length, job.zone, &r);
	} else {
		settings_t *s = settings_begin();	// only the list is copied, not the whole settings (RAM)
		memcpy(job_imports, s->zs_import, sizeof job_imports);
		settings_end(hal_millis());		// unchanged: nothing to save
		st = zone_import_zone(data, o.length, o.src, job_imports, &r);
		if (st == ZI_OK) {
			s = settings_begin();
			memcpy(s->zs_import, job_imports, sizeof s->zs_import);
			settings_end(hal_millis());
		}
	}
	if (st != ZI_OK) {
		import_done(false, import_failure(st), NULL);	// the offer stays: the user can make room and try again
		return;
	}
	zone_share_offer_done(o.serial);
	import_done(true, NULL, &r);
}

bool ui_svc_zone_offer_import(uint32_t serial, uint16_t zone)
{
	if (storage_job_busy())
		return false;
	job.serial = serial;
	job.zone = zone;
	zone_share_offer_take(serial);
	return storage_job_post(import_job, NULL);
}

// --- Events ---

bool ui_svc_zone_event(uint8_t kind, uint32_t id, ui_event_t *ev)
{
	memset(ev, 0, sizeof *ev);
	ev->id = id;
	if (kind == DMR_DATA_EV_ZONE_SENT || kind == DMR_DATA_EV_ZONE_FAILED) {
		ev->kind = UI_EV_ZONE_SENT;
		ev->ok = kind == DMR_DATA_EV_ZONE_SENT;
		ev->index = sent_group;
		contact_name(id, sent_group, ev->name, sizeof ev->name);
		return true;
	}
	ui_zone_offer_t o;
	if (kind != DMR_DATA_EV_ZONE_OFFER || !ui_svc_zone_offer(&o))
		return false;
	ev->kind = UI_EV_ZONE_OFFER;
	ev->index = o.channel;
	strcpy(ev->name, o.from);
	// "Zone Club, 24 channels" / "Channel Repeater 1", in the user's language (bounded: a translation is longer)
	char name[sizeof o.name];
	memcpy(name, o.name, sizeof name - 1);	// as sent: 16 characters, perhaps without the terminator
	name[sizeof name - 1] = 0;
	sbuf_t b;
	sb_init(&b, ev->text, sizeof ev->text);
	sb_str(&b, o.channel ? TR("Channel ") : TR("Zone "));
	sb_str(&b, name);
	if (!o.channel) {
		sb_str(&b, ", ");
		sb_u32(&b, o.channels, 1);
		sb_str(&b, o.channels == 1 ? TR(" channel") : TR(" channels"));
	}
	return true;
}
#endif
