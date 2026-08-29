// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR data service: text messages (Motorola TMS and ETSI defined short data) with their ACKs, ETSI responses to
 * confirmed data, CSBK signaling (call alert, radio check in the Motorola and the ETSI form: answered and sent), the
 * emergency alarm (sent, heard: core/dmr/dmr_emergency.c), Motorola ARS registration and LRRP location answers.
 *
 * It sits on the DMR call stack (core/dmr/call/dmr_call.h): the host (core/radio/radio_sm.c in the firmware, a fake in
 * the host tests) hands it every data burst heard on our slot, ticks it, tells it about the channel, and sends the
 * burst lists it asks for (dmrDataEnv_t.txStart, then dmrDataServiceTxDone). Everything here runs in that one task
 * (the radio task); the user-facing calls in dmr/dmr_data_api.h may come from any task and only touch a mailbox and
 * copies under the lock given to dmrDataServiceSetLock(). Specs: docs/spec/dmr-data.md, gotchas 3.6-3.9, 5, 7.
 */

#ifndef ODR_DMRDATASERVICE_H
#define ODR_DMRDATASERVICE_H

#include "dmr/dmrData.h"
#include "dmr/dmrLrrp.h"

#define DMR_DATA_INBOX_SIZE			4
#define DMR_DATA_SMS_PREAMBLES		2		// preamble CSBKs before an SMS, gives receivers time to sync
#define DMR_DATA_TEXT_MAX			(sizeof(((dmrDataTMS_t *)0)->text) - 1)

typedef struct
{
	uint32_t src;
	uint32_t dst;
	uint32_t receivedAt;					// the service's clock (ms)
	bool     group;
	uint8_t  seq;							// TMS sequence number, to spot a sender's retries
	bool     unread;						// not shown on the radio yet
	uint8_t  format;						// dmr_sms_format_t it came in (a reply goes the same way)
	char     text[sizeof(((dmrDataTMS_t *)0)->text)];
} dmrDataMessage_t;

// What the service needs to know about the channel, from its host at every tick
typedef struct
{
	uint32_t ownId;							// our DMR ID on this channel; 0: nothing is sent
	uint32_t talkGroup;						// the channel's talkgroup (group data to it is ours); 0: none
	bool     rmo;							// through a repeater
	bool     canTransmit;					// a DMR channel we may send on now: not RX only, in band, no hotspot
	uint32_t channelKey;					// changes with the frequency, slot or color code (ARS registers again)
} dmrDataChannel_t;

// The service's user settings. Only committed settings, never an edit in progress (gotcha 3.7)
typedef struct
{
	uint32_t arsServer;						// the ARS radio ID to register with; 0: ARS off
	bool     radioCheckReply;				// answer radio checks
	bool     callAlertAck;					// acknowledge call alerts (they ring either way)
	bool     lrrpReply;						// answer LRRP location requests
	bool     hasGps;						// the radio has a position source (else LRRP says "no fix")
	uint8_t  smsFormat;						// dmr_sms_format_t of sms_send(..., DMR_SMS_FORMAT_DEFAULT): TMS or ETSI
	bool     radioCheckEtsi;				// our radio checks in the ETSI form (FID 0x00), else Motorola's (FID 0x10)
} dmrDataSettings_t;

// Events for the UI (dmr_data_api.h: dmr_data_last_event)
typedef enum
{
	DMR_DATA_EV_NONE = 0,
	DMR_DATA_EV_SMS_RECEIVED,				// id: the sender
	DMR_DATA_EV_SMS_SENT,					// id: the destination; acknowledged (or sent, to a group / unacknowledged)
	DMR_DATA_EV_SMS_FAILED,					// id: the destination; no ACK after the retries, or couldn't be sent
	DMR_DATA_EV_CALL_ALERT,					// id: the caller
	DMR_DATA_EV_CALL_ALERT_ACKED,			// id: the radio we alerted answered
	DMR_DATA_EV_CALL_ALERT_FAILED,			// id: no answer after the retries
	DMR_DATA_EV_RADIO_CHECK_OK,				// id: the radio we checked answered
	DMR_DATA_EV_RADIO_CHECK_FAILED,			// id: no answer after the retries
	DMR_DATA_EV_RADIO_CHECK_ANSWERED,		// id: a radio checked us and was answered
	DMR_DATA_EV_ARS_CHANGED,				// id: the ARS state (dmrArsState_t)
	DMR_DATA_EV_EMERGENCY_ALARM,			// id: the radio in emergency (an alarm to us or a group we listen to)
	DMR_DATA_EV_EMERGENCY_SENT,				// id: the alarm's destination; all tries have gone out
	DMR_DATA_EV_EMERGENCY_FAILED,			// id: the alarm's destination; it couldn't be sent
} dmrDataEventKind_t;

typedef struct
{
	uint32_t serial;						// bumped with every event
	uint8_t  kind;							// dmrDataEventKind_t
	uint32_t id;
	uint32_t atMs;
} dmrDataEvent_t;

typedef enum { DMR_ARS_STATE_OFF, DMR_ARS_STATE_REGISTERING, DMR_ARS_STATE_REGISTERED, DMR_ARS_STATE_REFUSED, DMR_ARS_STATE_NO_ANSWER } dmrArsState_t;

// The host's side
typedef struct
{
	// Send a burst list (the service's buffer, valid until dmrDataServiceTxDone): true when started. The service has
	// done its listen before talk
	bool (*txStart)(const dmrBurst_t *bursts, int count, void *ctx);
	// Listen before talk for machine traffic (gotcha 3.6; dmr_call_channel_clear in the firmware)
	bool (*channelClear)(uint32_t nowMs, void *ctx);
	// Group calls of the channel's listen list count as ours (may be NULL)
	bool (*inRxGroup)(uint32_t talkGroup, void *ctx);
	// An event for the UI was posted (may be NULL; any task may poll dmr_data_last_event instead)
	void (*notify)(const dmrDataEvent_t *event, void *ctx);
	// Every packet received (to us or not), for the IP gateway when it is ported (may be NULL)
	void (*packet)(const dmrDataPacket_t *packet, void *ctx);
	void *ctx;
} dmrDataEnv_t;

// Counters for the debug link and the HIL tests (wrap freely)
typedef struct
{
	uint32_t rxBursts;						// data / control bursts handed to the service
	uint32_t rxCsbk;						// CSBKs with a good CRC (preambles included)
	uint32_t rxHeaders;						// data headers with a good CRC
	uint32_t rxBlocks;						// rate 1/2, 3/4, 1 blocks
	uint32_t rxBlocksStale;					// blocks dropped: no header in the last 400 ms (gotcha 5.4)
	uint32_t rxCrcErrors;					// headers / CSBKs failing their CRC, packets failing the CRC-32
	uint32_t rxCrcFlagMismatch;				// headers / CSBKs whose CRC checks but the modem flagged (or the reverse)
	uint32_t rxPackets;						// complete packets
	uint32_t rxForUs;						// ... addressed to us (or a group we listen to)
	uint32_t rxConfirmed;					// ... confirmed data to us
	uint32_t smsReceived;					// text messages kept
	uint32_t smsDuplicates;					// a sender's retry: ACKed, not kept
	uint32_t smsSent;						// SMS lists sent
	uint32_t smsAcked;						// TMS ACKs received for ours
	uint32_t smsFailed;
	uint32_t tmsAcksSent;
	uint32_t responsesSent;					// ETSI responses to confirmed data
	uint32_t csbkAnswersSent;				// call alert acks and radio check answers
	uint32_t callAlertsReceived;
	uint32_t radioChecksReceived;			// radio checks to us (answered when the setting allows)
	uint32_t callAlertsSent;				// CSBKs sent, retries included
	uint32_t callAlertsAcked;
	uint32_t radioChecksSent;
	uint32_t radioChecksAnswered;
	uint32_t arsSent;						// registrations and deregistrations sent
	uint32_t arsAcks;
	uint32_t lrrpRequests;
	uint32_t lrrpAnswers;					// answers and reports sent
	uint32_t lrrpNoFix;						// ... of those, saying "no fix"
	uint32_t etsiSmsReceived;				// ... of smsReceived, ETSI short data (defined short data, UDT text)
	uint32_t etsiSmsSent;					// ... of smsSent, ETSI
	uint32_t etsiResponsesReceived;			// ETSI response ACKs for ours
	uint32_t emergencyAlarmsHeard;			// emergency alarm CSBKs with a good CRC, to anyone
	uint32_t emergencyAlarmsReceived;		// ... to us or a group we listen to (the alert)
	uint32_t emergencyAlarmsSent;			// alarm CSBKs sent, repeats included
	uint32_t txLists;						// burst lists started
	uint32_t txOk;							// ... that went out whole
	uint32_t txFailed;						// ... dropped by the stack (no repeater, refused, time-out)
	uint32_t txGaveUp;						// lists never started: no clear channel in time
	uint8_t  txLastError;					// dmr_tx_error_t of the last list
} dmrDataStats_t;

// --- The host's calls (the radio task) ---

void dmrDataServiceInit(const dmrDataEnv_t *env);
void dmrDataServiceSetSettings(const dmrDataSettings_t *settings);
void dmrDataServiceGetSettings(dmrDataSettings_t *settings);
// The lock that keeps the API callers' copies consistent (the firmware: scheduler suspend); NULL: none
void dmrDataServiceSetLock(void (*lock)(void), void (*unlock)(void));
// Every 10-25 ms with the time and the channel
void dmrDataServiceTick(uint32_t nowMs, const dmrDataChannel_t *channel);
// A data / control burst heard on our slot (dmr_call_set_data_rx)
void dmrDataServiceRxBurst(const dmrBurst_t *burst, uint32_t nowMs);
// The list given to txStart is over: err 0 = sent whole, else the dmr_tx_error_t
void dmrDataServiceTxDone(uint8_t err);
bool dmrDataServiceIsBusy(void);
// Power off: true while the ARS deregistration is still going out, call again until it's false (gotcha 7.4)
bool dmrDataServicePowerOff(void);
void dmrDataServicePowerOffCancelled(void);

// Requests (the radio task, or through the dmr_data_api.h mailbox). False when one of the kind is still running or
// it can't be sent now
bool dmrDataServiceSendSMS(bool group, uint32_t dst, const char *text, bool ackRequested);
bool dmrDataServiceSendUDP(bool group, uint32_t dst, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length);
bool dmrDataServiceSendBursts(const dmrBurst_t *bursts, int count);
bool dmrDataServiceCallAlert(uint32_t dst);
bool dmrDataServiceRadioCheck(uint32_t dst);
// format: dmr_sms_format_t (TMS or ETSI; not DEFAULT)
bool dmrDataServiceSendSMSFormat(bool group, uint32_t dst, const char *text, bool ackRequested, uint8_t format);
// The emergency alarm (CONFIG_DMR_EMERGENCY): the CSBK to dst (group or individual) with the feature set ID fid,
// tries times, DMR_DATA_EMERGENCY_GAP_MS apart. Ahead of everything but a reply already queued. Ends with the event
// EMERGENCY_SENT or EMERGENCY_FAILED. False when one is running already or it can't be sent now
#define DMR_DATA_EMERGENCY_GAP_MS	1000
bool dmrDataServiceEmergencyAlarm(uint32_t dst, bool group, uint8_t fid, uint8_t tries);
void dmrDataServiceEmergencyAlarmCancel(void);
bool dmrDataServiceEmergencyAlarmBusy(void);
// The 10 octets of an emergency alarm CSBK (docs/spec/dmr-emergency.md 2)
void dmrDataBuildEmergencyAlarm(uint32_t dst, uint32_t src, bool group, uint8_t fid, uint8_t out[10]);

dmrArsState_t dmrDataServiceArsState(void);
int dmrDataServiceLrrpTriggerCount(void);
// When the service next needs a position, in ms (0: now or any time, UINT32_MAX: never): GPS power management
uint32_t dmrDataServiceLocationNeededInMs(void);
// The platform's position for LRRP (weak: none). Only asked when the settings say hasGps
bool dmrDataServiceGetPosition(lrrpPosition_t *position);

// The inbox: the last DMR_DATA_INBOX_SIZE messages, newest first; a new message replaces the oldest
int dmrDataServiceMessageCount(void);
const dmrDataMessage_t *dmrDataServiceMessage(int index);	// 0: the newest

void dmrDataServiceGetStats(dmrDataStats_t *stats);
void dmrDataServiceResetStats(void);

#endif
