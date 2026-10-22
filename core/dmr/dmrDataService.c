// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR data service, see dmrDataService.h. Single task (the host's, the radio task in the firmware), except the
 * dmr_data_api.h calls at the end, which only touch the mailbox and copies under the lock.
 */

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"
#if CONFIG_DMR_EMERGENCY
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_DMR_CONFIRMED
#include "dmr/call/dmr_lc.h"
#endif

#if CONFIG_DMR_DATA_CORE

#define START_TIMEOUT_MS		10000	// a transmission still waiting for a clear channel after this is dropped
#define REPLY_TIMEOUT_MS		3000	// the same for an ACK or CSBK answer: later than this the sender has given up
#define BLOCK_WINDOW_MS			400		// data blocks count only this soon after a header / CSBK / block (gotcha 5.4)
#define CALL_ALERT_REPEAT_MS	10000	// the caller retries until it hears the ack: ring once
#define DUPLICATE_MESSAGE_MS	60000	// the same message again from the same radio this soon is a retry: ACKed, not kept
// Our own call alert / radio check: the answer is a CSBK in the next frames; tried this many times (docs/ux task flow 8)
#define SIGNAL_TRIES			3
#define SIGNAL_ANSWER_MS		1500	// after our CSBK has gone
// Our SMS with an ACK requested: the TMS ACK comes as a data call of its own after the ETSI part
#define SMS_TRIES				2
#define SMS_ACK_MS				8000
#define SMS_ETSI_ACK_MS			4000	// the ETSI response comes straight after the message
#define SMS_BLOCK_TYPE			DT_RATE_12_DATA	// rate 1/2 by default: rate 1 fails on one bit error (gotcha 5.3)
// Motorola ARS (gotchas 7.1-7.4): registration 5-15 s after power on (spread by radio ID, so that radios switched on
// together don't collide), and again once the channel, our ID or the server has been unchanged for ARS_SETTLE_MS. A
// registration the server doesn't acknowledge is retried ARS_RETRIES times, ARS_RETRY_FIRST_MS apart and doubling
// (30 s to 8 min), then left until something changes: with no server on the channel, MOTOTRBO radios retry for ever
#define ARS_POWER_ON_MIN_MS		5000
#define ARS_POWER_ON_SPREAD_MS	10000
#define ARS_SETTLE_MS			5000
#define ARS_RETRY_FIRST_MS		30000
#define ARS_RETRIES				5
// Registered: again after the refresh time the server's answer gives (never when it gives 0, refresh not expected), or
// this when its answer has no refresh time, so that a server that was restarted, or ages radios out, learns we're still here
#define ARS_REFRESH_DEFAULT_MS	(30U * 60U * 1000U)
// IP packets (dmrDataServiceSendIP): one taken but never on air within this is given up (the gateway's mailbox waits)
#define IP_QUEUE_TIMEOUT_MS		START_TIMEOUT_MS
#define IP_PREAMBLES			DMR_DATA_SMS_PREAMBLES
// Confirmed data to us whose blocks stopped coming before the count in its header: lost bursts, asked for again
#define CNF_RX_STALL_MS			300
// A confirmed packet again from the same radio with the N(S) we last ACKed this soon is a retry (our ACK was lost)
#define CNF_RX_DUPLICATE_MS		20000
#define CNF_PEERS				4		// radios whose N(S) we keep (ours to them, theirs to us)

static dmrDataEnv_t env;
static dmrDataSettings_t settings = { .callAlertAck = true, .lrrpReply = true, .smsFormat = DMR_SMS_FORMAT_ETSI_IP,
										.udpCompress = DMR_UDPC_AUTO, .ipConfirmed = DMR_IPC_RULES,
										.ipConfirmedTries = DMR_IPC_TRIES_DEFAULT };
static bool passthrough;				// control station: IP based packets to us belong to the PC
static dmrDataChannel_t channel;
static uint32_t now;
static dmrDataStats_t stats;
static void (*lockFn)(void);
static void (*unlockFn)(void);

static void lock(void)
{
	if (lockFn)
	{
		lockFn();
	}
}

static void unlock(void)
{
	if (unlockFn)
	{
		unlockFn();
	}
}

// --- The burst list being sent ---

// What the list on air is for, so that its end reaches the right owner
enum { TXK_OTHER, TXK_SMS, TXK_SIGNAL, TXK_ARS, TXK_REPLY, TXK_EMERGENCY, TXK_IP, TXK_UDP };

DMR_DATA_BUFFER static dmrBurst_t txBursts[DMR_DATA_MAX_BURSTS];
static int txBurstCount = 0;
static bool txPending = false;			// waiting for a clear channel
static bool txRunning = false;			// given to the host, waiting for dmrDataServiceTxDone
static uint32_t txPendingSince;
static bool txIsReply;
static uint8_t txKind;

static uint8_t smsSeq = 0;

// Replies to a received message: the ETSI response for confirmed data, then the TMS ACK in a transmission of its own,
// as MOTOTRBO radios send it (a repeater or hotspot takes one transmission for one data call). A queue, so messages
// arriving back to back each get theirs (gotcha 5.7)
#define REPLY_QUEUE_SIZE		5		// one slot is kept free: 4 replies
#define REPLY_QUEUE_MAX_AGE_MS	5000	// a reply still queued after this is dropped: the sender has given up
typedef struct
{
	uint32_t queuedAt;
	uint32_t dst;
	uint8_t response;					// ETSI response (class / type / status octet; 0: none): ACK, NACK, selective ACK
	uint8_t sap;
	bool tms;							// Motorola TMS ACK
	uint8_t tmsSeq;
	bool sack;							// a selective ACK: flags follow
	uint8_t flags[8];
} pendingReply_t;
static pendingReply_t replyQueue[REPLY_QUEUE_SIZE];
static uint8_t replyWriteIdx = 0;
static uint8_t replyReadIdx = 0;
static bool tmsAckPending = false;
static uint32_t tmsAckDst;
static uint8_t tmsAckSeq;

// Call alert ack or radio check answer, sent on its own
static bool csbkAnswerPending = false;
static dmrBurst_t csbkAnswer;
static uint32_t lastCallAlertSrc = 0;
static uint32_t lastCallAlertTime;
static uint32_t lastBlockWindow;		// last header / CSBK / block (gotcha 5.4)
static bool blockWindowValid;

// Our own call alert / radio check
static struct
{
	uint8_t kind;						// 0 none, DMR_DATA_EV_CALL_ALERT_ACKED (alert), DMR_DATA_EV_RADIO_CHECK_OK (check)
	uint32_t dst;
	uint8_t state;						// dmr_req_state_t
	uint8_t tries;
	bool waiting;						// sent, waiting for the answer until due
	uint32_t due;
	uint32_t startedAt;
} sig;

// Our own SMS
static struct
{
	uint32_t dst;
	bool group;
	bool ack;
	uint8_t format;						// dmr_sms_format_t
	uint8_t state;						// dmr_req_state_t
	uint8_t tries;
	uint8_t seq;
	bool waiting;
	uint32_t due;
	char text[sizeof(((dmrDataTMS_t *)0)->text)];
} sms;
#if CONFIG_DMR_CONFIRMED
ODR_CPU_RAM static uint8_t smsL4[DMR_ETSI_TEXT_UDP_MAX];	// a confirmed ETSI IP text's datagram, for its retries
#endif

// Our emergency alarm (CONFIG_DMR_EMERGENCY)
static struct
{
	bool active;
	uint32_t dst;
	bool group;
	uint8_t fid;
	uint8_t tries;						// still to send
	bool waiting;						// one has gone: the next after the gap
	uint32_t due;
} emg;

// Requests from other tasks (dmr_data_api.h), taken at the next tick
static struct
{
	bool smsPending;
	uint32_t smsDst;
	bool smsGroup;
	bool smsAck;
	uint8_t smsFormat;
	char smsText[sizeof(((dmrDataTMS_t *)0)->text)];
	bool sigPending;
	uint8_t sigKind;
	uint32_t sigDst;
	bool settingsPending;
	dmrDataSettings_t settings;
} mailbox;

static dmrDataEvent_t lastEvent;
static uint32_t dataSerial;

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
	uint8_t shownState;						// the dmrArsState_t last posted as an event
} ars;
enum { ARS_POWER_OFF_NONE = 0, ARS_POWER_OFF_SENDING, ARS_POWER_OFF_DONE };
static bool arsAckReceived = false;
static bool arsNakReceived = false;
static uint16_t arsAckRefreshMinutes = DMR_ARS_REFRESH_NOT_GIVEN;
static bool arsQueryReceived = false;

DMR_DATA_BUFFER static dmrDataMessage_t inbox[DMR_DATA_INBOX_SIZE];
static uint8_t inboxNewest = 0;
static uint8_t inboxUsed = 0;

static void post(uint8_t kind, uint32_t id)
{
	lock();
	lastEvent.serial++;
	lastEvent.kind = kind;
	lastEvent.id = id;
	lastEvent.atMs = now;
	dataSerial++;
	dmrDataEvent_t copy = lastEvent;
	unlock();
	if (env.notify)
	{
		env.notify(&copy, env.ctx);
	}
}

static bool canTransmit(void)
{
	return channel.canTransmit && (channel.ownId != 0);
}

bool dmrDataServiceIsBusy(void)
{
	return txPending || txRunning;
}

static bool queueTx(int count, uint8_t kind)
{
	if (count <= 0)
	{
		return false;
	}

	txBurstCount = count;
	txPendingSince = now;
	txIsReply = false;
	txKind = kind;
	txPending = true;
	return true;
}

// ACKs and CSBK answers: only worth sending for REPLY_TIMEOUT_MS
static void queueReply(int count)
{
	if (queueTx(count, TXK_REPLY))
	{
		txIsReply = true;
	}
}

static bool sendUDP(bool group, uint32_t dst, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length, uint8_t blockType,
					uint8_t kind)
{
	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	return queueTx(dmrDataBuildUDP(group, dst, channel.ownId, srcPort, dstPort, payload, length, blockType, DMR_DATA_SMS_PREAMBLES,
									txBursts, DMR_DATA_MAX_BURSTS), kind);
}

bool dmrDataServiceSendUDP(bool group, uint32_t dst, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	return sendUDP(group, dst, srcPort, dstPort, payload, length, DT_RATE_12_DATA, TXK_OTHER);
}

#if DMR_DATA_UDP_API
// --- One UDP datagram from another task (dmr_data_send_udp): sent once the channel lets us, given up after
// UDP_WAIT_MS ---

static struct
{
	volatile uint8_t state;				// dmr_req_state_t
	bool stamped;						// since holds the first tick that saw it queued
	bool group;
	uint8_t length;
	uint16_t srcPort;
	uint16_t dstPort;
	uint32_t dst;
	uint32_t since;
	uint8_t data[DMR_DATA_UDP_MAX];
} udpReq;

static void udpTick(void)
{
	if (udpReq.state != DMR_REQ_QUEUED)
	{
		return;
	}
	__atomic_signal_fence(__ATOMIC_ACQUIRE);// the fields after the state, as dmr_data_send_udp wrote them before it
	if (!udpReq.stamped)
	{
		udpReq.stamped = true;
		udpReq.since = now;
	}
	if (sendUDP(udpReq.group, udpReq.dst, udpReq.srcPort, udpReq.dstPort, udpReq.data, udpReq.length, DT_RATE_12_DATA, TXK_UDP))
	{
		udpReq.state = DMR_REQ_SENDING;
	}
	else if ((now - udpReq.since) >= UDP_WAIT_MS)
	{
		udpReq.state = DMR_REQ_FAILED;// no DMR channel we may transmit on, or always busy
	}
}

static void udpTxDone(bool ok)
{
	if (udpReq.state == DMR_REQ_SENDING)
	{
		udpReq.state = ok ? DMR_REQ_OK : DMR_REQ_FAILED;
	}
}
#endif

bool dmrDataServiceSendBursts(const dmrBurst_t *bursts, int count)
{
	if (dmrDataServiceIsBusy() || !canTransmit() || (count > DMR_DATA_MAX_BURSTS))
	{
		return false;
	}

	memmove(txBursts, bursts, count * sizeof(dmrBurst_t));
	return queueTx(count, TXK_OTHER);
}

// --- IP packets (dmrDataServiceSendIP): compressed headers, confirmed delivery ---

#if CONFIG_DMR_CONFIRMED
static void rxResult(dmrDataRxResult_t result, bool block);
#endif

enum { IPX_IDLE, IPX_QUEUED, IPX_SENDING, IPX_WAITING };

static struct
{
	uint8_t state;
	uint8_t result;						// dmr_req_state_t of the last one
	bool group;
	bool compress;
	bool confirmed;
	bool sms;							// our own ETSI IP text (smsFinish at the end), not the gateway's
	bool firstTry;						// the next transmission is the packet's first (F set, every block)
	uint8_t protocol;
	uint8_t sap;
	uint8_t ns;
	uint8_t tries;
	uint16_t ipId;
	uint32_t dst;
	const uint8_t *l4;
	int length;
	uint64_t blocks;					// a selective retry's blocks (0: all of them)
	uint32_t since;						// taken / due
	uint32_t due;
} ipx;

static uint16_t udpPort(const uint8_t *l4, int length, int at)
{
	return (length >= 8) ? (uint16_t)((l4[at] << 8) | l4[at + 1]) : 0;
}

bool dmrDataServiceIpWantConfirmed(const dmrDataSettings_t *s, bool group, uint32_t dst, uint8_t protocol, uint16_t srcPort,
									uint16_t dstPort)
{
	if (!CONFIG_DMR_CONFIRMED || group || (s->ipConfirmed == DMR_IPC_OFF))
	{
		return false;
	}
	if (s->ipConfirmed == DMR_IPC_ALL)
	{
		return true;
	}
	for (int i = 0; i < DMR_IPC_RULES_MAX; i++)
	{
		uint32_t r = s->ipConfirmedRule[i];

		if ((r & DMR_IPC_PORT) ? ((protocol == DMR_IP_PROTO_UDP) && (((r & 0xFFFF) == srcPort) || ((r & 0xFFFF) == dstPort)))
								: ((r != 0) && (r == dst)))
		{
			return true;
		}
	}
	return false;
}

#if CONFIG_DMR_CONFIRMED
static struct
{
	uint32_t id;
	uint8_t ns;
	bool txUsed;						// ns: our next N(S) to it
	bool rxUsed;						// rxNs: the N(S) we last ACKed from it, at rxAt
	uint8_t rxNs;
	uint32_t rxAt;
} peers[CNF_PEERS];
static uint8_t peerNext;

static int peerFind(uint32_t id, bool make)
{
	for (int i = 0; i < CNF_PEERS; i++)
	{
		if ((peers[i].txUsed || peers[i].rxUsed) && (peers[i].id == id))
		{
			return i;
		}
	}
	if (!make)
	{
		return -1;
	}
	int i = peerNext;
	peerNext = (peerNext + 1) % CNF_PEERS;
	memset(&peers[i], 0, sizeof(peers[i]));
	peers[i].id = id;
	return i;
}

// Our N(S) to a radio: from a table of the last few; a new one starts at a number from the clock, so that a receiver
// still holding our number from before a restart doesn't take the first packet as a duplicate
static uint8_t nsFor(uint32_t dst)
{
	int i = peerFind(dst, true);

	if (!peers[i].txUsed)
	{
		peers[i].txUsed = true;
		peers[i].ns = (uint8_t)((now ^ dst ^ (now >> 7)) & 0x07);
	}
	return peers[i].ns;
}

static void nsSet(uint32_t dst, uint8_t ns)
{
	int i = peerFind(dst, true);

	peers[i].txUsed = true;
	peers[i].ns = ns & 0x07;
}

// Confirmed data to us from src with N(S) ns: true when it is the one we ACKed last (TS 102 361-1 9.3.24 b)
static bool rxDuplicate(uint32_t src, uint8_t ns)
{
	int i = peerFind(src, true);
	bool dup = peers[i].rxUsed && (peers[i].rxNs == ns) && ((now - peers[i].rxAt) < CNF_RX_DUPLICATE_MS);

	peers[i].rxUsed = true;
	peers[i].rxNs = ns;
	peers[i].rxAt = now;
	return dup;
}

// The Data Terminator LC after a confirmed packet in direct mode (TS 102 361-3 5.4.1, TS 102 361-1 Figure 8.18): it
// opens the receiver's response window
static void terminatorLC(uint32_t dst, uint32_t src, bool firstTry, uint8_t ns, dmrBurst_t *out)
{
	uint8_t lc[DMR_LC_BYTES];

	lc[0] = 0x30;// PF 0, R 0, FLCO 110000
	lc[1] = 0x00;// FID
	lc[2] = (dst >> 16) & 0xFF;
	lc[3] = (dst >> 8) & 0xFF;
	lc[4] = dst & 0xFF;
	lc[5] = (src >> 16) & 0xFF;
	lc[6] = (src >> 8) & 0xFF;
	lc[7] = src & 0xFF;
	lc[8] = 0x40 | (firstTry ? 0x20 : 0x00) | (ns & 0x07);// individual, A, F, S clear, N(S)
	out->dataType = DT_TERMINATOR_WITH_LC;
	out->length = DMR_LC_FULL_BYTES;
	out->flags = DMR_BURST_FLAG_MCU_CRC;
	dmr_lc_encode_full(lc, DMR_RS_MASK_TERMINATOR, out->payload);
}
#endif

static void smsFinish(uint8_t state);

static void ipFinish(uint8_t result)
{
	if (result != DMR_REQ_OK)
	{
		stats.ipCnfFailed += ipx.confirmed ? 1 : 0;
	}
	ipx.state = IPX_IDLE;
	if (ipx.sms)
	{
		ipx.sms = false;
		smsFinish(result == DMR_REQ_OK ? DMR_REQ_OK : DMR_REQ_FAILED);// the ETSI response confirmed it, or never came
		return;
	}
	if (result == DMR_REQ_OK)
	{
		stats.ipSent++;
	}
	else
	{
		stats.ipFailed++;
	}
	ipx.result = result;
}

static void ipTransmit(void)
{
	const uint8_t *pdu;
	int n = dmrDataEncodeIP(ipx.group, ipx.dst, channel.ownId, ipx.protocol, ipx.l4, ipx.length, ipx.compress, ipx.ipId, &pdu,
							&ipx.sap);
	int count = 0;

	if (n <= 0)
	{
		ipFinish(DMR_REQ_FAILED);
		return;
	}
#if CONFIG_DMR_CONFIRMED
	if (ipx.confirmed)
	{
		// One burst kept for the terminator
		count = dmrDataBuildConfirmed(ipx.sap, ipx.dst, channel.ownId, pdu, n, DT_RATE_12_DATA, ipx.ns, ipx.firstTry,
										(ipx.blocks != 0) ? ipx.blocks : ~0ULL, IP_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS - 1);
		if ((count == 0) && (ipx.tries == 0))
		{
			stats.ipCnfTooLong++;// longer than one confirmed packet can carry: as it would go without the setting
			ipx.confirmed = false;
		}
		else if ((count > 0) && !channel.rmo)
		{
			terminatorLC(ipx.dst, channel.ownId, ipx.firstTry, ipx.ns, &txBursts[count++]);
		}
	}
#endif
	if (!ipx.confirmed)
	{
		count = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, ipx.sap, ipx.group, ipx.dst, channel.ownId, pdu, n, DT_RATE_12_DATA,
									IP_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS);
	}
	if (!queueTx(count, TXK_IP))
	{
		ipFinish(DMR_REQ_FAILED);
		return;
	}
	if (ipx.tries == 0)
	{
		dmrDataCountIpTx(ipx.sap, pdu, n, DT_RATE_12_DATA, ipx.confirmed);
		stats.ipConfirmed += ipx.confirmed ? 1 : 0;
	}
	else
	{
		stats.ipCnfRetries++;
	}
	stats.ipCnfTransmissions += ipx.confirmed ? 1 : 0;
	ipx.tries++;
	ipx.state = IPX_SENDING;
}

// The one IP packet in the service (ipx): queued, sent from the tick
static void ipStart(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length, bool compress, bool confirmed)
{
	memset(&ipx, 0, sizeof(ipx));
	ipx.group = group;
	ipx.dst = dst;
	ipx.protocol = protocol;
	ipx.l4 = l4;
	ipx.length = length;
	ipx.compress = compress;
	ipx.ipId = dmrDataNextIpId();
	ipx.firstTry = true;
	ipx.since = now;
	ipx.state = IPX_QUEUED;
#if CONFIG_DMR_CONFIRMED
	ipx.confirmed = confirmed;
	if (ipx.confirmed)
	{
		ipx.ns = nsFor(dst);
	}
#else
	(void)confirmed;
#endif
}

bool dmrDataServiceSendIP(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length)
{
	if ((ipx.state != IPX_IDLE) || !canTransmit() || (l4 == NULL) || (length <= 0) || (dst == 0))
	{
		return false;
	}
	uint16_t srcPort = (protocol == DMR_IP_PROTO_UDP) ? udpPort(l4, length, 0) : 0;
	uint16_t dstPort = (protocol == DMR_IP_PROTO_UDP) ? udpPort(l4, length, 2) : 0;

	ipStart(group, dst, protocol, l4, length, (protocol == DMR_IP_PROTO_UDP) && dmrDataUdpCompress(srcPort, dstPort),
			dmrDataServiceIpWantConfirmed(&settings, group, dst, protocol, srcPort, dstPort));
	stats.ipPackets++;
	return true;
}

bool dmrDataServiceIpBusy(void)
{
	return ipx.state != IPX_IDLE;
}

uint8_t dmrDataServiceIpResult(void)
{
	return ipx.result;
}

static uint8_t ipTries(void)
{
	uint8_t t = settings.ipConfirmedTries;

	return ((t == 0) || (t > DMR_IPC_TRIES_MAX)) ? DMR_IPC_TRIES_DEFAULT : t;
}

// Another transmission of the packet (blocks: those of a selective ACK, 0 the whole packet), or the end when the tries
// are used up
static void ipRetry(uint64_t blocks)
{
	if (ipx.tries >= ipTries())
	{
		ipFinish(DMR_REQ_FAILED);
		return;
	}
	ipx.blocks = blocks;
	ipx.state = IPX_QUEUED;
	ipx.since = now;
	if (!dmrDataServiceIsBusy() && canTransmit())
	{
		ipTransmit();// at once when the service is free, else from the tick
	}
}

static void ipTick(void)
{
	if ((ipx.state == IPX_QUEUED) && !dmrDataServiceIsBusy() && canTransmit())
	{
		ipTransmit();
	}
	else if ((ipx.state == IPX_QUEUED) && ((now - ipx.since) > IP_QUEUE_TIMEOUT_MS))
	{
		ipFinish(DMR_REQ_FAILED);// never a clear DMR channel we may send on
	}
	else if ((ipx.state == IPX_WAITING) && ((int32_t)(now - ipx.due) >= 0))
	{
		// No response (TS 102 361-3 5.4.3.1.3: as a NACK): the whole packet again, F clear
		stats.ipCnfTimeouts++;
		ipx.firstTry = false;
		ipRetry(0);
	}
}

static void ipTxDone(bool ok)
{
	if (ipx.state != IPX_SENDING)
	{
		return;
	}
	if (!ipx.confirmed)
	{
		ipFinish(ok ? DMR_REQ_OK : DMR_REQ_FAILED);
		return;
	}
	ipx.state = IPX_WAITING;// a list that didn't go out counts as a try: waited out like one without a response
	ipx.due = now + (ok ? DMR_IPC_RESPONSE_MS : 500);
}

#if CONFIG_DMR_CONFIRMED
// A response packet to us: true when it answers our confirmed packet (TS 102 361-1 Table 8.3, -3 Table 5.7)
static bool ipResponse(const dmrDataPacket_t *packet)
{
	if ((ipx.state != IPX_WAITING) || !ipx.confirmed || (packet->src != ipx.dst) || (packet->sap != ipx.sap))
	{
		return false;
	}
	uint8_t r = packet->response;
	uint8_t status = r & 0x07;

	switch (DMR_RESPONSE_CLASS(r))
	{
		case 0:
			if ((DMR_RESPONSE_TYPE(r) != 1) || (status != ipx.ns))
			{
				return false;// not the ACK of this packet
			}
			stats.ipCnfAcks++;
			nsSet(ipx.dst, ipx.ns + 1);
			ipFinish(DMR_REQ_OK);
			return true;
		case 2:
		{
			if ((DMR_RESPONSE_TYPE(r) != 0) || (packet->length < 8))
			{
				return false;
			}
			uint64_t again = 0;
			for (int k = 0; k < DMR_CONFIRMED_MAX_BLOCKS; k++)
			{
				if (!((packet->data[k / 8] >> (k % 8)) & 1))
				{
					again |= 1ULL << k;
				}
			}
			stats.ipCnfSacks++;
			ipx.firstTry = false;
			if (again == 0)
			{
				stats.ipCnfAcks++;// nothing to send again: as an ACK
				nsSet(ipx.dst, ipx.ns + 1);
				ipFinish(DMR_REQ_OK);
			}
			else
			{
				ipRetry(again);
			}
			return true;
		}
		case 1:
			stats.ipCnfNacks++;
			switch (DMR_RESPONSE_TYPE(r))
			{
				case DMR_RESPONSE_NACK_SEQUENCE:
					// The receiver expects VI + 1 (9.3.24): that number, as a new packet
					stats.ipCnfResyncs++;
					ipx.ns = (status + 1) & 0x07;
					nsSet(ipx.dst, ipx.ns);
					ipx.firstTry = true;
					ipRetry(0);
					break;
				case 4:// undeliverable
				case 6:// not allowed by the system
					ipFinish(DMR_REQ_FAILED);
					break;
				default:// format, CRC, memory full: the whole packet again
					ipx.firstTry = false;
					ipRetry(0);
					break;
			}
			return true;
		default:
			return false;
	}
}
#endif

// --- Our SMS ---

static void smsFinish(uint8_t state)
{
	sms.state = state;
	sms.waiting = false;
	if (state == DMR_REQ_OK)
	{
		post(DMR_DATA_EV_SMS_SENT, sms.dst);
	}
	else
	{
		stats.smsFailed++;
		post(DMR_DATA_EV_SMS_FAILED, sms.dst);
	}
}

static bool smsTransmit(void)
{
	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	if (sms.tries == 0)
	{
		smsSeq = (smsSeq + 1) & 0x1F;
		sms.seq = smsSeq;
	}
	int count = 0;
#if CONFIG_DMR_CONFIRMED
	if ((sms.format == DMR_SMS_FORMAT_ETSI_IP) && sms.ack)
	{
		// Confirmed data: the IP packet machinery sends it, retries it and ends it on the ETSI response (ipFinish)
		int n = dmrDataIPTextUDP(sms.text, smsL4);

		if ((ipx.state != IPX_IDLE) || (n <= 0))
		{
			if (n <= 0)
			{
				smsFinish(DMR_REQ_FAILED);
			}
			return false;// after the gateway's packet
		}
		ipStart(false, sms.dst, DMR_IP_PROTO_UDP, smsL4, n, false, true);	// the full header, as unconfirmed (5.3)
		ipx.sms = true;
		ipTransmit();
		if (ipx.state != IPX_SENDING)
		{
			return false;// failed at once: ipFinish ended the message
		}
		sms.tries++;
		sms.state = DMR_REQ_SENDING;
		stats.smsSent++;
		stats.ipTextSent++;
		return true;
	}
#endif

	switch (sms.format)
	{
		case DMR_SMS_FORMAT_ETSI:
			count = dmrDataBuildEtsiText(sms.group, sms.dst, channel.ownId, sms.text, sms.ack, DMR_DATA_SMS_PREAMBLES,
										 txBursts, DMR_DATA_MAX_BURSTS);
			break;
		case DMR_SMS_FORMAT_ETSI_IP:
			count = dmrDataBuildIPText(sms.group, sms.dst, channel.ownId, sms.text, SMS_BLOCK_TYPE, DMR_DATA_SMS_PREAMBLES,
									   txBursts, DMR_DATA_MAX_BURSTS);
			break;
#if CONFIG_DMR_TMS
		case DMR_SMS_FORMAT_TMS:
			count = dmrDataBuildTMS(sms.group, sms.dst, channel.ownId, sms.text, sms.seq, sms.ack, SMS_BLOCK_TYPE,
									DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS);
			break;
#endif
		default:
			break;
	}
	if (!queueTx(count, TXK_SMS))
	{
		return false;
	}
	sms.tries++;
	sms.state = DMR_REQ_SENDING;
	stats.smsSent++;
	stats.etsiSmsSent += (sms.format == DMR_SMS_FORMAT_ETSI) ? 1 : 0;
	stats.ipTextSent += (sms.format == DMR_SMS_FORMAT_ETSI_IP) ? 1 : 0;
	return true;
}

bool dmrDataServiceSendSMS(bool group, uint32_t dst, const char *text, bool ackRequested)
{
	return dmrDataServiceSendSMSFormat(group, dst, text, ackRequested, DMR_SMS_FORMAT_DEFAULT);
}

// A format the build can send (TMS only with CONFIG_DMR_TMS)
static bool smsFormatBuilt(uint8_t format)
{
	return (format == DMR_SMS_FORMAT_ETSI) || (format == DMR_SMS_FORMAT_ETSI_IP) ||
			(CONFIG_DMR_TMS && (format == DMR_SMS_FORMAT_TMS));
}

bool dmrDataServiceSendSMSFormat(bool group, uint32_t dst, const char *text, bool ackRequested, uint8_t format)
{
	if (format == DMR_SMS_FORMAT_DEFAULT)
	{
		// A setting this build can't send (TMS stored by a build with it): the standard default instead
		format = smsFormatBuilt(settings.smsFormat) ? settings.smsFormat : DMR_SMS_FORMAT_ETSI_IP;
	}
	if (!smsFormatBuilt(format))
	{
		return false;
	}
	if (!CONFIG_DMR_SMS || (sms.state == DMR_REQ_QUEUED) || (sms.state == DMR_REQ_SENDING) || (text == NULL) ||
			(text[0] == '\0') || (dst == 0))
	{
		return false;
	}

	sms.dst = dst;
	sms.group = group;
	// ETSI IP text has no acknowledgment of its own: an individual one goes as confirmed data, so the ETSI response
	// confirms delivery, unless confirmed IP data is off; a group one is done once on air
	sms.ack = ackRequested && !group &&
			((format != DMR_SMS_FORMAT_ETSI_IP) || (CONFIG_DMR_CONFIRMED && (settings.ipConfirmed != DMR_IPC_OFF)));
	sms.format = format;
	sms.tries = 0;
	sms.waiting = false;
	snprintf(sms.text, sizeof(sms.text), "%s", text);
	sms.state = DMR_REQ_QUEUED;
	if (!canTransmit())
	{
		smsFinish(DMR_REQ_FAILED);
		return false;
	}
	return true;
}

static void smsTick(void)
{
	if (sms.state == DMR_REQ_QUEUED)
	{
		smsTransmit();// sent when the service is free
	}
	else if ((sms.state == DMR_REQ_SENDING) && sms.waiting && ((int32_t)(now - sms.due) >= 0))
	{
		sms.waiting = false;
		if (sms.tries < SMS_TRIES)
		{
			sms.state = DMR_REQ_QUEUED;
			smsTransmit();
		}
		else
		{
			smsFinish(DMR_REQ_FAILED);
		}
	}
}

static void smsTxDone(bool ok)
{
	if (sms.state != DMR_REQ_SENDING)
	{
		return;
	}
	if (!sms.ack)
	{
		smsFinish(ok ? DMR_REQ_OK : DMR_REQ_FAILED);
		return;
	}
	sms.waiting = true;// a list that didn't go out counts as a try: waited out like one without an ACK
	sms.due = now + (ok ? ((sms.format == DMR_SMS_FORMAT_ETSI) ? SMS_ETSI_ACK_MS : SMS_ACK_MS) : 1000);
}

// --- Our call alert / radio check ---

static bool signalTransmit(void)
{
	uint8_t buf[10];

	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	memset(buf, 0, sizeof(buf));
	if (sig.kind == DMR_DATA_EV_CALL_ALERT_ACKED)
	{
		buf[0] = 0x80 | DMR_CSBKO_CALL_ALERT;// last block; ETSI call alert (TS 102 361-2), FID 0
		buf[1] = DMR_FID_ETSI;
		stats.callAlertsSent++;
	}
	else
	{
		buf[0] = 0x80 | DMR_CSBKO_RADIO_CHECK;// radio check request: octet 3 0x80; FID per the setting (Motorola / ETSI)
		buf[1] = settings.radioCheckEtsi ? DMR_FID_ETSI : DMR_FID_MOTOROLA;
		buf[3] = 0x80;
		stats.radioChecksSent++;
	}
	buf[4] = (sig.dst >> 16) & 0xFF;
	buf[5] = (sig.dst >> 8) & 0xFF;
	buf[6] = sig.dst & 0xFF;
	buf[7] = (channel.ownId >> 16) & 0xFF;
	buf[8] = (channel.ownId >> 8) & 0xFF;
	buf[9] = channel.ownId & 0xFF;
	queueTx(dmrDataBuildCSBK(buf, txBursts), TXK_SIGNAL);
	sig.tries++;
	sig.state = DMR_REQ_SENDING;
	return true;
}

static void signalFinish(bool ok)
{
	sig.state = ok ? DMR_REQ_OK : DMR_REQ_FAILED;
	sig.waiting = false;
	if (sig.kind == DMR_DATA_EV_CALL_ALERT_ACKED)
	{
		post(ok ? DMR_DATA_EV_CALL_ALERT_ACKED : DMR_DATA_EV_CALL_ALERT_FAILED, sig.dst);
	}
	else
	{
		post(ok ? DMR_DATA_EV_RADIO_CHECK_OK : DMR_DATA_EV_RADIO_CHECK_FAILED, sig.dst);
	}
}

static bool signalStart(uint8_t kind, uint32_t dst)
{
	if (!CONFIG_DMR_SIGNALING || (sig.state == DMR_REQ_QUEUED) || (sig.state == DMR_REQ_SENDING) || (dst == 0))
	{
		return false;
	}

	sig.kind = kind;
	sig.dst = dst;
	sig.tries = 0;
	sig.waiting = false;
	sig.startedAt = now;
	sig.state = DMR_REQ_QUEUED;
	if (!canTransmit())
	{
		signalFinish(false);
		return false;
	}
	return true;
}

bool dmrDataServiceCallAlert(uint32_t dst)
{
	return signalStart(DMR_DATA_EV_CALL_ALERT_ACKED, dst);
}

bool dmrDataServiceRadioCheck(uint32_t dst)
{
	return signalStart(DMR_DATA_EV_RADIO_CHECK_OK, dst);
}

static void signalTick(void)
{
	if (sig.state == DMR_REQ_QUEUED)
	{
		signalTransmit();
	}
	else if ((sig.state == DMR_REQ_SENDING) && sig.waiting && ((int32_t)(now - sig.due) >= 0))
	{
		sig.waiting = false;
		if (sig.tries < SIGNAL_TRIES)
		{
			sig.state = DMR_REQ_QUEUED;
			signalTransmit();
		}
		else
		{
			signalFinish(false);
		}
	}
}

static void signalTxDone(bool ok)
{
	if (sig.state == DMR_REQ_SENDING)
	{
		sig.waiting = true;
		sig.due = now + (ok ? SIGNAL_ANSWER_MS : 500);
	}
}

// --- Our emergency alarm (docs/spec/dmr-emergency.md 2) ---

void dmrDataBuildEmergencyAlarm(uint32_t dst, uint32_t src, bool group, uint8_t fid, uint8_t out[10])
{
	memset(out, 0, 10);
	out[0] = 0x80 | DMR_CSBKO_EMERGENCY_ALARM;// last block
	out[1] = fid;
	out[2] = group ? 0x00 : 0x80;// individual: bit 7 of octet 2 (ours; group is the usual case)
	out[4] = (dst >> 16) & 0xFF;
	out[5] = (dst >> 8) & 0xFF;
	out[6] = dst & 0xFF;
	out[7] = (src >> 16) & 0xFF;
	out[8] = (src >> 8) & 0xFF;
	out[9] = src & 0xFF;
}

static void emergencyFinish(bool ok)
{
	if (!emg.active)
	{
		return;
	}
	emg.active = false;
	emg.waiting = false;
	post(ok ? DMR_DATA_EV_EMERGENCY_SENT : DMR_DATA_EV_EMERGENCY_FAILED, emg.dst);
}

bool dmrDataServiceEmergencyAlarm(uint32_t dst, bool group, uint8_t fid, uint8_t tries)
{
	if (!CONFIG_DMR_EMERGENCY || emg.active || (dst == 0) || (dst > 0xFFFFFFu) || (tries == 0))
	{
		return false;
	}
	emg.dst = dst;
	emg.group = group;
	emg.fid = fid;
	emg.tries = tries;
	emg.waiting = false;
	emg.due = now;
	emg.active = true;
	if (!canTransmit())
	{
		emergencyFinish(false);
		return false;
	}
	return true;
}

void dmrDataServiceEmergencyAlarmCancel(void)
{
	emg.active = false;// a CSBK already on its way still goes
	emg.waiting = false;
}

bool dmrDataServiceEmergencyAlarmBusy(void)
{
	return emg.active;
}

static void emergencyTick(void)
{
	if (!CONFIG_DMR_EMERGENCY || !emg.active || emg.waiting || dmrDataServiceIsBusy() || ((int32_t)(now - emg.due) < 0))
	{
		return;
	}
	if (!canTransmit())
	{
		emergencyFinish(false);
		return;
	}
	uint8_t buf[10];

	dmrDataBuildEmergencyAlarm(emg.dst, channel.ownId, emg.group, emg.fid, buf);
	if (queueTx(dmrDataBuildCSBK(buf, txBursts), TXK_EMERGENCY))
	{
		emg.waiting = true;
		stats.emergencyAlarmsSent++;
	}
}

static void emergencyTxDone(bool ok)
{
	if (!emg.active || !emg.waiting)
	{
		return;
	}
	emg.waiting = false;
	if (emg.tries > 0)
	{
		emg.tries--;
	}
	if (emg.tries == 0)
	{
		emergencyFinish(ok);
		return;
	}
	emg.due = now + DMR_DATA_EMERGENCY_GAP_MS;
}

// --- ARS ---

// What a registration is for: the server, our ID and the channel. 0 when there is nothing to register (ARS off, or
// a channel we can't send data on)
static uint32_t arsContext(uint32_t server)
{
	if ((server == 0) || !canTransmit())
	{
		return 0;
	}

	uint32_t h = 2166136261u;// FNV-1a
	uint32_t values[] = { server, channel.ownId, channel.channelKey };

	for (unsigned int i = 0; i < (sizeof(values) / sizeof(values[0])); i++)
	{
		h = (h ^ values[i]) * 16777619u;
	}

	return (h != 0) ? h : 1;
}

static bool arsSend(uint32_t server, bool registration)
{
	uint8_t pdu[DMR_ARS_MAX_PDU];
	int length = registration ? dmrDataBuildARSRegistration(channel.ownId, pdu) : dmrDataBuildARSDeregistration(pdu);

	if (!queueTx(dmrDataBuildUDP(false, server, channel.ownId, DMR_UDP_PORT_ARS, DMR_UDP_PORT_ARS, pdu, length,
								 DT_RATE_12_DATA, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS), TXK_ARS))
	{
		return false;
	}
	stats.arsSent++;
	return true;
}

static void arsTick(void)
{
	if (!CONFIG_DMR_ARS || (ars.powerOff != ARS_POWER_OFF_NONE) || passthrough)
	{
		return;// passthrough: the dispatch PC registers on the radio's behalf; our state is kept for afterwards
	}

	uint32_t server = settings.arsServer;
	uint32_t context = arsContext(server);

	if (ars.settleMs == 0)
	{
		ars.settleMs = ARS_POWER_ON_MIN_MS + (((channel.ownId * 2654435761u) ^ now) % ARS_POWER_ON_SPREAD_MS);
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

	uint8_t state = dmrDataServiceArsState();

	if (state != ars.shownState)
	{
		ars.shownState = state;
		post(DMR_DATA_EV_ARS_CHANGED, state);
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

dmrArsState_t dmrDataServiceArsState(void)
{
	if (!CONFIG_DMR_ARS || (settings.arsServer == 0) || passthrough)
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

// --- LRRP ---

// Motorola LRRP (UDP 4001): a location server (TRBOnet, SmartPTT, ...) asks for a position now, or starts reports
// every n seconds or every n meters (checked every interval, 30 s when it gives none), and stops them. Without a
// position source (settings.hasGps false: the DM-1701) every answer says "no fix" and no trigger is started
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
static uint8_t lrrpWriteIdx = 0;
static uint8_t lrrpReadIdx = 0;

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

// The platform's position for LRRP. Weak: none (the host test brings its own)
#if !defined(DMR_DATA_SERVICE_TEST)
__attribute__((weak)) bool dmrDataServiceGetPosition(lrrpPosition_t *position)
{
	memset(position, 0, sizeof(*position));
	return false;
}
#endif

static bool havePosition(lrrpPosition_t *position)
{
	return settings.hasGps && dmrDataServiceGetPosition(position);
}

static void lrrpReceived(uint32_t src, const uint8_t *payload, int length)
{
	uint8_t next = (lrrpWriteIdx + 1) % LRRP_QUEUE_SIZE;
	lrrpPendingRequest_t *pending = &lrrpQueue[lrrpWriteIdx];

	if (!CONFIG_DMR_LRRP || !settings.lrrpReply || (next == lrrpReadIdx) || !lrrpDecodeRequest(payload, length, &pending->request))
	{
		return;// off, full (the server asks again), or not a request
	}

	stats.lrrpRequests++;
	pending->src = src;
	lrrpWriteIdx = next;
}

static bool lrrpSend(uint32_t dst, const uint8_t *doc, int length, int preambles, bool noFix)
{
	if (length <= 0)
	{
		return false;
	}

	if (!queueTx(dmrDataBuildUDP(false, dst, channel.ownId, DMR_UDP_PORT_LRRP, DMR_UDP_PORT_LRRP, doc, length,
								 DT_RATE_12_DATA, preambles, txBursts, DMR_DATA_MAX_BURSTS), TXK_OTHER))
	{
		return false;
	}
	stats.lrrpAnswers++;
	stats.lrrpNoFix += noFix ? 1 : 0;
	return true;
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

	if (dmrDataServiceIsBusy() || !canTransmit())
	{
		return false;
	}

	switch (request->type)
	{
		case LRRP_REQUEST_IMMEDIATE:
			{
				bool have = havePosition(&position);
				bool fix = have && position.location.fixValid;

				lrrpSend(pending->src, doc, lrrpBuildImmediateResponse(request, (have ? &position : NULL), doc),
						 DMR_DATA_SMS_PREAMBLES, !fix);
			}
			break;

		case LRRP_REQUEST_TRIGGERED_START:
			{
				bool usable = (request->triggers & (LRRP_TRIGGER_PERIODIC | LRRP_TRIGGER_DISTANCE)) &&
						((request->intervalSeconds != 0) || (request->distanceMeters != 0));

				if (!settings.hasGps)
				{
					// No position source at all: reports would never come
					lrrpSend(pending->src, doc, lrrpBuildTriggeredStartResponse(request->requestId, request->requestIdLength,
								LRRP_RESULT_NO_FIX, doc), DMR_DATA_SMS_PREAMBLES, true);
					break;
				}
				if (!usable)
				{
					// Emergency or one shot triggers: this radio has no emergency, so they'd never fire
					lrrpSend(pending->src, doc, lrrpBuildTriggeredStartResponse(request->requestId, request->requestIdLength,
								LRRP_RESULT_NOT_UNDERSTOOD, doc), DMR_DATA_SMS_PREAMBLES, false);
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
						 DMR_DATA_SMS_PREAMBLES, false);
			}
			break;

		case LRRP_REQUEST_TRIGGERED_STOP:
			{
				lrrpTrigger_t *t = lrrpFindTrigger(pending->src, request->requestId, request->requestIdLength);

				if (t != NULL)
				{
					t->active = false;
				}
				lrrpSend(pending->src, doc, lrrpBuildTriggeredStopResponse(request->requestId, request->requestIdLength, doc),
						 DMR_DATA_SMS_PREAMBLES, false);
			}
			break;

		default:
			break;// protocol requests: not answered
	}

	return true;
}

static void lrrpTick(void)
{
	if (!CONFIG_DMR_LRRP || passthrough)
	{
		return;
	}
	if (lrrpReadIdx != lrrpWriteIdx)
	{
		if (lrrpHandleRequest(&lrrpQueue[lrrpReadIdx]))
		{
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

		if (!havePosition(&position))
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
					 DMR_DATA_SMS_PREAMBLES, !position.location.fixValid))
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

uint32_t dmrDataServiceLocationNeededInMs(void)
{
	uint32_t soonest = UINT32_MAX;

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
	return soonest;
}

// --- Power off ---

// The radio is being switched off: deregister from the ARS server if registered, or tried to. True while the
// deregistration still has to go out; the caller gives up on it after a while (3 s, gotcha 7.4)
bool dmrDataServicePowerOff(void)
{
	if (!CONFIG_DMR_ARS || passthrough)
	{
		return false;// passthrough: the PC owns the registration
	}
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
}

// Switched back on before the power off finished: register again if we deregistered
void dmrDataServicePowerOffCanceled(void)
{
	if (ars.powerOff == ARS_POWER_OFF_SENDING)
	{
		ars.context = 0;
	}
	ars.powerOff = ARS_POWER_OFF_NONE;
}

// --- Tick ---

static void takeMailbox(void)
{
	bool smsReq = false, sigReq = false, setReq = false;
	uint32_t smsDst = 0, sigDst = 0;
	bool smsGroup = false, smsAck = false;
	uint8_t smsFormat = DMR_SMS_FORMAT_DEFAULT;
	uint8_t sigKind = 0;
	static char text[sizeof(mailbox.smsText)];
	dmrDataSettings_t newSettings;

	lock();
	if (mailbox.smsPending)
	{
		smsReq = true;
		smsDst = mailbox.smsDst;
		smsGroup = mailbox.smsGroup;
		smsAck = mailbox.smsAck;
		smsFormat = mailbox.smsFormat;
		memcpy(text, mailbox.smsText, sizeof(text));
		mailbox.smsPending = false;
	}
	if (mailbox.sigPending)
	{
		sigReq = true;
		sigKind = mailbox.sigKind;
		sigDst = mailbox.sigDst;
		mailbox.sigPending = false;
	}
	if (mailbox.settingsPending)
	{
		setReq = true;
		newSettings = mailbox.settings;
		mailbox.settingsPending = false;
	}
	unlock();

	if (setReq)
	{
		dmrDataServiceSetSettings(&newSettings);
	}
	if (smsReq && !dmrDataServiceSendSMSFormat(smsGroup, smsDst, text, smsAck, smsFormat) && (sms.state != DMR_REQ_FAILED))
	{
		post(DMR_DATA_EV_SMS_FAILED, smsDst);// another one still running
	}
	if (sigReq && !signalStart(sigKind, sigDst) && (sig.state != DMR_REQ_FAILED))
	{
		post((sigKind == DMR_DATA_EV_CALL_ALERT_ACKED) ? DMR_DATA_EV_CALL_ALERT_FAILED : DMR_DATA_EV_RADIO_CHECK_FAILED, sigDst);
	}
}

static void txGiveUp(void)
{
	stats.txGaveUp++;
	switch (txKind)
	{
		case TXK_SMS:
			smsTxDone(false);
			break;
		case TXK_SIGNAL:
			signalTxDone(false);
			break;
		case TXK_EMERGENCY:
			emergencyTxDone(false);
			break;
		case TXK_IP:
			ipTxDone(false);
			break;
#if DMR_DATA_UDP_API
		case TXK_UDP:
			udpTxDone(false);
			break;
#endif
		default:
			break;
	}
}

void dmrDataServiceTick(uint32_t nowMs, const dmrDataChannel_t *ch)
{
	now = nowMs;
	if (ch != NULL)
	{
		channel = *ch;
	}

	takeMailbox();

	if (csbkAnswerPending && !dmrDataServiceIsBusy() && canTransmit())
	{
		txBursts[0] = csbkAnswer;
		csbkAnswerPending = false;
		queueReply(1);
		stats.csbkAnswersSent++;
	}

	// The next queued reply, once the last one's TMS ACK has gone
	while (!tmsAckPending && (replyReadIdx != replyWriteIdx) && !dmrDataServiceIsBusy() && canTransmit())
	{
		pendingReply_t reply = replyQueue[replyReadIdx];

		replyReadIdx = (replyReadIdx + 1) % REPLY_QUEUE_SIZE;

		if ((now - reply.queuedAt) > REPLY_QUEUE_MAX_AGE_MS)
		{
			continue;
		}

		if (reply.tms)
		{
			tmsAckDst = reply.dst;
			tmsAckSeq = reply.tmsSeq;
			tmsAckPending = true;
		}
		if (reply.response != 0)
		{
			queueReply(dmrDataBuildResponse(reply.sap, reply.dst, channel.ownId, reply.response, reply.sack ? reply.flags : NULL,
											txBursts));
			stats.responsesSent++;
		}
		break;
	}

#if CONFIG_DMR_TMS
	// After the response has gone (the service is busy until then)
	if (tmsAckPending && !dmrDataServiceIsBusy() && canTransmit())
	{
		tmsAckPending = false;
		queueReply(dmrDataBuildTMSAck(tmsAckDst, channel.ownId, tmsAckSeq, DMR_DATA_SMS_PREAMBLES, txBursts, DMR_DATA_MAX_BURSTS));
		stats.tmsAcksSent++;
	}
#endif

	emergencyTick();// ahead of the user's other requests and the automatic traffic
	signalTick();
	smsTick();
	ipTick();
#if DMR_DATA_UDP_API
	udpTick();// after the replies and the user's own requests: nothing about it is urgent
#endif
	arsTick();
	lrrpTick();
#if CONFIG_DMR_CONFIRMED
	// Confirmed data to us whose last blocks never came (lost bursts): what is missing is asked for
	if (dmrDataRxConfirmedActive() && ((now - lastBlockWindow) > CNF_RX_STALL_MS))
	{
		rxResult(dmrDataRxConfirmedEnd(), false);
	}
#endif

	if (txPending && !txRunning)
	{
		bool clear = canTransmit() && (env.channelClear == NULL || env.channelClear(now, env.ctx));

		if (clear && (env.txStart != NULL) && env.txStart(txBursts, txBurstCount, env.ctx))
		{
			txPending = false;
			txRunning = true;
			stats.txLists++;
		}
		else if ((now - txPendingSince) > (txIsReply ? REPLY_TIMEOUT_MS : START_TIMEOUT_MS))
		{
			txPending = false;
			txGiveUp();
		}
	}
}

void dmrDataServiceTxDone(uint8_t err)
{
	if (!txRunning)
	{
		return;
	}
	txRunning = false;
	stats.txLastError = err;
	if (err == 0)
	{
		stats.txOk++;
	}
	else
	{
		stats.txFailed++;
	}
	switch (txKind)
	{
		case TXK_SMS:
			smsTxDone(err == 0);
			break;
		case TXK_SIGNAL:
			signalTxDone(err == 0);
			break;
		case TXK_EMERGENCY:
			emergencyTxDone(err == 0);
			break;
		case TXK_IP:
			ipTxDone(err == 0);
			break;
#if DMR_DATA_UDP_API
		case TXK_UDP:
			udpTxDone(err == 0);
			break;
#endif
		default:
			break;
	}
}

// --- RX ---

static bool isForUs(const dmrDataPacket_t *packet)
{
	if (packet->src == channel.ownId)
	{
		return false;// our own transmission, repeated back
	}

	if (!packet->group)
	{
		return (packet->dst == channel.ownId);
	}

	if ((channel.talkGroup != 0) && (packet->dst == channel.talkGroup))
	{
		return true;
	}

	return (env.inRxGroup != NULL) && env.inRxGroup(packet->dst, env.ctx);
}

static void queueReplyEntry(const pendingReply_t *reply)
{
	uint8_t next = (replyWriteIdx + 1) % REPLY_QUEUE_SIZE;

	if (next != replyReadIdx)// full: the sender will retry
	{
		replyQueue[replyWriteIdx] = *reply;
		replyWriteIdx = next;
	}
}

#if CONFIG_DMR_CONFIRMED
static void handlePacket(const dmrDataPacket_t *packet);

// What the receiver made of a confirmed packet: a selective ACK for bad or lost blocks, a NACK for a CRC-32 failure
// with good blocks (TS 102 361-3 5.4.2.2), when it is to us and asks for a response
static void rxResult(dmrDataRxResult_t result, bool block)
{
	const dmrDataPacket_t *p = &dmrDataRxPacket;

	if (result == DMR_DATA_RX_PACKET)
	{
		handlePacket(p);
		return;
	}
	if ((result != DMR_DATA_RX_RETRY) && (result != DMR_DATA_RX_NACK))
	{
		stats.rxCrcErrors += (block && (result == DMR_DATA_RX_ERROR)) ? 1 : 0;
		return;
	}
	stats.rxCrcErrors++;
	if (!isForUs(p) || p->group || !p->responseRequested)
	{
		return;
	}
	pendingReply_t reply = { .queuedAt = now, .dst = p->src, .sap = p->sap };

	if (result == DMR_DATA_RX_RETRY)
	{
		reply.response = DMR_RESPONSE_SACK(p->sendSeq);
		reply.sack = true;
		dmrDataRxRetryFlags(reply.flags);
		stats.rxSacksSent++;
	}
	else
	{
		reply.response = DMR_RESPONSE_NACK_CRC(p->sendSeq);
		stats.rxNacksSent++;
	}
	queueReplyEntry(&reply);
}
#endif

static void handlePacket(const dmrDataPacket_t *packet)
{
	dmrDataUDP_t udp;
	dmrDataTMS_t tms;
	bool isUDP = dmrDataGetUDP(packet, &udp);
	bool forUs = isForUs(packet);
	bool confirmed = (packet->dpf == DMR_DPF_CONFIRMED) && !packet->group;

	stats.rxPackets++;
	// Confirmed data to us must be acknowledged, whatever it carries, or the sender keeps retrying
	pendingReply_t reply = {
			.queuedAt = now,
			.dst = packet->src,
			.response = (confirmed && packet->responseRequested) ? DMR_RESPONSE_ACK(packet->sendSeq) : 0,
			.sap = packet->sap };

#if CONFIG_DMR_CONFIRMED
	// The same packet again (our ACK was lost): ACKed again, not passed on twice
	if (forUs && confirmed && rxDuplicate(packet->src, packet->sendSeq))
	{
		stats.rxConfirmedDuplicates++;
		queueReplyEntry(&reply);
		return;
	}
#endif
	if (env.packet)
	{
		env.packet(packet, env.ctx);
	}

	if (!forUs)
	{
		return;
	}
	stats.rxForUs++;

	if (packet->dpf == DMR_DPF_CONFIRMED)
	{
		stats.rxConfirmed++;
	}
#if CONFIG_DMR_CONFIRMED
	// The answer to our confirmed IP packet
	if ((packet->dpf == DMR_DPF_RESPONSE) && !packet->group && ipResponse(packet))
	{
		return;
	}
#endif

	// Control station passthrough: what has an IP header belongs to the PC, which got it through env.packet; only the
	// link layer (the response above) and the ACK of our own message, which sends nothing, are looked at here
	bool toPc = CONFIG_DMR_CONTROL_STATION && passthrough &&
			((packet->sap == DMR_SAP_IP) || (packet->sap == DMR_SAP_UDPIP_COMPRESSION));

	stats.ptYielded += toPc ? 1 : 0;

	// The presence server's answer to our registration, or its request for one
	if (CONFIG_DMR_ARS && !toPc && isUDP && (udp.appPort == DMR_UDP_PORT_ARS) && !packet->group && (packet->src == ars.server) && (ars.server != 0))
	{
		switch (dmrDataDecodeARS(udp.payload, udp.length))
		{
			case DMR_ARS_RESPONSE:
				{
					bool accepted;
					uint16_t refreshMinutes;

					if (dmrDataDecodeARSResponse(udp.payload, udp.length, &accepted, &refreshMinutes))
					{
						stats.arsAcks++;
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

	if (CONFIG_DMR_LRRP && !toPc && isUDP && (udp.appPort == DMR_UDP_PORT_LRRP) && !packet->group)
	{
		lrrpReceived(packet->src, udp.payload, udp.length);
	}

	// Motorola TMS: without CONFIG_DMR_TMS not decoded, so neither ACKed nor kept (the gateway still forwards it, and
	// confirmed data still gets its ETSI response above)
#if CONFIG_DMR_TMS
	bool isTMS = CONFIG_DMR_SMS && isUDP && (udp.appPort == DMR_UDP_PORT_TMS) && dmrDataDecodeTMS(udp.payload, udp.length, &tms);
#else
	bool isTMS = false;
#endif
	uint8_t format = DMR_SMS_FORMAT_TMS;

	if (toPc && !(isTMS && tms.isAck))
	{
		isTMS = false;// not ours to keep or to ACK (an ACK of our own message is still noted below)
	}

	// The ETSI response to our ETSI message
	if (CONFIG_DMR_SMS && (packet->dpf == DMR_DPF_RESPONSE) && !packet->group && (sms.state == DMR_REQ_SENDING) && sms.ack &&
			(sms.format == DMR_SMS_FORMAT_ETSI) && (packet->src == sms.dst) && DMR_RESPONSE_IS_ACK(packet->response))
	{
		stats.etsiResponsesReceived++;
		stats.smsAcked++;
		smsFinish(DMR_REQ_OK);
		return;
	}

	// ETSI IP text (TS 102 361-3 5.7.1, UDP 5016): into the inbox; no acknowledgment of its own (confirmed data got
	// its response above)
	if (CONFIG_DMR_SMS && !toPc && !isTMS && isUDP && (udp.dstPort == DMR_UDP_PORT_ETSI_TEXT) &&
			dmrDataDecodeIPText(udp.payload, udp.length, tms.text, sizeof(tms.text)))
	{
		isTMS = true;
		format = DMR_SMS_FORMAT_ETSI_IP;
		tms.isAck = false;
		tms.ackRequested = false;
		tms.seq = 0;
	}

	// ETSI short data text (defined short data, UDT text): into the same inbox; the sender's response request (A) is
	// answered with an ETSI ACK
	if (CONFIG_DMR_SMS && !isTMS && ((packet->dpf == DMR_DPF_DEFINED_SHORT) || (packet->dpf == DMR_DPF_UDT)) &&
			dmrDataDecodeEtsiText(packet, tms.text, sizeof(tms.text)))
	{
		isTMS = true;
		format = DMR_SMS_FORMAT_ETSI;
		tms.isAck = false;
		tms.ackRequested = false;
		tms.seq = 0;
		reply.response = (packet->responseRequested && !packet->group) ? DMR_RESPONSE_ACK(0) : 0;
		reply.sap = packet->sap;
	}

	// The ACK of our own message
	if (isTMS && tms.isAck && !packet->group && (sms.state == DMR_REQ_SENDING) && sms.ack &&
			(packet->src == sms.dst) && ((tms.seq & 0x1F) == sms.seq))
	{
		stats.smsAcked++;
		smsFinish(DMR_REQ_OK);
	}
	isTMS = isTMS && !tms.isAck;

	if (isTMS && tms.ackRequested && !packet->group)
	{
		reply.tms = true;
		reply.tmsSeq = tms.seq;
	}

	if ((reply.response != 0) || reply.tms)
	{
		queueReplyEntry(&reply);
	}

	if (!isTMS)
	{
		return;
	}

	// A sender that missed our ACK sends the message again: ACKed again above, but kept (and announced) once
	for (int i = 0; i < inboxUsed; i++)
	{
		const dmrDataMessage_t *m = dmrDataServiceMessage(i);

		if ((m->src == packet->src) && (m->dst == packet->dst) && (m->group == packet->group) && (m->seq == tms.seq) &&
				(m->format == format) &&
				((now - m->receivedAt) < DUPLICATE_MESSAGE_MS) && (strcmp(m->text, tms.text) == 0))
		{
			stats.smsDuplicates++;
			return;
		}
	}

	lock();
	uint8_t next = (inboxUsed == 0) ? 0 : ((inboxNewest + 1) % DMR_DATA_INBOX_SIZE);// replaces the oldest when full
	dmrDataMessage_t *message = &inbox[next];
	message->src = packet->src;
	message->dst = packet->dst;
	message->group = packet->group;
	message->seq = tms.seq;
	message->receivedAt = now;
	message->unread = true;
	message->format = format;
	memcpy(message->text, tms.text, sizeof(message->text));
	inboxNewest = next;
	if (inboxUsed < DMR_DATA_INBOX_SIZE)
	{
		inboxUsed++;
	}
	unlock();
	stats.smsReceived++;
	stats.etsiSmsReceived += (format == DMR_SMS_FORMAT_ETSI) ? 1 : 0;
	stats.ipTextReceived += (format == DMR_SMS_FORMAT_ETSI_IP) ? 1 : 0;
	post(DMR_DATA_EV_SMS_RECEIVED, packet->src);
}

static uint32_t getId(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static bool groupIsOurs(uint32_t tg)
{
	return (tg == 0xFFFFFFu) || // all call
			((channel.talkGroup != 0) && (tg == channel.talkGroup)) ||
			((env.inRxGroup != NULL) && env.inRxGroup(tg, env.ctx));
}

// An emergency alarm heard (docs/spec/dmr-emergency.md 2): to us, or to a group we listen to, it alerts the user
static void handleEmergencyAlarm(const uint8_t *csbk)
{
	uint32_t target = getId(&csbk[4]);
	uint32_t source = getId(&csbk[7]);
	bool group = (csbk[2] & 0x80) == 0;

	if (((csbk[1] != DMR_FID_ETSI) && (csbk[1] != DMR_FID_MOTOROLA)) || (source == channel.ownId) || (source == 0))
	{
		return;// another manufacturer's opcode 0x27, or our own alarm repeated back
	}
	stats.emergencyAlarmsHeard++;
	if (group ? !groupIsOurs(target) : (target != channel.ownId))
	{
		return;
	}
	stats.emergencyAlarmsReceived++;
	post(DMR_DATA_EV_EMERGENCY_ALARM, source);
#if CONFIG_DMR_EMERGENCY
	dmr_emergency_heard(DMR_EMERGENCY_ALERT_ALARM, source, target, group, now);
#endif
}

// Call alert and radio check: answers to ours, and requests to us (answered, a call alert rings); emergency alarms
static void handleCSBK(const uint8_t *csbk)
{
	uint8_t opcode = csbk[0] & 0x3F;
	uint32_t target = getId(&csbk[4]);
	uint32_t source = getId(&csbk[7]);

	if (CONFIG_DMR_EMERGENCY && (opcode == DMR_CSBKO_EMERGENCY_ALARM))
	{
		handleEmergencyAlarm(csbk);
		return;
	}
	if (!CONFIG_DMR_SIGNALING)
	{
		return;
	}

	if ((sig.state == DMR_REQ_SENDING) && (source == channel.ownId) && (target == sig.dst))
	{
		if (((sig.kind == DMR_DATA_EV_CALL_ALERT_ACKED) && (opcode == DMR_CSBKO_CALL_ALERT_ACK)) ||
				((sig.kind == DMR_DATA_EV_RADIO_CHECK_OK) && (opcode == DMR_CSBKO_RADIO_CHECK) && (csbk[3] != 0x80)))
		{
			if (sig.kind == DMR_DATA_EV_CALL_ALERT_ACKED)
			{
				stats.callAlertsAcked++;
			}
			else
			{
				stats.radioChecksAnswered++;
			}
			signalFinish(true);
			return;
		}
	}
	// The ack's IDs are the other way round: to the caller (us) from the alerted radio. A radio check answered in
	// that order (the ETSI acknowledgment convention) counts too
	if ((sig.state == DMR_REQ_SENDING) && (target == channel.ownId) && (source == sig.dst))
	{
		if ((sig.kind == DMR_DATA_EV_CALL_ALERT_ACKED) && (opcode == DMR_CSBKO_CALL_ALERT_ACK))
		{
			stats.callAlertsAcked++;
			signalFinish(true);
			return;
		}
		if ((sig.kind == DMR_DATA_EV_RADIO_CHECK_OK) && (opcode == DMR_CSBKO_RADIO_CHECK) && (csbk[3] != 0x80))
		{
			stats.radioChecksAnswered++;
			signalFinish(true);
			return;
		}
	}

	dmrBurst_t answer;

	if ((channel.ownId == 0) || (dmrDataBuildCSBKAnswer(csbk, channel.ownId, &answer) == 0))
	{
		return;
	}

	if (opcode == DMR_CSBKO_CALL_ALERT)
	{
		stats.callAlertsReceived++;
		if ((source != lastCallAlertSrc) || ((now - lastCallAlertTime) > CALL_ALERT_REPEAT_MS))
		{
			post(DMR_DATA_EV_CALL_ALERT, source);
		}
		lastCallAlertSrc = source;
		lastCallAlertTime = now;
		if (!settings.callAlertAck)
		{
			return;
		}
	}
	else
	{
		stats.radioChecksReceived++;
		if (!settings.radioCheckReply)
		{
			return;
		}
		post(DMR_DATA_EV_RADIO_CHECK_ANSWERED, source);
	}

	if (!csbkAnswerPending)
	{
		csbkAnswer = answer;
		csbkAnswerPending = true;
	}
}

void dmrDataServiceRxBurst(const dmrBurst_t *burst, uint32_t nowMs)
{
	now = nowMs;
	stats.rxBursts++;

	bool block = (burst->dataType == DT_RATE_12_DATA) || (burst->dataType == DT_RATE_34_DATA) || (burst->dataType == DT_RATE_1_DATA);

	if (block)
	{
		stats.rxBlocks++;
		if (!blockWindowValid || ((now - lastBlockWindow) > BLOCK_WINDOW_MS))
		{
			stats.rxBlocksStale++;
			dmrDataRxReset();
			return;// noise decodes as data blocks
		}
	}
	else if ((burst->dataType == DT_CSBK) || (burst->dataType == DT_DATA_HEADER))
	{
		// The receiver checks the CRC itself (dmr-modem.md 2.2: headers and CSBKs with a bad one are dropped); the
		// modem's flag is only compared with it, for the bench
		bool ok = dmrDataCheckCRC16(burst->payload, (burst->dataType == DT_CSBK) ? 0xA5 : 0xCC);
		bool flagOk = (burst->flags & DMR_BURST_FLAG_RX_CRC_ERROR) == 0;

		stats.rxCrcFlagMismatch += (ok != flagOk) ? 1 : 0;
		if (!ok)
		{
			stats.rxCrcErrors++;
			return;
		}
		if (burst->dataType == DT_CSBK)
		{
			stats.rxCsbk++;
		}
		else
		{
			stats.rxHeaders++;
		}
	}
	lastBlockWindow = now;
	blockWindowValid = true;

	dmrDataRxResult_t r = dmrDataRxBurst(burst);

	switch (r)
	{
		case DMR_DATA_RX_PACKET:
			handlePacket(&dmrDataRxPacket);
			break;
		case DMR_DATA_RX_CSBK:
			handleCSBK(burst->payload);
			break;
		case DMR_DATA_RX_ERROR:
			if (block)
			{
				stats.rxCrcErrors++;// the packet's CRC-32 (headers were counted above)
			}
			break;
#if CONFIG_DMR_CONFIRMED
		case DMR_DATA_RX_RETRY:
		case DMR_DATA_RX_NACK:
			rxResult(r, block);
			break;
#endif
		default:
			break;
	}
}

// --- The rest of the host's API ---

void dmrDataServiceInit(const dmrDataEnv_t *e)
{
	memset(&env, 0, sizeof(env));
	if (e != NULL)
	{
		env = *e;
	}
	dmrDataRxReset();
}

void dmrDataServiceSetSettings(const dmrDataSettings_t *s)
{
	settings = *s;
	dmrDataSetUdpCompression(settings.udpCompress);
}

void dmrDataServiceGetSettings(dmrDataSettings_t *s)
{
	*s = settings;
}

void dmrDataServiceSetPassthrough(bool on)
{
	on = on && CONFIG_DMR_CONTROL_STATION;
	if (on && !passthrough)
	{
		// Location requests and triggers belong to the PC from now on: nothing queued is answered later
		lrrpReadIdx = lrrpWriteIdx;
		memset(lrrpTriggers, 0, sizeof(lrrpTriggers));
	}
	passthrough = on;
}

bool dmrDataServicePassthrough(void)
{
	return passthrough;
}

void dmrDataServiceSetLock(void (*l)(void), void (*u)(void))
{
	lockFn = l;
	unlockFn = u;
}

int dmrDataServiceMessageCount(void)
{
	return inboxUsed;
}

const dmrDataMessage_t *dmrDataServiceMessage(int index)
{
	if ((index < 0) || (index >= inboxUsed))
	{
		return NULL;
	}

	return &inbox[(inboxNewest + DMR_DATA_INBOX_SIZE - index) % DMR_DATA_INBOX_SIZE];
}

void dmrDataServiceGetStats(dmrDataStats_t *out)
{
	*out = stats;
}

void dmrDataServiceResetStats(void)
{
	memset(&stats, 0, sizeof(stats));
}

// --- dmr_data_api.h: any task ---

int sms_inbox_count(void)
{
	return inboxUsed;
}

int sms_inbox_unread(void)
{
	int n = 0;

	lock();
	for (int i = 0; i < inboxUsed; i++)
	{
		n += dmrDataServiceMessage(i)->unread ? 1 : 0;
	}
	unlock();
	return n;
}

bool sms_inbox_get(int i, dmrDataMessage_t *out)
{
	lock();
	const dmrDataMessage_t *m = dmrDataServiceMessage(i);

	if (m != NULL)
	{
		*out = *m;
	}
	unlock();
	return m != NULL;
}

void sms_inbox_mark_read(int i)
{
	lock();
	dmrDataMessage_t *m = (dmrDataMessage_t *)dmrDataServiceMessage(i);

	if ((m != NULL) && m->unread)
	{
		m->unread = false;
		dataSerial++;
	}
	unlock();
}

bool sms_inbox_delete(int i)
{
	bool ok = false;

	lock();
	if ((i >= 0) && (i < inboxUsed))
	{
		// Logical index k is inbox[(newest - k) mod N]: each older message moves one place towards the newest, the
		// oldest slot becomes free (the next message goes to newest + 1, which is that slot once one is free)
		for (int k = i; k < inboxUsed - 1; k++)
		{
			inbox[(inboxNewest + DMR_DATA_INBOX_SIZE - k) % DMR_DATA_INBOX_SIZE] =
				inbox[(inboxNewest + DMR_DATA_INBOX_SIZE - (k + 1)) % DMR_DATA_INBOX_SIZE];
		}
		inboxUsed--;
		dataSerial++;
		ok = true;
	}
	unlock();
	return ok;
}

bool sms_send(uint32_t dst, bool group, const char *text, bool ack_requested, dmr_sms_format_t format)
{
	bool ok = false;

	if (!CONFIG_DMR_SMS || (text == NULL) || (text[0] == '\0') || (dst == 0) ||
			(!smsFormatBuilt(format) && (format != DMR_SMS_FORMAT_DEFAULT)))
	{
		return false;
	}
	lock();
	if (!mailbox.smsPending && (sms.state != DMR_REQ_QUEUED) && (sms.state != DMR_REQ_SENDING))
	{
		mailbox.smsDst = dst;
		mailbox.smsGroup = group;
		mailbox.smsAck = ack_requested;
		mailbox.smsFormat = format;
		snprintf(mailbox.smsText, sizeof(mailbox.smsText), "%s", text);
		mailbox.smsPending = true;
		ok = true;
	}
	unlock();
	return ok;
}

bool sms_send_confirmed(void)
{
	return sms.ack;
}

dmr_req_state_t sms_send_state(void)
{
	return mailbox.smsPending ? DMR_REQ_QUEUED : (dmr_req_state_t)sms.state;
}

#if DMR_DATA_UDP_API
bool dmr_data_send_udp(uint32_t dst, bool group, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len)
{
	bool ok = false;

	if ((dst == 0) || (dst > 0xFFFFFFu) || (payload == NULL) || (len == 0) || (len > DMR_DATA_UDP_MAX))
	{
		return false;
	}
	lock();
	if ((udpReq.state != DMR_REQ_QUEUED) && (udpReq.state != DMR_REQ_SENDING))
	{
		udpReq.dst = dst;
		udpReq.group = group;
		udpReq.srcPort = src_port;
		udpReq.dstPort = dst_port;
		udpReq.length = (uint8_t)len;
		memcpy(udpReq.data, payload, len);
		udpReq.stamped = false;
		// udpTick reads the fields without the lock once it sees QUEUED: the compiler must not move these stores
		// after the volatile one
		__atomic_signal_fence(__ATOMIC_RELEASE);
		udpReq.state = DMR_REQ_QUEUED;// last: the radio task takes it from here
		ok = true;
	}
	unlock();
	return ok;
}

dmr_req_state_t dmr_data_udp_state(void)
{
	return (dmr_req_state_t)udpReq.state;
}
#endif

static bool signalRequest(uint8_t kind, uint32_t dst)
{
	bool ok = false;

	if (!CONFIG_DMR_SIGNALING || (dst == 0))
	{
		return false;
	}
	lock();
	if (!mailbox.sigPending && (sig.state != DMR_REQ_QUEUED) && (sig.state != DMR_REQ_SENDING))
	{
		mailbox.sigKind = kind;
		mailbox.sigDst = dst;
		mailbox.sigPending = true;
		ok = true;
	}
	unlock();
	return ok;
}

bool dmr_signal_call_alert(uint32_t dst)
{
	return signalRequest(DMR_DATA_EV_CALL_ALERT_ACKED, dst);
}

bool dmr_signal_radio_check(uint32_t dst)
{
	return signalRequest(DMR_DATA_EV_RADIO_CHECK_OK, dst);
}

dmr_req_state_t dmr_signal_state(uint8_t *kind, uint32_t *dst)
{
	dmr_req_state_t st;

	lock();
	if (mailbox.sigPending)
	{
		st = DMR_REQ_QUEUED;
		if (kind)
		{
			*kind = mailbox.sigKind;
		}
		if (dst)
		{
			*dst = mailbox.sigDst;
		}
	}
	else
	{
		st = (dmr_req_state_t)sig.state;
		if (kind)
		{
			*kind = sig.kind;
		}
		if (dst)
		{
			*dst = sig.dst;
		}
	}
	unlock();
	return st;
}

uint32_t dmr_data_serial(void)
{
	return dataSerial;
}

dmrDataEvent_t dmr_data_last_event(void)
{
	lock();
	dmrDataEvent_t e = lastEvent;
	unlock();
	return e;
}

dmrArsState_t dmr_data_ars_state(void)
{
	return dmrDataServiceArsState();
}

void dmr_data_get_settings(dmrDataSettings_t *out)
{
	lock();
	*out = mailbox.settingsPending ? mailbox.settings : settings;
	unlock();
}

void dmr_data_set_settings(const dmrDataSettings_t *in)
{
	lock();
	mailbox.settings = *in;
	mailbox.settingsPending = true;
	unlock();
}

#endif
