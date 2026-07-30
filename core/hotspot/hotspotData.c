// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Hotspot packet data from the host: MMDVM data frames collected into burst lists for the radio's data TX
 *
 * CSBKs, MBC, data headers and Rate 1/2, Rate 3/4 and Rate 1 blocks are sent with their info bits unchanged (taken
 * from the frame by BPTC, Trellis or Rate 1 decoding, so the host's CRCs are kept).
 */

#include <string.h>
#include "hotspot/hotspotData.h"
#include "hotspot/dmrDataFrame.h"

static dmrBurst_t *bursts;			// the list on air first, then the list being filled
static int onAir = 0;				// bursts of the list on air, 0 if none
static struct
{
	int count;
	int expected;					// bursts this list should hold, from what it has so far
	bool hasHeader;					// a data header has set expected, the preambles no longer count
	uint32_t lastFrameMs;
} list;

static void clearList(void)
{
	list.count = 0;
	list.expected = 0;
	list.hasHeader = false;
}

void hotspotDataReset(dmrBurst_t *storage)
{
	bursts = storage;
	onAir = 0;
	clearList();
}

// Blocks announced by a data header (ETSI TS 102 361-1 9.3)
static int headerBlocks(const uint8_t *h)
{
	switch (h[0] & 0x0F)
	{
		case DMR_DPF_UDT:
			return (h[8] & 0x03) + 1;
		case DMR_DPF_DEFINED_SHORT:
		case DMR_DPF_RAW_SHORT:
			return (((h[0] >> 4) & 0x03) << 4) | (h[1] & 0x0F);
		default:// response, unconfirmed, confirmed
			return h[8] & 0x7F;
	}
}

uint8_t hotspotDataQueue(uint8_t dataType, const uint8_t *frame, uint32_t nowMs)
{
	if ((dataType < DT_CSBK) || (dataType > DT_RATE_1_DATA) || (dataType == DT_IDLE))
	{
		return HOTSPOT_DATA_NAK_UNSUPPORTED;// PI header, Idle, USBD
	}

	if ((onAir + list.count) >= DMR_DATA_MAX_BURSTS)
	{
		return HOTSPOT_DATA_NAK_FULL;
	}

	dmrBurst_t *burst = &bursts[onAir + list.count++];
	dmrDataFrameToBurst(dataType, frame, burst);
	const uint8_t *p = burst->payload;

	if (dataType == DT_DATA_HEADER)
	{
		// A proprietary header after a data header is one of the blocks it announced
		if (!(list.hasHeader && ((p[0] & 0x0F) == DMR_DPF_PROPRIETARY)))
		{
			list.expected = list.count + headerBlocks(p);
			list.hasHeader = true;
		}
	}
	else if ((dataType == DT_CSBK) && ((p[0] & 0x3F) == DMR_CSBKO_PREAMBLE) && !list.hasHeader)
	{
		list.expected = list.count + p[3];// blocks to follow
	}
	else if (((dataType == DT_MBC_HEADER) || (dataType == DT_MBC_CONTINUATION)) && ((p[0] & 0x80) == 0))
	{
		list.expected = list.count + 1;// not the last block
	}

	if (list.expected < list.count)
	{
		list.expected = list.count;// a lone CSBK, or more blocks than announced
	}

	list.lastFrameMs = nowMs;
	return 0;
}

void hotspotDataTick(uint32_t nowMs, bool canStart)
{
	if ((onAir > 0) && hotspotDataTxDone())
	{
		memmove(bursts, &bursts[onAir], list.count * sizeof(dmrBurst_t));// the next list to the front
		onAir = 0;
	}

	if ((onAir > 0) || (list.count == 0))
	{
		return;
	}

	uint32_t idle = nowMs - list.lastFrameMs;

	if ((list.count < list.expected) && (idle <= HOTSPOT_DATA_GAP_MS))
	{
		return;// more to come
	}

	if (canStart && hotspotDataTxStart(bursts, list.count))
	{
		onAir = list.count;
		clearList();
	}
	else if (idle > HOTSPOT_DATA_GIVE_UP_MS)
	{
		clearList();
	}
}

bool hotspotDataIsBusy(void)
{
	return (onAir > 0) || (list.count > 0);
}

int hotspotDataSpace(void)
{
	return DMR_DATA_MAX_BURSTS - onAir - list.count;
}

// The 48 sync bits (burst bits 108-155), ETSI TS 102 361-1 table 9.2
static const uint64_t DATA_SYNCS[] = { 0xDFF57D75DF5DULL, 0xD5D7F77FD757ULL, 0xF7FDD5DDFD55ULL, 0xD7557F5FF7F5ULL };// BS, MS, TS1, TS2
static const uint64_t VOICE_SYNCS[] = { 0x755FD7DF75F7ULL, 0x7F7D5DD57DFDULL, 0x5D577F7757FFULL, 0x7DFFD5F55D5FULL };

static bool syncMatches(uint64_t sync, const uint64_t *patterns)
{
	for (int i = 0; i < 4; i++)
	{
		uint64_t diff = sync ^ patterns[i];
		int errors = 0;

		while ((diff != 0) && (errors <= 4))
		{
			diff &= (diff - 1);
			errors++;
		}

		if (errors <= 4)
		{
			return true;
		}
	}

	return false;
}

uint8_t hotspotNetFrameControl(uint8_t control, const uint8_t *frame)
{
	uint64_t sync = 0;

	if (control & (HOTSPOT_NET_SYNC_DATA | HOTSPOT_NET_SYNC_AUDIO))
	{
		return control;
	}

	// Bits 108-155: the low nibble of byte 13, bytes 14-18, the high nibble of byte 19
	sync = (uint64_t)(frame[13] & 0x0F);
	for (int i = 14; i <= 18; i++)
	{
		sync = (sync << 8) | frame[i];
	}
	sync = (sync << 4) | (frame[19] >> 4);

	if (syncMatches(sync, DATA_SYNCS))
	{
		// The slot type's Golay(20,8) is systematic: colour code bits 98-101, data type bits 102-105 (the low 2 bits of
		// byte 12, the top 2 of byte 13). The host's frames are clean, no correction needed (a full Golay decode isn't worth its flash here)
		return (uint8_t)(HOTSPOT_NET_SYNC_DATA | ((frame[12] & 0x03) << 2) | (frame[13] >> 6));
	}

	if (syncMatches(sync, VOICE_SYNCS))
	{
		return HOTSPOT_NET_SYNC_AUDIO;
	}

	return control;
}
