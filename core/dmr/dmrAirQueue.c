// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air queue (dmrAirQueue.h, docs/spec/dmr-data.md 12)
#include "config.h"
#if CONFIG_DMR_AIR_QUEUE	// the whole file
#include <stddef.h>
#include <string.h>
#include "dmr/dmrAirQueue.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"

#define RAW_PREAMBLES	DMR_DATA_SMS_PREAMBLES	// as for an SMS: receivers get time to sync
#define RING_BYTES		((CONFIG_DMR_AIR_QUEUE_BYTES + 3) & ~3)

// One packet in the ring: this header, then its octets, the whole rounded up to 4
typedef struct
{
	uint16_t size;						// the record's octets, header and padding included
	uint16_t length;					// the packet's
	uint8_t  kind;
	uint8_t  owner;
	uint8_t  protocol;
	uint8_t  delivery;
	uint8_t  group;
	uint8_t  stamped;					// queuedAt is set (at the first tick after the put)
	uint8_t  canceled;
	uint8_t  spare;
	uint32_t dst;
	uint32_t tag;
	uint32_t queuedAt;
	uint16_t waitMs;
	uint16_t maxAgeMs;
} record_t;

_Static_assert(sizeof(record_t) == AIRQ_RECORD_HEADER, "record header size");
_Static_assert(RING_BYTES >= 2 * (AIRQ_RECORD_HEADER + DMR_DATA_MAX_PACKET), "the air queue holds two longest packets");

// The records, oldest first, from rd: up to wr, or when wrapped up to wrapEnd and then from the start up to wr (the
// end past wrapEnd is unused: the record after it didn't fit there). Producers (any task, under the lock) only write
// at wr; the radio task only reads the oldest and frees it, so the bytes the service reads in place stay put
static union
{
	uint32_t align;
	uint8_t  bytes[RING_BYTES];
} ring;
static uint16_t rd, wr, wrapEnd;
static bool wrapped;
static uint16_t count;					// records
static uint16_t usedBytes;				// their sizes, the unused end of a wrapped ring not included
static uint8_t inService;				// the oldest this many are IP packets the data service has (a chain)
static bool headStamped;
static uint32_t headSince;				// the oldest first offered to the service
static airqDoneFn doneFn[AIRQ_OWNERS];
static airqStats_t stats;
static DMR_DATA_BUFFER dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];

static record_t *recAt(uint16_t off)
{
	return (record_t *)(void *)&ring.bytes[off];
}

static uint16_t nextOf(uint16_t off)
{
	uint16_t next = (uint16_t)(off + recAt(off)->size);

	return (wrapped && (next == wrapEnd)) ? 0 : next;
}

static void reset(void)
{
	rd = wr = wrapEnd = 0;
	wrapped = false;
	count = 0;
	usedBytes = 0;
	inService = 0;
	headStamped = false;
}

void airqInit(void)
{
	dmrDataServiceLock();
	reset();
	memset(&stats, 0, sizeof(stats));
	stats.capacity = RING_BYTES;
	dmrDataServiceUnlock();
}

void airqSetDone(uint8_t owner, airqDoneFn done)
{
	if (owner < AIRQ_OWNERS)
	{
		doneFn[owner] = done;
	}
}

// Room for a record of size octets, in one piece: its offset, -1 when it doesn't fit (under the lock). May wrap the
// ring (the end left unused)
static int alloc(uint16_t size)
{
	if (count == 0)
	{
		rd = wr = 0;
		wrapped = false;
	}
	if (!wrapped)
	{
		if (wr + size <= RING_BYTES)
		{
			return wr;
		}
		if ((count > 0) && (size <= rd))
		{
			wrapped = true;
			wrapEnd = wr;
			wr = 0;
			return 0;
		}
		return -1;
	}
	return (wr + size <= rd) ? wr : -1;
}

static void updateDepth(void)
{
	stats.depth = count;
	stats.bytes = usedBytes;
	stats.highWater = (count > stats.highWater) ? count : stats.highWater;
	stats.highWaterBytes = (usedBytes > stats.highWaterBytes) ? usedBytes : stats.highWaterBytes;
	stats.capacity = RING_BYTES;
}

bool airqPutBatch(const airqItem_t *items, int n)
{
	if ((items == NULL) || (n <= 0))
	{
		return false;
	}
	for (int i = 0; i < n; i++)
	{
		if ((items[i].data == NULL) || (items[i].length == 0) || (items[i].length > DMR_DATA_MAX_PACKET) ||
				(items[i].owner >= AIRQ_OWNERS))
		{
			return false;
		}
	}
	dmrDataServiceLock();
	// Kept to go back to when the batch doesn't fit whole
	uint16_t wr0 = wr, wrapEnd0 = wrapEnd, count0 = count, used0 = usedBytes, rd0 = rd;
	bool wrapped0 = wrapped;
	bool ok = true;

	for (int i = 0; ok && (i < n); i++)
	{
		uint16_t size = (uint16_t)((AIRQ_RECORD_HEADER + items[i].length + 3) & ~3);
		int off = alloc(size);

		if (off < 0)
		{
			ok = false;
			break;
		}
		record_t *r = recAt((uint16_t)off);

		r->size = size;
		r->length = items[i].length;
		r->kind = items[i].kind;
		r->owner = items[i].owner;
		r->protocol = items[i].protocol;
		r->delivery = items[i].delivery;
		r->group = items[i].group;
		r->stamped = 0;
		r->canceled = 0;
		r->dst = items[i].dst;
		r->tag = items[i].tag;
		r->queuedAt = 0;
		r->waitMs = items[i].waitMs;
		r->maxAgeMs = items[i].maxAgeMs;
		memcpy(&ring.bytes[off + AIRQ_RECORD_HEADER], items[i].data, items[i].length);
		wr = (uint16_t)(off + size);
		count++;
		usedBytes = (uint16_t)(usedBytes + size);
	}
	if (ok)
	{
		stats.queued += (uint32_t)n;
	}
	else
	{
		wr = wr0;
		wrapEnd = wrapEnd0;
		wrapped = wrapped0;
		count = count0;
		usedBytes = used0;
		rd = rd0;
		stats.refusedFull += (uint32_t)n;
	}
	updateDepth();
	dmrDataServiceUnlock();
	return ok;
}

bool airqPut(const airqItem_t *item)
{
	return airqPutBatch(item, 1);
}

void airqCancel(uint8_t owner, uint32_t tag)
{
	dmrDataServiceLock();
	uint16_t off = rd;

	for (int i = 0; i < count; i++, off = nextOf(off))
	{
		record_t *r = recAt(off);

		if ((r->owner == owner) && (r->tag == tag) && (i >= inService))
		{
			r->canceled = 1;
		}
	}
	dmrDataServiceUnlock();
}

int airqCount(uint8_t owner)
{
	int n = 0;

	dmrDataServiceLock();
	uint16_t off = rd;

	for (int i = 0; i < count; i++, off = nextOf(off))
	{
		n += (recAt(off)->owner == owner) ? 1 : 0;
	}
	dmrDataServiceUnlock();
	return n;
}

// The oldest has ended: freed, counted, its owner told
static void finish(airqResult_t result)
{
	dmrDataServiceLock();
	const record_t *r = recAt(rd);
	airqItem_t item = { .kind = r->kind, .owner = r->owner, .protocol = r->protocol, .delivery = r->delivery,
						.group = r->group != 0, .dst = r->dst, .tag = r->tag, .waitMs = r->waitMs,
						.maxAgeMs = r->maxAgeMs, .data = NULL, .length = r->length };

	usedBytes = (uint16_t)(usedBytes - r->size);
	rd = nextOf(rd);
	if (wrapped && (rd == 0))
	{
		wrapped = false;// the oldest is at the start again: one stretch
	}
	count--;
	if (count == 0)
	{
		rd = wr = 0;
		wrapped = false;
	}
	headStamped = false;
	inService = (inService > 0) ? (uint8_t)(inService - 1) : 0;
	switch (result)
	{
		case AIRQ_SENT:
			stats.sent++;
			break;
		case AIRQ_FAILED:
			stats.failed++;
			break;
		case AIRQ_GAVE_UP:
			stats.gaveUp++;
			break;
		case AIRQ_AGED:
			stats.aged++;
			break;
		default:
			stats.other++;
			break;
	}
	updateDepth();
	dmrDataServiceUnlock();
	if ((item.owner < AIRQ_OWNERS) && (doneFn[item.owner] != NULL))
	{
		doneFn[item.owner](&item, result);
	}
}

// The oldest IP packet and those right after it to the same destination and call type, as a chain for the service
// (it takes those that go unconfirmed and fit one transmission). Stops at anything else, so the order stays
static int gather(uint32_t nowMs, dmrDataChainPacket_t *chain)
{
	const record_t *head = recAt(rd);
	uint16_t off = rd;
	int c = 0;

	dmrDataServiceLock();// producers may be adding at wr (and wrapping) meanwhile
	int n = count;

	for (int i = 0; (i < n) && (c < DMR_DATA_CHAIN_MAX); i++, off = nextOf(off))
	{
		const record_t *r = recAt(off);

		if ((i > 0) && ((r->kind != AIRQ_IP) || (r->group != head->group) || (r->dst != head->dst) || r->canceled ||
				((r->maxAgeMs != 0) && ((nowMs - r->queuedAt) > r->maxAgeMs))))
		{
			break;
		}
		chain[c].protocol = r->protocol;
		chain[c].delivery = r->delivery;
		chain[c].l4 = &ring.bytes[off + AIRQ_RECORD_HEADER];
		chain[c].length = r->length;
		c++;
	}
	dmrDataServiceUnlock();
	return c;
}

void airqTick(uint32_t ownId, uint32_t nowMs)
{
	// The age counts from the first tick that saw the packet (producers don't have the clock)
	dmrDataServiceLock();
	int n = count;
	uint16_t off = rd;

	for (int i = 0; i < n; i++, off = nextOf(off))
	{
		record_t *r = recAt(off);

		if (!r->stamped)
		{
			r->stamped = 1;
			r->queuedAt = nowMs;
		}
	}
	dmrDataServiceUnlock();

	if (inService > 0)
	{
		if (dmrDataServiceIpBusy())
		{
			return;
		}
		// One transmission: the same end for each packet in it
		airqResult_t result = (dmrDataServiceIpResult() == DMR_REQ_OK) ? AIRQ_SENT : AIRQ_FAILED;

		while (inService > 0)
		{
			finish(result);
			n--;
		}
	}
	while (n > 0)
	{
		const record_t *r = recAt(rd);
		const uint8_t *data = &ring.bytes[rd + AIRQ_RECORD_HEADER];

		n--;
		if (r->canceled)
		{
			finish(AIRQ_CANCELED);
			continue;
		}
		if ((ownId & 0xFFFFFFu) == 0)
		{
			finish(AIRQ_NO_ID);
			continue;
		}
		if ((r->maxAgeMs != 0) && ((nowMs - r->queuedAt) > r->maxAgeMs))
		{
			finish(AIRQ_AGED);
			continue;
		}
		if (!headStamped)
		{
			headStamped = true;
			headSince = nowMs;
		}
		if (r->kind == AIRQ_IP)
		{
			// The service builds it, and the IP packets right after it to the same destination that can go in the
			// same transmission, and reads them in place until it's done
			dmrDataChainPacket_t chain[DMR_DATA_CHAIN_MAX];
			int c = gather(nowMs, chain);
			int taken = dmrDataServiceSendIPChain(r->group != 0, r->dst, chain, c);

			if (taken > 0)
			{
				inService = (uint8_t)taken;
				stats.chained += (uint32_t)(taken - 1);
				return;
			}
		}
		else
		{
			int bc = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_SHORT_DATA, r->group != 0, r->dst, ownId & 0xFFFFFFu, data,
										r->length, DT_RATE_12_DATA, RAW_PREAMBLES, bursts, DMR_DATA_MAX_BURSTS);

			if (bc <= 0)
			{
				finish(AIRQ_TOO_LONG);// more bursts than a list holds
				continue;
			}
			if (dmrDataServiceSendBursts(bursts, bc))
			{
				finish(AIRQ_SENT);
				return;
			}
		}
		// Refused: the service is busy, or this isn't a DMR channel we may send on now
		stats.serviceBusy++;
		if ((nowMs - headSince) >= r->waitMs)
		{
			finish(AIRQ_GAVE_UP);
		}
		return;
	}
}

void airqGetStats(airqStats_t *out)
{
	dmrDataServiceLock();
	*out = stats;
	out->capacity = RING_BYTES;
	dmrDataServiceUnlock();
}

#endif	// CONFIG_DMR_AIR_QUEUE
