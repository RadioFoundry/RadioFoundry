// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone sharing's service (core/dmr/zoneShareSvc.c, docs/spec/zone-share.md) on the real air queue and a fake data
// service: offers Off and Ask, one sender at a time, the pending offer kept as it is, expiry and the hourly limit,
// malformed input, a send's fragments through the queue (confirmed to a radio, unconfirmed to a group), a failed
// fragment ending the send, and the directory request with its answer and refusal.
#include <stdio.h>
#include <string.h>
#include "dmr/zone_share_api.h"
#include "dmr/zoneShare.h"
#include "dmr/dmrAirQueue.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define OUR_ID		3100001u
#define RADIO_A		3100002u
#define RADIO_B		3100003u
#define SERVICE		9990001u

// ---- fake data service ----
static bool pending;
static uint8_t result = DMR_REQ_OK;
static struct { uint32_t dst; bool group; uint8_t delivery; int length; uint8_t l4[DMR_DATA_MAX_PACKET]; } sent[16];
static int sentCount;
static int events[32];
static uint32_t eventId;

bool dmrDataServiceSendIPDelivery(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length, uint8_t delivery)
{
	if (pending || (protocol != DMR_IP_PROTO_UDP))
	{
		return false;
	}
	pending = true;
	if (sentCount < 16)
	{
		sent[sentCount].dst = dst;
		sent[sentCount].group = group;
		sent[sentCount].delivery = delivery;
		sent[sentCount].length = length;
		memcpy(sent[sentCount].l4, l4, (size_t)length);
	}
	sentCount++;
	return true;
}
// As the service: unconfirmed packets after an unconfirmed first go in the same transmission
static int chains;
static int foreignAddresses;			// chains not from our own addresses and TTL: zone sharing sends none
int dmrDataServiceSendIPChain(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl,
								const dmrDataChainPacket_t *p, int n)
{
	foreignAddresses += (srcIp != DMR_DATA_IP_OURS) || (dstIp != DMR_DATA_IP_OURS) || (ttl != DMR_IP_TTL_DEFAULT);
	if ((n <= 0) || !dmrDataServiceSendIPDelivery(group, dst, p[0].protocol, p[0].l4, p[0].length, p[0].delivery))
	{
		return 0;
	}
	int k = 1;

	pending = false;
	while ((p[0].delivery == DMR_IPS_UNCONFIRMED) && (k < n) && (p[k].delivery == DMR_IPS_UNCONFIRMED) &&
			dmrDataServiceSendIPDelivery(group, dst, p[k].protocol, p[k].l4, p[k].length, p[k].delivery))
	{
		pending = false;
		k++;
	}
	pending = true;
	chains += (k > 1);
	return k;
}
bool dmrDataServiceSendBursts(const dmrBurst_t *bursts, int count)
{
	(void)bursts;
	(void)count;
	return false;
}
bool dmrDataServiceIpBusy(void) { return pending; }
uint8_t dmrDataServiceIpResult(void) { return result; }
void dmrDataServiceLock(void) {}
void dmrDataServiceUnlock(void) {}
void dmrDataServicePostEvent(uint8_t kind, uint32_t id)
{
	events[kind & 31]++;
	eventId = id;
}

static uint32_t now = 1000;

// The radio task: the queue hands on what it has, the service finishes it at once
static void tick(int ms)
{
	for (int t = 0; t < ms; t += 10)
	{
		now += 10;
		airqTick(OUR_ID, now);
		zone_share_tick(now);
		pending = false;
	}
}

// ---- records ----
static uint8_t record[ZS_RECORD_MAX];
static int makeRecord(uint32_t zoneId, int channels, const char *name)
{
	zsWriter_t w;
	zsContact_t c = { .type = ZS_CONTACT_GROUP, .id = 91, .name = "World" };

	zsWriterBegin(&w, record, sizeof(record), zoneId, 0, name);
	zsWriterContact(&w, &c);
	for (int i = 0; i < channels; i++)
	{
		zsChannel_t ch = { .dmr = true, .rxHz = 461000000u + (uint32_t)i * 12500u, .colorCode = 1, .timeslot = 1,
						   .contact = 1 };

		snprintf(ch.name, sizeof(ch.name), "Channel %d", i + 1);
		zsWriterChannel(&w, &ch);
	}
	return zsWriterEnd(&w);
}

// The record's fragments arrive from src as the data service hands them over
static void deliver(uint32_t src, bool group, uint8_t transfer, const uint8_t *rec, int length, int only)
{
	uint8_t msg[ZS_MSG_MAX];

	for (int i = 0; i < zsFragmentCount(length); i++)
	{
		if ((only >= 0) && (i != only))
		{
			continue;
		}
		int n = zsBuildFragment(transfer, rec, length, i, msg);

		dmrDataServiceZoneShareRx(src, group, msg, n);
	}
}

static void testOffers(void)
{
	zone_share_stats_t st;
	zone_share_offer_t o;
	const uint8_t *r;
	int len = makeRecord(0x1234, 24, "Site Zone");

	CHECK(len > ZS_CHUNK_MAX && zsFragmentCount(len) == 2);	// two fragments

	// Off, the default: nothing offered
	CHECK(zone_share_offers() == ZS_OFFERS_OFF);
	deliver(RADIO_A, false, 1, record, len, -1);
	zone_share_get_stats(&st);
	CHECK(!zone_share_offer(&o, &r) && r == NULL && st.rxOff == 2 && st.offers == 0 && events[DMR_DATA_EV_ZONE_OFFER] == 0);

	// Ask: the whole record is the offer; another sender's fragment meanwhile is dropped
	zone_share_set_offers(ZS_OFFERS_ASK);
	CHECK(zone_share_offers() == ZS_OFFERS_ASK);
	deliver(RADIO_A, false, 2, record, len, 0);
	deliver(RADIO_B, false, 7, record, len, 0);
	zone_share_get_stats(&st);
	CHECK(st.rxBusy == 1 && !zone_share_offer(NULL, NULL));
	deliver(RADIO_A, false, 2, record, len, 1);
	CHECK(zone_share_offer(&o, &r) && o.src == RADIO_A && !o.group && !o.directory && o.length == len);
	CHECK(r != NULL && memcmp(r, record, (size_t)len) == 0);
	CHECK(events[DMR_DATA_EV_ZONE_OFFER] == 1 && eventId == RADIO_A);

	// While it is pending nothing else is taken, and the record stays as it is
	uint8_t copy[ZS_RECORD_MAX];
	memcpy(copy, record, (size_t)len);
	int other = makeRecord(0x9999, 2, "Other");
	deliver(RADIO_B, true, 3, record, other, -1);
	zone_share_get_stats(&st);
	CHECK(st.rxBusy == 2 && st.offers == 1);
	CHECK(zone_share_offer(&o, &r) && o.src == RADIO_A && memcmp(r, copy, (size_t)len) == 0);

	// Not answered: gone after ZS_OFFER_TTL_MS
	tick(ZS_OFFER_TTL_MS + 20);
	zone_share_get_stats(&st);
	CHECK(!zone_share_offer(NULL, NULL) && st.expired == 1);

	// Opened by the user: it stays until done; done with another serial changes nothing
	len = makeRecord(0x1234, 3, "Small");
	deliver(RADIO_B, true, 4, record, len, -1);
	CHECK(zone_share_offer(&o, NULL) && o.group && o.src == RADIO_B);
	zone_share_offer_take(o.serial);
	tick(ZS_OFFER_TTL_MS + 20);
	CHECK(zone_share_offer(NULL, NULL));
	zone_share_offer_done(o.serial + 1);
	CHECK(zone_share_offer(NULL, NULL));
	zone_share_offer_done(o.serial);
	CHECK(!zone_share_offer(NULL, NULL));

	// Malformed: not a message, a fragment that doesn't fit its transfer, a record that doesn't parse
	uint8_t junk[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
	zone_share_get_stats(&st);
	uint32_t bad = st.rxBad;
	dmrDataServiceZoneShareRx(RADIO_A, false, junk, sizeof(junk));
	dmrDataServiceZoneShareRx(RADIO_A, false, junk, 0);
	uint8_t broken[ZS_RECORD_MAX];
	memcpy(broken, record, (size_t)len);
	broken[0] = 2;		// a version we don't know
	deliver(RADIO_A, false, 5, broken, len, -1);
	zone_share_get_stats(&st);
	CHECK(st.rxBad == bad + 3 && !zone_share_offer(NULL, NULL));
}

// At most ZS_OFFERS_PER_HOUR offers an hour, whatever the user does with them
static void testRateLimit(void)
{
	zone_share_stats_t st;
	zone_share_offer_t o;
	int len = makeRecord(0x55, 1, "One");

	tick(61UL * 60UL * 1000UL);		// the hour of the earlier offers is over
	zone_share_get_stats(&st);
	uint32_t offers = st.offers;

	for (int i = 0; i < ZS_OFFERS_PER_HOUR + 1; i++)
	{
		deliver(RADIO_A, false, (uint8_t)(10 + i), record, len, -1);
		if (zone_share_offer(&o, NULL))
		{
			zone_share_offer_done(o.serial);
		}
		tick(60000);
	}
	zone_share_get_stats(&st);
	CHECK(st.offers == offers + ZS_OFFERS_PER_HOUR && st.rateLimited == 1);
	tick(60UL * 60UL * 1000UL);
	deliver(RADIO_A, false, 30, record, len, -1);
	CHECK(zone_share_offer(&o, NULL));
	zone_share_offer_done(o.serial);
}

// The fragments of a send, read back from what the fake service was given
static int readBack(int first, int count, uint8_t *out)
{
	zsAssembly_t a;
	zsMessage_t m;
	int length = 0;

	zsAssemblyReset(&a);
	for (int i = first; i < first + count; i++)
	{
		const uint8_t *u = sent[i].l4;

		CHECK(((u[0] << 8) | u[1]) == ZS_PORT && ((u[2] << 8) | u[3]) == ZS_PORT && ((u[4] << 8) | u[5]) == sent[i].length);
		CHECK(zsParseMessage(u + 8, sent[i].length - 8, &m) && m.type == ZS_MSG_FRAGMENT);
		if (zsAssemble(&a, OUR_ID, &m, now) == ZS_ASM_DONE)
		{
			length = a.length;
			memcpy(out, a.data, (size_t)length);
		}
	}
	return length;
}

static void testSend(void)
{
	zone_share_stats_t st;
	uint8_t back[ZS_RECORD_MAX];
	int len = makeRecord(0x2222, 20, "Send Zone");
	int frags = zsFragmentCount(len);

	CHECK(frags == 2);
	sentCount = 0;
	memset(events, 0, sizeof(events));

	// Refused: no destination, an invalid record
	CHECK(!zone_share_send(0, false, record, len));
	CHECK(!zone_share_send(RADIO_B, false, record, len - 1));
	CHECK(zone_share_send_state() == ZS_SEND_IDLE);

	// To a radio: confirmed, the fragments in order; a second send waits for the first
	CHECK(zone_share_send(RADIO_B, false, record, len));
	CHECK(zone_share_send_state() == ZS_SEND_SENDING);
	CHECK(!zone_share_send(RADIO_A, false, record, len));
	airqStats_t q;
	airqGetStats(&q);
	CHECK(q.depth == frags);
	tick(100);
	CHECK(sentCount == frags && sent[0].dst == RADIO_B && !sent[0].group && sent[0].delivery == DMR_IPS_CONFIRMED);
	CHECK(readBack(0, frags, back) == len && memcmp(back, record, (size_t)len) == 0);
	CHECK(zone_share_send_state() == ZS_SEND_SENT && events[DMR_DATA_EV_ZONE_SENT] == 1 && eventId == RADIO_B);

	// To a radio, each confirmed fragment went on its own
	CHECK(chains == 0);

	// To a talkgroup: unconfirmed, the fragments in one transmission; it fails, the send fails once
	sentCount = 0;
	CHECK(zone_share_send(91, true, record, len));
	result = DMR_REQ_FAILED;
	tick(100);
	result = DMR_REQ_OK;
	CHECK(foreignAddresses == 0);
	CHECK(sentCount == frags && chains == 1 && sent[0].group && sent[0].dst == 91 && sent[0].delivery == DMR_IPS_UNCONFIRMED);
	CHECK(zone_share_send_state() == ZS_SEND_FAILED && events[DMR_DATA_EV_ZONE_FAILED] == 1);
	airqGetStats(&q);
	CHECK(q.depth == 0);
	zone_share_get_stats(&st);
	CHECK(st.sends == 2 && st.sent == 1 && st.sendFailed == 1);

	// The queue without room for the whole record: nothing queued, the send refused
	pending = true;
	uint8_t big[DMR_DATA_MAX_PACKET - 80];
	memset(big, 0, sizeof(big));
	airqItem_t it = { .kind = AIRQ_IP, .owner = AIRQ_OWNER_GATEWAY, .protocol = DMR_IP_PROTO_UDP, .dst = RADIO_A,
					  .waitMs = 60000, .data = big, .length = sizeof(big) };
	int filled = 0;
	while (airqPut(&it))
	{
		filled++;
	}
	airqGetStats(&q);
	uint16_t depth = q.depth;
	CHECK(!zone_share_send(RADIO_B, false, record, len));
	airqGetStats(&q);
	CHECK(filled > 0 && q.depth == depth && zone_share_send_state() == ZS_SEND_IDLE);
	airqCancel(AIRQ_OWNER_GATEWAY, 0);
	pending = false;
	tick(200);
}

// A directory request: its answer is an offer though offers are Off; a refusal is told; strays are ignored
static void testDirectory(void)
{
	zone_share_offer_t o;
	zone_share_stats_t st;
	uint8_t msg[ZS_MSG_MAX];

	zone_share_set_offers(ZS_OFFERS_OFF);
	sentCount = 0;
	memset(events, 0, sizeof(events));
	CHECK(!zone_share_request(0));
	CHECK(zone_share_request(SERVICE));
	tick(50);
	zsMessage_t m;
	CHECK(sentCount == 1 && sent[0].dst == SERVICE && sent[0].delivery == DMR_IPS_UNCONFIRMED);
	CHECK(zsParseMessage(sent[0].l4 + 8, sent[0].length - 8, &m) && m.type == ZS_MSG_REQUEST && m.code == ZS_WANT_SITE_ZONE);
	uint8_t number = m.number;

	// Another radio's zone, or one from the service under another number: offers are Off
	int len = makeRecord(0x3333, 4, "Site");
	deliver(RADIO_A, false, number, record, len, -1);
	deliver(SERVICE, false, (uint8_t)(number + 1), record, len, -1);
	CHECK(!zone_share_offer(NULL, NULL));
	// The answer
	deliver(SERVICE, false, number, record, len, -1);
	CHECK(zone_share_offer(&o, NULL) && o.directory && o.src == SERVICE);
	zone_share_offer_done(o.serial);
	// Only once
	deliver(SERVICE, false, number, record, len, -1);
	CHECK(!zone_share_offer(NULL, NULL));

	// A refusal
	CHECK(zone_share_request(SERVICE));
	tick(50);
	CHECK(zsParseMessage(sent[1].l4 + 8, sent[1].length - 8, &m) && m.number == (uint8_t)(number + 1));
	int n = zsBuildRefusal(m.number, ZS_REFUSE_STALE, msg);
	dmrDataServiceZoneShareRx(RADIO_A, false, msg, n);		// not the service
	CHECK(events[DMR_DATA_EV_ZONE_REFUSED] == 0);
	dmrDataServiceZoneShareRx(SERVICE, false, msg, n);
	CHECK(events[DMR_DATA_EV_ZONE_REFUSED] == 1 && zone_share_last_refusal() == ZS_REFUSE_STALE);

	// Too late: after ZS_REQUEST_WAIT_MS the answer is not taken
	CHECK(zone_share_request(SERVICE));
	tick(ZS_REQUEST_WAIT_MS + 20);
	n = zsBuildRefusal((uint8_t)(m.number + 1), ZS_REFUSE_BUSY, msg);
	dmrDataServiceZoneShareRx(SERVICE, false, msg, n);
	CHECK(events[DMR_DATA_EV_ZONE_REFUSED] == 1);
	zone_share_get_stats(&st);
	CHECK(st.requests == 3 && st.refusals == 1);
}

int main(void)
{
	airqInit();
	zone_share_init();
	tick(10);

	testOffers();
	testRateLimit();
	testSend();
	testDirectory();

	printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
	return failures ? 1 : 0;
}
