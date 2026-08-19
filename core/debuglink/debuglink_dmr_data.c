// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR data parts of the debug link (docs/spec/debuglink.md 3.7 dmr_data, 3.12 DMR_DATA): the service's state and
// counters as key=value text, and requests (send an SMS, call alert, radio check, settings, read the inbox) for the
// HIL tests. Requests go through dmr_data_api.h, so they are handled by the radio task at its next tick.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmr_data_api.h"
#endif

#if CONFIG_DMR_DATA_CORE

static const char *req_key(uint8_t s)
{
	static const char *const names[] = { "idle", "queued", "sending", "ok", "failed" };
	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static const char *ars_key(uint8_t s)
{
	static const char *const names[] = { "off", "registering", "registered", "refused", "no_answer" };
	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static const char *event_key(uint8_t e)
{
	static const char *const names[] = { "none", "sms_received", "sms_sent", "sms_failed", "call_alert",
					     "call_alert_acked", "call_alert_failed", "radio_check_ok",
					     "radio_check_failed", "radio_check_answered", "ars_changed" };
	return e < sizeof(names) / sizeof(names[0]) ? names[e] : "?";
}

void debuglink_view_dmr_data(dl_text_t *t)
{
	dmrDataStats_t s;
	dmrDataSettings_t set;
	dmrDataEvent_t ev = dmr_data_last_event();
	uint8_t kind;
	uint32_t dst;
	dmr_req_state_t sig = dmr_signal_state(&kind, &dst);

	dmrDataServiceGetStats(&s);
	dmr_data_get_settings(&set);
	dl_text(t, "data.built=1\n");
	dl_text(t, "data.busy=%u\n", dmrDataServiceIsBusy());
	dl_text(t, "data.sms_state=%s\n", req_key(sms_send_state()));
	dl_text(t, "data.signal_state=%s\ndata.signal_kind=%s\ndata.signal_dst=%u\n", req_key(sig),
		kind == DMR_DATA_EV_CALL_ALERT_ACKED ? "call_alert" : kind == DMR_DATA_EV_RADIO_CHECK_OK ? "radio_check"
													: "none",
		(unsigned)dst);
	dl_text(t, "data.ars_state=%s\n", ars_key(dmr_data_ars_state()));
	dl_text(t, "data.inbox_count=%u\ndata.inbox_unread=%u\n", (unsigned)sms_inbox_count(),
		(unsigned)sms_inbox_unread());
	dl_text(t, "data.serial=%u\ndata.event=%s\ndata.event_serial=%u\ndata.event_id=%u\n", (unsigned)dmr_data_serial(),
		event_key(ev.kind), (unsigned)ev.serial, (unsigned)ev.id);
	dl_text(t, "data.ars_server=%u\ndata.radio_check_reply=%u\ndata.call_alert_ack=%u\ndata.lrrp_reply=%u\n",
		(unsigned)set.arsServer, set.radioCheckReply, set.callAlertAck, set.lrrpReply);
	dl_text(t, "data.has_gps=%u\ndata.lrrp_triggers=%u\n", set.hasGps, (unsigned)dmrDataServiceLrrpTriggerCount());
	dl_text(t, "data.rx_bursts=%u\ndata.rx_csbk=%u\ndata.rx_headers=%u\ndata.rx_blocks=%u\n", (unsigned)s.rxBursts,
		(unsigned)s.rxCsbk, (unsigned)s.rxHeaders, (unsigned)s.rxBlocks);
	dl_text(t, "data.rx_blocks_stale=%u\ndata.rx_crc_errors=%u\ndata.rx_crc_flag_mismatch=%u\n",
		(unsigned)s.rxBlocksStale, (unsigned)s.rxCrcErrors, (unsigned)s.rxCrcFlagMismatch);
	dl_text(t, "data.rx_packets=%u\ndata.rx_for_us=%u\ndata.rx_confirmed=%u\n", (unsigned)s.rxPackets,
		(unsigned)s.rxForUs, (unsigned)s.rxConfirmed);
	dl_text(t, "data.sms_received=%u\ndata.sms_duplicates=%u\ndata.sms_sent=%u\ndata.sms_acked=%u\n"
		"data.sms_failed=%u\n", (unsigned)s.smsReceived, (unsigned)s.smsDuplicates, (unsigned)s.smsSent,
		(unsigned)s.smsAcked, (unsigned)s.smsFailed);
	dl_text(t, "data.tms_acks_sent=%u\ndata.responses_sent=%u\ndata.csbk_answers_sent=%u\n",
		(unsigned)s.tmsAcksSent, (unsigned)s.responsesSent, (unsigned)s.csbkAnswersSent);
	dl_text(t, "data.call_alerts_received=%u\ndata.radio_checks_received=%u\n", (unsigned)s.callAlertsReceived,
		(unsigned)s.radioChecksReceived);
	dl_text(t, "data.call_alerts_sent=%u\ndata.call_alerts_acked=%u\ndata.radio_checks_sent=%u\n"
		"data.radio_checks_answered=%u\n", (unsigned)s.callAlertsSent, (unsigned)s.callAlertsAcked,
		(unsigned)s.radioChecksSent, (unsigned)s.radioChecksAnswered);
	dl_text(t, "data.ars_sent=%u\ndata.ars_acks=%u\ndata.lrrp_requests=%u\ndata.lrrp_answers=%u\n"
		"data.lrrp_no_fix=%u\n", (unsigned)s.arsSent, (unsigned)s.arsAcks, (unsigned)s.lrrpRequests,
		(unsigned)s.lrrpAnswers, (unsigned)s.lrrpNoFix);
	dl_text(t, "data.tx_lists=%u\ndata.tx_ok=%u\ndata.tx_failed=%u\ndata.tx_gave_up=%u\ndata.tx_last_error=%u\n",
		(unsigned)s.txLists, (unsigned)s.txOk, (unsigned)s.txFailed, (unsigned)s.txGaveUp,
		(unsigned)s.txLastError);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

size_t debuglink_cmd_dmr_data(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t st = DL_ST_OK;

	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);

	switch (req[0]) {
	case DL_DATA_OP_SMS: {
		char text[DL_REQ_MAX];
		uint16_t tl = (uint16_t)(n - 5u);

		if (n < 6) {
			st = DL_ST_BAD_ARG;
			break;
		}
		memcpy(text, a + 5, tl);
		text[tl] = '\0';
		st = sms_send(get32(a), (a[4] & 1u) != 0, text, (a[4] & 2u) != 0, DMR_SMS_FORMAT_TMS) ? DL_ST_OK
													 : DL_ST_BUSY;
		break;
	}
	case DL_DATA_OP_CALL_ALERT:
	case DL_DATA_OP_RADIO_CHECK:
		if (n < 4) {
			st = DL_ST_BAD_ARG;
			break;
		}
		st = (req[0] == DL_DATA_OP_CALL_ALERT ? dmr_signal_call_alert(get32(a)) : dmr_signal_radio_check(get32(a)))
			     ? DL_ST_OK : DL_ST_BUSY;
		break;
	case DL_DATA_OP_SETTINGS: {
		dmrDataSettings_t set;

		if (n < 5) {
			st = DL_ST_BAD_ARG;
			break;
		}
		dmr_data_get_settings(&set);
		set.arsServer = get32(a) & 0xFFFFFFu;
		set.radioCheckReply = (a[4] & 1u) != 0;
		set.callAlertAck = (a[4] & 2u) != 0;
		set.lrrpReply = (a[4] & 4u) != 0;
		dmr_data_set_settings(&set);
		break;
	}
	case DL_DATA_OP_INBOX: {
		dmrDataMessage_t m;

		if (n < 1) {
			st = DL_ST_BAD_ARG;
			break;
		}
		if (!sms_inbox_get(a[0], &m)) {
			st = DL_ST_UNAVAILABLE;
			break;
		}
		size_t tl = strlen(m.text);
		if (11u + tl > cap) {
			tl = cap - 11u;
		}
		out[0] = DL_ST_OK;
		put32(out + 1, m.src);
		put32(out + 5, m.dst);
		out[9] = m.group;
		out[10] = m.unread;
		memcpy(out + 11, m.text, tl);
		return 11u + tl;
	}
	case DL_DATA_OP_MARK_READ:
		if (n < 1) {
			st = DL_ST_BAD_ARG;
			break;
		}
		sms_inbox_mark_read(a[0]);
		break;
	default:
		st = DL_ST_BAD_ARG;
		break;
	}
	out[0] = st;
	return 1;
}

void debuglink_dmr_data_reset(void)
{
	dmrDataServiceResetStats();
}

#else	// built without the data features

void debuglink_view_dmr_data(dl_text_t *t)
{
	dl_text(t, "data.built=0\n");
}

size_t debuglink_cmd_dmr_data(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

void debuglink_dmr_data_reset(void)
{
}

#endif
