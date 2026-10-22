// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR data features for the UI (and the debug link): text messages, call alert, radio check, ARS state. Safe
// from any task: requests go through a mailbox the radio task empties at its next tick (10 ms), reads are copies
// taken under the service's lock. Built without the data features (CONFIG_DMR_DATA_CORE=0) every call is a no-op
// that says "not available", so UI code needs no #if.
//
// Typical UI use:
//   - redraw when dmr_data_serial() changes; dmr_data_last_event() says what happened (a banner / toast:
//     "Message from 3101234", "Call alert from 3101234", "K1ABC answered", "No answer from 3101234")
//   - inbox list: sms_inbox_count(), sms_inbox_get(i) (0 = newest), sms_inbox_mark_read(i), sms_inbox_unread(),
//     sms_inbox_delete(i)
//   - send: sms_send() then watch sms_send_state() (or the SMS_SENT / SMS_FAILED events)
//   - dmr_signal_call_alert() / dmr_signal_radio_check() then dmr_signal_state() (or the events)
#ifndef ODR_DMR_DATA_API_H
#define ODR_DMR_DATA_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "dmr/dmrDataService.h"

// State of the last request of a kind (SMS, or the last call alert / radio check)
typedef enum {
	DMR_REQ_IDLE,		// nothing asked yet
	DMR_REQ_QUEUED,		// waiting for the radio task / a clear channel
	DMR_REQ_SENDING,	// on air, or waiting for the answer (retries included)
	DMR_REQ_OK,		// sent and acknowledged (an unacknowledged / group SMS: sent)
	DMR_REQ_FAILED,		// no answer after the retries, or it couldn't be sent (not a DMR channel, RX only, ...)
} dmr_req_state_t;

typedef enum {
	DMR_SMS_FORMAT_TMS = 0,		// Motorola TMS (IP / UDP 4007; CONFIG_DMR_TMS)
	DMR_SMS_FORMAT_ETSI = 1,	// ETSI defined short data, ISO 8859-1 text (docs/spec/dmr-data.md 5.2)
	DMR_SMS_FORMAT_ETSI_IP = 2,	// ETSI IP text, UTF-16BE on UDP 5016 (docs/spec/dmr-data.md 5.3): the default
	DMR_SMS_FORMAT_DEFAULT = 0xFF,	// sms_send only: the radio's setting (dmrDataSettings_t.smsFormat)
} dmr_sms_format_t;

// One UDP datagram on the current DMR channel (its only user so far: the crash report, CONFIG_CRASH_REPORT)
#define DMR_DATA_UDP_API	(CONFIG_DMR_DATA_CORE && CONFIG_CRASH_REPORT)
#define DMR_DATA_UDP_MAX	128	// payload bytes
#define UDP_WAIT_MS		120000	// queued this long without a chance to send (an FM channel, busy): failed

#if CONFIG_DMR_DATA_CORE

// --- Text messages ---
int sms_inbox_count(void);
int sms_inbox_unread(void);
// Copy message i (0 = newest); false when there is none
bool sms_inbox_get(int i, dmrDataMessage_t *out);
void sms_inbox_mark_read(int i);
// Delete message i (0 = newest): the older ones move up. False when there is none
bool sms_inbox_delete(int i);
// Queue a message (group: to a talkgroup, no ACK). False when the mailbox is full (one SMS at a time) or the text is
// empty. The text is cut at DMR_DATA_TEXT_MAX characters (Latin-1; ETSI: DMR_ETSI_TEXT_MAX). format: ETSI_IP, ETSI, TMS
// (refused when CONFIG_DMR_TMS is off), or DEFAULT for the radio's setting; a reply uses the message's own
// dmrDataMessage_t.format
bool sms_send(uint32_t dst, bool group, const char *text, bool ack_requested, dmr_sms_format_t format);
dmr_req_state_t sms_send_state(void);
// The last message finished OK was acknowledged by the receiver (false: sent without a confirmation: to a group, or in
// a format without one, ETSI IP text)
bool sms_send_confirmed(void);

// --- CSBK signaling ---
bool dmr_signal_call_alert(uint32_t dst);
bool dmr_signal_radio_check(uint32_t dst);
// The last call alert / radio check started (kind: DMR_DATA_EV_CALL_ALERT_ACKED for an alert,
// DMR_DATA_EV_RADIO_CHECK_OK for a check, 0 none yet), its target and state
dmr_req_state_t dmr_signal_state(uint8_t *kind, uint32_t *dst);

#if DMR_DATA_UDP_API
// --- One UDP datagram ---
// Queue len bytes (at most DMR_DATA_UDP_MAX) from src_port to dst_port at dst (a radio, or a talkgroup), unconfirmed,
// rate 1/2. It goes when the channel lets us (a DMR channel we may transmit on, nothing else of ours on air), or
// fails after UDP_WAIT_MS. False when one is still queued or going, or the arguments are wrong
bool dmr_data_send_udp(uint32_t dst, bool group, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len);
// The last one: DMR_REQ_IDLE (none yet), QUEUED, SENDING, OK (on air) or FAILED
dmr_req_state_t dmr_data_udp_state(void);
#endif

// --- Status ---
uint32_t dmr_data_serial(void);		// bumped on every event and inbox change
dmrDataEvent_t dmr_data_last_event(void);
dmrArsState_t dmr_data_ars_state(void);
void dmr_data_get_settings(dmrDataSettings_t *out);
void dmr_data_set_settings(const dmrDataSettings_t *in);	// committed settings only (gotcha 3.7)

#else

static inline int sms_inbox_count(void) { return 0; }
static inline int sms_inbox_unread(void) { return 0; }
static inline bool sms_inbox_get(int i, dmrDataMessage_t *out) { (void)i; (void)out; return false; }
static inline void sms_inbox_mark_read(int i) { (void)i; }
static inline bool sms_inbox_delete(int i) { (void)i; return false; }
static inline bool sms_send(uint32_t dst, bool group, const char *text, bool ack, dmr_sms_format_t f)
{ (void)dst; (void)group; (void)text; (void)ack; (void)f; return false; }
static inline dmr_req_state_t sms_send_state(void) { return DMR_REQ_IDLE; }
static inline bool sms_send_confirmed(void) { return false; }
static inline bool dmr_signal_call_alert(uint32_t dst) { (void)dst; return false; }
static inline bool dmr_signal_radio_check(uint32_t dst) { (void)dst; return false; }
static inline dmr_req_state_t dmr_signal_state(uint8_t *kind, uint32_t *dst)
{ if (kind) *kind = 0; if (dst) *dst = 0; return DMR_REQ_IDLE; }
static inline uint32_t dmr_data_serial(void) { return 0; }
static inline dmrDataEvent_t dmr_data_last_event(void) { dmrDataEvent_t e = { 0, 0, 0, 0 }; return e; }
static inline void dmr_data_get_settings(dmrDataSettings_t *out) { (void)out; }
static inline void dmr_data_set_settings(const dmrDataSettings_t *in) { (void)in; }
static inline dmrArsState_t dmr_data_ars_state(void) { return DMR_ARS_STATE_OFF; }

#endif

#endif
