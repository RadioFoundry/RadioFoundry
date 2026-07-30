// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Host tests for the single frequency repeater's generic logic (functions/sfr.c)
 *
 *   make -C tests/host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/sfr.h"
#include "dmr/dmrData.h"
#include "dmr/fec/dmrFec.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t LC1[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x23, 0x45, 0x67 };// group call to TG 9 from 0x234567
static const uint8_t LC2[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B, 0x76, 0x54, 0x32 };

static sfrBurst_t voice(int seq, uint8_t fill)
{
	sfrBurst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = SFR_BURST_VOICE;
	b.type = seq;
	b.length = SFR_VOICE_LENGTH;
	memset(b.payload, fill, SFR_VOICE_LENGTH);
	return b;
}

static sfrBurst_t data(uint8_t dataType, const uint8_t *lc)
{
	sfrBurst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = SFR_BURST_DATA;
	b.type = dataType;
	b.length = 12;
	if (lc != NULL)
	{
		memcpy(b.payload, lc, 9);
	}
	else
	{
		b.flags = SFR_BURST_FLAG_MCU_CRC;
		memset(b.payload, dataType, 12);
	}
	return b;
}

static sfrConfig_t repeaterConfig(uint8_t role, uint8_t framing, uint8_t inboundSlot)
{
	sfrChannelSettings_t s = { .role = role, .framing = framing, .inboundSlot = inboundSlot, .hangIndex = 0 };
	sfrConfig_t cfg;

	sfrConfigBuild(&s, 1, 0, &cfg);
	return cfg;
}

static void testChannelEncoding(void)
{
	sfrChannelSettings_t s, d;
	uint8_t flag1 = 0x85, a = 0xAA, b = 0x55;// other LibreDMR_flag1 bits set; garbage in the spare bytes

	// Without the flag, whatever the spare bytes hold (0x00 or 0xFF from a CPS), the channel isn't an SFR one
	CHECK(!sfrChannelDecode(0x00, 0xFF, 0xFF, &d) && (d.role == SFR_ROLE_OFF), "spare bytes alone mean nothing");
	CHECK(!sfrChannelDecode(0xFD, 0xFF, 0xFF, &d), "the other flag1 bits don't enable it");

	s = (sfrChannelSettings_t){ .role = SFR_ROLE_RPT_MOTOROLA, .framing = SFR_FRAMING_TS, .inboundSlot = 1, .hangIndex = 2,
		.outboundCCSet = true, .outboundCC = 12 };
	sfrChannelEncode(&s, &flag1, &a, &b);
	CHECK(flag1 == (0x85 | SFR_CHANNEL_FLAG1_ENABLED), "only the SFR bit of flag1 changes (%02X)", flag1);
	CHECK(sfrChannelDecode(flag1, a, b, &d), "decodes");
	CHECK(memcmp(&s, &d, sizeof(s)) == 0, "round trip");

	s = (sfrChannelSettings_t){ .role = SFR_ROLE_OFF };
	sfrChannelEncode(&s, &flag1, &a, &b);
	CHECK((flag1 == 0x85) && (a == 0) && (b == 0), "off clears it all");

	// A role this firmware never writes
	CHECK(!sfrChannelDecode(SFR_CHANNEL_FLAG1_ENABLED, 0x07, 0, &d) && (d.role == SFR_ROLE_OFF), "unknown role is off");

	for (int role = SFR_ROLE_USER_STANDARD; role < SFR_ROLE_NUM_STORED; role++)
	{
		for (int f = 0; f < SFR_FRAMING_NUM; f++)
		{
			for (int h = 0; h < SFR_HANG_NUM; h++)
			{
				s = (sfrChannelSettings_t){ .role = role, .framing = f, .inboundSlot = (h & 1), .hangIndex = h, .outboundCCSet = (f & 1), .outboundCC = (f & 1) ? (role + h) : 0 };
				flag1 = 0;
				sfrChannelEncode(&s, &flag1, &a, &b);
				CHECK(sfrChannelDecode(flag1, a, b, &d) && (memcmp(&s, &d, sizeof(s)) == 0), "round trip %d %d %d", role, f, h);
			}
		}
	}
}

static void testConfig(void)
{
	sfrChannelSettings_t s = { .role = SFR_ROLE_RPT_STANDARD, .framing = SFR_FRAMING_AUTO, .outboundCCSet = true, .outboundCC = 7 };
	sfrConfig_t cfg;

	sfrConfigBuild(&s, 3, 0, &cfg);
	CHECK(cfg.framing == SFR_FRAMING_TS, "standard: TS by default");
	CHECK((cfg.inboundCC == 3) && (cfg.outboundCC == 3), "standard: one colour code (%d %d)", cfg.inboundCC, cfg.outboundCC);
	CHECK(cfg.totMs == SFR_TOT_DEFAULT_MS, "no channel TOT: the default");
	CHECK(cfg.hangMs == 1000, "hang default 1 s");

	s.role = SFR_ROLE_RPT_MOTOROLA;
	sfrConfigBuild(&s, 3, 4, &cfg);
	CHECK(cfg.framing == SFR_FRAMING_BS, "Motorola: BS by default");
	CHECK((cfg.inboundCC == 3) && (cfg.outboundCC == 7), "Motorola: its outbound colour code");
	CHECK(cfg.totMs == 60000, "channel TOT 4 x 15 s");

	s.outboundCCSet = false;
	sfrConfigBuild(&s, 3, 0, &cfg);
	CHECK(cfg.outboundCC == 3, "Motorola without one: the channel's");

	CHECK(sfrResolveFraming(SFR_ROLE_HOTSPOT, SFR_FRAMING_AUTO) == SFR_FRAMING_TS, "hotspot: TS by default");
	CHECK(sfrResolveFraming(SFR_ROLE_HOTSPOT, SFR_FRAMING_BS) == SFR_FRAMING_BS, "an explicit framing wins");
	CHECK((sfrHangMs(0) == 1000) && (sfrHangMs(1) == 2000) && (sfrHangMs(2) == 4000) && (sfrHangMs(3) == 0), "hang table");
}

static void testSync(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_RPT_STANDARD, SFR_FRAMING_MS, 0);

	CHECK(sfrOutboundSync(&cfg) == SFR_SYNC_MS, "MS framing");
	cfg.framing = SFR_FRAMING_TS;
	CHECK(sfrOutboundSync(&cfg) == SFR_SYNC_TS2, "TS framing, inbound TS1: TS2 sync out");
	cfg.inboundSlot = 1;
	CHECK(sfrOutboundSync(&cfg) == SFR_SYNC_TS1, "TS framing, inbound TS2: TS1 sync out");
	cfg.framing = SFR_FRAMING_BS;
	CHECK(sfrOutboundSync(&cfg) == SFR_SYNC_BS, "BS framing");

	cfg.inboundSlot = 0;
	CHECK(sfrInboundSyncAccepted(&cfg, SFR_SYNC_MS), "MS in");
	CHECK(sfrInboundSyncAccepted(&cfg, SFR_SYNC_TS1), "inbound slot's DM sync in");
	CHECK(!sfrInboundSyncAccepted(&cfg, SFR_SYNC_TS2), "outbound slot's DM sync (an echo) not repeated");
	CHECK(!sfrInboundSyncAccepted(&cfg, SFR_SYNC_BS), "a repeater's not repeated");
}

static void testDedup(void)
{
	sfrDedup_t d;
	uint32_t t = 1000;
	int played = 0;

	sfrDedupReset(&d);

	// The direct copy of bursts 0-5, with the repeat 90 ms later: D0 R? D1 R0 D2 R1 ...
	for (int n = 0; n < 12; n++)
	{
		int seq = n % 6;

		played += sfrDedupVoice(&d, seq, t) ? 1 : 0;// direct
		if (n > 0)
		{
			CHECK(!sfrDedupVoice(&d, (seq + 5) % 6, t + 30), "repeat of %d dropped", (seq + 5) % 6);
		}
		t += 60;
	}
	CHECK(played == 12, "every direct burst played (%d)", played);

	// The direct copy fades at burst 3: the repeats carry on, from the first one after the last played
	sfrDedupReset(&d);
	t = 5000;
	CHECK(sfrDedupVoice(&d, 0, t), "D0");
	CHECK(sfrDedupVoice(&d, 1, t + 60), "D1");
	CHECK(!sfrDedupVoice(&d, 0, t + 90), "R0 dropped");
	CHECK(sfrDedupVoice(&d, 2, t + 120), "D2");
	CHECK(!sfrDedupVoice(&d, 1, t + 150), "R1 dropped");
	CHECK(!sfrDedupVoice(&d, 2, t + 210), "R2 dropped");
	CHECK(sfrDedupVoice(&d, 3, t + 270), "R3 played: D3 lost");
	CHECK(sfrDedupVoice(&d, 4, t + 330), "R4");
	CHECK(sfrDedupVoice(&d, 5, t + 390), "R5");
	CHECK(sfrDedupVoice(&d, 0, t + 450), "R0 of the next superframe");

	// A gap starts over
	CHECK(sfrDedupVoice(&d, 0, t + 450 + SFR_DEDUP_RESET_MS + 1), "after a gap anything goes");
}

static void testEngineRepeat(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_RPT_STANDARD, SFR_FRAMING_MS, 0);
	sfrEngine_t e;
	sfrBurst_t b, out;
	uint32_t t = 10000;

	sfrEngineInit(&e, &cfg);
	CHECK(sfrEngineOutboundSlot(&e, t, &out) == SFR_TX_NONE, "idle");
	CHECK(sfrEngineSpace(&e) == SFR_QUEUE_SIZE, "empty");

	b = data(DT_VOICE_LC_HEADER, LC1);
	CHECK(sfrEngineRfBurst(&e, &b, 2, SFR_SYNC_MS, t) == SFR_RX_IGNORED, "other colour code ignored");
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_BS, t) == SFR_RX_IGNORED, "BS sync ignored");
	b = data(DT_TERMINATOR_WITH_LC, LC1);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t) == SFR_RX_IGNORED, "a terminator alone isn't a call");
	CHECK(!sfrEngineRfBusy(&e), "not busy");

	b = data(DT_VOICE_LC_HEADER, LC1);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t) == SFR_RX_REPEATED, "header repeated");
	CHECK(sfrEngineRfBusy(&e), "busy from the first burst");
	CHECK(sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST, "sent");
	CHECK((out.kind == SFR_BURST_DATA) && (out.type == DT_VOICE_LC_HEADER) && (memcmp(out.payload, LC1, 9) == 0), "the header");
	CHECK(sfrEngineKeyed(&e), "keyed");

	t += 60;
	for (int n = 0; n < 6; n++, t += 60)
	{
		b = voice(n, 0x10 + n);
		CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t) == SFR_RX_REPEATED, "voice %d", n);
		CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 1) == SFR_RX_DUPLICATE, "voice %d twice", n);
		CHECK(sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST, "voice %d sent", n);
		CHECK((out.kind == SFR_BURST_VOICE) && (out.type == n) && (out.payload[0] == 0x10 + n), "voice %d is it", n);
		CHECK(memcmp(sfrEngineTxLc(&e), LC1, 9) == 0, "with the call's LC");
	}

	// A burst lost on the inbound slot: an idle burst fills the gap
	CHECK(sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_IDLE, "gap filled");
	t += 60;

	// CSBK / data go out as received
	b = data(DT_CSBK, NULL);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t) == SFR_RX_REPEATED, "CSBK");
	CHECK((sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST) && (out.type == DT_CSBK) && (out.flags & SFR_BURST_FLAG_MCU_CRC), "CSBK verbatim");
	t += 60;

	b = data(DT_TERMINATOR_WITH_LC, LC1);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t) == SFR_RX_REPEATED, "terminator");
	CHECK(sfrEngineRfBusy(&e), "busy until the terminator is repeated");
	CHECK((sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST) && (out.type == DT_TERMINATOR_WITH_LC), "terminator sent");
	CHECK(!sfrEngineRfBusy(&e), "not busy once it's repeated");

	// Hang: idle bursts for cfg.hangMs after the last traffic, then the end
	CHECK(sfrEngineOutboundSlot(&e, t + 90, &out) == SFR_TX_IDLE, "hang");
	CHECK(sfrEngineOutboundSlot(&e, t + 30 + 990, &out) == SFR_TX_IDLE, "still hang");
	CHECK(sfrEngineOutboundSlot(&e, t + 30 + 1010, &out) == SFR_TX_END, "hang over");
	CHECK(!sfrEngineKeyed(&e) && (sfrEngineOutboundSlot(&e, t + 1100, &out) == SFR_TX_NONE), "unkeyed");

	// Extra terminators after the call aren't repeated, and don't make it busy again
	b = data(DT_TERMINATOR_WITH_LC, LC1);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 60) == SFR_RX_IGNORED, "extra terminator");
}

static void testEngineLossAndTot(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_RPT_STANDARD, SFR_FRAMING_MS, 0);
	sfrEngine_t e;
	sfrBurst_t b, out;
	uint32_t t = 0;

	// An inbound call lost without its terminator: busy ends once what was heard is sent and the loss timeout passes
	sfrEngineInit(&e, &cfg);
	b = voice(0, 1);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t);
	CHECK(sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST, "sent");
	CHECK(sfrEngineOutboundSlot(&e, t + 90, &out) == SFR_TX_IDLE, "waiting for more");
	CHECK(sfrEngineRfBusy(&e), "still busy");
	CHECK(sfrEngineOutboundSlot(&e, t + SFR_RF_LOSS_MS + 30, &out) == SFR_TX_IDLE, "lost: hang");
	CHECK(!sfrEngineRfBusy(&e), "not busy after the loss");

	// Late entry: the LC from the embedded signalling
	sfrEngineInit(&e, &cfg);
	b = voice(3, 1);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t);
	sfrEngineRfLc(&e, LC2);// (learned from the call's voice)
	sfrEngineOutboundSlot(&e, t + 30, &out);
	CHECK(memcmp(sfrEngineTxLc(&e), LC2, 9) == 0, "late entry LC");

	// TOT: an inbound call longer than it isn't repeated any more, and the repeater stops after its hang
	cfg.totMs = 1000;
	sfrEngineInit(&e, &cfg);
	t = 0;
	int repeated = 0;
	sfrTxAction_t last = SFR_TX_NONE;
	for (int n = 0; n < 50; n++, t += 60)
	{
		b = voice(n % 6, 1);
		if (sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t) == SFR_RX_REPEATED)
		{
			repeated++;
		}
		last = sfrEngineOutboundSlot(&e, t + 30, &out);
	}
	CHECK((repeated >= 16) && (repeated <= 18), "repeated for the TOT only (%d)", repeated);
	CHECK(last == SFR_TX_NONE, "unkeyed after its hang (%d)", last);
	CHECK(sfrEngineRfBusy(&e), "still busy: the radio is still talking");
	sfrEngineOutboundSlot(&e, t + SFR_RF_LOSS_MS + 100, &out);
	CHECK(!sfrEngineRfBusy(&e), "the call ended");
	b = voice(0, 1);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 1000) == SFR_RX_REPEATED, "the next call is repeated");
}

static void testEngineNetwork(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_HOTSPOT, SFR_FRAMING_AUTO, 0);
	sfrEngine_t e;
	sfrBurst_t b, out;
	uint32_t t = 0;

	sfrEngineInit(&e, &cfg);
	CHECK(cfg.framing == SFR_FRAMING_TS, "hotspot TS");

	// A network stream goes out straight away
	b = data(DT_VOICE_LC_HEADER, LC2);
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t) == 0, "header");
	b = voice(0, 0x55);
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t + 1) == 0, "voice");
	CHECK(sfrEngineSpace(&e) == SFR_QUEUE_SIZE - 2, "space");
	CHECK((sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST) && (out.flags & SFR_BURST_FLAG_NET), "net header out");
	CHECK((sfrEngineOutboundSlot(&e, t + 90, &out) == SFR_TX_BURST) && (memcmp(sfrEngineTxLc(&e), LC2, 9) == 0), "net voice with its LC");

	// An RF call cuts in: what the network queued is dropped, the rest of its stream refused
	b = voice(1, 0x55);
	sfrEngineNetBurst(&e, &b, NULL, t + 60);
	b = voice(2, 0x55);
	sfrEngineNetBurst(&e, &b, NULL, t + 120);
	b = data(DT_VOICE_LC_HEADER, LC1);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 130) == SFR_RX_REPEATED, "RF call");
	CHECK(sfrEngineSpace(&e) == SFR_QUEUE_SIZE - 1, "net bursts dropped, the RF one queued");
	b = voice(3, 0x55);
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t + 180) == SFR_MMDVM_NAK_RF_BUSY, "RF busy");
	CHECK((sfrEngineOutboundSlot(&e, t + 190, &out) == SFR_TX_BURST) && !(out.flags & SFR_BURST_FLAG_NET), "RF first");

	// Its terminator queued: network bursts can queue behind it; the busy flag stays until it's sent
	b = data(DT_TERMINATOR_WITH_LC, LC1);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 250);
	b = voice(4, 0x55);
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t + 260) == SFR_MMDVM_NAK_RF_BUSY, "the rest of the cut stream is still refused");
	b = data(DT_VOICE_LC_HEADER, LC2);
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t + 270) == 0, "a new stream queues behind the RF terminator");
	CHECK(sfrEngineRfBusy(&e), "busy flag until the terminator has gone out");
	CHECK((sfrEngineOutboundSlot(&e, t + 280, &out) == SFR_TX_BURST) && (out.type == DT_TERMINATOR_WITH_LC), "RF terminator");
	CHECK(!sfrEngineRfBusy(&e), "not busy");
	CHECK((sfrEngineOutboundSlot(&e, t + 340, &out) == SFR_TX_BURST) && (out.flags & SFR_BURST_FLAG_NET), "then the network");

	// DMR_ABORT drops what the network has queued
	b = voice(0, 0x55);
	sfrEngineNetBurst(&e, &b, NULL, t + 350);
	sfrEngineNetBurst(&e, &b, NULL, t + 351);
	sfrEngineNetAbort(&e);
	CHECK(sfrEngineSpace(&e) == SFR_QUEUE_SIZE, "aborted");

	// A cut stream that just stops: after a gap the next one goes out
	sfrEngineInit(&e, &cfg);
	t = 100000;
	b = voice(0, 1);
	sfrEngineNetBurst(&e, &b, NULL, t);
	b = data(DT_VOICE_LC_HEADER, LC1);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 10);
	b = data(DT_TERMINATOR_WITH_LC, LC1);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 70);
	sfrEngineOutboundSlot(&e, t + 80, &out);
	sfrEngineOutboundSlot(&e, t + 140, &out);
	b = voice(1, 1);
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t + 200) == SFR_MMDVM_NAK_RF_BUSY, "cut stream refused");
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t + 200 + SFR_NET_LOSS_MS + 1) == 0, "after a gap: a new stream");

	// Full
	sfrEngineInit(&e, &cfg);
	b = voice(0, 1);
	for (int n = 0; n < SFR_QUEUE_SIZE; n++)
	{
		CHECK(sfrEngineNetBurst(&e, &b, NULL, n) == 0, "fill %d", n);
	}
	CHECK(sfrEngineNetBurst(&e, &b, NULL, 100) == SFR_MMDVM_NAK_FULL, "full");
	CHECK(sfrEngineSpace(&e) == 0, "no space");
}

static void testMmdvm(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_HOTSPOT, SFR_FRAMING_AUTO, 0);
	char buf[40];

	CHECK(sfrMmdvmTrigger(false, 430000000, 430000000), "duplex, RX = TX");
	CHECK(!sfrMmdvmTrigger(true, 430000000, 430000000), "simplex");
	CHECK(!sfrMmdvmTrigger(false, 430000000, 435000000), "duplex, two frequencies: as today");
	CHECK(!sfrMmdvmTrigger(false, 0, 0), "no frequency yet");

	sfrMmdvmVersionSuffix(&cfg, buf, sizeof(buf));
	CHECK(strcmp(buf, " SFR TS1>TS2 TS") == 0, "suffix '%s'", buf);
	cfg.inboundSlot = 1;
	cfg.framing = SFR_FRAMING_BS;
	sfrMmdvmVersionSuffix(&cfg, buf, sizeof(buf));
	CHECK(strcmp(buf, " SFR TS2>TS1 BS") == 0, "suffix '%s'", buf);
	cfg.framing = SFR_FRAMING_MS;
	sfrMmdvmVersionSuffix(&cfg, buf, sizeof(buf));
	CHECK(strcmp(buf, " SFR TS2>TS1 MS") == 0, "suffix '%s'", buf);
}

// The idle message's info bits, BPTC(196,96) encoded, decode back to themselves (and the table read is ETSI's: its
// first rows are TS 102 361-1 figure D.1's)
static void testIdle(void)
{
	uint8_t frame[33] = { 0 }, back[12];

	dmrFecBptcEncode(SFR_IDLE_INFO, frame);
	dmrFecBptcDecode(frame, back);
	CHECK(memcmp(back, SFR_IDLE_INFO, 12) == 0, "BPTC round trip");
	CHECK((SFR_IDLE_INFO[0] == 0xFF) && (SFR_IDLE_INFO[1] == 0x83), "D.1 rows 0-1: I(95..80) = FF 83");
}

static void testUplink(void)
{
	sfrUplink_t u;
	sfrBurst_t h = data(DT_VOICE_LC_HEADER, LC1), t = data(DT_TERMINATOR_WITH_LC, LC1), c = data(DT_CSBK, NULL);
	sfrBurst_t v;
	uint32_t now = 1000;

	sfrUplinkReset(&u);

	// Every header goes up, then voice, then the terminator closes the call
	for (int n = 0; n < 8; n++, now += 60)
	{
		CHECK(sfrUplinkBurst(&u, &h, true, now) == SFR_UP_SEND, "header %d", n);
	}
	for (int n = 0; n < 6; n++, now += 60)
	{
		v = voice(n, 0);
		CHECK(sfrUplinkBurst(&u, &v, true, now) == SFR_UP_SEND, "voice %d", n);
	}
	for (int n = 0; n < 3; n++, now += 60)
	{
		CHECK(sfrUplinkBurst(&u, &t, true, now) == SFR_UP_SEND, "terminator %d", n);
	}
	CHECK(!sfrUplinkLost(&u, now + 1000), "closed by its terminator: no DMR_LOST");

	// Late entry: no LC yet, then a header made from it
	v = voice(2, 0);
	CHECK(sfrUplinkBurst(&u, &v, false, now) == 0, "voice without an LC waits");
	v = voice(3, 0);
	CHECK(sfrUplinkBurst(&u, &v, true, now + 60) == (SFR_UP_SEND | SFR_UP_HEADER_FIRST), "header made first");
	v = voice(4, 0);
	CHECK(sfrUplinkBurst(&u, &v, true, now + 120) == SFR_UP_SEND, "then voice");
	CHECK(!sfrUplinkLost(&u, now + 120 + SFR_UPLINK_LOST_MS), "not lost yet");
	CHECK(sfrUplinkLost(&u, now + 121 + SFR_UPLINK_LOST_MS), "lost");
	CHECK(!sfrUplinkLost(&u, now + 2000), "once");

	// Data needs no header
	CHECK(sfrUplinkBurst(&u, &c, false, now + 3000) == SFR_UP_SEND, "CSBK");
}

// RS(12,9) over GF(2^8) (ETSI TS 102 361-1 B.3.6: field polynomial x^8 + x^4 + x^3 + x^2 + 1, generator roots
// alpha^1..alpha^3), written plainly from the definition as an independent reference. par[2] is the first parity octet.
static uint8_t gfMul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	while (b)
	{
		if (b & 1)
		{
			p ^= a;
		}
		a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1D : 0));
		b >>= 1;
	}
	return p;
}

static void refRs129(const uint8_t *in, uint8_t *par)
{
	uint8_t g[4] = { 1, 0, 0, 0 };// g(x) = (x + a)(x + a^2)(x + a^3), g[i] the x^i coefficient
	uint8_t root = 1;

	for (int r = 1; r <= 3; r++)
	{
		root = gfMul(root, 2);
		for (int i = r; i > 0; i--)
		{
			g[i] = g[i - 1] ^ gfMul(g[i], root);
		}
		g[0] = gfMul(g[0], root);
	}

	// The remainder of in(x) * x^3 / g(x), by long division, in[0] the highest power
	uint8_t rem[12] = { 0 };
	memcpy(rem, in, 9);
	for (int i = 0; i < 9; i++)
	{
		uint8_t q = rem[i];

		for (int j = 0; j <= 3; j++)
		{
			rem[i + j] ^= gfMul(q, g[3 - j]);
		}
	}
	par[2] = rem[9];
	par[1] = rem[10];
	par[0] = rem[11];
	par[3] = 0;
}

static void testCrc(void)
{
	uint8_t lc[12] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x23, 0xCA, 0xCE };// TG 9 from 2345678
	uint8_t csbk[10] = { 0xBD, 0x00, 0x80, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01 };
	uint8_t par[4];
	dmrBurst_t built;

	// dmrData's CSBK builder (CRC-CCITT, mask A5A5) is accepted, a flipped bit isn't
	CHECK(dmrDataBuildCSBK(csbk, &built) > 0, "CSBK built");
	CHECK(sfrDataBurstCrcOk(DT_CSBK, built.payload), "CSBK CRC");
	built.payload[3] ^= 0x10;
	CHECK(!sfrDataBurstCrcOk(DT_CSBK, built.payload), "CSBK bit error");
	built.payload[3] ^= 0x10;
	CHECK(!sfrDataBurstCrcOk(DT_DATA_HEADER, built.payload), "the CSBK's mask isn't a data header's");

	for (int n = 0; n < 200; n++)
	{
		for (int i = 0; i < 9; i++)
		{
			lc[i] = (uint8_t)rand();
		}

		refRs129(lc, par);
		lc[9] = par[2] ^ 0x96;
		lc[10] = par[1] ^ 0x96;
		lc[11] = par[0] ^ 0x96;
		CHECK(sfrDataBurstCrcOk(DT_VOICE_LC_HEADER, lc), "header RS %d", n);
		CHECK(!sfrDataBurstCrcOk(DT_TERMINATOR_WITH_LC, lc), "a header isn't a terminator %d", n);
		lc[9] = par[2] ^ 0x99;
		lc[10] = par[1] ^ 0x99;
		lc[11] = par[0] ^ 0x99;
		CHECK(sfrDataBurstCrcOk(DT_TERMINATOR_WITH_LC, lc), "terminator RS %d", n);
		lc[9] = par[2];
		lc[10] = par[1];
		lc[11] = par[0];
		CHECK(sfrDataBurstCrcOk(DT_TERMINATOR_WITH_LC, lc) && sfrDataBurstCrcOk(DT_VOICE_LC_HEADER, lc), "unmasked RS %d", n);
		lc[n % 9] ^= (uint8_t)(1 << (n % 8));
		CHECK(!sfrDataBurstCrcOk(DT_TERMINATOR_WITH_LC, lc), "terminator bit error %d", n);
	}

	CHECK(sfrDataBurstCrcOk(DT_RATE_12_DATA, lc), "blocks: no check");
}


static void putMiddle(uint8_t *frame, uint64_t v)
{
	frame[13] = (frame[13] & 0xF0) | (uint8_t)((v >> 44) & 0x0F);
	for (int i = 14; i <= 18; i++)
	{
		frame[i] = (uint8_t)(v >> (36 - ((i - 14) * 8)));
	}
	frame[19] = (frame[19] & 0x0F) | (uint8_t)((v & 0x0F) << 4);
}

static void testRaw(void)
{
	uint8_t frame[33], info[12], dt = 0, sync = 0, ambe[27], in[12] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xD7, 0x98, 0x7E, 0x9D };
	uint8_t par[4];

	// A terminator for TG 215 from 9990301, CC 2: BPTC, slot type, MS data sync, as the hotspot builds them
	refRs129(in, par);
	in[9] = par[2] ^ 0x99;
	in[10] = par[1] ^ 0x99;
	in[11] = par[0] ^ 0x99;
	memset(frame, 0, sizeof(frame));
	dmrFecBptcEncode(in, frame);
	dmrFecSlotTypeEncode(2, DT_TERMINATOR_WITH_LC, frame);
	putMiddle(frame, 0xD5D7F77FD757ULL);
	CHECK(sfrRawClassify(frame, 2, &dt, info, &sync) == SFR_RAW_DATA, "terminator");
	CHECK((dt == DT_TERMINATOR_WITH_LC) && (sync == SFR_SYNC_MS) && (memcmp(info, in, 12) == 0), "its type, sync and info");
	CHECK(sfrRawClassify(frame, 3, &dt, info, &sync) == SFR_RAW_NONE, "another colour code");

	frame[19] ^= 0x60;// 2 sync bit errors: still found
	CHECK(sfrRawClassify(frame, 2, &dt, info, &sync) == SFR_RAW_DATA, "sync errors");
	// Any one bit flipped in the BPTC part either changes nothing it carries (a parity bit) or fails the RS (no FEC here)
	int changed = 0;
	for (int n = 0; n < 98; n++)
	{
		uint8_t f2[33];

		memcpy(f2, frame, 33);
		f2[n >> 3] ^= (uint8_t)(0x80 >> (n & 7));
		uint8_t k = sfrRawClassify(f2, 2, &dt, info, &sync);
		if (k == SFR_RAW_NONE)
		{
			changed++;
		}
		else
		{
			CHECK(memcmp(info, in, 12) == 0, "bit %d: info changed but accepted", n);
		}
	}
	CHECK(changed >= 40, "info bit errors caught (%d)", changed);

	// Voice: TS1 voice sync, then an EMB with CC 2
	for (int i = 0; i < 33; i++)
	{
		frame[i] = (uint8_t)(i * 7);
	}
	putMiddle(frame, 0x5D577F7757FFULL);
	CHECK((sfrRawClassify(frame, 2, &dt, info, &sync) == SFR_RAW_VOICE_A) && (sync == SFR_SYNC_TS1), "voice A, TS1");
	sfrRawAmbe(frame, ambe);
	CHECK((ambe[0] == 0) && (ambe[12] == 84) && (ambe[13] == ((frame[13] & 0xF0) | (frame[19] & 0x0F))) && (ambe[14] == frame[20]) && (ambe[26] == frame[32]), "AMBE");
	putMiddle(frame, 0x2123456789ABULL);
	CHECK(sfrRawClassify(frame, 2, &dt, info, &sync) == SFR_RAW_VOICE_EMB, "EMB CC 2");
	CHECK(sfrRawClassify(frame, 5, &dt, info, &sync) == SFR_RAW_NONE, "EMB CC not ours");
}

static void testLcGuess(void)
{
	// On air (test 9): the HAT's call TG 215 from 9990301, a terminator heard while keyed, data type read as 0
	static const uint8_t LC[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xD7, 0x98, 0x7E, 0x9D };
	static const uint8_t BAD[12] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xC3, 0xD0, 0x70, 0xDD, 0x93, 0x86, 0x13 };
	static const uint8_t NOT_OURS[12] = { 0x00, 0x00, 0x20, 0x00, 0x00, 0xC3, 0xD0, 0x70, 0xDD, 0x93, 0x86, 0x13 };
	uint8_t none[9] = { 0 }, good[12], par[4];

	CHECK(sfrLcBurstGuess(DT_VOICE_PI_HEADER, BAD, LC, true) == DT_TERMINATOR_WITH_LC, "type 0, after voice: terminator");
	CHECK(sfrLcBurstGuess(DT_VOICE_PI_HEADER, BAD, LC, false) == DT_VOICE_LC_HEADER, "type 0, no voice yet: header");
	CHECK(sfrLcBurstGuess(DT_TERMINATOR_WITH_LC, BAD, LC, true) == DT_TERMINATOR_WITH_LC, "terminator, bad bytes");
	CHECK(sfrLcBurstGuess(DT_VOICE_LC_HEADER, BAD, LC, false) == DT_VOICE_LC_HEADER, "header, bad bytes");
	CHECK(sfrLcBurstGuess(DT_CSBK, BAD, LC, true) == 0xFF, "not a header / terminator type");
	CHECK(sfrLcBurstGuess(DT_VOICE_PI_HEADER, NOT_OURS, LC, true) == 0xFF, "first bytes not the LC's");
	CHECK(sfrLcBurstGuess(DT_VOICE_PI_HEADER, BAD, none, true) == 0xFF, "no LC known");

	memcpy(good, LC, 9);
	refRs129(good, par);
	good[9] = par[2] ^ 0x96;
	good[10] = par[1] ^ 0x96;
	good[11] = par[0] ^ 0x96;
	CHECK(sfrLcBurstGuess(DT_VOICE_LC_HEADER, good, LC, false) == 0xFF, "a good header is used as it is");
}

// A data call (CSBK x2, data header, 4 blocks) straight followed by a voice call (8 headers, A-F, 3 terminators), as on
// air: all of it repeated, in order, and the voice call's terminator ends it
static void testDataThenVoice(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_HOTSPOT, SFR_FRAMING_AUTO, 0);
	sfrEngine_t e;
	sfrBurst_t b, out;
	uint32_t t = 50000;
	int repeated = 0, sent = 0;
	uint8_t types[40];

	sfrEngineInit(&e, &cfg);

#define HEAR(burst) do { if (sfrEngineRfBurst(&e, &(burst), 1, SFR_SYNC_MS, t) == SFR_RX_REPEATED) repeated++; \
		if ((sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST) && (sent < 40)) types[sent++] = (out.kind == SFR_BURST_VOICE) ? (0x20 | out.type) : out.type; \
		t += 60; } while (0)

	b = data(DT_CSBK, NULL);
	HEAR(b);
	HEAR(b);
	b = data(DT_DATA_HEADER, NULL);
	HEAR(b);
	for (int n = 0; n < 4; n++)
	{
		b = data(DT_RATE_12_DATA, NULL);
		HEAR(b);
	}
	for (int n = 0; n < 8; n++)
	{
		b = data(DT_VOICE_LC_HEADER, LC1);
		HEAR(b);
	}
	for (int n = 0; n < 6; n++)
	{
		b = voice(n, 0x40 + n);
		HEAR(b);
	}
	CHECK(sfrEngineRfBusy(&e), "busy in the voice call");
	for (int n = 0; n < 3; n++)
	{
		b = data(DT_TERMINATOR_WITH_LC, LC1);
		HEAR(b);
	}
#undef HEAR

	// The data call was held until the voice call's header came (60 ms after its last block, no gap): what's queued
	// goes out after it
	for (int n = 0; n < 20; n++, t += 60)
	{
		if ((sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST) && (sent < 40))
		{
			types[sent++] = (out.kind == SFR_BURST_VOICE) ? (0x20 | out.type) : out.type;
		}
	}

	CHECK(repeated == (7 + 8 + 6 + 1), "repeated: the data call, the headers, A-F, one terminator (%d)", repeated);
	CHECK((sent == repeated) && (types[0] == DT_CSBK) && (types[7] == DT_VOICE_LC_HEADER) && (types[15] == 0x20) && (types[20] == 0x25) &&
			(types[21] == DT_TERMINATOR_WITH_LC), "in order (%d)", sent);
	CHECK(!sfrEngineRfBusy(&e), "the terminator ended it");
}

// On-air test 17 (d_u1): a data call (2 CSBKs, a data header, 4 blocks) is heard whole with the repeater not sending,
// then repeated, all of it, in order; the HR-C6000's voice reports after it don't start a voice call, and no LC of an
// earlier call is left for one
static void testDataHold(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_HOTSPOT, SFR_FRAMING_AUTO, 0);
	sfrEngine_t e;
	sfrBurst_t b, out;
	uint32_t t = 70000;
	uint8_t types[10];
	int sent = 0;

	sfrEngineInit(&e, &cfg);

	// A voice call first: its LC
	b = data(DT_VOICE_LC_HEADER, LC1);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t);
	b = data(DT_TERMINATOR_WITH_LC, LC1);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 60);
	CHECK(sfrEngineOutboundSlot(&e, t + 90, &out) == SFR_TX_BURST, "header");
	CHECK(sfrEngineOutboundSlot(&e, t + 150, &out) == SFR_TX_BURST, "terminator");
	CHECK(sfrEngineOutboundSlot(&e, t + 210, &out) == SFR_TX_IDLE, "hang");

	// The data call in the hang: the hang ends for it (sfrEngineEndHang(), the glue's), it's held
	t += 300;
	CHECK(sfrEngineEndHang(&e) && !sfrEngineKeyed(&e), "the hang ends");
	CHECK(!sfrEngineEndHang(&e), "once");
	uint8_t dts[7] = { DT_CSBK, DT_CSBK, DT_DATA_HEADER, DT_RATE_12_DATA, DT_RATE_12_DATA, DT_RATE_12_DATA, DT_RATE_12_DATA };
	for (int n = 0; n < 7; n++, t += 60)
	{
		b = data(dts[n], NULL);
		b.payload[0] = (uint8_t)n;
		CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t) == SFR_RX_REPEATED, "data burst %d queued", n);
		CHECK(e.rfHold && sfrEngineRfBusy(&e) && !sfrEngineRfVoiceCall(&e), "held, busy, not a voice call (%d)", n);
		CHECK(!sfrLcKnown(e.rfLc), "no LC (the voice call's is gone)");
		CHECK(sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_NONE, "nothing sent while it's heard (%d)", n);
	}
	CHECK(sfrEngineNetBurst(&e, &b, NULL, t) == SFR_MMDVM_NAK_RF_BUSY, "the network waits");

	// Over: SFR_DATA_HOLD_GAP_MS without a burst. Sent, one per outbound slot
	CHECK(sfrEngineOutboundSlot(&e, t - 60 + 90, &out) == SFR_TX_NONE, "90 ms after the last block: still held");
	for (int n = 0; n < 10; n++, t += 60)
	{
		if ((sfrEngineOutboundSlot(&e, t + 90, &out) == SFR_TX_BURST) && (sent < 10))
		{
			types[sent++] = out.type;
			CHECK(out.payload[0] == (uint8_t)(sent - 1), "in order (%d)", sent);
		}
	}
	CHECK((sent == 7) && (types[0] == DT_CSBK) && (types[2] == DT_DATA_HEADER) && (types[6] == DT_RATE_12_DATA), "all of it (%d)", sent);
	CHECK(!sfrEngineRfBusy(&e) && sfrEngineKeyed(&e), "not busy, the hang");

	// A held data call cut short by a voice header: sent from then on, before it
	sfrEngineInit(&e, &cfg);
	t += 2000;
	b = data(DT_CSBK, NULL);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t);
	CHECK(e.rfHold, "held");
	b = data(DT_VOICE_LC_HEADER, LC2);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 60);
	CHECK(!e.rfHold && sfrEngineRfVoiceCall(&e) && (memcmp(e.rfLc, LC2, 9) == 0), "a voice call now");
	CHECK((sfrEngineOutboundSlot(&e, t + 90, &out) == SFR_TX_BURST) && (out.type == DT_CSBK), "the CSBK first");

	// Keyed for network traffic when a data call starts: the transmission ends while it's held
	sfrEngineInit(&e, &cfg);
	b = data(DT_VOICE_LC_HEADER, LC1);
	sfrEngineNetBurst(&e, &b, NULL, t);
	CHECK(sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST, "network header");
	b = data(DT_CSBK, NULL);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t + 40);
	CHECK(sfrEngineOutboundSlot(&e, t + 90, &out) == SFR_TX_END, "the transmission ends");
	CHECK(sfrEngineOutboundSlot(&e, t + 150, &out) == SFR_TX_NONE, "held");
	CHECK(sfrEngineOutboundSlot(&e, t + 40 + SFR_DATA_HOLD_GAP_MS + 30, &out) == SFR_TX_BURST, "then sent");

	// The hold off (the glue's debug knob): repeated as heard
	sfrEngineInit(&e, &cfg);
	e.noDataHold = true;
	b = data(DT_CSBK, NULL);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t);
	CHECK(!e.rfHold && (sfrEngineOutboundSlot(&e, t + 30, &out) == SFR_TX_BURST), "no hold");

	// A late entry voice call isn't held, and has no LC until its embedded LC
	sfrEngineInit(&e, &cfg);
	memcpy(e.rfLc, LC1, 9);
	b = voice(1, 3);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, t);
	CHECK(!e.rfHold && sfrEngineRfVoiceCall(&e) && !sfrLcKnown(e.rfLc), "late entry: voice, no LC yet");

	// Not ended: a hang with something queued, or a call on
	sfrEngineInit(&e, &cfg);
	CHECK(!sfrEngineEndHang(&e), "not keyed");
}

// A resync drops what's queued but keeps the call (its LC, busy)
static void testFlush(void)
{
	sfrConfig_t cfg = repeaterConfig(SFR_ROLE_HOTSPOT, SFR_FRAMING_AUTO, 0);
	sfrEngine_t e;
	sfrBurst_t b = data(DT_VOICE_LC_HEADER, LC2), out;

	sfrEngineInit(&e, &cfg);
	sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, 100);
	sfrEngineFlush(&e);
	CHECK((sfrEngineSpace(&e) == SFR_QUEUE_SIZE) && sfrEngineRfBusy(&e) && e.rfCall, "flushed, call kept");
	b = voice(0, 9);
	CHECK(sfrEngineRfBurst(&e, &b, 1, SFR_SYNC_MS, 160) == SFR_RX_REPEATED, "the call goes on");
	CHECK((sfrEngineOutboundSlot(&e, 190, &out) == SFR_TX_BURST) && (memcmp(sfrEngineTxLc(&e), LC2, 9) == 0), "with its LC");
}

static void testReportStepAndLateEntry(void)
{
	sfrLateEntry_t l;

	CHECK(sfrReportStep(4) == SFR_STEP_IN, "report 4 ms after the timeslot interrupt: in step");
	CHECK(sfrReportStep(2) == SFR_STEP_IN, "2 ms: in step");
	CHECK(sfrReportStep(10) == SFR_STEP_IN, "10 ms: in step");
	CHECK(sfrReportStep(1) == SFR_STEP_OUT, "1 ms: out of step");
	CHECK(sfrReportStep(26) == SFR_STEP_OUT, "26 ms (on-air test 16, z_g): out of step");
	CHECK(sfrReportStep(30) == SFR_STEP_OUT, "30 ms: out of step");
	CHECK(sfrReportStep(500) == SFR_STEP_UNKNOWN, "no slot timing: unknown");

	sfrLateEntryReset(&l);
	CHECK(!sfrLateEntryVoice(&l, 0, 1000), "a lone voice A doesn't start a call");
	CHECK(!sfrLateEntryVoice(&l, 2, 1060), "a C after it doesn't");
	CHECK(!sfrLateEntryVoice(&l, 1, 1120), "nor a B without an A before it");
	CHECK(!sfrLateEntryVoice(&l, 0, 2000), "A");
	CHECK(!sfrLateEntryVoice(&l, 1, 2200), "B 200 ms later: not in sequence");
	CHECK(!sfrLateEntryVoice(&l, 0, 3000), "A");
	CHECK(sfrLateEntryVoice(&l, 1, 3060), "B 60 ms after A: the call starts");
	CHECK(!sfrLateEntryVoice(&l, 1, 3120), "not twice");
}

int main(void)
{
	testReportStepAndLateEntry();
	testFlush();
	testDataThenVoice();
	testDataHold();
	testLcGuess();
	testRaw();
	testCrc();
	testUplink();
	testChannelEncoding();
	testConfig();
	testSync();
	testDedup();
	testEngineRepeat();
	testEngineLossAndTot();
	testEngineNetwork();
	testMmdvm();
	testIdle();

	printf("# %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
