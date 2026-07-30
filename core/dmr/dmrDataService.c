// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR data service: SMS (Motorola TMS) inbox, ACKs and the host interface
 */

#include <stdio.h>
#include <string.h>
#include "dmr/dmrDataService.h"
// TODO(hal): the legacy firmware headers below go when this moves onto hal/. Used from them: trxGetMode,
// trxCheckFrequencyInAmateurBand, trxCarrierDetected, trxGetDMRTimeSlot, trxGetDMRColourCode, trxDMRID,
// trxTalkGroupOrPcId, trxDMRModeRx; HRC6000DataTxStart, HRC6000DataTxGetStatus, HRC6000GetReceivedSrcId,
// HRC6000SetDmrRxGain; settingsIsOptionBitSet(FromSettings), settingsSetOptionBit, settingsSetDirty, settingsUsbMode,
// nonVolatileSettings, originalNonVolatileSettings; codeplugChannelGetFlag / codeplugChannelIsFlagSet,
// currentChannelData, currentRxGroupData; contactIDLookup, dmrIDLookup; soundSetMelody, soundMelodyIsPlaying;
// uiNotificationShow, displayLightTrigger, uiDataGlobal; rxPowerSavingSetState; ticksGetMillis; the USB CDC buffers.
#include "hardware/HR-C6000.h"
#include "functions/trx.h"
#include "functions/settings.h"
#include "functions/sound.h"
#include "functions/ticks.h"
#include "functions/rxPowerSaving.h"
#include "user_interface/uiGlobals.h"
#include "usb/usb_com.h"
#include "usb/usb_ncm.h"
#include "dmr/ipGateway.h"
#include "dmr/dmrLrrp.h"
#include "functions/codeplug.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiUtilities.h"

#define MILLIS()				ticksGetMillis()
#if defined(PLATFORM_MD380)// the 2023 MD-380 tree: its own channel flag and melody API, no radio device abstraction
#define CHANNEL_IS_RX_ONLY()	codeplugChannelIsFlagSet(currentChannelData, CHANNEL_FLAG_RX_ONLY)
typedef int melodyNote_t;
#else
#define CHANNEL_IS_RX_ONLY()	(codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_RX_ONLY) != 0)
typedef int16_t melodyNote_t;
#define HAS_RADIO_DEVICES		// currentRadioDevice, trxCarrierDetected(device)
#endif
#if !defined(STM32F405xx) && !defined(PLATFORM_DM32)
#undef HAS_RADIO_DEVICES
#endif
#if !defined(NOTIFICATION_MESSAGE_LEN_MAX)// TODO(hal): the notification text limit comes from the UI layer
#define NOTIFICATION_MESSAGE_LEN_MAX	MESSAGEBOX_MESSAGE_LEN_MAX
#endif

#define RX_BURST_QUEUE_SIZE		8
#define USB_BUFFER_SIZE			300
#define START_TIMEOUT_MS		10000	// a transmission still waiting for a clear channel after this is dropped
#define REPLY_TIMEOUT_MS		3000	// the same for an ACK or CSBK answer: later than this the sender has given up
#define REPLY_HOLDOFF_MS		180		// through a repeater: our slot quiet this long means the sender has finished
#define LBT_CLEAR_MS			120		// direct mode: nothing heard for this long before transmitting (listen before talk). A
												// radio sends a burst every 60 ms, so this is at least one frame with none
#define CALL_ALERT_REPEAT_MS	10000	// the caller retries until it hears the ack: ring once
#define CALL_ALERT_SHOW_MS		10000
// Motorola ARS: registration with the presence server (the "ARS radio ID"), as MOTOTRBO radios do it: 5-15 s after power
// on (spread by radio ID, so that radios switched on together don't collide), and again once the channel, our ID or the
// server has been unchanged for ARS_SETTLE_MS. A registration the server doesn't acknowledge is retried ARS_RETRIES times,
// ARS_RETRY_FIRST_MS apart and doubling (30 s to 8 min), then left until something changes: with no server on the
// channel, MOTOTRBO radios retry for ever
#define ARS_POWER_ON_MIN_MS		5000
#define ARS_POWER_ON_SPREAD_MS	10000
#define ARS_SETTLE_MS			5000
#define ARS_RETRY_FIRST_MS		30000
#define ARS_RETRIES				5
// Registered: again after the refresh time the server's answer gives (never when it gives 0, refresh not expected), or
// this when its answer has no refresh time, so that a server that was restarted, or ages radios out, learns we're still here
#define ARS_REFRESH_DEFAULT_MS	(30U * 60U * 1000U)
#if defined(HAS_SOFT_VOLUME)
#define CALL_ALERT_LOUD_GAIN	16		// HR-C6000 line out gain while the alert rings with BIT_CALL_ALERT_LOUD (knob: -31..31)
#endif

// USB 'D' sub commands
enum
{
	USB_DATA_CLEAR = 1,				// clear the byte buffer and the burst list
	USB_DATA_APPEND_BYTES,			// [len][bytes]
	USB_DATA_APPEND_BURST,			// [dataType][flags][len][payload]
	USB_DATA_SEND,					// [kind][flags][dst 3][port or dpf/sap 2]
	USB_DATA_STATUS,				// -> [tx status][rx bursts][inbox count]
	USB_DATA_POP_BURST,				// -> [1][dataType][flags][len][payload] or [0]
	USB_DATA_POP_MESSAGE,			// -> [1][src 3][dst 3][group][len][text] or [0]
	USB_DATA_NETWORK_MODE			// [0 off / 1 on / 0xFF query] -> [result][mode saved for the next boot][network link up]
};

enum { USB_SEND_BURSTS = 0, USB_SEND_TMS, USB_SEND_UDP, USB_SEND_PACKET };

#define USB_SEND_FLAG_GROUP		0x01
#define USB_SEND_FLAG_ACK		0x02
#define USB_SEND_RATE_SHIFT		2		// flag bits 2-3: the block rate of kinds 1-3, 0 Rate 1/2, 1 Rate 3/4, 2 Rate 1

DMR_DATA_BUFFER static dmrBurst_t txBursts[DMR_DATA_MAX_BURSTS];
static int txBurstCount = 0;
static volatile bool txPending = false;
static uint32_t txPendingSince;
static bool txIsReply;

static uint8_t smsSeq = 0;
// Replies to a received message: the ETSI response for confirmed data, then the TMS ACK in a transmission of its own,
// as MOTOTRBO radios send it (a repeater or hotspot takes one transmission for one data call). Queued by the HR-C6000
// task (single producer) and sent by the main task (single consumer), so messages arriving back to back each get theirs
#define REPLY_QUEUE_SIZE		5		// one slot is kept free: 4 replies
#define REPLY_QUEUE_MAX_AGE_MS	5000	// a reply still queued after this is dropped: the sender has given up
typedef struct
{
	uint32_t queuedAt;
	uint32_t dst;
	bool response;						// ETSI response (ACK) for confirmed data
	uint8_t sap;
	uint8_t sendSeq;
	bool tms;							// Motorola TMS ACK
	uint8_t tmsSeq;
} pendingReply_t;
static pendingReply_t replyQueue[REPLY_QUEUE_SIZE];
static volatile uint8_t replyWriteIdx = 0;
static volatile uint8_t replyReadIdx = 0;
static bool tmsAckPending = false;
static uint32_t tmsAckDst;
static uint8_t tmsAckSeq;
static volatile bool newMessage = false;
#define DUPLICATE_MESSAGE_MS	60000	// the same message again from the same radio this soon is a retry: ACKed, not kept

// Call alert ack or radio check answer, sent on its own
static volatile bool csbkAnswerPending = false;
static dmrBurst_t csbkAnswer;
static volatile uint32_t lastRxBurstTime;
static uint32_t channelBusyTime;
static volatile bool callAlertPending = false;
static uint32_t callAlertSrc;
static uint32_t lastCallAlertSrc = 0;
static uint32_t lastCallAlertTime;
#if defined(HAS_SOFT_VOLUME)
static bool callAlertGainRaised = false;
#endif

// Ring twice, like a phone
static const melodyNote_t MELODY_CALL_ALERT[] = {
		1000, 50, 1300, 50, 1000, 50, 1300, 50, 1000, 50, 1300, 50, 1000, 50, 1300, 50, 1000, 50, 1300, 50, 0, 400,
		1000, 50, 1300, 50, 1000, 50, 1300, 50, 1000, 50, 1300, 50, 1000, 50, 1300, 50, 1000, 50, 1300, 50,
		-1, -1 };

#if defined(HAS_DMR_ARS)
static struct
{
	uint32_t seen;							// what to register (arsContext()) as last seen, and since when
	uint32_t seenSince;
	uint32_t settleMs;						// how long it must stay unchanged: the power on delay at first
	uint32_t context;						// what was registered, or is being: 0 nothing
	uint32_t server;						// the server registered with, deregistered when ARS is switched off
	uint32_t due;							// the next attempt
	uint8_t attempts;
	bool registered;						// the server acknowledged
	bool refused;							// the server's last answer refused the registration
	uint32_t registeredAt;
	uint32_t refreshMs;						// re-register after this, 0: never
	bool deregister;						// a deregistration to the old server is waiting to go
	uint8_t powerOff;						// ARS_POWER_OFF_xxx: deregistering because the radio is being switched off
} ars;
enum { ARS_POWER_OFF_NONE = 0, ARS_POWER_OFF_SENDING, ARS_POWER_OFF_DONE };
static volatile bool arsAckReceived = false;
static volatile bool arsNakReceived = false;
static volatile uint16_t arsAckRefreshMinutes = DMR_ARS_REFRESH_NOT_GIVEN;
static volatile bool arsQueryReceived = false;
#endif

DMR_DATA_BUFFER static dmrDataMessage_t inbox[DMR_DATA_INBOX_SIZE];
static volatile uint8_t inboxNewest = 0;
static volatile uint8_t inboxUsed = 0;

static dmrBurst_t rxBursts[RX_BURST_QUEUE_SIZE];
static volatile uint8_t rxBurstWriteIdx = 0;
static volatile uint8_t rxBurstReadIdx = 0;

// An IP packet received over the air (ICMP, UDP or SCTP, from layer 4 on), waiting for the main task to give it to the
// USB network gateway
static volatile bool airToHostPending = false;
DMR_DATA_BUFFER static struct
{
	bool group;
	uint32_t dst;
	uint32_t src;
	uint8_t protocol;
	int length;
	uint8_t l4[DMR_DATA_MAX_PACKET];
} airToHost;

// Every data burst received, for the USB network host (see IPGW_MONITOR_PORT)
#define MONITOR_RECORD_HEADER	6
static uint8_t monitorRecords[RX_BURST_QUEUE_SIZE][MONITOR_RECORD_HEADER + DMR_BURST_PAYLOAD_MAX];
static volatile uint8_t monitorWriteIdx = 0;
static volatile uint8_t monitorReadIdx = 0;

DMR_DATA_BUFFER static uint8_t usbBuffer[USB_BUFFER_SIZE];
static int usbBufferLength = 0;

// TODO(hal): the radio layer should answer "may we transmit on this channel now" (band limits, RX only)
static bool txFrequencyAllowed(void)
{
	if (nonVolatileSettings.txFreqLimited != BAND_LIMITS_NONE)
	{
		return trxCheckFrequencyInAmateurBand(currentChannelData->txFreq);
	}
	return true;
}

static bool canTransmit(void)
{
	if ((settingsUsbMode == USB_MODE_HOTSPOT) || (trxGetMode() != RADIO_MODE_DIGITAL) || CHANNEL_IS_RX_ONLY())
	{
		return false;
	}
	return txFrequencyAllowed();
}

bool dmrDataServiceIsBusy(void)
{
	return txPending || (HRC6000DataTxGetStatus() == DMR_DATA_TX_RUNNING);
}

static bool queueTx(int count)
{
	if (count <= 0)
	{
		return false;
	}

	txBurstCount = count;
	txPendingSince = MILLIS();
	channelBusyTime = txPendingSince;// listen for LBT_CLEAR_MS at least
	txIsReply = false;
	txPending = true;
	return true;
}

// ACKs and CSBK answers: sent in the repeater's hang time, and only worth sending for REPLY_TIMEOUT_MS
static void queueReply(int count)
{
	if (queueTx(count))
	{
		txIsReply = true;
	}
}

static bool sendSMS(bool group, uint32_t dst, const char *text, bool ackRequested, uint8_t blockType)
{
	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	smsSeq = (smsSeq + 1) & 0x1F;
	return queueTx(dmrDataBuildTMS(group, dst, trxDMRID, text, smsSeq, ackRequested, blockType, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS));
}

static bool sendUDP(bool group, uint32_t dst, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length, uint8_t blockType)
{
	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	return queueTx(dmrDataBuildUDP(group, dst, trxDMRID, srcPort, dstPort, payload, length, blockType, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS));
}

bool dmrDataServiceSendSMS(bool group, uint32_t dst, const char *text, bool ackRequested)
{
	return sendSMS(group, dst, text, ackRequested, DT_RATE_12_DATA);
}

bool dmrDataServiceSendUDP(bool group, uint32_t dst, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	return sendUDP(group, dst, srcPort, dstPort, payload, length, DT_RATE_12_DATA);
}

bool dmrDataServiceSendBursts(const dmrBurst_t *bursts, int count)
{
	if (dmrDataServiceIsBusy() || !canTransmit() || (count > DMR_DATA_MAX_BURSTS))
	{
		return false;
	}

	memmove(txBursts, bursts, count * sizeof(dmrBurst_t));
	return queueTx(count);
}

// TODO(hal): name lookup (contacts, then the DMR ID database, else "ID:n") belongs to the codeplug / UI layer
static void callerName(uint32_t id, char *name, size_t size)
{
	dmrIdDataStruct_t record;

	if (contactIDLookup(id, CONTACT_CALLTYPE_PC, name))
	{
		return;
	}
	dmrIDLookup(id, &record);
	snprintf(name, size, "%s", record.text);
}

static void showCallAlert(uint32_t src)
{
	char name[MAX_DMR_ID_CONTACT_TEXT_LENGTH];
	char message[NOTIFICATION_MESSAGE_LEN_MAX];

	callerName(src, name, sizeof(name));
	snprintf(message, sizeof(message), "Call alert\n%s", name);

	displayLightTrigger(true);
	uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, CALL_ALERT_SHOW_MS, message, true);

#if defined(HAS_SOFT_VOLUME)
	// The volume knob only sets the HR-C6000 line out gain, which the beeps go through as well
	if (settingsIsOptionBitSet(BIT_CALL_ALERT_LOUD) && (lastVolume < CALL_ALERT_LOUD_GAIN))
	{
		HRC6000SetDmrRxGain(CALL_ALERT_LOUD_GAIN);
		callAlertGainRaised = true;
	}
#endif
	soundSetMelody(MELODY_CALL_ALERT);
}

// Listen before talk, for everything the service transmits. Direct mode: no voice call (its terminator clears the ID),
// and for LBT_CLEAR_MS no data burst and no carrier, decodable or not. The HR-C6000 slot state isn't used: without a
// terminator (data, CSBKs) it only goes idle on the voice fade timeout, 200-400 ms after the last burst. A repeater's
// outbound carrier stays up through its hang time, so there: our slot has had no data bursts for REPLY_HOLDOFF_MS, and
// unless this is a reply (expected in the hang time), no call is going on on our slot. The caller's ID is only cleared
// when the repeater stops, so new traffic waits for the hang time to end
static bool channelIsClear(bool isReply)
{
#if defined(HAS_RADIO_DEVICES)
	if (currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO)
#else
	if (trxDMRModeRx == DMR_MODE_RMO)
#endif
	{
		return ((MILLIS() - lastRxBurstTime) > REPLY_HOLDOFF_MS) && (isReply || (HRC6000GetReceivedSrcId() == 0));
	}

	if ((HRC6000GetReceivedSrcId() != 0) || ((MILLIS() - lastRxBurstTime) < LBT_CLEAR_MS) ||
#if defined(HAS_RADIO_DEVICES)
			trxCarrierDetected(RADIO_DEVICE_PRIMARY))
#else
			trxCarrierDetected())
#endif
	{
		channelBusyTime = MILLIS();
		return false;
	}

	return ((MILLIS() - channelBusyTime) > LBT_CLEAR_MS);
}

#if defined(HAS_DMR_ARS)
// The ARS radio ID, 0 when ARS is off. Saved settings only: while an options menu is open its changes aren't, and
// leaving it with Red puts the saved ones back (originalNonVolatileSettings holds them until then)
static uint32_t arsServer(void)
{
	settingsStruct_t *saved = ((uint32_t)originalNonVolatileSettings.magicNumber != 0xDEADBEEF) ? &originalNonVolatileSettings : &nonVolatileSettings;

	if (!settingsIsOptionBitSetFromSettings(saved, BIT_DMR_ARS))
	{
		return 0;
	}

	return (saved->dmrArsId[0] << 16) | (saved->dmrArsId[1] << 8) | saved->dmrArsId[2];
}

// What a registration is for: the server, our ID and the channel (frequency, timeslot, colour code). 0 when there is
// nothing to register (ARS off, or a channel we can't send data on)
static uint32_t arsContext(uint32_t server)
{
	if ((server == 0) || !canTransmit())
	{
		return 0;
	}

	uint32_t h = 2166136261u;// FNV-1a
	uint32_t values[] = { server, trxDMRID, currentChannelData->txFreq, trxGetDMRTimeSlot(), trxGetDMRColourCode() };

	for (unsigned int i = 0; i < (sizeof(values) / sizeof(values[0])); i++)
	{
		h = (h ^ values[i]) * 16777619u;
	}

	return (h != 0) ? h : 1;
}

static bool arsSend(uint32_t server, bool registration)
{
	uint8_t pdu[DMR_ARS_MAX_PDU];
	int length = registration ? dmrDataBuildARSRegistration(trxDMRID, pdu) : dmrDataBuildARSDeregistration(pdu);

	return queueTx(dmrDataBuildUDP(false, server, trxDMRID, DMR_UDP_PORT_ARS, DMR_UDP_PORT_ARS, pdu, length,
									DT_RATE_12_DATA, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS));
}

static void arsTick(void)
{
	if (ars.powerOff != ARS_POWER_OFF_NONE)
	{
		return;
	}

	uint32_t now = MILLIS();
	uint32_t server = arsServer();
	uint32_t context = arsContext(server);

	if (ars.settleMs == 0)
	{
		ars.settleMs = ARS_POWER_ON_MIN_MS + (((trxDMRID * 2654435761u) ^ now) % ARS_POWER_ON_SPREAD_MS);
		ars.seen = context;
		ars.seenSince = now;
	}

	if (arsAckReceived)
	{
		arsAckReceived = false;
		ars.registered = (ars.context != 0);
		ars.refused = false;
		ars.registeredAt = now;
		ars.refreshMs = (arsAckRefreshMinutes != DMR_ARS_REFRESH_NOT_GIVEN) ? (arsAckRefreshMinutes * 60000U) : ARS_REFRESH_DEFAULT_MS;
	}

	if (arsNakReceived)
	{
		arsNakReceived = false;// refused: tried again on the retry schedule, as if it hadn't answered
		ars.registered = false;
		ars.refused = true;
	}

	if (ars.registered && (ars.refreshMs != 0) && ((now - ars.registeredAt) >= ars.refreshMs))
	{
		ars.registered = false;// refresh
		ars.attempts = 0;
		ars.due = now;
	}

	if (arsQueryReceived)
	{
		arsQueryReceived = false;// the server asks us to register again
		ars.registered = false;
		ars.attempts = 0;
		ars.due = now;
	}

	if (context != ars.seen)
	{
		ars.seen = context;
		ars.seenSince = now;
	}

	if ((now - ars.seenSince) >= ars.settleMs)
	{
		ars.settleMs = ARS_SETTLE_MS;

		if (context != ars.context)
		{
			// Switched off, or to another server: say goodbye to the one we registered with (or tried to: a server
			// that doesn't acknowledge may still be listening). A channel we can't send on (analog, RX only, hotspot)
			// isn't a reason to, the registration still holds when we come back
			if ((ars.context != 0) && (server != ars.server) && (ars.registered || (ars.attempts > 0)))
			{
				ars.deregister = true;
			}

			if ((context != 0) || (server != ars.server))
			{
				ars.context = context;
				ars.registered = false;
				ars.refused = false;
				ars.attempts = 0;
				ars.due = now;
			}
		}
	}

	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return;
	}

	if (ars.deregister)
	{
		ars.deregister = false;
		arsSend(ars.server, false);
		return;
	}

	if ((ars.context != 0) && (ars.context == context) && !ars.registered && (ars.attempts <= ARS_RETRIES) &&
			((int32_t)(now - ars.due) >= 0) && arsSend(server, true))
	{
		ars.server = server;
		ars.due = now + (ARS_RETRY_FIRST_MS << ars.attempts);
		ars.attempts++;
	}
}
#endif

#if defined(HAS_DMR_ARS)
dmrArsState_t dmrDataServiceArsState(void)
{
	if (arsServer() == 0)
	{
		return DMR_ARS_STATE_OFF;
	}
	if (ars.registered)
	{
		return DMR_ARS_STATE_REGISTERED;
	}
	if (ars.refused)
	{
		return DMR_ARS_STATE_REFUSED;
	}
	return (ars.attempts > ARS_RETRIES) ? DMR_ARS_STATE_NO_ANSWER : DMR_ARS_STATE_REGISTERING;
}
#endif

#if defined(HAS_DMR_GPS_REPORTS)
// Location reports (ETSI UDT NMEA) to GPS_REPORT_ID_GET(): every gpsReportInterval seconds, with preambles and listen
// before talk like the other data, and with GPS_REPORT_FLAG_AFTER_CALL straight after each voice call of ours, without
// preambles (the receivers are in sync) and in the repeater's hang time. Only when the platform has a position
static volatile bool voiceTxEnded = false;
static bool gpsReportScheduled = false;
static uint32_t gpsReportDue;

// The platform's position (main task), false when it has none. Weak: no GPS (the host test, which includes this file,
// brings its own)
#if !defined(DMR_DATA_SERVICE_TEST)
__attribute__((weak)) bool dmrDataServiceGetLocation(dmrDataLocation_t *location)
{
	(void)location;
	return false;
}
#endif

// HR-C6000 / HR-C5000 driver (ISR): a voice transmission of ours has just ended (its terminator went)
void dmrDataServiceVoiceTxEnded(void)
{
	voiceTxEnded = true;
}

static bool gpsReportQueue(int preambles, bool isReply)
{
	dmrDataLocation_t location;
	uint32_t dst = GPS_REPORT_ID_GET();

	if ((dst == 0) || dmrDataServiceIsBusy() || !canTransmit() || !dmrDataServiceGetLocation(&location))
	{
		return false;
	}

	int count = dmrDataBuildUDTLocation(((nonVolatileSettings.gpsReportFlags & GPS_REPORT_FLAG_GROUP) != 0), dst, trxDMRID, &location,
										preambles, txBursts, DMR_DATA_MAX_BURSTS);

	if (isReply)
	{
		queueReply(count);
		return (count > 0);
	}

	return queueTx(count);
}

static void gpsReportTick(void)
{
	uint32_t now = MILLIS();

	if (voiceTxEnded)
	{
		voiceTxEnded = false;

		if (nonVolatileSettings.gpsReportFlags & GPS_REPORT_FLAG_AFTER_CALL)
		{
			gpsReportQueue(0, true);
		}
	}

	if ((nonVolatileSettings.gpsReportInterval == 0) || (GPS_REPORT_ID_GET() == 0))
	{
		gpsReportScheduled = false;
		return;
	}

	if (!gpsReportScheduled)
	{
		gpsReportScheduled = true;
		gpsReportDue = now + (nonVolatileSettings.gpsReportInterval * 1000U);
	}

	if ((int32_t)(now - gpsReportDue) >= 0)
	{
		if (dmrDataServiceIsBusy() || !canTransmit())
		{
			return;// when the channel is free again
		}

		gpsReportQueue(DMR_DATA_SMS_PREAMBLES, false);
		gpsReportDue = now + (nonVolatileSettings.gpsReportInterval * 1000U);
	}
}
#endif

#if defined(HAS_DMR_LRRP)
// Motorola LRRP (UDP 4001): a location server (RadioDesk, TRBOnet, ...) asks for a position now, or starts reports
// every n seconds or every n meters (checked every interval, 30 s when it gives none), and stops them. Requests come
// in from the HR-C6000 task (single producer), are answered from the main task; up to LRRP_TRIGGERS_MAX triggers run
#define LRRP_QUEUE_SIZE				3		// one slot kept free
#define LRRP_TRIGGERS_MAX			2
#define LRRP_MIN_INTERVAL_S			5
#define LRRP_DISTANCE_CHECK_S		30
#define LRRP_FIRST_REPORT_MS		5000	// a new trigger's first report, after its start response

typedef struct
{
	uint32_t src;
	lrrpRequest_t request;
} lrrpPendingRequest_t;
static lrrpPendingRequest_t lrrpQueue[LRRP_QUEUE_SIZE];
static volatile uint8_t lrrpWriteIdx = 0;
static volatile uint8_t lrrpReadIdx = 0;

typedef struct
{
	bool active;
	uint32_t requester;
	uint8_t requestId[LRRP_REQUEST_ID_MAX];
	uint8_t requestIdLength;
	uint8_t wanted;
	uint32_t intervalMs;
	uint32_t distanceMeters;					// 0: periodic only
	uint32_t due;
	uint32_t startedAt;
	bool haveLast;
	dmrDataLocation_t last;					// where the last report was sent from (distance trigger)
} lrrpTrigger_t;
static lrrpTrigger_t lrrpTriggers[LRRP_TRIGGERS_MAX];

// The platform's position with the date and course for LRRP (main task). Weak: from dmrDataServiceGetLocation()
#if !defined(DMR_DATA_SERVICE_TEST)
__attribute__((weak)) bool dmrDataServiceGetPosition(lrrpPosition_t *position)
{
	memset(position, 0, sizeof(*position));
	return dmrDataServiceGetLocation(&position->location);
}
#endif

// HR-C6000 task: an LRRP document to us
static void lrrpReceived(uint32_t src, const uint8_t *payload, int length)
{
	uint8_t next = (lrrpWriteIdx + 1) % LRRP_QUEUE_SIZE;
	lrrpPendingRequest_t *pending = &lrrpQueue[lrrpWriteIdx];

	if ((next == lrrpReadIdx) || !lrrpDecodeRequest(payload, length, &pending->request))
	{
		return;// full (the server asks again), or not a request
	}

	pending->src = src;
	__asm__ volatile ("" ::: "memory");
	lrrpWriteIdx = next;
}

static bool lrrpSend(uint32_t dst, const uint8_t *doc, int length, int preambles)
{
	if (length <= 0)
	{
		return false;
	}

	return queueTx(dmrDataBuildUDP(false, dst, trxDMRID, DMR_UDP_PORT_LRRP, DMR_UDP_PORT_LRRP, doc, length,
									DT_RATE_12_DATA, preambles, txBursts, DMR_DATA_MAX_BURSTS));
}

static lrrpTrigger_t *lrrpFindTrigger(uint32_t requester, const uint8_t *requestId, uint8_t requestIdLength)
{
	for (int i = 0; i < LRRP_TRIGGERS_MAX; i++)
	{
		lrrpTrigger_t *t = &lrrpTriggers[i];

		if (t->active && (t->requester == requester) && (t->requestIdLength == requestIdLength) &&
				(memcmp(t->requestId, requestId, requestIdLength) == 0))
		{
			return t;
		}
	}

	return NULL;
}

// A request from the queue; false while the service can't send its answer yet
static bool lrrpHandleRequest(const lrrpPendingRequest_t *pending)
{
	const lrrpRequest_t *request = &pending->request;
	uint8_t doc[LRRP_MAX_DOCUMENT];
	lrrpPosition_t position;
	uint32_t now = MILLIS();

	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	switch (request->type)
	{
		case LRRP_REQUEST_IMMEDIATE:
			lrrpSend(pending->src, doc, lrrpBuildImmediateResponse(request, (dmrDataServiceGetPosition(&position) ? &position : NULL), doc),
						DMR_DATA_SMS_PREAMBLES);
			break;

		case LRRP_REQUEST_TRIGGERED_START:
			{
				bool usable = (request->triggers & (LRRP_TRIGGER_PERIODIC | LRRP_TRIGGER_DISTANCE)) &&
						((request->intervalSeconds != 0) || (request->distanceMeters != 0));

				if (!usable)
				{
					// Emergency or one shot triggers: this radio has no emergency, so they'd never fire
					lrrpSend(pending->src, doc, lrrpBuildTriggeredStartResponse(request->requestId, request->requestIdLength,
								LRRP_RESULT_NOT_UNDERSTOOD, doc), DMR_DATA_SMS_PREAMBLES);
					break;
				}

				lrrpTrigger_t *t = lrrpFindTrigger(pending->src, request->requestId, request->requestIdLength);

				if (t == NULL)
				{
					t = &lrrpTriggers[0];// a free slot, else the oldest
					for (int i = 0; i < LRRP_TRIGGERS_MAX; i++)
					{
						if (!lrrpTriggers[i].active)
						{
							t = &lrrpTriggers[i];
							break;
						}
						if ((int32_t)(lrrpTriggers[i].startedAt - t->startedAt) < 0)
						{
							t = &lrrpTriggers[i];
						}
					}
				}

				uint32_t interval = request->intervalSeconds;

				if (interval == 0)
				{
					interval = LRRP_DISTANCE_CHECK_S;
				}
				if (interval < LRRP_MIN_INTERVAL_S)
				{
					interval = LRRP_MIN_INTERVAL_S;
				}

				memset(t, 0, sizeof(*t));
				t->active = true;
				t->requester = pending->src;
				memcpy(t->requestId, request->requestId, request->requestIdLength);
				t->requestIdLength = request->requestIdLength;
				t->wanted = request->wanted;
				t->intervalMs = interval * 1000U;
				t->distanceMeters = (request->triggers & LRRP_TRIGGER_DISTANCE) ? request->distanceMeters : 0;
				t->startedAt = now;
				t->due = now + LRRP_FIRST_REPORT_MS;

				lrrpSend(pending->src, doc, lrrpBuildTriggeredStartResponse(request->requestId, request->requestIdLength, LRRP_RESULT_SUCCESS, doc),
							DMR_DATA_SMS_PREAMBLES);
			}
			break;

		case LRRP_REQUEST_TRIGGERED_STOP:
			{
				lrrpTrigger_t *t = lrrpFindTrigger(pending->src, request->requestId, request->requestIdLength);

				if (t != NULL)
				{
					t->active = false;
				}
				lrrpSend(pending->src, doc, lrrpBuildTriggeredStopResponse(request->requestId, request->requestIdLength, doc), DMR_DATA_SMS_PREAMBLES);
			}
			break;

		default:
			break;// protocol requests: not answered
	}

	return true;
}

static void lrrpTick(void)
{
	uint32_t now = MILLIS();

	if (lrrpReadIdx != lrrpWriteIdx)
	{
		if (lrrpHandleRequest(&lrrpQueue[lrrpReadIdx]))
		{
			__asm__ volatile ("" ::: "memory");
			lrrpReadIdx = (lrrpReadIdx + 1) % LRRP_QUEUE_SIZE;
		}
		return;// one transmission at a time
	}

	for (int i = 0; i < LRRP_TRIGGERS_MAX; i++)
	{
		lrrpTrigger_t *t = &lrrpTriggers[i];
		lrrpPosition_t position;

		if (!t->active || ((int32_t)(now - t->due) < 0))
		{
			continue;
		}

		if (dmrDataServiceIsBusy() || !canTransmit())
		{
			return;// when the channel is free again
		}

		t->due = now + t->intervalMs;

		if (!dmrDataServiceGetPosition(&position))
		{
			continue;// no position at all: nothing to report
		}

		if ((t->distanceMeters != 0) && t->haveLast && position.location.fixValid &&
				(lrrpDistanceMeters(&t->last, &position.location) < t->distanceMeters))
		{
			continue;// hasn't moved far enough
		}

		uint8_t doc[LRRP_MAX_DOCUMENT];

		if (lrrpSend(t->requester, doc, lrrpBuildTriggeredReport(t->requestId, t->requestIdLength, t->wanted, &position, doc),
						DMR_DATA_SMS_PREAMBLES))
		{
			if (position.location.fixValid)
			{
				t->last = position.location;
				t->haveLast = true;
			}
		}
		return;
	}
}

int dmrDataServiceLrrpTriggerCount(void)
{
	int n = 0;

	for (int i = 0; i < LRRP_TRIGGERS_MAX; i++)
	{
		n += lrrpTriggers[i].active ? 1 : 0;
	}
	return n;
}
#endif

// Milliseconds until the service next needs a position (0: now or all the time, UINT32_MAX: not at all), for the
// platform's GPS power management
uint32_t dmrDataServiceLocationNeededInMs(void)
{
	uint32_t now = MILLIS();
	uint32_t soonest = UINT32_MAX;

#if defined(HAS_DMR_GPS_REPORTS)
	if (GPS_REPORT_ID_GET() != 0)
	{
		if (nonVolatileSettings.gpsReportFlags & GPS_REPORT_FLAG_AFTER_CALL)
		{
			return 0;// a call can end any time
		}
		if ((nonVolatileSettings.gpsReportInterval != 0) && gpsReportScheduled)
		{
			int32_t in = (int32_t)(gpsReportDue - now);

			soonest = (in <= 0) ? 0 : (uint32_t)in;
		}
	}
#endif
#if defined(HAS_DMR_LRRP)
	if (lrrpReadIdx != lrrpWriteIdx)
	{
		return 0;
	}
	for (int i = 0; i < LRRP_TRIGGERS_MAX; i++)
	{
		if (lrrpTriggers[i].active)
		{
			int32_t in = (int32_t)(lrrpTriggers[i].due - now);
			uint32_t ms = (in <= 0) ? 0 : (uint32_t)in;

			if (ms < soonest)
			{
				soonest = ms;
			}
		}
	}
#endif
	(void)now;
	return soonest;
}

// The radio is being switched off (the power off screen, main task): deregister from the ARS server if registered, or
// tried to. True while the deregistration still has to go out; the caller gives up on it after a while
bool dmrDataServicePowerOff(void)
{
#if defined(HAS_DMR_ARS)
	if (ars.powerOff == ARS_POWER_OFF_NONE)
	{
		if (!(ars.deregister || ((ars.context != 0) && (ars.registered || (ars.attempts > 0)))) || !canTransmit())
		{
			ars.powerOff = ARS_POWER_OFF_DONE;
			return false;
		}

		if (dmrDataServiceIsBusy())
		{
			return true;// after what's already on its way
		}

		ars.deregister = false;
		ars.powerOff = arsSend(ars.server, false) ? ARS_POWER_OFF_SENDING : ARS_POWER_OFF_DONE;
	}

	return (ars.powerOff == ARS_POWER_OFF_SENDING) && dmrDataServiceIsBusy();
#else
	return false;
#endif
}

// Switched back on before the power off finished: register again if we deregistered
void dmrDataServicePowerOffCancelled(void)
{
#if defined(HAS_DMR_ARS)
	if (ars.powerOff == ARS_POWER_OFF_SENDING)
	{
		ars.context = 0;
	}
	ars.powerOff = ARS_POWER_OFF_NONE;
#endif
}

void dmrDataServiceTick(void)
{
	if (csbkAnswerPending && !dmrDataServiceIsBusy() && canTransmit())
	{
		txBursts[0] = csbkAnswer;
		csbkAnswerPending = false;
		queueReply(1);
	}

	if (callAlertPending)
	{
		callAlertPending = false;
		if ((callAlertSrc != lastCallAlertSrc) || ((MILLIS() - lastCallAlertTime) > CALL_ALERT_REPEAT_MS))
		{
			showCallAlert(callAlertSrc);
		}
		lastCallAlertSrc = callAlertSrc;
		lastCallAlertTime = MILLIS();
	}

#if defined(HAS_SOFT_VOLUME)
	if (callAlertGainRaised && !soundMelodyIsPlaying())
	{
		callAlertGainRaised = false;
		HRC6000SetDmrRxGain(lastVolume);
	}
#endif

	// The next queued reply, once the last one's TMS ACK has gone
	while (!tmsAckPending && (replyReadIdx != replyWriteIdx) && !dmrDataServiceIsBusy() && canTransmit())
	{
		pendingReply_t reply = replyQueue[replyReadIdx];

		__asm__ volatile ("" ::: "memory");// copied before the producer may reuse the slot
		replyReadIdx = (replyReadIdx + 1) % REPLY_QUEUE_SIZE;

		if ((MILLIS() - reply.queuedAt) > REPLY_QUEUE_MAX_AGE_MS)
		{
			continue;
		}

		if (reply.tms)
		{
			tmsAckDst = reply.dst;
			tmsAckSeq = reply.tmsSeq;
			tmsAckPending = true;
		}
		if (reply.response)
		{
			queueReply(dmrDataBuildResponseAck(reply.sap, reply.dst, trxDMRID, reply.sendSeq, txBursts));
		}
		break;
	}

	// After the response has gone (the service is busy until then)
	if (tmsAckPending && !dmrDataServiceIsBusy() && canTransmit())
	{
		tmsAckPending = false;
		queueReply(dmrDataBuildTMSAck(tmsAckDst, trxDMRID, tmsAckSeq, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS));
	}

#if defined(HAS_DMR_ARS)
	arsTick();
#endif
#if defined(HAS_DMR_GPS_REPORTS)
	gpsReportTick();
#endif
#if defined(HAS_DMR_LRRP)
	lrrpTick();
#endif

	if (txPending)
	{
		rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE);

		if (channelIsClear(txIsReply) && HRC6000DataTxStart(txBursts, txBurstCount))
		{
			txPending = false;
		}
		else if ((MILLIS() - txPendingSince) > (txIsReply ? REPLY_TIMEOUT_MS : START_TIMEOUT_MS))
		{
			txPending = false;
		}
	}

	if (newMessage)
	{
		newMessage = false;
		soundSetMelody(MELODY_PRIVATE_CALL);
		dmrDataServiceMessageReceived(dmrDataServiceMessage(0));
	}

	while (monitorReadIdx != monitorWriteIdx)
	{
		uint8_t *record = monitorRecords[monitorReadIdx];

		if (usbNcmIsUp() && !ipGatewayDeliverMonitor(record, MONITOR_RECORD_HEADER + record[5]))
		{
			break;// USB IN busy, next tick
		}
		monitorReadIdx = (monitorReadIdx + 1) % RX_BURST_QUEUE_SIZE;
	}

	if (airToHostPending)
	{
		// Retried on the next tick if the USB IN endpoint is still busy, dropped if the link went down
		if (!usbNcmIsUp() || ipGatewayDeliverIP(airToHost.group, airToHost.dst, airToHost.src, airToHost.protocol,
													airToHost.l4, airToHost.length))
		{
			airToHostPending = false;
		}
	}
}

// The radio's own DMR ID from the codeplug, not trxDMRID: per channel IDs, manual overrides and the hotspot (which uses
// the caller's ID) change that, and the host's address must stay put
uint32_t ipGatewayRadioId(void)
{
	return (uiDataGlobal.userDMRId != 0) ? uiDataGlobal.userDMRId : trxDMRID;
}

// USB network gateway: ICMP, UDP or SCTP from the host to a radio ID or talkgroup
bool ipGatewayIPToAir(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length)
{
	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	return queueTx(dmrDataBuildIP(group, dst, trxDMRID, protocol, l4, length, DT_RATE_12_DATA, DMR_DATA_SMS_PREAMBLES,
									txBursts, DMR_DATA_MAX_BURSTS));
}

// USB network gateway: the payload of a datagram to IPGW_RAW_PORT, as the user data of a short data packet
bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length)
{
	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	return queueTx(dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_SHORT_DATA, group, dst, trxDMRID, data, length,
										DT_RATE_12_DATA, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS));
}

// TODO(hal): RX group membership from the codeplug layer
static bool inRxGroupList(uint32_t talkGroup)
{
	int remaining = currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup;
	const uint32_t *tg = currentRxGroupData.NOT_IN_CODEPLUG_contactsTG;

	while (remaining-- > 0)
	{
		if (*tg++ == talkGroup)
		{
			return true;
		}
	}
	return false;
}

static bool isForUs(const dmrDataPacket_t *packet)
{
	if (packet->src == trxDMRID)
	{
		return false;// our own transmission, repeated back
	}

	if (!packet->group)
	{
		return (packet->dst == trxDMRID);
	}

	if (packet->dst == (trxTalkGroupOrPcId & 0x00FFFFFF))
	{
		return true;
	}

	return inRxGroupList(packet->dst);
}

static void handlePacket(const dmrDataPacket_t *packet)
{
	dmrDataUDP_t udp;
	dmrDataTMS_t tms;
	bool isUDP = dmrDataGetUDP(packet, &udp);

	// ICMP, UDP and SCTP go to the USB network host, also between other radios so that a capture shows them. UDP is
	// rebuilt from its ports and payload (it may have come with a compressed header, SAP 3). Data that isn't IP based
	// goes as the payload of a datagram to IPGW_RAW_PORT
	if ((packet->src != trxDMRID) && usbNcmIsUp() && !airToHostPending)
	{
		dmrDataIP_t ip;

		if (isUDP && ((8 + udp.length) <= (int)sizeof(airToHost.l4)))
		{
			airToHost.protocol = DMR_IP_PROTO_UDP;
			airToHost.l4[0] = udp.srcPort >> 8;
			airToHost.l4[1] = udp.srcPort & 0xFF;
			airToHost.l4[2] = udp.dstPort >> 8;
			airToHost.l4[3] = udp.dstPort & 0xFF;
			memset(&airToHost.l4[4], 0, 4);// length and checksum, worked out by the gateway
			memcpy(&airToHost.l4[8], udp.payload, udp.length);
			airToHost.length = 8 + udp.length;
			airToHostPending = true;
		}
		else if (!isUDP && dmrDataGetIP(packet, &ip) && ((ip.protocol == DMR_IP_PROTO_ICMP) || (ip.protocol == DMR_IP_PROTO_SCTP)) &&
					(ip.length > 0) && (ip.length <= (int)sizeof(airToHost.l4)))
		{
			airToHost.protocol = ip.protocol;
			memcpy(airToHost.l4, ip.payload, ip.length);
			airToHost.length = ip.length;
			airToHostPending = true;
		}
		else if (((packet->dpf == DMR_DPF_UNCONFIRMED) || (packet->dpf == DMR_DPF_CONFIRMED)) &&
					(packet->sap != DMR_SAP_IP) && (packet->sap != DMR_SAP_UDPIP_COMPRESSION) &&
					(packet->length > 0) && ((8 + packet->length) <= (int)sizeof(airToHost.l4)))
		{
			airToHost.protocol = DMR_IP_PROTO_UDP;
			airToHost.l4[0] = IPGW_RAW_PORT >> 8;
			airToHost.l4[1] = IPGW_RAW_PORT & 0xFF;
			airToHost.l4[2] = IPGW_RAW_PORT >> 8;
			airToHost.l4[3] = IPGW_RAW_PORT & 0xFF;
			memset(&airToHost.l4[4], 0, 4);
			memcpy(&airToHost.l4[8], packet->data, packet->length);
			airToHost.length = 8 + packet->length;
			airToHostPending = true;
		}

		if (airToHostPending)
		{
			airToHost.group = packet->group;
			airToHost.dst = packet->dst;
			airToHost.src = packet->src;
		}
	}

	if (!isForUs(packet))
	{
		return;
	}

	// Confirmed data to us must be acknowledged, whatever it carries, or the sender keeps retrying
	pendingReply_t reply = {
			.queuedAt = MILLIS(),
			.dst = packet->src,
			.response = (packet->dpf == DMR_DPF_CONFIRMED) && packet->responseRequested && !packet->group,
			.sap = packet->sap,
			.sendSeq = packet->sendSeq,
			.tms = false,
			.tmsSeq = 0 };

#if defined(HAS_DMR_ARS)
	// The presence server's answer to our registration, or its request for one
	if (isUDP && (udp.appPort == DMR_UDP_PORT_ARS) && !packet->group && (packet->src == ars.server))
	{
		switch (dmrDataDecodeARS(udp.payload, udp.length))
		{
			case DMR_ARS_RESPONSE:
				{
					bool accepted;
					uint16_t refreshMinutes;

					if (dmrDataDecodeARSResponse(udp.payload, udp.length, &accepted, &refreshMinutes))
					{
						if (accepted)
						{
							arsAckRefreshMinutes = refreshMinutes;
							arsAckReceived = true;
						}
						else
						{
							arsNakReceived = true;
						}
					}
				}
				break;
			case DMR_ARS_QUERY:
				arsQueryReceived = true;
				break;
			default:
				break;
		}
	}
#endif

#if defined(HAS_DMR_LRRP)
	if (isUDP && (udp.appPort == DMR_UDP_PORT_LRRP) && !packet->group)
	{
		lrrpReceived(packet->src, udp.payload, udp.length);
	}
#endif

	bool isTMS = isUDP && (udp.appPort == DMR_UDP_PORT_TMS) && dmrDataDecodeTMS(udp.payload, udp.length, &tms) && !tms.isAck;

	if (isTMS && tms.ackRequested && !packet->group)
	{
		reply.tms = true;
		reply.tmsSeq = tms.seq;
	}

	if (reply.response || reply.tms)
	{
		uint8_t next = (replyWriteIdx + 1) % REPLY_QUEUE_SIZE;

		if (next != replyReadIdx)// full: the sender will retry
		{
			replyQueue[replyWriteIdx] = reply;
			__asm__ volatile ("" ::: "memory");// the entry is complete before the consumer can see it
			replyWriteIdx = next;
		}
	}

	if (!isTMS)
	{
		return;
	}

	// A sender that missed our ACK sends the message again: ACKed again above, but kept (and rung) once
	for (int i = 0; i < inboxUsed; i++)
	{
		const dmrDataMessage_t *m = dmrDataServiceMessage(i);

		if ((m->src == packet->src) && (m->dst == packet->dst) && (m->group == packet->group) && (m->seq == tms.seq) &&
				((MILLIS() - m->receivedAt) < DUPLICATE_MESSAGE_MS) && (strcmp(m->text, tms.text) == 0))
		{
			return;
		}
	}

	uint8_t next = (inboxUsed == 0) ? 0 : ((inboxNewest + 1) % DMR_DATA_INBOX_SIZE);// replaces the oldest when full
	dmrDataMessage_t *message = &inbox[next];
	message->src = packet->src;
	message->dst = packet->dst;
	message->group = packet->group;
	message->seq = tms.seq;
	message->receivedAt = MILLIS();
	message->hostPending = true;
	message->unread = true;
	memcpy(message->text, tms.text, sizeof(message->text));
	inboxNewest = next;
	if (inboxUsed < DMR_DATA_INBOX_SIZE)
	{
		inboxUsed++;
	}
	newMessage = true;
}

// Main task context: a text message to us has arrived. The platform's UI shows it (see uiMessages.c)
__attribute__((weak)) void dmrDataServiceMessageReceived(const dmrDataMessage_t *message)
{
	(void)message;
}

// Call alert and radio check to us: answered, and a call alert rings
static void handleCSBK(const uint8_t *csbk)
{
	if (csbkAnswerPending || (dmrDataBuildCSBKAnswer(csbk, trxDMRID, &csbkAnswer) == 0))
	{
		return;
	}

	csbkAnswerPending = true;

	if (((csbk[0] & 0x3F) == DMR_CSBKO_CALL_ALERT) && !callAlertPending)
	{
		callAlertSrc = (csbk[7] << 16) | (csbk[8] << 8) | csbk[9];
		callAlertPending = true;
	}
}

// HR-C6000 task context
void dmrDataServiceRxBurst(const dmrBurst_t *burst)
{
	lastRxBurstTime = MILLIS();// through a repeater, holds our transmissions back until the sender has finished

	uint8_t next = (rxBurstWriteIdx + 1) % RX_BURST_QUEUE_SIZE;

	if (next == rxBurstReadIdx)
	{
		rxBurstReadIdx = (rxBurstReadIdx + 1) % RX_BURST_QUEUE_SIZE;// drop the oldest
	}
	rxBursts[rxBurstWriteIdx] = *burst;
	rxBurstWriteIdx = next;

	next = (monitorWriteIdx + 1) % RX_BURST_QUEUE_SIZE;
	if (usbNcmIsUp() && (next != monitorReadIdx))
	{
		uint8_t *record = monitorRecords[monitorWriteIdx];

		record[0] = 1;// version
		record[1] = trxGetDMRTimeSlot() + 1;
		record[2] = trxGetDMRColourCode();
		record[3] = burst->dataType;
		record[4] = burst->flags;
		record[5] = burst->length;
		memcpy(&record[MONITOR_RECORD_HEADER], burst->payload, burst->length);
		monitorWriteIdx = next;
	}

	// In hotspot mode MMDVMHost gets the bursts (see hotspotDataTick), the radio itself is not the recipient
	if (settingsUsbMode != USB_MODE_HOTSPOT)
	{
		switch (dmrDataRxBurst(burst))
		{
			case DMR_DATA_RX_PACKET:
				handlePacket(&dmrDataRxPacket);
				break;
			case DMR_DATA_RX_CSBK:
				handleCSBK(burst->payload);
				break;
			default:
				break;
		}
	}
}

int dmrDataServiceMessageCount(void)
{
	return inboxUsed;
}

dmrDataMessage_t *dmrDataServiceMessage(int index)
{
	if ((index < 0) || (index >= inboxUsed))
	{
		return NULL;
	}

	return &inbox[(inboxNewest + DMR_DATA_INBOX_SIZE - index) % DMR_DATA_INBOX_SIZE];
}

int dmrDataServiceInboxCount(void)
{
	int count = 0;

	for (int i = 0; i < inboxUsed; i++)
	{
		if (dmrDataServiceMessage(i)->hostPending)
		{
			count++;
		}
	}

	return count;
}

// The oldest message the host hasn't had
bool dmrDataServiceInboxPop(dmrDataMessage_t *message)
{
	for (int i = inboxUsed - 1; i >= 0; i--)
	{
		dmrDataMessage_t *m = dmrDataServiceMessage(i);

		if (m->hostPending)
		{
			m->hostPending = false;
			*message = *m;
			return true;
		}
	}

	return false;
}

dmrBurst_t *dmrDataServiceTxBursts(void)
{
	return txBursts;
}

int dmrDataServiceRxBurstCount(void)
{
	return (rxBurstWriteIdx + RX_BURST_QUEUE_SIZE - rxBurstReadIdx) % RX_BURST_QUEUE_SIZE;
}

bool dmrDataServiceRxBurstPop(dmrBurst_t *burst)
{
	if (rxBurstReadIdx == rxBurstWriteIdx)
	{
		return false;
	}

	*burst = rxBursts[rxBurstReadIdx];
	rxBurstReadIdx = (rxBurstReadIdx + 1) % RX_BURST_QUEUE_SIZE;
	return true;
}

static bool usbSend(const uint8_t *r)
{
	bool group = (r[1] & USB_SEND_FLAG_GROUP) != 0;
	uint32_t dst = (r[2] << 16) | (r[3] << 8) | r[4];
	uint16_t port = (r[5] << 8) | r[6];
	static const uint8_t RATES[4] = { DT_RATE_12_DATA, DT_RATE_34_DATA, DT_RATE_1_DATA, 0 };
	uint8_t blockType = RATES[(r[1] >> USB_SEND_RATE_SHIFT) & 0x03];

	if ((r[0] != USB_SEND_BURSTS) && (blockType == 0))
	{
		return false;
	}

	switch (r[0])
	{
		case USB_SEND_BURSTS:
			return dmrDataServiceSendBursts(txBursts, txBurstCount);
		case USB_SEND_TMS:
			usbBuffer[(usbBufferLength < USB_BUFFER_SIZE) ? usbBufferLength : (USB_BUFFER_SIZE - 1)] = 0;
			return sendSMS(group, dst, (const char *)usbBuffer, (r[1] & USB_SEND_FLAG_ACK) != 0, blockType);
		case USB_SEND_UDP:
			return sendUDP(group, dst, port, port, usbBuffer, usbBufferLength, blockType);
		case USB_SEND_PACKET:
			if (dmrDataServiceIsBusy() || !canTransmit())
			{
				return false;
			}
			return queueTx(dmrDataBuildPacket(port >> 8, port & 0x0F, group, dst, trxDMRID, usbBuffer, usbBufferLength,
									blockType, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS));
		default:
			return false;
	}
}

// request: 'D', sub command, arguments. Returns the reply length
int dmrDataServiceHandleUSB(const uint8_t *request, uint8_t *reply)
{
	const uint8_t *args = &request[2];
	int len = 3;

	reply[0] = 'D';
	reply[1] = request[1];
	reply[2] = 1;// result, 1 = OK

	switch (request[1])
	{
		case USB_DATA_CLEAR:
			if (dmrDataServiceIsBusy())
			{
				reply[2] = 0;
				break;
			}
			usbBufferLength = 0;
			txBurstCount = 0;
			break;

		case USB_DATA_APPEND_BYTES:
			if ((args[0] > (COM_REQUESTBUFFER_SIZE - 3)) || ((usbBufferLength + args[0]) > USB_BUFFER_SIZE))
			{
				reply[2] = 0;
				break;
			}
			memcpy(&usbBuffer[usbBufferLength], &args[1], args[0]);
			usbBufferLength += args[0];
			break;

		case USB_DATA_APPEND_BURST:
			if (dmrDataServiceIsBusy() || (txBurstCount >= DMR_DATA_MAX_BURSTS) || (args[2] > DMR_BURST_PAYLOAD_MAX))
			{
				reply[2] = 0;
				break;
			}
			txBursts[txBurstCount].dataType = args[0];
			txBursts[txBurstCount].flags = args[1];
			txBursts[txBurstCount].length = args[2];
			memcpy(txBursts[txBurstCount].payload, &args[3], args[2]);
			txBurstCount++;
			break;

		case USB_DATA_SEND:
			reply[2] = usbSend(args) ? 1 : 0;
			break;

		case USB_DATA_STATUS:
			reply[2] = txPending ? DMR_DATA_TX_RUNNING : HRC6000DataTxGetStatus();
			reply[3] = dmrDataServiceRxBurstCount();
			reply[4] = dmrDataServiceInboxCount();
			len = 5;
			break;

		case USB_DATA_POP_BURST:
			{
				dmrBurst_t burst;
				reply[2] = dmrDataServiceRxBurstPop(&burst) ? 1 : 0;
				if (reply[2])
				{
					reply[3] = burst.dataType;
					reply[4] = burst.flags;
					reply[5] = burst.length;
					memcpy(&reply[6], burst.payload, burst.length);
					len = 6 + burst.length;
				}
			}
			break;

		case USB_DATA_POP_MESSAGE:
			{
				dmrDataMessage_t message;
				reply[2] = dmrDataServiceInboxPop(&message) ? 1 : 0;
				if (reply[2])
				{
					int textLen = strlen(message.text);
					reply[3] = message.src >> 16; reply[4] = message.src >> 8; reply[5] = message.src;
					reply[6] = message.dst >> 16; reply[7] = message.dst >> 8; reply[8] = message.dst;
					reply[9] = message.group;
					reply[10] = textLen;
					memcpy(&reply[11], message.text, textLen);
					len = 11 + textLen;
				}
			}
			break;

		case USB_DATA_NETWORK_MODE:
			if (args[0] != 0xFF)
			{
				settingsSetOptionBit(BIT_USB_NETWORK, (args[0] != 0));
				settingsSetDirty();// saved with the other settings, e.g. by the CPS save and reboot command
			}
			reply[3] = settingsIsOptionBitSet(BIT_USB_NETWORK) ? 1 : 0;
			reply[4] = usbNcmIsUp() ? 1 : 0;
			len = 5;
			break;

		default:
			reply[2] = 0;
			break;
	}

	return len;
}
