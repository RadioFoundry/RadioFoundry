// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host unit test for the hotspot uplink (hotspot/hotspotUplink.c): the bursts heard on RF, through the call tracker,
 * into a model of what MMDVMHost's DMRSlot::writeModem() accepts from a simplex modem
 */

#include <stdio.h>
#include <string.h>
#include "dmr/dmrData.h"
#include "hotspot/hotspotUplink.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// MMDVMHost DMRSlot::writeModem(), the RF side, reduced to its states and checks
typedef struct
{
	enum { LISTENING, LATE_ENTRY, AUDIO } state;
	unsigned lastN;
	uint8_t lc[9];
	int starts, ends, losts, voice, silence, dropped;
	int headersSeen;
} host_t;

static void hostHeader(host_t *h, const uint8_t *lc)
{
	h->headersSeen++;
	if (h->state == AUDIO)
	{
		return;// "if (m_rfState == RS_RF_AUDIO) return true"
	}
	memcpy(h->lc, lc, 9);
	h->state = AUDIO;
	h->starts++;
}

static void hostVoice(host_t *h, uint8_t seq, bool silence)
{
	if (seq == 0)
	{
		if (h->state == AUDIO)
		{
			h->lastN = 0;
			h->voice++;
			h->silence += silence;
		}
		else
		{
			h->state = LATE_ENTRY;// its own late entry needs the EMB, which the radio regenerates from the LC it sent
			h->dropped++;
		}
		return;
	}

	if ((h->state != AUDIO) || (seq == h->lastN) || (seq != (h->lastN + 1)))
	{
		h->dropped++;
		return;
	}
	h->lastN = seq;
	h->voice++;
	h->silence += silence;
}

static void hostTerminator(host_t *h)
{
	if (h->state == AUDIO)
	{
		h->ends++;
	}
	h->state = LISTENING;
}

static void hostLost(host_t *h)
{
	if (h->state == AUDIO)
	{
		h->losts++;
	}
	h->state = LISTENING;
}

// The hotspot task: a burst through the tracker to the host model, as hotspot.c does it
static hotspotUplink_t up;
static host_t host;
static uint32_t now;

static void deliver(const hotspotUplinkBurst_t *b)
{
	uint8_t fill = 0;
	uint8_t actions;

	if (hotspotUplinkLost(&up, b->ms))
	{
		hostLost(&host);
	}

	actions = hotspotUplinkBurst(&up, b, &fill);

	if (actions & HOTSPOT_UP_LOST)
	{
		hostLost(&host);
	}
	if (actions & HOTSPOT_UP_HEADER)
	{
		hostHeader(&host, hotspotUplinkLc(&up));
	}
	if (actions & HOTSPOT_UP_FILL)
	{
		for (uint8_t s = fill; s != b->seq; s = (uint8_t)((s + 1) % 6))
		{
			hostVoice(&host, s, true);
		}
	}
	if (actions & HOTSPOT_UP_VOICE)
	{
		hostVoice(&host, b->seq, false);
	}
	if (actions & HOTSPOT_UP_TERMINATOR)
	{
		hostTerminator(&host);
	}
}

static void tick(uint32_t toMs)
{
	while (now < toMs)
	{
		now++;
		if (hotspotUplinkLost(&up, now))
		{
			hostLost(&host);
		}
	}
}

static const uint8_t LC_A[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x2F, 0x52, 0x5C };// TG 9 from 3101276
static const uint8_t LC_B[9] = { 0x03, 0x10, 0x20, 0x2F, 0x52, 0x5C, 0x00, 0x27, 0x0F };// private, FID 0x10, options

static void burst(uint8_t kind, uint8_t seq, const uint8_t *lc, uint8_t flags)
{
	hotspotUplinkBurst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.seq = seq;
	b.flags = flags;
	if (lc != NULL)
	{
		memcpy(b.lc, lc, 9);
	}
	tick(now + HOTSPOT_UPLINK_BURST_MS);
	b.ms = now;
	deliver(&b);
}

static void header(const uint8_t *lc)
{
	burst(HOTSPOT_UPLINK_HEADER, 0, lc, HOTSPOT_UPLINK_FLAG_LC_VALID);
}

static void terminator(const uint8_t *lc)
{
	burst(HOTSPOT_UPLINK_TERMINATOR, 0, lc, HOTSPOT_UPLINK_FLAG_LC_VALID);
}

static void voice(uint8_t seq, const uint8_t *lc)
{
	burst(HOTSPOT_UPLINK_VOICE, seq, lc, HOTSPOT_UPLINK_FLAG_SYNC_OK | (lc ? HOTSPOT_UPLINK_FLAG_LC_VALID : 0));
}

static void superframes(int n, const uint8_t *lc)
{
	for (int i = 0; i < n * 6; i++)
	{
		voice((uint8_t)(i % 6), lc);
	}
}

static void start(void)
{
	hotspotUplinkReset(&up);
	memset(&host, 0, sizeof(host));
	now = 100000;
}

static void testEightHeaders(void)
{
	start();
	for (int i = 0; i < 8; i++)
	{
		header(LC_A);
	}
	superframes(3, LC_A);
	terminator(LC_A);
	terminator(LC_A);
	terminator(LC_A);
	tick(now + 2000);

	CHECK(host.starts == 1);
	CHECK(host.headersSeen == 8);
	CHECK(host.losts == 0);// the 480 ms of headers didn't end the call
	CHECK(host.voice == 18);
	CHECK(host.dropped == 0);
	CHECK(host.ends == 1);
	CHECK(memcmp(host.lc, LC_A, 9) == 0);
}

static void testLongHeaderRun(void)
{
	// 20 headers (1.2 s, like a data preamble length) then voice: still one call
	start();
	for (int i = 0; i < 20; i++)
	{
		header(LC_B);
	}
	superframes(1, LC_B);
	terminator(LC_B);
	CHECK(host.starts == 1);
	CHECK(host.losts == 0);
	CHECK(host.voice == 6);
	CHECK(host.ends == 1);
	CHECK(memcmp(host.lc, LC_B, 9) == 0);// FID and service options kept
}

static void testLateEntry(void)
{
	// Heard from burst C, the LC only known from burst F (embedded LC in B-E)
	start();
	voice(2, NULL);
	voice(3, NULL);
	voice(4, NULL);
	voice(5, LC_A);// header made, A-E as silence, then F
	CHECK(host.starts == 1);
	CHECK(host.voice == 6);
	CHECK(host.silence == 5);
	superframes(2, LC_A);
	terminator(LC_A);
	CHECK(host.voice == 18);
	CHECK(host.dropped == 0);
	CHECK(host.ends == 1);
	CHECK(host.losts == 0);
}

static void testLateEntryMissedA(void)
{
	// rf_late (hs_normal.py, on air 6 of 10 on 800b57e): the LC known from C, the next A missed by the HR-C6000. The
	// voice goes on from C anyway (it used to wait for the A after it, a superframe later)
	start();
	voice(1, NULL);
	voice(2, LC_A);
	voice(3, LC_A);
	voice(4, LC_A);
	voice(5, LC_A);
	tick(now + HOTSPOT_UPLINK_BURST_MS);// A missed
	voice(1, LC_A);
	voice(2, LC_A);
	terminator(LC_A);
	CHECK(host.starts == 1);
	CHECK(host.voice == 9);// silent A, B; C-F; silent A; B, C
	CHECK(host.silence == 3);
	CHECK(host.dropped == 0);
	CHECK(host.ends == 1);
}

static void testLateEntryMidSuperframe(void)
{
	// The LC known from burst C: silent A and B, then C-F and the next superframe
	start();
	voice(1, NULL);
	voice(2, LC_A);
	voice(3, LC_A);
	voice(4, LC_A);
	voice(5, LC_A);
	superframes(1, LC_A);
	CHECK(host.starts == 1);
	CHECK(host.voice == 12);
	CHECK(host.dropped == 0);
	CHECK(host.silence == 2);
}

static void testLateEntryAtA(void)
{
	start();
	voice(0, LC_A);
	voice(1, LC_A);
	CHECK(host.starts == 1);
	CHECK(host.voice == 2);
}

static void testLateEntryNoSync(void)
{
	// Noise: bursts without a good EMB don't start a call
	start();
	burst(HOTSPOT_UPLINK_VOICE, 3, LC_A, HOTSPOT_UPLINK_FLAG_LC_VALID);
	CHECK(host.starts == 0);
	CHECK(!up.open);
}

static void testLongCall(void)
{
	// 60 s
	start();
	header(LC_A);
	superframes(1000 / 6 + 1, LC_A);
	terminator(LC_A);
	CHECK(host.starts == 1);
	CHECK(host.voice == (1000 / 6 + 1) * 6);
	CHECK(host.dropped == 0);
	CHECK(host.losts == 0);
	CHECK(host.ends == 1);
}

static void testMissedBursts(void)
{
	// D of the second superframe never heard: a silent D keeps MMDVMHost's sequence (else it drops E and F)
	start();
	header(LC_A);
	superframes(1, LC_A);
	voice(0, LC_A);
	voice(1, LC_A);
	voice(2, LC_A);
	tick(now + HOTSPOT_UPLINK_BURST_MS);// D
	voice(4, LC_A);
	voice(5, LC_A);
	superframes(1, LC_A);
	terminator(LC_A);
	CHECK(host.voice == 18);
	CHECK(host.silence == 1);
	CHECK(host.dropped == 0);

	// A missed after the headers: a silent A first
	start();
	header(LC_A);
	tick(now + HOTSPOT_UPLINK_BURST_MS);// A
	voice(1, LC_A);
	voice(2, LC_A);
	CHECK(host.voice == 3);
	CHECK(host.silence == 1);
	CHECK(host.dropped == 0);

	// Bursts out of order without the time for the missing ones: not filled (MMDVMHost picks up at the next A)
	start();
	header(LC_A);
	voice(0, LC_A);
	hotspotUplinkBurst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = HOTSPOT_UPLINK_VOICE;
	b.seq = 4;
	b.flags = HOTSPOT_UPLINK_FLAG_SYNC_OK;
	b.ms = now + 5;
	deliver(&b);
	CHECK(host.silence == 0);
	CHECK(host.dropped == 1);
}

static void testDuplicate(void)
{
	start();
	header(LC_A);
	voice(0, LC_A);
	hotspotUplinkBurst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = HOTSPOT_UPLINK_VOICE;
	b.seq = 0;
	b.flags = HOTSPOT_UPLINK_FLAG_SYNC_OK;
	b.ms = now + 2;
	deliver(&b);
	CHECK(host.voice == 1);
	CHECK(host.dropped == 0);
}

static void testLost(void)
{
	start();
	header(LC_A);
	superframes(2, LC_A);
	tick(now + HOTSPOT_UPLINK_LOST_MS - 1);
	CHECK(host.losts == 0);
	tick(now + 2);
	CHECK(host.losts == 1);
	tick(now + 2000);
	CHECK(host.losts == 1);// once
	CHECK(!up.open);

	// A fade shorter than that keeps the call (the missed bursts filled)
	start();
	header(LC_A);
	superframes(1, LC_A);
	voice(0, LC_A);
	tick(now + 4 * HOTSPOT_UPLINK_BURST_MS);// B-E
	voice(5, LC_A);
	superframes(1, LC_A);
	terminator(LC_A);
	CHECK(host.losts == 0);
	CHECK(host.voice == 18);
	CHECK(host.silence == 4);

	// A call never announced (no LC heard) isn't lost upstream
	start();
	voice(1, NULL);
	tick(now + 2000);
	CHECK(host.losts == 0);
	CHECK(!up.open);
}

static void testNewCallWithoutTerminator(void)
{
	start();
	header(LC_A);
	superframes(1, LC_A);
	header(LC_B);// the first call's terminator was missed
	superframes(1, LC_B);
	terminator(LC_B);
	CHECK(host.starts == 2);
	CHECK(host.losts == 1);
	CHECK(host.ends == 1);
	CHECK(memcmp(host.lc, LC_B, 9) == 0);
}

static void testBackToBack(void)
{
	start();
	for (int call = 0; call < 5; call++)
	{
		const uint8_t *lc = (call & 1) ? LC_B : LC_A;

		header(lc);
		header(lc);
		superframes(2, lc);
		terminator(lc);
		terminator(lc);
	}
	CHECK(host.starts == 5);
	CHECK(host.ends == 5);
	CHECK(host.losts == 0);
	CHECK(host.voice == 60);
	CHECK(host.dropped == 0);
}

static void testTerminatorOnly(void)
{
	// The tail of a call heard after a gap: nothing up
	start();
	terminator(LC_A);
	CHECK(host.starts == 0);
	CHECK(host.ends == 0);
	CHECK(!up.open);
}

static void testData(void)
{
	start();
	CHECK(!hotspotUplinkData(&up, DT_RATE_12_DATA, 1000));// a block from noise, idle
	CHECK(hotspotUplinkData(&up, DT_CSBK, 1000));
	CHECK(hotspotUplinkData(&up, DT_DATA_HEADER, 1060));
	CHECK(hotspotUplinkData(&up, DT_RATE_12_DATA, 1120));
	CHECK(hotspotUplinkData(&up, DT_RATE_34_DATA, 1180));
	CHECK(hotspotUplinkData(&up, DT_RATE_1_DATA, 1240));
	CHECK(!hotspotUplinkData(&up, DT_RATE_12_DATA, 1240 + HOTSPOT_UPLINK_DATA_GAP_MS + 1));
	CHECK(!hotspotUplinkData(&up, DT_RATE_12_DATA, 1240 + HOTSPOT_UPLINK_DATA_GAP_MS + 60));
	CHECK(!hotspotUplinkData(&up, DT_IDLE, 3000));
	CHECK(hotspotUplinkData(&up, DT_MBC_HEADER, 3000));
}

static void testQueue(void)
{
	hotspotUplinkBurst_t b, out;
	int popped = 0;

	hotspotUplinkRxReset();
	memset(&b, 0, sizeof(b));
	for (int i = 0; i < HOTSPOT_UPLINK_QUEUE_SIZE + 2; i++)
	{
		b.seq = (uint8_t)i;
		hotspotUplinkRxPush(&b);
	}
	CHECK(hotspotUplinkRxOverflows() == 3);// one slot is kept free
	while (hotspotUplinkRxPop(&out))
	{
		CHECK(out.seq == popped);
		popped++;
	}
	CHECK(popped == HOTSPOT_UPLINK_QUEUE_SIZE - 1);

	b.seq = 42;
	CHECK(hotspotUplinkRxPush(&b));
	hotspotUplinkRxReset();
	CHECK(!hotspotUplinkRxPop(&out));
}

static void testLcIsVoice(void)
{
	uint8_t lc[9];

	CHECK(hotspotUplinkLcIsVoice(LC_A));
	CHECK(hotspotUplinkLcIsVoice(LC_B));
	memcpy(lc, LC_A, 9);
	lc[0] = 0x04;// talker alias header
	CHECK(!hotspotUplinkLcIsVoice(lc));
	memcpy(lc, LC_A, 9);
	lc[6] = lc[7] = lc[8] = 0;
	CHECK(!hotspotUplinkLcIsVoice(lc));
	memcpy(lc, LC_A, 9);
	lc[0] = 0x80;// PF set, group
	CHECK(hotspotUplinkLcIsVoice(lc));
}

static void testVoiceSeq(void)
{
	uint32_t last = 0;

	// On air (eb78cd3): voice comes with data types 0x9-0xE as well as 1-6; strict 1-6 took none
	CHECK(hotspotUplinkVoiceSeq(0x09, 1000, &last) == 0);
	CHECK(hotspotUplinkVoiceSeq(0x0A, 1060, &last) == 1);
	CHECK(hotspotUplinkVoiceSeq(0x03, 1120, &last) == 2);
	CHECK(hotspotUplinkVoiceSeq(0x0C, 1180, &last) == 3);
	CHECK(hotspotUplinkVoiceSeq(0x0D, 1240, &last) == 4);
	CHECK(hotspotUplinkVoiceSeq(0x0E, 1260, &last) == -1);// the EMB parsing interrupt for E, 20 ms later
	CHECK(hotspotUplinkVoiceSeq(0x06, 1300, &last) == 5);
	CHECK(hotspotUplinkVoiceSeq(0x00, 1360, &last) == -1);
	CHECK(hotspotUplinkVoiceSeq(0x07, 1360, &last) == -1);
	CHECK(hotspotUplinkVoiceSeq(0x0F, 1360, &last) == -1);
	CHECK(hotspotUplinkVoiceSeq(0x01, 1360, &last) == 0);
	CHECK(last == 1360);
	last = 0xFFFFFFF0u;// wraps
	CHECK(hotspotUplinkVoiceSeq(0x01, 0x30, &last) == 0);
}

int main(void)
{
	testVoiceSeq();
	testEightHeaders();
	testLongHeaderRun();
	testLateEntry();
	testLateEntryMidSuperframe();
	testLateEntryAtA();
	testLateEntryMissedA();
	testLateEntryNoSync();
	testLongCall();
	testMissedBursts();
	testDuplicate();
	testLost();
	testNewCallWithoutTerminator();
	testBackToBack();
	testTerminatorOnly();
	testData();
	testQueue();
	testLcIsVoice();

	printf("# hotspotUplinkTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
