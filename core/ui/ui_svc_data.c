// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The ui_svc.h calls for text messages, call alert and radio check, on the DMR data service's API
// (core/dmr/dmr_data_api.h, safe from any task). Its events are polled (dmr_data_serial) and handed to the UI as
// ui_notify events, which become banners. Built only with the data core; otherwise the weak stubs stay.
#include "config.h"

#if CONFIG_DMR_DATA_CORE
#include <string.h>
#include "ui_svc.h"
#include "dmr/dmr_data_api.h"
#include "codeplug/codeplug.h"
#include "hal_time.h"

uint32_t ui_svc_feat_data(void)
{
	return (CONFIG_DMR_SMS ? UI_FEAT_SMS : 0) | (CONFIG_DMR_SIGNALING ? UI_FEAT_SIGNALING : 0);
}

static void contact_name(uint32_t id, bool group, char *out, size_t len)
{
	contact_t c;
	out[0] = 0;
	if (codeplug_contact_find(id, group, &c)) {
		strncpy(out, c.name, len - 1);
		out[len - 1] = 0;
	}
}

uint16_t ui_svc_msg_count(void)
{
	int n = sms_inbox_count();
	return (uint16_t)(n < 0 ? 0 : n);
}

uint16_t ui_svc_msg_unread(void)
{
	int n = sms_inbox_unread();
	return (uint16_t)(n < 0 ? 0 : n);
}

bool ui_svc_msg_get(uint16_t i, ui_msg_t *out)
{
	static dmrDataMessage_t m;	// too big for the UI task's stack
	if (!sms_inbox_get(i, &m))
		return false;
	memset(out, 0, sizeof *out);
	out->src = m.src;
	out->group = m.group;
	out->unread = m.unread;
	out->age_ms = hal_millis() - m.receivedAt;
	contact_name(m.src, false, out->name, sizeof out->name);
	strncpy(out->text, m.text, UI_MSG_TEXT_MAX);
	out->text[UI_MSG_TEXT_MAX] = 0;
	return true;
}

void ui_svc_msg_read(uint16_t i)
{
	sms_inbox_mark_read(i);
}

bool ui_svc_msg_delete(uint16_t i)
{
	return sms_inbox_delete(i);
}

bool ui_svc_msg_send(uint32_t dst, bool group, const char *text)
{
	// A message to a person asks for the acknowledgment ("Delivered"); a group message can't have one
	return sms_send(dst, group, text, !group, DMR_SMS_FORMAT_TMS);
}

static uint32_t check_sent_ms;

bool ui_svc_radio_check(uint32_t id)
{
	check_sent_ms = hal_millis();
	return dmr_signal_radio_check(id);
}

bool ui_svc_call_alert(uint32_t id)
{
	return dmr_signal_call_alert(id);
}

void ui_svc_poll(void)
{
	static uint32_t seen;
	uint32_t serial = dmr_data_serial();
	if (serial == seen)
		return;
	seen = serial;
	dmrDataEvent_t d = dmr_data_last_event();
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.id = d.id;
	switch (d.kind) {
	case DMR_DATA_EV_SMS_RECEIVED: {
		ui_msg_t m;
		ev.kind = UI_EV_MESSAGE;
		ev.index = 0;	// the newest
		if (ui_svc_msg_get(0, &m)) {
			strncpy(ev.text, m.text, sizeof ev.text - 1);
		}
		break;
	}
	case DMR_DATA_EV_SMS_SENT:
	case DMR_DATA_EV_SMS_FAILED:
		ev.kind = UI_EV_MESSAGE_SENT;
		ev.ok = d.kind == DMR_DATA_EV_SMS_SENT;
		break;
	case DMR_DATA_EV_CALL_ALERT:
		ev.kind = UI_EV_CALL_ALERT;
		break;
	case DMR_DATA_EV_CALL_ALERT_ACKED:
	case DMR_DATA_EV_CALL_ALERT_FAILED:
		ev.kind = UI_EV_CALL_ALERT_RESULT;
		ev.ok = d.kind == DMR_DATA_EV_CALL_ALERT_ACKED;
		break;
	case DMR_DATA_EV_RADIO_CHECK_OK:
	case DMR_DATA_EV_RADIO_CHECK_FAILED:
		ev.kind = UI_EV_RADIO_CHECK;
		ev.ok = d.kind == DMR_DATA_EV_RADIO_CHECK_OK;
		ev.ms = d.atMs - check_sent_ms;
		break;
	default:
		return;		// ARS changes and checks of us stay off the screen
	}
	contact_name(d.id, false, ev.name, sizeof ev.name);
	ui_notify(&ev);
}
#endif
