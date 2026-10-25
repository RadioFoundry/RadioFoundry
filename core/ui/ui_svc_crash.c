// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls for the crash log (core/crash/crash.h) and its report: the record packed (crash_pack) and sent as
// one UDP datagram through the DMR data service (dmr_data_send_udp, safe from any task) to the target the settings
// name. Built with the crash log only; otherwise the weak stubs stay.
#include "config.h"

#if CONFIG_CRASH_SCREEN
#include "ui_svc.h"
#include "crash/crash.h"
#if CONFIG_CRASH_REPORT
#include "dmr/dmr_data_api.h"
#include "codeplug/codeplug.h"
#endif

bool ui_svc_crash_get(ui_crash_t *out)
{
	const crash_record_t *r = crash_last();
	if (r == NULL)
		return false;
	out->seen = (r->flags & CRASH_F_SEEN) != 0;
	out->sent = (r->flags & CRASH_F_SENT) != 0;
	out->count = r->count;
	out->uptime_ms = r->uptime_ms;
	out->build = r->build;
	crash_summary(r, out->summary, sizeof out->summary);
	return true;
}

void ui_svc_crash_seen(void)
{
	crash_mark(CRASH_F_SEEN);
}

void ui_svc_crash_clear(void)
{
	crash_clear();
}

#if CONFIG_CRASH_REPORT
static bool sending;	// the data service's UDP request is ours
static bool marked;	// its OK already marked the record sent

bool ui_svc_crash_send(void)
{
	const crash_record_t *r = crash_last();
	int32_t num = ui_svc_setting_get(UI_SET_CRASH_CONTACT);
	contact_t c;
	if (r == NULL || num <= 0 || !codeplug_contact((uint16_t)num, &c) || c.id == 0)
		return false;
	static uint8_t pkt[CRASH_PACK_MAX];	// off the UI task's stack
	size_t n = crash_pack(r, pkt, sizeof pkt, hal_crash_code_addr);
	if (n == 0 || !dmr_data_send_udp(c.id, c.type != CONTACT_PRIVATE, CRASH_UDP_PORT, CRASH_UDP_PORT, pkt, n))
		return false;
	sending = true;
	marked = false;
	return true;
}

uint8_t ui_svc_crash_tx_state(void)
{
	if (!sending)
		return UI_CRASH_TX_IDLE;
	switch (dmr_data_udp_state()) {
	case DMR_REQ_QUEUED: return UI_CRASH_TX_QUEUED;
	case DMR_REQ_SENDING: return UI_CRASH_TX_SENDING;
	case DMR_REQ_OK:
		if (!marked) {
			marked = true;
			crash_mark(CRASH_F_SENT | CRASH_F_SEEN);
		}
		return UI_CRASH_TX_OK;
	case DMR_REQ_FAILED: return UI_CRASH_TX_FAILED;
	default: return UI_CRASH_TX_IDLE;
	}
}
#endif
#endif
