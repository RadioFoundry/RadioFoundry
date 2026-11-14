// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "../ui/ui_svc.h"
#include "../ui/ui_nav.h"
#include "satellite/tracking.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "hal_time.h"
#include "storage/storage.h"
#include "ui/i18n.h"
#include "ui/fmt.h"

static const char *pending_line1;
static const char *pending_line2;
static uint8_t pending_index;
static uint8_t scan_last_status, scan_last_completed, scan_last_passes;
static bool scan_state_known;

static void satellite_replace_job(void *ctx)
{
	(void)ctx;
	if (!satellite_store_replace(pending_index, pending_line1, pending_line2)) {
		ui_event_t ev = { 0 };
		ev.kind = UI_EV_SAVE_FAILED;
		str_copy(ev.text, sizeof(ev.text), TRN("Couldn't save satellite TLE"));
		ui_notify(&ev);
	}
	pending_line1 = NULL;
	pending_line2 = NULL;
}

uint8_t ui_svc_satellite_count(void)
{
	return satellite_store_count();
}

bool ui_svc_satellite_get(uint8_t index, satellite_record_t *record)
{
	return satellite_store_get(index, record);
}

uint8_t ui_svc_satellite_selected(void)
{
	settings_t settings;
	settings_read(&settings);
	return settings.satellite_index;
}

bool ui_svc_satellite_select(uint8_t index)
{
	if (index >= satellite_store_count())
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
	radio_req_satellite_tracking(enabled, index);
	return true;
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
	(void)total;
	if (!scan_state_known || status != scan_last_status || completed != scan_last_completed ||
	    passes != scan_last_passes) {
		scan_state_known = true;
		scan_last_status = status;
		scan_last_completed = completed;
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

bool ui_svc_satellite_path(satellite_path_view_t *path)
{
	return radio_get_satellite_path(path);
}

bool ui_svc_satellite_replace(uint8_t index, const char *line1, const char *line2)
{
	if (line1 == NULL || line2 == NULL || index >= satellite_store_count() || storage_job_busy())
		return false;
	pending_index = index;
	pending_line1 = line1;
	pending_line2 = line2;
	if (!storage_job_post(satellite_replace_job, NULL)) {
		pending_line1 = NULL;
		pending_line2 = NULL;
		return false;
	}
	return true;
}

bool ui_svc_satellite_update_busy(void)
{
	return storage_job_busy();
}