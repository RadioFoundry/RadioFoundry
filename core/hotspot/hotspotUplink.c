// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Hotspot uplink: what the simplex MMDVM hotspot hears on RF, as MMDVMHost wants it (see hotspotUplink.h)
 */

#include <string.h>
#include "hotspot/hotspotUplink.h"
#include "dmr/dmrData.h"

#define FLCO_GROUP			0x00
#define FLCO_PRIVATE		0x03
#define DUPLICATE_MS		45U		// the same burst again within this is the same burst

const uint8_t HOTSPOT_UPLINK_SILENCE[HOTSPOT_UPLINK_AMBE_LENGTH] = {
		0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
		0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
		0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

// The queue: written by the Sys interrupt only, read by the hotspot task only
static hotspotUplinkBurst_t rxQueue[HOTSPOT_UPLINK_QUEUE_SIZE];
static volatile uint8_t rxIn = 0;
static volatile uint8_t rxOut = 0;
static volatile uint32_t rxOverflows = 0;

void hotspotUplinkRxReset(void)
{
	rxOut = rxIn;
}

bool hotspotUplinkRxPush(const hotspotUplinkBurst_t *b)
{
	uint8_t next = (uint8_t)((rxIn + 1) % HOTSPOT_UPLINK_QUEUE_SIZE);

	if (next == rxOut)
	{
		rxOverflows++;
		return false;
	}

	memcpy(&rxQueue[rxIn], b, sizeof(hotspotUplinkBurst_t));
	rxIn = next;

	return true;
}

bool hotspotUplinkRxPop(hotspotUplinkBurst_t *b)
{
	uint8_t out = rxOut;

	if (out == rxIn)
	{
		return false;
	}

	memcpy(b, &rxQueue[out], sizeof(hotspotUplinkBurst_t));
	rxOut = (uint8_t)((out + 1) % HOTSPOT_UPLINK_QUEUE_SIZE);

	return true;
}

uint32_t hotspotUplinkRxOverflows(void)
{
	return rxOverflows;
}

bool hotspotUplinkLcIsVoice(const uint8_t *lc)
{
	uint8_t flco = lc[0] & 0x3F;

	return ((flco == FLCO_GROUP) || (flco == FLCO_PRIVATE)) &&
			((lc[3] | lc[4] | lc[5]) != 0) && ((lc[6] | lc[7] | lc[8]) != 0);
}

int hotspotUplinkVoiceSeq(uint8_t dataType, uint32_t nowMs, uint32_t *lastVoiceMs)
{
	int seq = dataType & 0x07;

	if ((seq < 1) || (seq > 6) || ((nowMs - *lastVoiceMs) < HOTSPOT_UPLINK_VOICE_REPEAT_MS))
	{
		return -1;
	}

	*lastVoiceMs = nowMs;
	return seq - 1;
}

void hotspotUplinkReset(hotspotUplink_t *u)
{
	memset(u, 0, sizeof(hotspotUplink_t));
	u->lastSeq = 5;
}

// The call is over (its terminator, a new call, or lost): true if MMDVMHost knew about it
static bool callClose(hotspotUplink_t *u)
{
	bool announced = u->open && u->headerSent;

	u->open = false;
	u->headerSent = false;
	u->lastSeq = 5;

	return announced;
}

static void callOpen(hotspotUplink_t *u, uint32_t ms)
{
	u->open = true;
	u->headerSent = false;
	u->lastSeq = 5;// burst A next
	u->lastMs = ms;
}

uint8_t hotspotUplinkBurst(hotspotUplink_t *u, const hotspotUplinkBurst_t *b, uint8_t *fillFirst)
{
	uint8_t actions = 0;
	bool lcValid = (b->flags & HOTSPOT_UPLINK_FLAG_LC_VALID) && hotspotUplinkLcIsVoice(b->lc);

	// A call heard nothing of for too long is over, if hotspotUplinkLost() wasn't polled in between
	if (u->open && ((b->ms - u->lastMs) > HOTSPOT_UPLINK_LOST_MS))
	{
		if (callClose(u))
		{
			actions |= HOTSPOT_UP_LOST;
		}
	}

	switch (b->kind)
	{
		case HOTSPOT_UPLINK_HEADER:
			if (!lcValid)
			{
				return actions;
			}

			// Another call's header while one is up (it lost its terminator): that one is over
			if (u->open && u->headerSent && (memcmp(u->lc, b->lc, HOTSPOT_UPLINK_LC_LENGTH) != 0))
			{
				callClose(u);
				actions |= HOTSPOT_UP_LOST;
			}

			// Every header goes up: MMDVMHost takes the call's first, and each one shows the call is still on
			if (!u->open || !u->headerSent)
			{
				callOpen(u, b->ms);
			}

			memcpy(u->lc, b->lc, HOTSPOT_UPLINK_LC_LENGTH);
			u->headerSent = true;
			u->lastSeq = 5;// burst A next
			u->lastMs = b->ms;

			return actions | HOTSPOT_UP_HEADER;

		case HOTSPOT_UPLINK_TERMINATOR:
			if (!u->open)
			{
				return actions;// the other terminators of a call already ended, or one never heard
			}

			u->lastMs = b->ms;

			if (callClose(u))
			{
				actions |= HOTSPOT_UP_TERMINATOR;
			}

			return actions;

		case HOTSPOT_UPLINK_VOICE:
			break;

		default:
			return actions;
	}

	if (b->seq > 5)
	{
		return actions;
	}

	if (!u->open)
	{
		// Only a burst with a good EMB starts a call (noise after the end of a call doesn't)
		if ((b->flags & HOTSPOT_UPLINK_FLAG_SYNC_OK) == 0)
		{
			return actions;
		}

		callOpen(u, b->ms);
	}

	uint32_t elapsed = b->ms - u->lastMs;

	// Late entry: MMDVMHost needs the call's LC first, a header made from it, once it's known
	if (!u->headerSent)
	{
		u->lastMs = b->ms;

		if (!lcValid)
		{
			return actions;
		}

		memcpy(u->lc, b->lc, HOTSPOT_UPLINK_LC_LENGTH);
		u->headerSent = true;
		actions |= HOTSPOT_UP_HEADER;

		// MMDVMHost only takes B-F in sequence after an A (its voice sync resets the sequence; a header doesn't): the
		// superframe so far is filled with silence from A, and the voice goes on from this burst. Waiting for the next A
		// instead cost a whole superframe whenever that A was missed (normal hotspot rf_late: 6 or 0 of 22 bursts)
		if (b->seq != 0)
		{
			*fillFirst = 0;
			actions |= HOTSPOT_UP_FILL;
		}

		u->lastSeq = b->seq;
		return actions | HOTSPOT_UP_VOICE;
	}

	// The same burst again
	if ((b->seq == u->lastSeq) && (elapsed < DUPLICATE_MS))
	{
		return actions;
	}

	uint8_t expected = (uint8_t)((u->lastSeq + 1) % 6);
	uint8_t missing = (uint8_t)((b->seq + 6 - expected) % 6);

	// Bursts missed: as many silent ones as there was time for (a burst every 60 ms), so the sequence MMDVMHost checks
	// carries on. If there wasn't time for them all the numbering is off: MMDVMHost picks the call up again at burst A
	if ((missing > 0) && ((elapsed + (HOTSPOT_UPLINK_BURST_MS / 2)) >= ((uint32_t)(missing + 1) * HOTSPOT_UPLINK_BURST_MS)))
	{
		*fillFirst = expected;
		actions |= HOTSPOT_UP_FILL;
	}

	u->lastSeq = b->seq;
	u->lastMs = b->ms;

	return actions | HOTSPOT_UP_VOICE;
}

bool hotspotUplinkLost(hotspotUplink_t *u, uint32_t nowMs)
{
	if (u->open && ((nowMs - u->lastMs) > HOTSPOT_UPLINK_LOST_MS))
	{
		return callClose(u);
	}

	return false;
}

bool hotspotUplinkData(hotspotUplink_t *u, uint8_t dataType, uint32_t nowMs)
{
	bool block = (dataType == DT_RATE_12_DATA) || (dataType == DT_RATE_34_DATA) || (dataType == DT_RATE_1_DATA);

	if (dataType == DT_IDLE)
	{
		return false;
	}

	if (block)
	{
		// No CRC of their own: only within a data transmission (noise decodes as them)
		if (!u->dataOpen || ((nowMs - u->dataLastMs) > HOTSPOT_UPLINK_DATA_GAP_MS))
		{
			u->dataOpen = false;
			return false;
		}
	}

	u->dataOpen = true;
	u->dataLastMs = nowMs;

	return true;
}
