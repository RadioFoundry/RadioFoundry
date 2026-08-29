// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The ui_svc.h emergency calls on core/dmr/dmr_emergency.h (dmr-ext, docs/spec/dmr-emergency.md): the emergency key
// starts or cancels the emergency, and each new alert heard (an alarm, or a call with the emergency service option)
// becomes UI_EV_EMERGENCY. Built only with CONFIG_DMR_EMERGENCY; otherwise the weak stubs say "not set up".
#include "config.h"

#if CONFIG_DMR_EMERGENCY
#include <string.h>
#include "ui_svc.h"
#include "dmr/dmr_emergency.h"
#include "codeplug/codeplug.h"

uint32_t ui_svc_feat_ext(void)
{
	return UI_FEAT_EMERGENCY;
}

// on: start (true also when one is running already); off: cancel. The outcome (alarm sent, hot mic, a channel we
// can't transmit on) follows in dmr_emergency_state()
bool ui_svc_emergency(bool on)
{
	if (!on) {
		dmr_emergency_cancel();
		return true;
	}
	if (dmr_emergency_start()) {
		return true;
	}
	dmr_emergency_state_t st = dmr_emergency_state();
	return st == DMR_EMERGENCY_ALARM || st == DMR_EMERGENCY_HOT_MIC || st == DMR_EMERGENCY_ACTIVE;
}

void ui_svc_emergency_dismiss(void)
{
	dmr_emergency_dismiss();
}

void ui_svc_emergency_poll(void)
{
	static uint32_t seen;
	dmr_emergency_alert_t a;
	bool active = dmr_emergency_alert(&a);
	if (a.serial == seen) {
		return;
	}
	seen = a.serial;
	if (!active) {
		return;
	}
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = UI_EV_EMERGENCY;
	ev.id = a.src;
	ev.ok = a.kind == DMR_EMERGENCY_ALERT_ALARM;	// an alarm (else a voice call)
	contact_t c;
	if (codeplug_contact_find(a.src, false, &c)) {
		strncpy(ev.name, c.name, sizeof ev.name - 1);
	}
	ui_notify(&ev);
}

#endif
