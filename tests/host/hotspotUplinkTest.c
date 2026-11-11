// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host unit test for the hotspot uplink (hotspot/hotspotUplink.c): the bursts heard on RF, through the call tracker,
 * into a record of the stream the radio sends up, checked against what ETSI TS 102 361-1 says a voice call looks like
 */

#include <stdio.h>
#include <string.h>
#include "dmr/dmrData.h"
#include "hotspot/hotspotUplink.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static hotspotUplink_t up;
static uint32_t now;

// What the radio sent up, one letter per frame so a whole call reads at a glance: H header, A-F voice at that position,
// a-f silence at that position, T terminator, L lost
#define SENT_MAX	1100

static char sent[SENT_MAX + 1];
static uint8_t sentLc[SENT_MAX][HOTSPOT_UPLINK_LC_LENGTH];	// for H and T: the LC as it was when the frame went up
static int sentLen;

static void record(char what)
{
	if (sentLen == SENT_MAX)
	{
		printf("FAIL %s:%d: more than %d frames sent\n", __FILE__, __LINE__, SENT_MAX);
		failures++;
		return;
	}

	memcpy(sentLc[sentLen], hotspotUplinkLc(&up), HOTSPOT_UPLINK_LC_LENGTH);
	sent[sentLen++] = what;
	sent[sentLen] = '\0';
}

static void checkSent(int line, const char *expected)
{
	char want[SENT_MAX + 1];
	int n = 0;

	for (const char *p = expected; (*p != '\0') && (n < SENT_MAX); p++)
	{
		if (*p != ' ')
		{
			want[n++] = *p;
		}
	}
	want[n] = '\0';

	if (strcmp(want, sent) != 0)
	{
		printf("FAIL %s:%d: sent \"%s\", expected \"%s\"\n", __FILE__, line, sent, want);
		failures++;
	}
}

// The whole stream so far, spaces ignored
#define CHECK_SENT(expected) checkSent(__LINE__, (expected))

static int sentCount(char what)
{
	int n = 0;

	for (int i = 0; i < sentLen; i++)
	{
		n += (sent[i] == what);
	}

	return n;
}

// Every frame of this kind carried this LC, and there was at least one
static bool allCarry(char what, const uint8_t *lc)
{
	for (int i = 0; i < sentLen; i++)
	{
		if ((sent[i] == what) && (memcmp(sentLc[i], lc, HOTSPOT_UPLINK_LC_LENGTH) != 0))
		{
			return false;
		}
	}

	return sentCount(what) > 0;
}

// The nth (from 0) frame of this kind carried this LC
static bool nthCarries(char what, int nth, const uint8_t *lc)
{
	for (int i = 0; i < sentLen; i++)
	{
		if ((sent[i] == what) && (nth-- == 0))
		{
			return memcmp(sentLc[i], lc, HOTSPOT_UPLINK_LC_LENGTH) == 0;
		}
	}

	return false;
}

static void deliver(const hotspotUplinkBurst_t *b)
{
	uint8_t fill = 0;
	uint8_t actions;

	if (hotspotUplinkLost(&up, b->ms))
	{
		record('L');
	}

	actions = hotspotUplinkBurst(&up, b, &fill);

	if (actions & HOTSPOT_UP_LOST)
	{
		record('L');
	}
	if (actions & HOTSPOT_UP_HEADER)
	{
		record('H');
	}
	if (actions & HOTSPOT_UP_FILL)
	{
		for (uint8_t s = fill; s != b->seq; s = (uint8_t)((s + 1) % 6))
		{
			record((char)('a' + s));
		}
	}
	if (actions & HOTSPOT_UP_VOICE)
	{
		record((char)('A' + b->seq));
	}
	if (actions & HOTSPOT_UP_TERMINATOR)
	{
		record('T');
	}
}

static void tick(uint32_t toMs)
{
	while (now < toMs)
	{
		now++;
		if (hotspotUplinkLost(&up, now))
		{
			record('L');
		}
	}
}

static const uint8_t LC_A[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x98, 0x88, 0xFF };// TG 9 from 9996543
static const uint8_t LC_B[9] = { 0x03, 0x10, 0x20, 0x98, 0x88, 0xFF, 0x00, 0x27, 0x0F };// private, FID 0x10, options

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
	sentLen = 0;
	sent[0] = '\0';
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

	// Measured with a real host through a fake modem: repeated headers and terminators made one call and one end. Only
	// the first terminator goes up: the call is closed by then, and nothing follows it, not even a late lost
	CHECK_SENT("HHHHHHHH ABCDEF ABCDEF ABCDEF T");
	CHECK(allCarry('H', LC_A));
	CHECK(allCarry('T', LC_A));
	CHECK(!up.open);
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

	// Longer than the lost time from the first header to the voice: each header has to count as the call still being on
	CHECK_SENT("HHHHHHHHHHHHHHHHHHHH ABCDEF T");
	CHECK(allCarry('H', LC_B));
	CHECK(allCarry('T', LC_B));
}

static void testLateEntry(void)
{
	// Heard from burst C, the LC only known from burst F (embedded LC in B-E)
	start();
	voice(2, NULL);
	voice(3, NULL);
	voice(4, NULL);
	CHECK_SENT("");// nothing can go up without an LC to put in the header
	voice(5, LC_A);// header made, A-E as silence, then F
	superframes(2, LC_A);
	terminator(LC_A);

	// Measured with a real host through a fake modem: B-F after a header, with no A before them, counted for nothing.
	// So the made header is followed by a superframe from A, never by the burst that was heard
	CHECK_SENT("H abcde F ABCDEF ABCDEF T");
	CHECK(allCarry('H', LC_A));
	CHECK(allCarry('T', LC_A));
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

	// The voice starts at C, in the superframe the LC became known in, and the missed A is filled like any other
	CHECK_SENT("H ab CDEF aBC T");
	CHECK(allCarry('H', LC_A));
	CHECK(allCarry('T', LC_A));
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

	CHECK_SENT("H ab CDEF ABCDEF");
	CHECK(allCarry('H', LC_A));
	CHECK(hotspotUplinkCallOpen(&up));
}

static void testLateEntryAtA(void)
{
	// Joined at A: the superframe is complete, so there is nothing to fill
	start();
	voice(0, LC_A);
	voice(1, LC_A);

	CHECK_SENT("H AB");
	CHECK(allCarry('H', LC_A));
	CHECK(hotspotUplinkCallOpen(&up));
}

static void testLateEntryNoSync(void)
{
	// Noise: bursts without a good EMB don't start a call
	start();
	burst(HOTSPOT_UPLINK_VOICE, 3, LC_A, HOTSPOT_UPLINK_FLAG_LC_VALID);
	CHECK(!up.open);
	CHECK_SENT("");
}

static void testLongCall(void)
{
	// 60 s
	char expected[SENT_MAX + 1];
	int n = 0;

	start();
	header(LC_A);
	superframes(1000 / 6 + 1, LC_A);
	terminator(LC_A);

	expected[n++] = 'H';
	for (int i = 0; i < 1000 / 6 + 1; i++)
	{
		memcpy(&expected[n], "ABCDEF", 6);
		n += 6;
	}
	expected[n++] = 'T';
	expected[n] = '\0';

	// A long call never loses its place, nor closes and reopens along the way
	CHECK_SENT(expected);
	CHECK(allCarry('H', LC_A));
	CHECK(allCarry('T', LC_A));
}

static void testMissedBursts(void)
{
	// D of the second superframe never heard: a silent D keeps the host's sequence (else it drops E and F)
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

	// Measured with a real host through a fake modem: with D left out of the stream, E and F counted for nothing until
	// the next A. So exactly one silent D, in D's place
	CHECK_SENT("H ABCDEF ABC d EF ABCDEF T");
	CHECK(sentCount('d') == 1);

	// A missed after the headers: a silent A first
	start();
	header(LC_A);
	tick(now + HOTSPOT_UPLINK_BURST_MS);// A
	voice(1, LC_A);
	voice(2, LC_A);
	CHECK_SENT("H a BC");

	// Bursts out of order without the time for the missing ones: not filled (the host picks up at the next A)
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
	CHECK_SENT("H A E");
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

	// One burst heard twice goes up once, and doesn't cost the next one its place
	CHECK_SENT("H A");
	voice(1, LC_A);
	CHECK_SENT("H A B");
}

static void testLost(void)
{
	start();
	header(LC_A);
	superframes(2, LC_A);
	tick(now + HOTSPOT_UPLINK_LOST_MS - 1);
	CHECK_SENT("H ABCDEF ABCDEF");// not yet
	CHECK(hotspotUplinkCallOpen(&up));
	tick(now + 2);
	CHECK_SENT("H ABCDEF ABCDEF L");
	tick(now + 2000);

	// Measured with a real host through a fake modem: a call left without an end stayed open for seconds, so the radio
	// has to say so itself, and once
	CHECK_SENT("H ABCDEF ABCDEF L");
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
	CHECK_SENT("H ABCDEF A bcde F ABCDEF T");

	// A call never announced (no LC heard) isn't lost upstream
	start();
	voice(1, NULL);
	tick(now + 2000);
	CHECK(!up.open);
	CHECK_SENT("");
}

static void testNewCallWithoutTerminator(void)
{
	start();
	header(LC_A);
	superframes(1, LC_A);
	header(LC_B);// the first call's terminator was missed
	superframes(1, LC_B);
	terminator(LC_B);

	// Measured with a real host through a fake modem: a new call's header with the old call still open was taken as
	// more of the old call. So the old one is closed first, and each header carries its own call's LC
	CHECK_SENT("H ABCDEF L H ABCDEF T");
	CHECK(nthCarries('H', 0, LC_A));
	CHECK(nthCarries('H', 1, LC_B));
	CHECK(allCarry('T', LC_B));
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

	// Each call closed by its own first terminator, none lost, and the next one starts clean at its header
	CHECK_SENT("HH ABCDEF ABCDEF T  HH ABCDEF ABCDEF T  HH ABCDEF ABCDEF T  HH ABCDEF ABCDEF T  HH ABCDEF ABCDEF T");
	for (int call = 0; call < 5; call++)
	{
		const uint8_t *lc = (call & 1) ? LC_B : LC_A;

		CHECK(nthCarries('H', call * 2, lc));
		CHECK(nthCarries('H', call * 2 + 1, lc));
		CHECK(nthCarries('T', call, lc));
	}
}

static void testTerminatorOnly(void)
{
	// The tail of a call heard after a gap: nothing up
	start();
	terminator(LC_A);
	CHECK(!up.open);
	CHECK_SENT("");
}

static void testData(void)
{
	start();
	CHECK(!hotspotUplinkData(&up, DMR_TYPE_RATE_1_2, 1000));// a block from noise, idle
	CHECK(hotspotUplinkData(&up, DMR_TYPE_CSBK, 1000));
	CHECK(hotspotUplinkData(&up, DMR_TYPE_DATA_HEADER, 1060));
	CHECK(hotspotUplinkData(&up, DMR_TYPE_RATE_1_2, 1120));
	CHECK(hotspotUplinkData(&up, DMR_TYPE_RATE_3_4, 1180));
	CHECK(hotspotUplinkData(&up, DMR_TYPE_RATE_1, 1240));
	CHECK(!hotspotUplinkData(&up, DMR_TYPE_RATE_1_2, 1240 + HOTSPOT_UPLINK_DATA_GAP_MS + 1));
	CHECK(!hotspotUplinkData(&up, DMR_TYPE_RATE_1_2, 1240 + HOTSPOT_UPLINK_DATA_GAP_MS + 60));
	CHECK(!hotspotUplinkData(&up, DMR_TYPE_IDLE, 3000));
	CHECK(hotspotUplinkData(&up, DMR_TYPE_MBC_HEADER, 3000));
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
