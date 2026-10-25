// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The air queue (CONFIG_DMR_AIR_QUEUE): the packets the IP gateway (from the host) and zone sharing (the user's
 * "Send zone to...") hand to the air, kept in order and sent through the data service. Unconfirmed IP packets to the
 * same destination, one after another in the queue, go in one transmission; the service keeps its guard between
 * transmissions. One queue and one guard for every producer, so a zone being sent and the host's datagrams don't
 * race each other for the service. docs/spec/dmr-data.md 12.
 *
 *   producers   airqPut / airqPutBatch from any task: the packet is copied into the queue's byte ring as one record
 *               (a header, then the packet) under the data service's lock (dmrDataServiceLock) and the call returns at
 *               once; false when it doesn't fit. A record never wraps: when the end of the buffer has no room for it,
 *               the end is left unused and it goes at the start, so the service can read the oldest in place
 *   radio task  airqTick, with the data service's tick: the oldest packet goes to the service. An IP packet goes to
 *               dmrDataServiceSendIPChain with the IP packets right after it to the same destination, with the
 *               same addresses and TTL; the service takes those that fit one transmission and reads them in place until dmrDataServiceIpBusy() is
 *               false. Raw data goes as an unconfirmed SAP 10 packet. While the service refuses the oldest (busy, not
 *               a DMR channel we may send on), it is tried again until its wait is over; a packet older than its
 *               maximum age is dropped unsent. Each packet's end goes to its owner's done function, in the radio task
 */

#ifndef RFOUNDRY_DMRAIRQUEUE_H
#define RFOUNDRY_DMRAIRQUEUE_H

#include <stdbool.h>
#include <stdint.h>

// The buffer is CONFIG_DMR_AIR_QUEUE_BYTES; each packet takes its own length rounded up to 4 plus AIRQ_RECORD_HEADER.
// No limit on the number of packets but the bytes: a put is refused only when the packet doesn't fit
#define AIRQ_RECORD_HEADER		36

// What a packet is
#define AIRQ_IP					0		// an IP packet from layer 4 on (the service builds the header)
#define AIRQ_RAW				1		// user data as an unconfirmed SAP 10 packet

// Who it belongs to (its done function)
#define AIRQ_OWNER_GATEWAY		0
#define AIRQ_OWNER_ZONE_SHARE	1
#define AIRQ_OWNERS				2

// How a packet ended
typedef enum
{
	AIRQ_SENT,							// on air (confirmed: acknowledged)
	AIRQ_FAILED,						// the service took it but couldn't deliver it (no clear channel, no ACK)
	AIRQ_GAVE_UP,						// the service refused it until its wait was over
	AIRQ_AGED,							// older than its maximum age before its turn
	AIRQ_NO_ID,							// the radio has no DMR ID to send from
	AIRQ_TOO_LONG,						// more than one packet carries
	AIRQ_CANCELED,						// airqCancel
} airqResult_t;

typedef struct
{
	uint8_t  kind;						// AIRQ_IP / AIRQ_RAW
	uint8_t  owner;						// AIRQ_OWNER_*
	uint8_t  protocol;					// IP: DMR_IP_PROTO_*
	uint8_t  delivery;					// IP: DMR_IPS_* (dmrDataService.h)
	bool     group;
	uint32_t dst;
	// IP: the IPv4 addresses and TTL, as dmrDataServiceSendIP takes them. 0 (DMR_DATA_IP_OURS, DMR_IP_TTL_DEFAULT) is
	// ours, so an item that leaves them out is a packet of our own
	uint32_t srcIp;
	uint32_t dstIp;
	uint8_t  ttl;
	uint32_t tag;						// the owner's (zone share: the transfer)
	uint16_t waitMs;					// the oldest packet is offered to the service this long
	uint16_t maxAgeMs;					// dropped unsent when queued longer than this; 0: no limit
	const uint8_t *data;				// airqPut: copied; the done function: NULL
	uint16_t length;					// octets at data (the done function: the whole packet's)
	// Optional, airqPut only: tailLength more octets copied after data's, from here. A zone share fragment is its
	// headers at data and its slice of the record here, so the sender needs no copy of each fragment of its own
	const uint8_t *tail;
	uint16_t tailLength;
} airqItem_t;

typedef struct
{
	uint32_t queued;					// taken into the queue
	uint32_t refusedFull;				// not taken: the queue was full
	uint32_t sent;
	uint32_t failed;					// AIRQ_FAILED
	uint32_t gaveUp;					// AIRQ_GAVE_UP
	uint32_t aged;						// AIRQ_AGED
	uint32_t other;						// AIRQ_NO_ID, AIRQ_TOO_LONG, AIRQ_CANCELED
	uint32_t serviceBusy;				// ticks the oldest packet waited for the service
	uint32_t chained;					// packets that went in the transmission of the one before them
	uint16_t depth;						// packets queued now (the one being sent included)
	uint16_t bytes;						// ... their octets, record headers and padding to 4 included
	uint16_t highWater;					// the most packets at once
	uint16_t highWaterBytes;			// the most octets at once
	uint16_t capacity;					// CONFIG_DMR_AIR_QUEUE_BYTES
} airqStats_t;

typedef void (*airqDoneFn)(const airqItem_t *item, airqResult_t result);

void airqInit(void);
void airqSetDone(uint8_t owner, airqDoneFn done);
bool airqPut(const airqItem_t *item);
// All of them or none (a zone's fragments)
bool airqPutBatch(const airqItem_t *items, int count);
// The owner's packets with this tag that haven't gone to the service yet end as AIRQ_CANCELED at the next tick
void airqCancel(uint8_t owner, uint32_t tag);
void airqTick(uint32_t ownId, uint32_t nowMs);
void airqGetStats(airqStats_t *stats);
// Packets of this owner in the queue
int airqCount(uint8_t owner);

#endif
