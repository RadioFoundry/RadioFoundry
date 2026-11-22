// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <string.h>
#include "../ui/ui_svc.h"
#include "../ui/ui_nav.h"
#include "satellite/tracking.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "hal_time.h"
#include "storage/storage.h"
#include "ui/i18n.h"
#include "ui/fmt.h"

static const satellite_tle_lines_t *pending_lines;
static uint8_t pending_index;
static uint8_t scan_last_status, scan_last_completed, scan_last_total, scan_last_passes;
static bool scan_state_known;

static void satellite_replace_job(void *ctx)
{
	(void)ctx;
	if (!satellite_store_replace(pending_index, pending_lines->line1, pending_lines->line2)) {
		ui_event_t ev = { 0 };
		ev.kind = UI_EV_SAVE_FAILED;
		str_copy(ev.text, sizeof(ev.text), TR(SATELLITE_ERR_TLE_SAVE, "Couldn't save satellite TLE"));
		ui_notify(&ev);
	}
	pending_lines = NULL;
}

uint8_t ui_svc_satellite_count(void)
{
	uint8_t count = 0u;
	radio_get_satellite_scan(NULL, NULL, &count, NULL);
	return count;
}

bool ui_svc_satellite_get(uint8_t index, satellite_record_t *record)
{
	return satellite_store_get(index, record);
}

bool ui_svc_satellite_summary(uint8_t index, satellite_record_brief_t *summary)
{
	if (summary == NULL)
		return false;
	satellite_record_summary_t record;
	if (!satellite_store_summary_get(index, &record))
		return false;
	memset(summary, 0, sizeof(*summary));
	summary->catalog_id = record.catalog_id;
	summary->epoch = record.epoch;
	summary->flags = record.flags;
	uint8_t length = record.name_length;
	if (length > SATELLITE_RECORD_BRIEF_NAME_MAX) {
		length = SATELLITE_RECORD_BRIEF_NAME_MAX;
		while (length > 0u && ((uint8_t)record.name[length] & 0xC0u) == 0x80u)
			length--;
	}
	summary->name_length = length;
	memcpy(summary->name, record.name, length);
	return true;
}

uint32_t ui_svc_satellite_generation(void)
{
	return radio_get_satellite_generation();
}

bool ui_svc_satellite_tle_age(uint32_t epoch, uint16_t *age_days)
{
	if (age_days == NULL)
		return false;
#if !CONFIG_POSITION
	(void)epoch;
	return false;
#else
	uint32_t date_ddmmyy, utc_hms;
	int64_t utc_s;
	if (!position_utc_fields(&date_ddmmyy, &utc_hms, hal_millis()) ||
	    !satellite_utc_from_fields(date_ddmmyy, utc_hms, &utc_s))
		return false;
	return satellite_tle_age_days(epoch, utc_s, age_days);
#endif
}

uint8_t ui_svc_satellite_selected(void)
{
	settings_t settings;
	settings_read(&settings);
	return settings.satellite_index;
}

bool ui_svc_satellite_select(uint8_t index)
{
	if (index >= ui_svc_satellite_count())
		return false;
	settings_t *settings = settings_begin();
	if (settings->satellite_index != index)
		settings->satellite_index = index;
	settings_end(hal_millis());
	return true;
}

bool ui_svc_satellite_track(bool enabled, uint8_t index)
{
	if (enabled && !ui_svc_satellite_select(index))
		return false;
	return radio_req_satellite_tracking(enabled, index);
}

void ui_svc_satellite_arm(void)
{
	radio_req_satellite_arm();
}

bool ui_svc_satellite_scan(bool enabled)
{
	scan_state_known = false;
	radio_req_satellite_scan(enabled);
	return true;
}

void ui_svc_satellite_poll(void)
{
	uint8_t status, completed, total, passes;
	radio_get_satellite_scan(&status, &completed, &total, &passes);
	if (!scan_state_known || status != scan_last_status || completed != scan_last_completed ||
	    total != scan_last_total || passes != scan_last_passes) {
		scan_state_known = true;
		scan_last_status = status;
		scan_last_completed = completed;
		scan_last_total = total;
		scan_last_passes = passes;
		ui_dirty();
	}
}

void ui_svc_satellite_scan_state(uint8_t *status, uint8_t *completed, uint8_t *total, uint8_t *passes)
{
	radio_get_satellite_scan(status, completed, total, passes);
}

bool ui_svc_satellite_pass(uint8_t rank, radio_satellite_pass_t *pass)
{
	return radio_get_satellite_pass(rank, pass);
}

bool ui_svc_satellite_pass_summary(uint8_t record_index, satellite_record_brief_t *summary)
{
	return radio_get_satellite_pass_summary(record_index, summary);
}

bool ui_svc_satellite_oldest_tle_epoch(uint32_t *epoch)
{
	return radio_get_satellite_oldest_tle_epoch(epoch);
}

bool ui_svc_satellite_view(radio_satellite_view_t *view)
{
	return radio_get_satellite_view(view);
}

bool ui_svc_satellite_path(satellite_path_view_t *path)
{
	return radio_get_satellite_path(path);
}

bool ui_svc_satellite_replace(uint8_t index, const satellite_tle_lines_t *lines)
{
	if (lines == NULL || index >= satellite_store_count() || storage_job_busy())
		return false;
	pending_index = index;
	pending_lines = lines;
	if (!storage_job_post(satellite_replace_job, NULL)) {
		pending_lines = NULL;
		return false;
	}
	return true;
}

bool ui_svc_satellite_update_busy(void)
{
	return storage_job_busy();
}