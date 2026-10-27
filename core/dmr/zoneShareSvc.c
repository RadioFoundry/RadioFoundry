// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone sharing on the data service: sending through the air queue, offers, directory requests (zone_share_api.h,
// docs/spec/zone-share.md)
#include "config.h"
#if CONFIG_ZONE_SHARE	// the whole file
#include <string.h>
#include "dmr/zone_share_api.h"
#include "dmr/zoneShare.h"
#include "dmr/dmrAirQueue.h"
#include "dmr/dmrDataService.h"

#define UDP_HEADER		8
#define HOUR_MS			(60UL * 60UL * 1000UL)

// A send puts all its fragments in the TX queue at once (records round up to 4 octets)
_Static_assert(ZS_SEND_FRAGMENTS_MAX * (AIRQ_RECORD_HEADER + ((UDP_HEADER + ZS_MSG_HEADER + ZS_SEND_CHUNK + 3) & ~3)) <= CONFIG_DMR_AIR_QUEUE_BYTES,
			   "the TX queue holds a whole record's fragments");

static uint32_t nowMs;
static volatile uint8_t offersMode = ZS_OFFERS_OFF;
static zone_share_stats_t stats;

// Receiving: one transfer at a time into asm; once whole and valid it is the offer, and nothing else is taken until
// the user is done with it, so the record the UI and the import read stays as it is
static zsAssembly_t assembly;
static zone_share_offer_t offer;
static uint32_t offerSerial;
static uint32_t offerTimes[ZS_OFFERS_PER_HOUR];	// when the last offers were made, for the hourly limit
static uint8_t offerTimesNext;

// Sending
static volatile uint8_t sendState = ZS_SEND_IDLE;
static uint8_t sendLeft;				// fragments not yet ended
static uint32_t sendDst;
static uint8_t sendTransfer;

// The directory request
static uint8_t requestNumber;
static uint32_t requestService;
static uint32_t requestAt;
static bool requestOpen;
static uint8_t lastRefusal;

static int udp(uint8_t *out, int payloadLength)
{
	int n = UDP_HEADER + payloadLength;

	out[0] = (uint8_t)(ZS_PORT >> 8);
	out[1] = (uint8_t)ZS_PORT;
	out[2] = (uint8_t)(ZS_PORT >> 8);
	out[3] = (uint8_t)ZS_PORT;
	out[4] = (uint8_t)(n >> 8);
	out[5] = (uint8_t)n;
	out[6] = 0;// computed with the IPv4 header
	out[7] = 0;
	return n;
}

// The radio task: a fragment or request of ours has ended
static void sendDone(const airqItem_t *item, airqResult_t result)
{
	if (item->tag == 0x100u)
	{
		return;// the directory request: its answer tells
	}
	if ((sendState != ZS_SEND_SENDING) || (item->tag != sendTransfer))
	{
		return;
	}
	if (result != AIRQ_SENT)
	{
		airqCancel(AIRQ_OWNER_ZONE_SHARE, item->tag);// the rest is no use without this one
		sendState = ZS_SEND_FAILED;
		stats.sendFailed++;
		dmrDataServicePostEvent(DMR_DATA_EV_ZONE_FAILED, sendDst);
		return;
	}
	if (--sendLeft == 0)
	{
		sendState = ZS_SEND_SENT;
		stats.sent++;
		dmrDataServicePostEvent(DMR_DATA_EV_ZONE_SENT, sendDst);
	}
}

void zone_share_init(void)
{
	memset(&stats, 0, sizeof(stats));
	zsAssemblyReset(&assembly);
	memset(&offer, 0, sizeof(offer));
	memset(offerTimes, 0, sizeof(offerTimes));
	offerTimesNext = 0;
	sendState = ZS_SEND_IDLE;
	requestOpen = false;
	airqSetDone(AIRQ_OWNER_ZONE_SHARE, sendDone);
}

void zone_share_set_offers(uint8_t mode)
{
	offersMode = (mode == ZS_OFFERS_ASK) ? ZS_OFFERS_ASK : ZS_OFFERS_OFF;
}

uint8_t zone_share_offers(void)
{
	return offersMode;
}

bool zone_share_send(uint32_t dst, bool group, const uint8_t *record, int length)
{
	zsRecord_t check;
	int count = zsFragmentCount(length);

	if ((dst == 0) || (dst > 0xFFFFFF) || (count == 0) || !zsParse(record, length, &check))
	{
		return false;
	}
	dmrDataServiceLock();
	bool busy = (sendState == ZS_SEND_SENDING);

	if (!busy)
	{
		sendState = ZS_SEND_SENDING;// claimed: another task's send is refused from here on
	}
	dmrDataServiceUnlock();
	if (busy)
	{
		return false;
	}
	uint8_t transfer = (uint8_t)(sendTransfer + 1);
	airqItem_t items[ZS_SEND_FRAGMENTS_MAX];
	// Each fragment's headers; its slice of the record goes into the queue straight from the record (no copy of the
	// fragments here: RAM is tight on some radios)
	uint8_t heads[ZS_SEND_FRAGMENTS_MAX][UDP_HEADER + ZS_MSG_HEADER];

	for (int i = 0; i < count; i++)
	{
		int offset = 0;
		int n = zsFragmentHeader(transfer, length, i, &heads[i][UDP_HEADER], &offset);

		udp(heads[i], ZS_MSG_HEADER + n);
		items[i] = (airqItem_t){ .kind = AIRQ_IP, .owner = AIRQ_OWNER_ZONE_SHARE, .protocol = DMR_IP_PROTO_UDP,
								 .delivery = group ? DMR_IPS_UNCONFIRMED : DMR_IPS_CONFIRMED, .group = group, .dst = dst,
								 .tag = transfer, .waitMs = ZS_SEND_WAIT_MS, .data = heads[i],
								 .length = UDP_HEADER + ZS_MSG_HEADER, .tail = &record[offset],
								 .tailLength = (uint16_t)n };
	}
	dmrDataServiceLock();
	sendTransfer = transfer;
	sendLeft = (uint8_t)count;
	sendDst = dst;
	dmrDataServiceUnlock();
	if (!airqPutBatch(items, count))
	{
		sendState = ZS_SEND_IDLE;
		return false;
	}
	stats.sends++;
	return true;
}

zs_send_state_t zone_share_send_state(void)
{
	return (zs_send_state_t)sendState;
}

bool zone_share_request(uint32_t service)
{
	static uint8_t msg[UDP_HEADER + 8];

	if ((service == 0) || (service > 0xFFFFFF))
	{
		return false;
	}
	uint8_t number = (uint8_t)(requestNumber + 1);
	int n = udp(msg, zsBuildRequest(number, ZS_WANT_SITE_ZONE, &msg[UDP_HEADER]));
	airqItem_t it = { .kind = AIRQ_IP, .owner = AIRQ_OWNER_ZONE_SHARE, .protocol = DMR_IP_PROTO_UDP, .delivery = DMR_IPS_UNCONFIRMED,
					  .dst = service, .tag = 0x100u, .waitMs = ZS_SEND_WAIT_MS, .data = msg, .length = (uint16_t)n };

	if (!airqPut(&it))
	{
		return false;
	}
	dmrDataServiceLock();
	requestNumber = number;
	requestService = service;
	requestAt = nowMs;
	requestOpen = true;
	dmrDataServiceUnlock();
	stats.requests++;
	return true;
}

uint8_t zone_share_last_refusal(void)
{
	return lastRefusal;
}

static bool rateAllows(void)
{
	// The oldest of the last ZS_OFFERS_PER_HOUR offers must be an hour old (0: that slot never used)
	uint32_t oldest = offerTimes[offerTimesNext];

	return (oldest == 0) || ((nowMs - oldest) >= HOUR_MS);
}

// The radio task (the data service's receive path). Not inlined: its locals would otherwise sit in handlePacket's
// frame on the radio task's deepest path, through the IP gateway (docs/dev/stack-check.md)
__attribute__((noinline)) void dmrDataServiceZoneShareRx(uint32_t src, bool group, const uint8_t *payload, int length)
{
	zsMessage_t m;

	stats.rxMessages++;
	if (!zsParseMessage(payload, length, &m))
	{
		stats.rxBad++;
		return;
	}
	bool answer = requestOpen && (src == requestService) && (m.number == requestNumber) && !group &&
			((nowMs - requestAt) < ZS_REQUEST_WAIT_MS);

	if (m.type == ZS_MSG_REFUSAL)
	{
		if (answer)
		{
			requestOpen = false;
			lastRefusal = m.code;
			stats.refusals++;
			dmrDataServicePostEvent(DMR_DATA_EV_ZONE_REFUSED, src);
		}
		return;
	}
	if (m.type != ZS_MSG_FRAGMENT)
	{
		return;// a request: we are not a directory
	}
	if ((offersMode == ZS_OFFERS_OFF) && !answer)
	{
		stats.rxOff++;
		return;
	}
	if (offer.pending)
	{
		stats.rxBusy++;
		return;
	}
	switch (zsAssemble(&assembly, src, &m, nowMs))
	{
		case ZS_ASM_DONE:
			break;
		case ZS_ASM_BUSY:
			stats.rxBusy++;
			return;
		case ZS_ASM_BAD:
			stats.rxBad++;
			return;
		default:
			return;
	}
	zsRecord_t rec;

	if (!zsParse(assembly.data, assembly.length, &rec))
	{
		stats.rxBad++;
		return;
	}
	if (!answer && !rateAllows())
	{
		stats.rateLimited++;
		return;
	}
	if (!answer)
	{
		offerTimes[offerTimesNext] = nowMs ? nowMs : 1;
		offerTimesNext = (uint8_t)((offerTimesNext + 1) % ZS_OFFERS_PER_HOUR);
	}
	requestOpen = requestOpen && !answer;
	dmrDataServiceLock();
	offer.pending = true;
	offer.taken = false;
	offer.group = group;
	offer.directory = answer;
	offer.src = src;
	offer.serial = ++offerSerial;
	offer.atMs = nowMs;
	offer.length = assembly.length;
	dmrDataServiceUnlock();
	stats.offers++;
	dmrDataServicePostEvent(DMR_DATA_EV_ZONE_OFFER, src);
}

void zone_share_tick(uint32_t now)
{
	nowMs = now;
	dmrDataServiceLock();
	bool expire = offer.pending && !offer.taken && ((now - offer.atMs) > ZS_OFFER_TTL_MS);

	if (expire)
	{
		offer.pending = false;
	}
	dmrDataServiceUnlock();
	if (expire)
	{
		stats.expired++;
	}
	if (requestOpen && ((now - requestAt) >= ZS_REQUEST_WAIT_MS))
	{
		requestOpen = false;
	}
}

bool zone_share_offer(zone_share_offer_t *out, const uint8_t **record)
{
	dmrDataServiceLock();
	bool pending = offer.pending;

	if (out != NULL)
	{
		*out = offer;
	}
	dmrDataServiceUnlock();
	if (record != NULL)
	{
		*record = pending ? assembly.data : NULL;
	}
	return pending;
}

void zone_share_offer_take(uint32_t serial)
{
	dmrDataServiceLock();
	if (offer.pending && (offer.serial == serial))
	{
		offer.taken = true;
	}
	dmrDataServiceUnlock();
}

void zone_share_offer_done(uint32_t serial)
{
	dmrDataServiceLock();
	if (offer.serial == serial)
	{
		offer.pending = false;
	}
	dmrDataServiceUnlock();
}

void zone_share_get_stats(zone_share_stats_t *out)
{
	*out = stats;
}

#endif	// CONFIG_ZONE_SHARE
