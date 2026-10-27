// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air queue (core/dmr/dmrAirQueue.c, docs/spec/dmr-data.md 12) on a fake data service: packets of mixed sizes
// sent in order, the byte ring filled to its limit and refused only when a packet doesn't fit, records kept in one
// piece across the wrap, the oldest held unchanged while the service sends it and others are added, batches all or
// none, the wait and the maximum age, cancel, and a long random run against a model.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"
#include "dmr/dmrAirQueue.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RING	CONFIG_DMR_AIR_QUEUE_BYTES
#define REC(n)	((AIRQ_RECORD_HEADER + (n) + 3) & ~3)
#define RADIO_ID_FOR_TEST	3101234u

// ---- fake data service ----
static bool busy;						// refuses everything
static bool pending;					// an IP packet it took is still going
static uint8_t result = DMR_REQ_OK;
static const uint8_t *held;				// the packet it reads in place
static int heldLength;
static uint8_t heldCopy[DMR_DATA_MAX_PACKET];
static uint32_t sentDst[256];
static int sentCount;

bool dmrDataServiceSendIPDelivery(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length, uint8_t delivery)
{
	(void)group;
	(void)protocol;
	(void)delivery;
	if (busy || pending)
	{
		return false;
	}
	held = l4;
	heldLength = length;
	memcpy(heldCopy, l4, (size_t)length);
	pending = true;
	sentDst[sentCount++ & 255] = dst;
	return true;
}
// Chaining: the service takes up to chainTake of the packets offered (the first as above)
static int chainTake = 1;
static int chainOffered;
static const uint8_t *chainHeld[DMR_DATA_CHAIN_MAX];
static uint8_t chainCopy[DMR_DATA_CHAIN_MAX][DMR_DATA_MAX_PACKET];
static int chainLength[DMR_DATA_CHAIN_MAX];
static int chainHeldCount;
static uint32_t chainSrcIp, chainDstIp;	// the last chain's addresses and TTL
static uint8_t chainTtl;
int dmrDataServiceSendIPChain(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl,
								const dmrDataChainPacket_t *p, int n)
{
	chainSrcIp = srcIp;
	chainDstIp = dstIp;
	chainTtl = ttl;
	if ((n <= 0) || !dmrDataServiceSendIPDelivery(group, dst, p[0].protocol, p[0].l4, p[0].length, p[0].delivery))
	{
		return 0;
	}
	int k = (n < chainTake) ? n : chainTake;

	chainOffered = n;
	chainHeldCount = k - 1;
	for (int i = 1; i < k; i++)
	{
		chainHeld[i - 1] = p[i].l4;
		chainLength[i - 1] = p[i].length;
		memcpy(chainCopy[i - 1], p[i].l4, (size_t)p[i].length);
		sentDst[sentCount++ & 255] = dst;
	}
	return k;
}
int dmrDataServiceSendShortData(bool group, uint32_t dst, uint32_t src, const uint8_t *data, int length, int preambles)
{
	(void)group;
	(void)dst;
	(void)src;
	(void)data;
	(void)length;
	(void)preambles;
	if (busy)
	{
		return 0;
	}
	sentDst[sentCount++ & 255] = 0xFFFFFF;
	return 1;
}
bool dmrDataServiceIpBusy(void) { return pending; }
uint8_t dmrDataServiceIpResult(void) { return result; }
void dmrDataServiceLock(void) {}
void dmrDataServiceUnlock(void) {}

// The service finishes what it holds; the bytes it read must not have changed meanwhile
static int heldChanged;
static void serviceDone(void)
{
	if (pending)
	{
		heldChanged += memcmp(held, heldCopy, (size_t)heldLength) != 0;
		for (int i = 0; i < chainHeldCount; i++)
		{
			heldChanged += memcmp(chainHeld[i], chainCopy[i], (size_t)chainLength[i]) != 0;
		}
	}
	chainHeldCount = 0;
	pending = false;
}

static int doneCount[8];
static uint32_t doneTag[64];
static int doneN;
static void done(const airqItem_t *it, airqResult_t r)
{
	doneCount[r]++;
	doneTag[doneN++ & 63] = it->tag;
}

static uint8_t data[DMR_DATA_MAX_PACKET + 1];
static airqItem_t item(uint32_t dst, int length)
{
	for (int i = 0; i < length; i++)
	{
		data[i] = (uint8_t)(dst + (uint32_t)i);
	}
	return (airqItem_t){ .kind = AIRQ_IP, .owner = AIRQ_OWNER_GATEWAY, .protocol = 17, .dst = dst, .tag = dst,
						 .waitMs = 2000, .data = data, .length = (uint16_t)length };
}

static bool put(uint32_t dst, int length)
{
	airqItem_t it = item(dst, length);
	return airqPut(&it);
}

static void reset(void)
{
	airqInit();
	airqSetDone(AIRQ_OWNER_GATEWAY, done);
	airqSetDone(AIRQ_OWNER_ZONE_SHARE, done);
	busy = pending = false;
	result = DMR_REQ_OK;
	chainTake = 1;
	chainOffered = 0;
	chainHeldCount = 0;
	sentCount = 0;
	doneN = 0;
	heldChanged = 0;
	memset(doneCount, 0, sizeof(doneCount));
}

// Ticks until the queue is empty (the service finishing each at once)
static void drain(uint32_t now)
{
	for (int i = 0; i < 300; i++)
	{
		serviceDone();
		airqTick(RADIO_ID_FOR_TEST, now);
	}
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, now);
}

static void test_mixed_sizes(void)
{
	reset();
	int sizes[] = { 1, 120, 480, 37, 120, 120, 2, 300 };
	int n = (int)(sizeof(sizes) / sizeof(sizes[0]));
	int bytes = 0;

	for (int i = 0; i < n; i++)
	{
		bool fits = bytes + REC(sizes[i]) <= RING;
		CHECK(put(100 + (uint32_t)i, sizes[i]) == fits);
		bytes += fits ? REC(sizes[i]) : 0;
	}
	airqStats_t s;
	airqGetStats(&s);
	// Each packet takes its own length, not a fixed slot
	CHECK(s.bytes == bytes && s.depth == n && s.capacity == RING && s.highWaterBytes == bytes);
	CHECK(put(0, 0) == false && put(0, DMR_DATA_MAX_PACKET + 1) == false);
	drain(0);
	CHECK(sentCount == n && doneCount[AIRQ_SENT] == n && heldChanged == 0);
	for (int i = 0; i < n; i++)
	{
		CHECK(sentDst[i] == 100 + (uint32_t)i);
	}
	airqGetStats(&s);
	CHECK(s.depth == 0 && s.bytes == 0 && s.sent == (uint32_t)n && s.highWater == n);
}

static void test_fill_and_refuse(void)
{
	reset();
	// Four typical datagrams (120 octets with the UDP header) and two of the longest fit at least
	CHECK(4 * REC(128) <= RING && 2 * REC(DMR_DATA_MAX_PACKET) <= RING);
	busy = true;
	int taken = 0;
	while (put(1000 + (uint32_t)taken, 128))
	{
		taken++;
	}
	CHECK(taken == RING / REC(128));
	airqStats_t s;
	airqGetStats(&s);
	CHECK(s.refusedFull == 1 && s.depth == taken && s.bytes == taken * REC(128));
	// What is left still takes a packet that fits in it: refused only when the packet doesn't fit
	int left = RING - taken * REC(128);
	if (left >= REC(1))
	{
		CHECK(put(1, left - AIRQ_RECORD_HEADER));
		CHECK(!put(2, 1));
	}
	busy = false;
	drain(0);
	CHECK(sentCount == taken + (left >= REC(1)) && doneCount[AIRQ_SENT] == sentCount);
}

static void test_wrap_with_head_held(void)
{
	reset();
	// Fill most of the ring with two long ones and a short one; the service takes the first and keeps it
	CHECK(put(1, 480) && put(2, 480) && put(3, 100));
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(pending && sentCount == 1 && held != NULL);
	const uint8_t *first = held;
	// While it is held, more is added: what doesn't fit after the newest goes at the start only when the oldest
	// (still held) isn't in the way
	int added = 0;
	while (put(10 + (uint32_t)added, 200))
	{
		added++;
	}
	CHECK(heldChanged == 0 && memcmp(first, heldCopy, (size_t)heldLength) == 0);
	serviceDone();
	CHECK(heldChanged == 0);
	airqTick(RADIO_ID_FOR_TEST, 0);	// the first ends, the second goes
	CHECK(sentCount == 2 && sentDst[1] == 2);
	// The first's room is free now: a packet that only fits there goes at the start (the wrap)
	int wrapped = 0;
	while (put(50 + (uint32_t)wrapped, 200))
	{
		wrapped++;
	}
	CHECK(wrapped >= 1);
	// Everything comes out whole, in order
	uint32_t expect[64];
	int e = 0;
	expect[e++] = 1;
	expect[e++] = 2;
	expect[e++] = 3;
	for (int i = 0; i < added; i++)
	{
		expect[e++] = 10 + (uint32_t)i;
	}
	for (int i = 0; i < wrapped; i++)
	{
		expect[e++] = 50 + (uint32_t)i;
	}
	for (int i = 0; i < 100; i++)
	{
		// Each packet's bytes as put: checked while the service holds it
		if (pending)
		{
			uint32_t dst = sentDst[(sentCount - 1) & 255];
			int bad = 0;
			for (int k = 0; k < heldLength; k++)
			{
				bad += held[k] != (uint8_t)(dst + (uint32_t)k);
			}
			CHECK(bad == 0);
		}
		serviceDone();
		airqTick(RADIO_ID_FOR_TEST, 0);
	}
	CHECK(sentCount == e && heldChanged == 0);
	for (int i = 0; i < e && i < sentCount; i++)
	{
		CHECK(sentDst[i] == expect[i]);
	}
}

static void test_batch_and_cancel(void)
{
	reset();
	busy = true;
	airqItem_t items[4];
	static uint8_t frag[4][336];
	for (int i = 0; i < 4; i++)
	{
		memset(frag[i], i, sizeof(frag[i]));
		items[i] = (airqItem_t){ .kind = AIRQ_IP, .owner = AIRQ_OWNER_ZONE_SHARE, .dst = 7, .tag = 9, .waitMs = 10000,
								 .data = frag[i], .length = sizeof(frag[i]) };
	}
	CHECK(put(1, 480));
	CHECK(airqPutBatch(items, 4));
	CHECK(airqCount(AIRQ_OWNER_ZONE_SHARE) == 4 && airqCount(AIRQ_OWNER_GATEWAY) == 1);
	// A second zone doesn't fit whole: none of it is taken, the queue as before
	airqStats_t before, after;
	airqGetStats(&before);
	CHECK(!airqPutBatch(items, 4));
	airqGetStats(&after);
	CHECK(after.depth == before.depth && after.bytes == before.bytes && after.refusedFull == before.refusedFull + 4);
	// Cancel: the zone's fragments end canceled, unsent
	airqCancel(AIRQ_OWNER_ZONE_SHARE, 9);
	busy = false;
	drain(0);
	CHECK(sentCount == 1 && doneCount[AIRQ_CANCELED] == 4 && doneCount[AIRQ_SENT] == 1);
}

static void test_timing(void)
{
	reset();
	// The oldest refused for its wait: given up; the next gets a wait of its own
	busy = true;
	CHECK(put(1, 10) && put(2, 10));
	airqTick(RADIO_ID_FOR_TEST, 1000);
	airqTick(RADIO_ID_FOR_TEST, 2999);
	CHECK(doneCount[AIRQ_GAVE_UP] == 0);
	airqTick(RADIO_ID_FOR_TEST, 3000);
	CHECK(doneCount[AIRQ_GAVE_UP] == 1);
	airqTick(RADIO_ID_FOR_TEST, 3010);	// the next is offered: its wait starts
	airqTick(RADIO_ID_FOR_TEST, 5009);
	CHECK(doneCount[AIRQ_GAVE_UP] == 1);
	airqTick(RADIO_ID_FOR_TEST, 5010);
	CHECK(doneCount[AIRQ_GAVE_UP] == 2);
	// The maximum age counts from the first tick that saw it
	busy = false;
	pending = false;
	airqItem_t it = item(3, 10);
	it.maxAgeMs = 500;
	CHECK(airqPut(&it));
	it = item(4, 10);
	it.maxAgeMs = 500;
	pending = true;// the service is busy with something of its own
	CHECK(dmrDataServiceIpBusy());
	CHECK(airqPut(&it));
	airqTick(RADIO_ID_FOR_TEST, 6000);
	airqTick(RADIO_ID_FOR_TEST, 6501);
	CHECK(doneCount[AIRQ_AGED] == 2 && sentCount == 0);
	// No DMR ID: dropped
	pending = false;
	CHECK(put(5, 10));
	airqTick(0, 7000);
	CHECK(doneCount[AIRQ_NO_ID] == 1);
	// A failed delivery is the owner's to know
	CHECK(put(6, 10));
	airqTick(RADIO_ID_FOR_TEST, 7000);
	result = DMR_REQ_FAILED;
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, 7000);
	CHECK(doneCount[AIRQ_FAILED] == 1);
}

// A long random run: puts of random sizes while the service holds packets for random times; checked against a model
static void test_random(void)
{
	reset();
	fuzz_t f;
	fuzz_init(&f, 0xA1A1);
	long iters = fuzz_iters(20000);
	uint32_t nextDst = 1, expectNext = 1;
	int model = 0;
	int bad = 0;
	for (long i = 0; i < iters; i++)
	{
		uint32_t op = fuzz_below(&f, 4);
		if (op < 2)
		{
			int len = 1 + (int)fuzz_below(&f, DMR_DATA_MAX_PACKET);
			if (put(nextDst, len))
			{
				nextDst++;
				model++;
			}
		}
		else if (op == 2)
		{
			serviceDone();
		}
		int before = sentCount;
		airqTick(RADIO_ID_FOR_TEST, (uint32_t)i);
		if (sentCount != before)
		{
			bad += sentDst[(sentCount - 1) & 255] != expectNext;
			expectNext++;
			// the bytes as put
			uint32_t dst = sentDst[(sentCount - 1) & 255];
			for (int k = 0; k < heldLength; k++)
			{
				bad += held[k] != (uint8_t)(dst + (uint32_t)k);
			}
		}
		airqStats_t s;
		airqGetStats(&s);
		bad += (s.bytes > RING);
	}
	drain(0);
	CHECK(bad == 0 && heldChanged == 0 && (uint32_t)sentCount == nextDst - 1 && model == sentCount);
	printf("# air queue random run: %ld steps, %d packets\n", iters, sentCount);
}

// Packets right after the oldest to the same destination go with it in one transmission: offered together, all
// ending with the transmission's result, kept in place meanwhile, not canceled while the service has them
static void test_chain(void)
{
	reset();
	chainTake = DMR_DATA_CHAIN_MAX;
	CHECK(put(1, 120) && put(1, 40) && put(1, 300) && put(2, 60) && put(1, 80));
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(chainOffered == 3 && sentCount == 3);	// stops at the packet to another destination
	airqStats_t s;
	airqGetStats(&s);
	CHECK(s.depth == 5 && s.chained == 2);
	airqCancel(AIRQ_OWNER_GATEWAY, 1);		// the service has the first three; only the last is canceled
	CHECK(put(3, 200) && put(3, 200));		// added while they are held
	result = DMR_REQ_FAILED;
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(doneCount[AIRQ_FAILED] == 3 && heldChanged == 0);
	CHECK(sentCount == 4 && sentDst[3] == 2 && chainOffered == 1);	// then the one to 2, alone
	result = DMR_REQ_OK;
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(doneCount[AIRQ_SENT] == 1 && doneCount[AIRQ_CANCELED] == 1);
	CHECK(sentCount == 6 && chainOffered == 2);	// the canceled one dropped, the two to 3 together
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, 0);
	airqGetStats(&s);
	CHECK(doneCount[AIRQ_SENT] == 3 && s.depth == 0 && s.chained == 3);

	// The service taking fewer than offered: the rest go next
	reset();
	chainTake = 2;
	CHECK(put(5, 100) && put(5, 100) && put(5, 100));
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(chainOffered == 3 && sentCount == 2);
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(doneCount[AIRQ_SENT] == 2 && sentCount == 3 && chainOffered == 1);
	drain(0);
	CHECK(doneCount[AIRQ_SENT] == 3);

	// Raw data and a group to the same number don't join an individual chain
	reset();
	chainTake = DMR_DATA_CHAIN_MAX;
	airqItem_t g = item(6, 50);
	g.group = true;
	CHECK(put(6, 50) && airqPut(&g));
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(chainOffered == 1);
	drain(0);
	CHECK(doneCount[AIRQ_SENT] == 2);

	// Packets with other IP addresses or another TTL (the gateway's off-net route) don't join a chain of ours, and
	// the service gets each one's own
	reset();
	chainTake = DMR_DATA_CHAIN_MAX;
	airqItem_t o = item(7, 50);
	o.dstIp = 0xC0A80A05u;
	o.ttl = 30;
	airqItem_t t = item(7, 50);
	t.ttl = 5;
	CHECK(put(7, 50) && airqPut(&o) && airqPut(&o) && airqPut(&t));
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(chainOffered == 1 && chainSrcIp == 0 && chainDstIp == 0 && chainTtl == 0);
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(chainOffered == 2 && chainSrcIp == 0 && chainDstIp == 0xC0A80A05u && chainTtl == 30);
	serviceDone();
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(chainOffered == 1 && chainDstIp == 0 && chainTtl == 5);
	drain(0);
	CHECK(doneCount[AIRQ_SENT] == 4);
}

// A packet in two pieces (a zone share fragment: its headers, then its slice of the record) goes as one
static void test_head_and_tail(void)
{
	reset();
	uint8_t head[10], tail[DMR_DATA_MAX_PACKET];
	for (int i = 0; i < (int)sizeof(head); i++)
	{
		head[i] = (uint8_t)(0xA0 + i);
	}
	for (int i = 0; i < (int)sizeof(tail); i++)
	{
		tail[i] = (uint8_t)i;
	}
	airqItem_t it = item(7, 0);
	it.data = head;
	it.length = sizeof(head);
	it.tail = tail;
	it.tailLength = 20;
	CHECK(airqPut(&it));
	airqStats_t st;
	airqGetStats(&st);
	CHECK(st.bytes == ((AIRQ_RECORD_HEADER + 30 + 3) & ~3));
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(heldLength == 30 && memcmp(heldCopy, head, sizeof(head)) == 0 && memcmp(heldCopy + 10, tail, 20) == 0);
	drain(0);
	CHECK(doneCount[AIRQ_SENT] == 1);
	// The two pieces together must fit one packet; a tail without its pointer counts for nothing
	it.tailLength = (uint16_t)(DMR_DATA_MAX_PACKET - sizeof(head) + 1);
	CHECK(!airqPut(&it));
	it.tailLength = (uint16_t)(DMR_DATA_MAX_PACKET - sizeof(head));
	CHECK(airqPut(&it));
	it.tail = NULL;
	it.tailLength = 500;
	CHECK(airqPut(&it));
	airqTick(RADIO_ID_FOR_TEST, 0);
	CHECK(heldLength == DMR_DATA_MAX_PACKET);
	drain(0);
	CHECK(heldLength == (int)sizeof(head) && doneCount[AIRQ_SENT] == 3);
}

int main(void)
{
	test_mixed_sizes();
	test_head_and_tail();
	test_fill_and_refuse();
	test_wrap_with_head_held();
	test_batch_and_cancel();
	test_timing();
	test_chain();
	test_random();
	printf("airQueueTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
