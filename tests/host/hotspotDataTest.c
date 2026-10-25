// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host unit test for the hotspot's packet data lists (hotspot/hotspotData.c)
 */

#include <stdio.h>
#include <string.h>
#include "dmr/dmrData.h"
#include "hotspot/dmrDataFrame.h"
#include "hotspot/hotspotData.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static struct
{
	int starts;
	int count;
	dmrBurst_t first;
	bool accept;
	bool done;
} tx;

bool hotspotDataTxStart(const dmrBurst_t *bursts, int count)
{
	if (!tx.accept)
	{
		return false;
	}
	tx.starts++;
	tx.count = count;
	tx.first = bursts[0];
	tx.done = false;
	return true;
}

bool hotspotDataTxDone(void)
{
	return tx.done;
}

static uint8_t queue(const dmrBurst_t *b, uint32_t now)
{
	uint8_t frame[33];

	dmrDataBurstToFrame(b, 2, frame);
	return hotspotDataQueue(b->dataType, frame, now);
}

static dmrBurst_t preamble(int blocksToFollow)
{
	const uint8_t csbk[10] = { 0xBD, 0x00, 0x80, blocksToFollow, 0x98, 0x70, 0x37, 0x2F, 0x52, 0x5C };
	dmrBurst_t b;

	dmrDataBuildCSBK(csbk, &b);
	return b;
}

// An unconfirmed packet of 6 Rate 1/2 blocks, without preambles: header + 6 blocks
static int packet(dmrBurst_t *out)
{
	uint8_t data[60];

	memset(data, 0x55, sizeof(data));
	return dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_IP, false, 9990199, 3101276, data, sizeof(data), DT_RATE_12_DATA, 0, out, DMR_DATA_MAX_BURSTS);
}

static dmrBurst_t storage[DMR_DATA_MAX_BURSTS];

static void reset(void)
{
	hotspotDataReset(storage);
	memset(&tx, 0, sizeof(tx));
	tx.accept = true;
}

// KNOWN_BUGS 12: preambles announcing 2 and 1, then a header announcing 6 blocks. All of it goes out in one list
static void testHeaderAfterShortPreambles(bool paced)
{
	dmrBurst_t p[DMR_DATA_MAX_BURSTS];
	int n = packet(p);
	uint32_t t = 1000;

	reset();
	CHECK(n == 7);

	dmrBurst_t pre[2] = { preamble(2), preamble(1) };
	for (int i = 0; i < 2; i++)
	{
		CHECK(queue(&pre[i], t) == 0);
		hotspotDataTick(t, true);
		t += paced ? 60 : 0;
	}
	for (int i = 0; i < n; i++)
	{
		CHECK(queue(&p[i], t) == 0);
		CHECK(tx.starts == 0);
		hotspotDataTick(t, true);
		CHECK(tx.starts == ((i == n - 1) ? 1 : 0));
		t += paced ? 60 : 0;
	}
	CHECK(tx.count == 9 && tx.first.dataType == DT_CSBK);
	CHECK(hotspotDataIsBusy());
}

// A packet cut short: it goes out HOTSPOT_DATA_GAP_MS after its last frame
static void testGap(void)
{
	dmrBurst_t p[DMR_DATA_MAX_BURSTS];
	packet(p);

	reset();
	for (int i = 0; i < 4; i++)
	{
		CHECK(queue(&p[i], 1000) == 0);
	}
	hotspotDataTick(1000 + HOTSPOT_DATA_GAP_MS, true);
	CHECK(tx.starts == 0);
	hotspotDataTick(1000 + HOTSPOT_DATA_GAP_MS + 1, true);
	CHECK(tx.starts == 1 && tx.count == 4);
}

// KNOWN_BUGS 13: frames that arrive while a list is on air go to the second list, which follows it
static void testSecondList(void)
{
	dmrBurst_t p[DMR_DATA_MAX_BURSTS];
	int n = packet(p);
	dmrBurst_t lone = preamble(0);

	reset();
	for (int i = 0; i < n; i++)
	{
		queue(&p[i], 1000);
	}
	hotspotDataTick(1000, true);
	CHECK(tx.starts == 1);

	CHECK(queue(&lone, 1010) == 0);// on air: taken, not dropped
	CHECK(hotspotDataSpace() == DMR_DATA_MAX_BURSTS - n - 1);// the list on air and the next one share the array
	for (int i = 0; i < DMR_DATA_MAX_BURSTS - n - 1; i++)
	{
		CHECK(queue(&lone, 1010) == 0);
	}
	CHECK(queue(&lone, 1010) == HOTSPOT_DATA_NAK_FULL);
	hotspotDataTick(1020, true);
	CHECK(tx.starts == 1);// waits for the first list

	tx.done = true;
	hotspotDataTick(1400, true);
	CHECK(tx.starts == 2 && tx.count == DMR_DATA_MAX_BURSTS - n && tx.first.dataType == DT_CSBK);
	CHECK(storage[0].dataType == DT_CSBK && memcmp(storage[0].payload, lone.payload, 12) == 0);// moved to the front

	tx.done = true;
	hotspotDataTick(1500, true);
	CHECK(!hotspotDataIsBusy() && hotspotDataSpace() == DMR_DATA_MAX_BURSTS);
}

// KNOWN_BUGS 13: what the radio can't take is refused, not acknowledged and dropped
static void testRefused(void)
{
	uint8_t frame[33] = { 0 };
	dmrBurst_t lone = preamble(0);

	reset();
	CHECK(hotspotDataQueue(DT_VOICE_PI_HEADER, frame, 0) == HOTSPOT_DATA_NAK_UNSUPPORTED);
	CHECK(hotspotDataQueue(DT_IDLE, frame, 0) == HOTSPOT_DATA_NAK_UNSUPPORTED);
	CHECK(hotspotDataQueue(0x0B, frame, 0) == HOTSPOT_DATA_NAK_UNSUPPORTED);// USBD
	CHECK(!hotspotDataIsBusy());

	for (int i = 0; i < DMR_DATA_MAX_BURSTS; i++)
	{
		CHECK(queue(&lone, 0) == 0);
	}
	CHECK(hotspotDataSpace() == 0);
	CHECK(queue(&lone, 0) == HOTSPOT_DATA_NAK_FULL);
}

// Rate 3/4 and Rate 1 blocks are taken, with their 18 and 24 bytes
static void testCodedBlocks(void)
{
	dmrBurst_t r34 = { .dataType = DT_RATE_34_DATA, .length = 18 }, r1 = { .dataType = DT_RATE_1_DATA, .length = 24 };

	reset();
	memset(r34.payload, 0x3C, 18);
	memset(r1.payload, 0xA5, 24);
	CHECK(queue(&r34, 0) == 0);
	CHECK(queue(&r1, 0) == 0);
	CHECK(storage[0].length == 18 && memcmp(storage[0].payload, r34.payload, 18) == 0);
	CHECK(storage[1].length == 24 && memcmp(storage[1].payload, r1.payload, 24) == 0);
}

// The channel never frees up: the list is dropped after HOTSPOT_DATA_GIVE_UP_MS
static void testGiveUp(void)
{
	dmrBurst_t lone = preamble(0);

	reset();
	queue(&lone, 1000);
	hotspotDataTick(1000, false);
	CHECK(hotspotDataIsBusy());// KNOWN_BUGS 14: queued counts as busy
	hotspotDataTick(1000 + HOTSPOT_DATA_GIVE_UP_MS, false);
	CHECK(hotspotDataIsBusy());
	hotspotDataTick(1001 + HOTSPOT_DATA_GIVE_UP_MS, false);
	CHECK(!hotspotDataIsBusy() && tx.starts == 0);
}

// A proprietary header after the data header counts as one of its blocks
static void testProprietaryHeader(void)
{
	dmrBurst_t p[DMR_DATA_MAX_BURSTS];
	int n = packet(p);
	dmrBurst_t prop = p[0];

	reset();
	prop.payload[0] = (prop.payload[0] & 0xF0) | DMR_DPF_PROPRIETARY;
	queue(&p[0], 0);
	queue(&prop, 0);
	for (int i = 1; i < n - 1; i++)
	{
		queue(&p[i], 0);
	}
	hotspotDataTick(0, true);
	CHECK(tx.starts == 1 && tx.count == n);// header + proprietary header + 5 blocks = the 6 announced
}

static void setSync(uint8_t *frame, uint64_t sync)
{
	frame[13] = (frame[13] & 0xF0) | (uint8_t)((sync >> 44) & 0x0F);
	for (int i = 0; i < 5; i++)
	{
		frame[14 + i] = (uint8_t)(sync >> (36 - (8 * i)));
	}
	frame[19] = (frame[19] & 0x0F) | (uint8_t)((sync & 0x0F) << 4);
}

// MMDVMHost's frames all come with control byte 0: the type from the burst's sync and slot type
static void testNetFrameControl(void)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	uint8_t frame[33];
	int n = packet(bursts);
	dmrBurst_t csbk = preamble(3);

	CHECK(n == 7);
	dmrDataBurstToFrame(&bursts[0], 2, frame);
	CHECK(hotspotNetFrameControl(0x00, frame) == (0x40 | DT_DATA_HEADER));
	CHECK(hotspotNetFrameControl(0x47, frame) == 0x47);// the host's own kept
	dmrDataBurstToFrame(&bursts[1], 7, frame);
	CHECK(hotspotNetFrameControl(0x00, frame) == (0x40 | DT_RATE_12_DATA));
	dmrDataBurstToFrame(&csbk, 15, frame);
	CHECK(hotspotNetFrameControl(0x00, frame) == (0x40 | DT_CSBK));

	// BS data sync (a repeater's), one bit wrong
	setSync(frame, 0xDFF57D75DF5DULL ^ 0x000100000000ULL);
	CHECK(hotspotNetFrameControl(0x00, frame) == (0x40 | DT_CSBK));

	// A terminator and an idle burst, color code 2 (the old END_FRAME_PATTERN only matched color code 1)
	csbk.dataType = DT_TERMINATOR_WITH_LC;
	dmrDataBurstToFrame(&csbk, 2, frame);
	CHECK(hotspotNetFrameControl(0x00, frame) == (0x40 | DT_TERMINATOR_WITH_LC));
	csbk.dataType = DT_IDLE;
	dmrDataBurstToFrame(&csbk, 2, frame);
	CHECK(hotspotNetFrameControl(0x00, frame) == (0x40 | DT_IDLE));

	// Voice: A has the voice sync (MS, or direct mode TS2), B-F the EMB (no sync): the control byte as given
	memset(frame, 0x5A, sizeof(frame));
	setSync(frame, 0x7F7D5DD57DFDULL);
	CHECK(hotspotNetFrameControl(0x00, frame) == 0x20);
	setSync(frame, 0x7DFFD5F55D5FULL);
	CHECK(hotspotNetFrameControl(0x00, frame) == 0x20);
	memset(frame, 0x5A, sizeof(frame));
	CHECK(hotspotNetFrameControl(0x03, frame) == 0x03);
	CHECK(hotspotNetFrameControl(0x00, frame) == 0x00);
}

int main(void)
{
	testNetFrameControl();
	testHeaderAfterShortPreambles(false);
	testHeaderAfterShortPreambles(true);
	testGap();
	testSecondList();
	testRefused();
	testCodedBlocks();
	testGiveUp();
	testProprietaryHeader();

	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
