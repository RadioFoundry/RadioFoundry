// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's APRS services over core/aprs (ui_svc_aprs.h)
#include "config.h"
#if CONFIG_APRS
#include "ui_svc_aprs.h"
#include "aprs/aprs_fmt.h"
#include "ui_svc.h"
#include "board.h"
#include "storage/storage.h"
#include "hal_time.h"
#include <string.h>

bool ui_svc_aprs_view(aprs_view_t *v)
{
	aprs_get_view(v);
	return true;
}

void ui_svc_aprs_cfg(aprs_cfg_t *c)
{
	aprs_get_cfg(c);
}

// The path and the SSID also live in the codeplug's APRS record (aprs.md 11): written there by the housekeeping task
static void codeplug_job(void *ctx)
{
	(void)ctx;
	(void)aprs_to_codeplug();
}

void ui_svc_aprs_set_cfg(const aprs_cfg_t *c)
{
	aprs_cfg_t old;
	aprs_get_cfg(&old);
	aprs_set_cfg(c, hal_millis());
	if (c->path != old.path || c->ssid != old.ssid) {
		(void)storage_job_post(codeplug_job, NULL);	// busy with another job: the codeplug keeps its own
	}
}

bool ui_svc_aprs_send_now(void)
{
	return aprs_send_now();
}

bool ui_svc_aprs_send_message(const char *to, const char *text, uint8_t flags)
{
	return aprs_send_message_opts(to, text, flags);
}

#if CONFIG_APRS_RX
uint8_t ui_svc_aprs_msg_count(void) { return aprs_msg_count(); }
bool ui_svc_aprs_msg(uint8_t i, aprs_msg_t *out, bool read)
{
	return read ? aprs_msg(i, out) : aprs_msg_peek(i, out);
}
bool ui_svc_aprs_msg_delete(uint8_t i) { return aprs_msg_delete(i); }
#endif

#if CONFIG_APRS_RX
void ui_svc_aprs_msg_poll(void)
{
	static uint32_t seen;
	aprs_view_t v;
	aprs_get_view(&v);
	if (v.msg_serial == seen)
		return;
	seen = v.msg_serial;
	aprs_msg_t m;
	if (!aprs_msg_peek(0, &m))
		return;
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = UI_EV_APRS_MESSAGE;
	strncpy(ev.name, m.from, sizeof ev.name - 1);
	strncpy(ev.text, m.text, sizeof ev.text - 1);
	ui_notify(&ev);
}
#else
void ui_svc_aprs_msg_poll(void) { }
#endif

#if CONFIG_APRS_RX
uint8_t ui_svc_aprs_station_count(void)
{
	return aprs_station_count();
}

bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out)
{
	return aprs_station(i, out);
}

bool ui_svc_aprs_station_find(const char *call, aprs_station_t *out)
{
	return aprs_station_find(call, out);
}

bool ui_svc_aprs_range(const aprs_station_t *s, uint32_t *m, uint16_t *deg)
{
	int32_t lat, lon;
	uint32_t err_m;
	bool dmr_gps;
	if (!s->has_pos || !position_now(&lat, &lon, &err_m, &dmr_gps, hal_millis())) {
		return false;
	}
	*m = aprs_distance_m(lat, lon, s->lat, s->lon);
	*deg = aprs_bearing_deg(lat, lon, s->lat, s->lon);
	return true;
}
#endif
#endif
