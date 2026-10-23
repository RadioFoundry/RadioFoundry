// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's MIB (docs/spec/snmp.md 4, docs/spec/RFOUNDRY-MIB.txt): the system group (RFC 3418), the snmp group, and the RadioFoundry enterprise objects, read from a snapshot (snmpData_t) the
// firmware glue fills (snmp_agent.c), so that this file, like snmp.c, runs unchanged in the host tests. SETs change
// an snmpConfig_t and the system name in the context and flag them for the glue to store.
//
// Also the notifications: the events the agent reports (snmpEvents_*), detected from successive snapshots, queued per
// target (the host over USB, a manager radio over RF), rate limited on RF, and encoded as SNMPv2-Trap messages.
#ifndef RFOUNDRY_SNMP_MIB_H
#define RFOUNDRY_SNMP_MIB_H

#include <stdbool.h>
#include <stdint.h>
#include "snmp/snmp.h"

// The enterprise number of the RadioFoundry MIB. TBD: 32473 is the IANA number reserved for documentation (RFC 5612), a
// placeholder until the project has its own Private Enterprise Number; change it here and in docs/spec/RFOUNDRY-MIB.txt
#ifndef RFOUNDRY_SNMP_PEN
#define RFOUNDRY_SNMP_PEN		32473u
#endif

#define SNMP_NAME_MAX		16	// = SETTINGS_NAME_LEN, the radio name (sysName)
#define SNMP_TEXT_MAX		64	// SMS text carried in a notification (longer is cut)
#define SNMP_TRAP_COMMUNITY	"public"

// What the MIB reads: a snapshot of the radio, filled by the glue before each request and each event check
typedef struct {
	uint32_t uptimeMs;
	// Identity
	uint32_t dmrId;
	char name[SNMP_NAME_MAX + 1];	// the radio name / callsign (sysName)
	const char *board;
	const char *firmware;
	// Power
	uint16_t batteryMv;
	uint8_t batteryPct;
	uint8_t batteryState;		// 0 ok, 1 low, 2 critical (radio_batt_t)
	// The channel
	uint8_t radioState;		// radio_state_t: 0 idle, 1 rx, 2 tx, 3 tx denied
	int16_t rssiDbm;
	uint8_t mode;			// 0 FM, 1 DMR
	bool vfo;
	char zoneName[SNMP_NAME_MAX + 1];
	uint16_t channelNumber;
	char channelName[SNMP_NAME_MAX + 1];
	uint32_t rxHz, txHz;
	uint8_t power;
	const char *powerName;
	uint8_t cc, ts;
	uint32_t talkTarget;
	bool talkPrivate;
	uint8_t role;			// SNMP_ROLE_*
	// DMR calls
	uint8_t dmrState;		// radio_dmr_state_t
	uint32_t callSrc, callDst;
	bool callGroup, callEmergency;
	uint32_t lastSrc, lastDst;
	bool lastGroup;
	uint32_t lastMs;		// uptime of the last call's last burst, 0 = none
	// DMR data
	uint32_t dataRxPackets, dataRxForUs, smsReceived, smsSent, smsFailed;
	uint8_t smsInbox, smsUnread;
	uint8_t arsState;		// dmrArsState_t
	uint32_t dataEventSerial;	// dmr_data_last_event()
	uint8_t dataEventKind;
	uint32_t dataEventId;
	// USB network
	bool usbNetwork;
	uint32_t netToAir, netPacketsUp;	// the gateway: packets to the air side, packets from the air to the host
	// Last, past what the MIB reads (its fields must stay within 255 octets: snmpMib.c)
	char smsText[SNMP_TEXT_MAX + 1];	// the newest message (filled when an SMS event is new)
} snmpData_t;

enum { SNMP_ROLE_RADIO, SNMP_ROLE_HOTSPOT, SNMP_ROLE_SFR, SNMP_ROLE_SPLIT_TX, SNMP_ROLE_SPLIT_RX };

// The agent's settings (settings_t snmp_*; all zero = the defaults)
typedef struct {
	uint32_t readHash;		// read community (snmpCommunityHash), 0 = "public"
	uint32_t writeHash;		// write community, 0 = none: SET refused
	uint32_t rfManager;		// DMR ID notifications go to over RF, 0 = none
	uint16_t rfTrapGapS;		// at most one RF notification per this many seconds (emergency exempt), 0 = 60
	uint8_t flags;			// SNMP_CFG_*
} snmpConfig_t;

#define SNMP_CFG_OFF		0x01u	// the agent answers nothing and sends nothing
#define SNMP_CFG_RF_OFF		0x02u	// no answers over RF
#define SNMP_CFG_RF_WRITE	0x04u	// SET over RF (with the write community)
#define SNMP_CFG_USB_TRAPS_OFF	0x08u	// no notifications to the host
#define SNMP_CFG_AUTH_TRAPS	0x10u	// authenticationFailure notifications (snmpEnableAuthenTraps)
#define SNMP_CFG_RF_CALL_TRAPS	0x20u	// call start / end also over RF (else only the host gets them)
#define SNMP_RF_TRAP_GAP_DEFAULT	60u

// The MIB's context (snmpMib_t.ctx)
typedef struct {
	snmpData_t *data;
	snmpConfig_t *cfg;
	const snmpStats_t *stats;
	uint32_t trapsSent, trapsDropped;
	char text[48];			// sysDescr
	char newName[SNMP_NAME_MAX + 1];	// sysName as SET
	bool cfgChanged, nameChanged;	// for the glue to store
} snmpMibCtx_t;

// The MIB on a context
void snmpMibInit(snmpMib_t *mib, snmpMibCtx_t *ctx);

// ---- Notifications ----

enum {
	SNMP_EV_COLD_START, SNMP_EV_AUTH_FAIL, SNMP_EV_CALL_START, SNMP_EV_CALL_END, SNMP_EV_SMS, SNMP_EV_EMERGENCY,
	SNMP_EV_ARS, SNMP_EV_LOW_BATTERY,
};
#define SNMP_TARGET_USB		0x01u
#define SNMP_TARGET_RF		0x02u
#define SNMP_EVENT_QUEUE	4

typedef struct {
	uint8_t kind;			// SNMP_EV_*
	uint8_t targets;		// SNMP_TARGET_* still to send to
	uint8_t slot;
	bool group;
	uint32_t src, dst;
	int32_t value;			// ARS state (MIB numbering), battery mV
	uint32_t durationMs;
	uint32_t atMs;			// uptime when it happened (the trap's sysUpTime)
	uint32_t rfSinceMs;		// the first RF try, 0 = none yet
	char text[SNMP_TEXT_MAX + 1];
} snmpEvent_t;

typedef struct {
	bool started;
	uint8_t dmrState, batteryState, arsState;
	uint32_t callSrc, callDst, callStartMs;
	bool callGroup;
	uint8_t callSlot;
	uint32_t dataEventSerial;
	snmpEvent_t q[SNMP_EVENT_QUEUE];
	uint8_t count;
	uint32_t lastRfMs;		// the last RF notification sent, 0 = none yet
	uint32_t dropped;		// events or targets given up (queue full, rate limit, RF busy too long)
} snmpEvents_t;

void snmpEventsInit(snmpEvents_t *e);
// Compare a new snapshot with the last one and queue what happened (the first call queues coldStart)
void snmpEventsDetect(snmpEvents_t *e, const snmpData_t *d, const snmpConfig_t *cfg);
// Queue one (targets chosen by cfg: the host unless USB notifications are off, the RF manager if set and the kind
// goes over RF); the oldest goes when the queue is full
void snmpEventsPush(snmpEvents_t *e, const snmpEvent_t *ev, const snmpConfig_t *cfg);
// The oldest event still to go to target, or NULL
snmpEvent_t *snmpEventsNext(snmpEvents_t *e, uint8_t target);
// Done with target for ev (sent, or given up: dropped counts it); the event leaves once no target is left
void snmpEventsDone(snmpEvents_t *e, snmpEvent_t *ev, uint8_t target, bool dropped);
// May ev go over RF now (rate limit: one per cfg gap, emergency exempt)? Note the sending with snmpEventsRfSent
bool snmpEventsRfAllowed(const snmpEvents_t *e, const snmpEvent_t *ev, const snmpConfig_t *cfg, uint32_t nowMs);
void snmpEventsRfSent(snmpEvents_t *e, uint32_t nowMs);
// The SNMPv2-Trap message for ev: its length, 0 if it doesn't fit
int snmpEventEncode(const snmpEvent_t *ev, int32_t requestId, uint8_t *out, int cap);

#endif
