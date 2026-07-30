// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR data service: SMS (Motorola TMS) inbox, ACKs and the host interface
 */

#ifndef ODR_DMRDATASERVICE_H
#define ODR_DMRDATASERVICE_H

#include "dmr/dmrData.h"
#include "dmr/dmrLrrp.h"

#define DMR_DATA_INBOX_SIZE			4
#define DMR_DATA_SMS_PREAMBLES		2		// preamble CSBKs before an SMS, gives receivers time to sync

typedef struct
{
	uint32_t src;
	uint32_t dst;
	uint32_t receivedAt;					// ticksGetMillis()
	bool     group;
	uint8_t  seq;							// TMS sequence number, to spot a sender's retries
	bool     hostPending;					// not popped by the USB host yet
	bool     unread;						// not shown on the radio yet
	char     text[sizeof(((dmrDataTMS_t *)0)->text)];
} dmrDataMessage_t;

// Requests are queued and started from the main task by dmrDataServiceTick()
bool dmrDataServiceSendSMS(bool group, uint32_t dst, const char *text, bool ackRequested);
bool dmrDataServiceSendUDP(bool group, uint32_t dst, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length);
bool dmrDataServiceSendBursts(const dmrBurst_t *bursts, int count);
bool dmrDataServiceIsBusy(void);
void dmrDataServiceTick(void);
// Power off (main task): true while the ARS deregistration is still going out, call again until it's false
bool dmrDataServicePowerOff(void);
// Motorola ARS registration, for the UI
typedef enum { DMR_ARS_STATE_OFF, DMR_ARS_STATE_REGISTERING, DMR_ARS_STATE_REGISTERED, DMR_ARS_STATE_REFUSED, DMR_ARS_STATE_NO_ANSWER } dmrArsState_t;
dmrArsState_t dmrDataServiceArsState(void);
// Location reports (HAS_DMR_GPS_REPORTS): the platform gives its position, the driver says when our voice call ended
bool dmrDataServiceGetLocation(dmrDataLocation_t *location);
void dmrDataServiceVoiceTxEnded(void);
// LRRP (HAS_DMR_LRRP): the platform's position with date and course (weak: from dmrDataServiceGetLocation), and the
// triggered reports running
bool dmrDataServiceGetPosition(lrrpPosition_t *position);
int dmrDataServiceLrrpTriggerCount(void);
// When the service next needs a position, in ms (0: now or any time, UINT32_MAX: never): GPS power management
uint32_t dmrDataServiceLocationNeededInMs(void);
void dmrDataServicePowerOffCancelled(void);

// Main task context, for each text message received (a weak no-op unless the platform's UI has one)
void dmrDataServiceMessageReceived(const dmrDataMessage_t *message);

// The inbox: the last DMR_DATA_INBOX_SIZE messages, shared by the USB host (which pops the oldest it hasn't had yet)
// and the radio's UI (which reads them all, newest first, and marks them read). A new message replaces the oldest
int dmrDataServiceInboxCount(void);// messages the USB host hasn't popped
bool dmrDataServiceInboxPop(dmrDataMessage_t *message);
int dmrDataServiceMessageCount(void);
dmrDataMessage_t *dmrDataServiceMessage(int index);// 0: the newest

// Raw received bursts kept for the host (USB 'D' commands)
// The TX burst list (DMR_DATA_MAX_BURSTS), lent to the hotspot: the service never transmits in hotspot mode
dmrBurst_t *dmrDataServiceTxBursts(void);

int dmrDataServiceRxBurstCount(void);
bool dmrDataServiceRxBurstPop(dmrBurst_t *burst);

// USB 'D' command handler, see usb_com.c
int dmrDataServiceHandleUSB(const uint8_t *request, uint8_t *reply);

#endif
