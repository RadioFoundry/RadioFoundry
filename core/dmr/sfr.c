// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR single frequency repeater (SFR): the hardware independent parts
 *
 * See functions/sfr.h and docs/sfr.md.
 */

#if defined(HAS_SFR) || CONFIG_SFR

#include <stdio.h>
#include <string.h>
#include "dmr/sfr.h"
#include "dmr/dmrData.h"

// ETSI TS 102 361-1 table D.2, I(95) first
const uint8_t SFR_IDLE_INFO[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91 };

static const uint16_t HANG_MS[SFR_HANG_NUM] = { 1000, 2000, 4000, 0 };

#define FLAG1_ENABLED				SFR_CHANNEL_FLAG1_ENABLED
#define A_ROLE_MASK					0x07
#define A_FRAMING_SHIFT				3
#define A_FRAMING_MASK				0x03
#define A_INBOUND_TS2				0x20
#define A_HANG_SHIFT				6
#define B_OUTBOUND_CC_SET			0x80

bool sfrChannelDecode(uint8_t flag1, uint8_t byteA, uint8_t byteB, sfrChannelSettings_t *s)
{
	memset(s, 0, sizeof(sfrChannelSettings_t));

	if ((flag1 & FLAG1_ENABLED) == 0)
	{
		return false;
	}

	s->role = byteA & A_ROLE_MASK;

	if (s->role >= SFR_ROLE_NUM_STORED)
	{
		s->role = SFR_ROLE_OFF;// not a value this firmware wrote
	}

	s->framing = (byteA >> A_FRAMING_SHIFT) & A_FRAMING_MASK;
	s->inboundSlot = ((byteA & A_INBOUND_TS2) != 0) ? 1 : 0;
	s->hangIndex = byteA >> A_HANG_SHIFT;
	s->outboundCCSet = ((byteB & B_OUTBOUND_CC_SET) != 0);
	s->outboundCC = byteB & 0x0F;

	return (s->role != SFR_ROLE_OFF);
}

// Only the bits this module owns are changed in flag1
void sfrChannelEncode(const sfrChannelSettings_t *s, uint8_t *flag1, uint8_t *byteA, uint8_t *byteB)
{
	if (s->role == SFR_ROLE_OFF)
	{
		*flag1 &= ~FLAG1_ENABLED;
		*byteA = 0;
		*byteB = 0;
		return;
	}

	*flag1 |= FLAG1_ENABLED;
	*byteA = (s->role & A_ROLE_MASK) | ((s->framing & A_FRAMING_MASK) << A_FRAMING_SHIFT) |
			(s->inboundSlot ? A_INBOUND_TS2 : 0) | ((s->hangIndex & 0x03) << A_HANG_SHIFT);
	*byteB = (s->outboundCCSet ? (B_OUTBOUND_CC_SET | (s->outboundCC & 0x0F)) : 0);
}

uint16_t sfrHangMs(uint8_t hangIndex)
{
	return HANG_MS[hangIndex % SFR_HANG_NUM];
}

uint8_t sfrResolveFraming(uint8_t role, uint8_t framing)
{
	if ((framing != SFR_FRAMING_AUTO) && (framing < SFR_FRAMING_NUM))
	{
		return framing;
	}

	switch (role)
	{
		case SFR_ROLE_RPT_MOTOROLA:
		case SFR_ROLE_USER_ERDM:
			return SFR_FRAMING_BS;
		default:
			// The ETSI direct mode sync of the outbound slot: what the AnyTone D578 relay (DCDM) and, most likely, the
			// Hytera ones send, and the hotspot's host needs the slot in the burst
			return SFR_FRAMING_TS;
	}
}

void sfrConfigBuild(const sfrChannelSettings_t *s, uint8_t channelCC, uint8_t channelTot15s, sfrConfig_t *cfg)
{
	cfg->role = s->role;
	cfg->framing = sfrResolveFraming(s->role, s->framing);
	cfg->inboundSlot = s->inboundSlot ? 1 : 0;
	cfg->inboundCC = channelCC & 0x0F;
	// Only the Motorola roles have an outbound colour code of their own
	cfg->outboundCC = (s->outboundCCSet && ((s->role == SFR_ROLE_RPT_MOTOROLA) || (s->role == SFR_ROLE_USER_ERDM))) ? (s->outboundCC & 0x0F) : cfg->inboundCC;
	cfg->hangMs = sfrHangMs(s->hangIndex);
	cfg->totMs = (channelTot15s != 0) ? (channelTot15s * 15000U) : SFR_TOT_DEFAULT_MS;
}

uint8_t sfrOutboundSync(const sfrConfig_t *cfg)
{
	switch (cfg->framing)
	{
		case SFR_FRAMING_BS:
			return SFR_SYNC_BS;
		case SFR_FRAMING_TS:
			return (cfg->inboundSlot == 0) ? SFR_SYNC_TS2 : SFR_SYNC_TS1;
		default:
			return SFR_SYNC_MS;
	}
}

// The repeater repeats what radios send: MS sync, or the direct mode sync of the inbound slot. Never a repeater's (BS
// sync, another SFR's output or our own echo) or the direct mode sync of the outbound slot
bool sfrInboundSyncAccepted(const sfrConfig_t *cfg, uint8_t sync)
{
	switch (sync)
	{
		case SFR_SYNC_MS:
			return true;
		case SFR_SYNC_TS1:
			return (cfg->inboundSlot == 0);
		case SFR_SYNC_TS2:
			return (cfg->inboundSlot == 1);
		default:
			return false;
	}
}

bool sfrRoleIsRepeater(uint8_t role)
{
	return (role == SFR_ROLE_RPT_STANDARD) || (role == SFR_ROLE_RPT_MOTOROLA) || (role == SFR_ROLE_HOTSPOT);
}

bool sfrRoleIsUser(uint8_t role)
{
	return (role == SFR_ROLE_USER_STANDARD) || (role == SFR_ROLE_USER_ERDM);
}

const char *sfrRoleName(uint8_t role)
{
	static const char *const NAMES[] = { "Off", "User", "ERDM", "Rpt", "Rpt Moto", "Hotspot" };

	return (role <= SFR_ROLE_HOTSPOT) ? NAMES[role] : "?";
}

const char *sfrFramingName(uint8_t framing)
{
	static const char *const NAMES[] = { "Auto", "MS", "TS", "BS" };

	return (framing < SFR_FRAMING_NUM) ? NAMES[framing] : "?";
}

void sfrDedupReset(sfrDedup_t *d)
{
	d->lastSeq = -1;
	d->lastMs = 0;
}

// Accepted: the first burst, one after a gap, or one 1 to 3 bursts after the last one played. A repeat (90 ms, so
// one or two bursts behind) or the same burst again is dropped
bool sfrDedupVoice(sfrDedup_t *d, uint8_t seq, uint32_t nowMs)
{
	if ((d->lastSeq >= 0) && ((nowMs - d->lastMs) < SFR_DEDUP_RESET_MS))
	{
		int ahead = (((int)seq - d->lastSeq) + 6) % 6;

		if ((ahead == 0) || (ahead > 3))
		{
			return false;
		}
	}

	d->lastSeq = (int8_t)(seq % 6);
	d->lastMs = nowMs;

	return true;
}

uint8_t sfrReportStep(uint32_t afterTimeslotMs)
{
	if (afterTimeslotMs >= SFR_REPORT_NO_TIMING_MS)
	{
		return SFR_STEP_UNKNOWN;
	}

	return ((afterTimeslotMs >= SFR_REPORT_MIN_MS) && (afterTimeslotMs <= SFR_REPORT_MAX_MS)) ? SFR_STEP_IN : SFR_STEP_OUT;
}

void sfrLateEntryReset(sfrLateEntry_t *l)
{
	l->armed = false;
	l->voiceAMs = 0;
}

bool sfrLateEntryVoice(sfrLateEntry_t *l, uint8_t seq, uint32_t nowMs)
{
	bool start = (seq == 1) && l->armed && ((nowMs - l->voiceAMs) >= SFR_LATE_ENTRY_MIN_MS) && ((nowMs - l->voiceAMs) <= SFR_LATE_ENTRY_MAX_MS);

	l->armed = (seq == 0);
	l->voiceAMs = nowMs;

	return start;
}

int sfrUserRxColourCode(const sfrConfig_t *cfg, uint8_t rxCC)
{
	return ((rxCC == cfg->inboundCC) || (rxCC == cfg->outboundCC)) ? rxCC : -1;
}

// Queue

static inline sfrBurst_t *queueAt(sfrEngine_t *e, int n)
{
	return &e->queue[(e->head + n) % SFR_QUEUE_SIZE];
}

static bool queuePush(sfrEngine_t *e, const sfrBurst_t *b)
{
	if (e->count >= SFR_QUEUE_SIZE)
	{
		return false;
	}

	memcpy(queueAt(e, e->count), b, sizeof(sfrBurst_t));
	e->count++;

	return true;
}

static bool queueHas(sfrEngine_t *e, uint8_t originFlag)
{
	for (int n = 0; n < e->count; n++)
	{
		if ((queueAt(e, n)->flags & SFR_BURST_FLAG_NET) == originFlag)
		{
			return true;
		}
	}

	return false;
}

// Drop the network bursts, keeping the others in order
static void queueDropNet(sfrEngine_t *e)
{
	int kept = 0;

	for (int n = 0; n < e->count; n++)
	{
		sfrBurst_t *b = queueAt(e, n);

		if ((b->flags & SFR_BURST_FLAG_NET) == 0)
		{
			if (kept != n)
			{
				memcpy(queueAt(e, kept), b, sizeof(sfrBurst_t));
			}
			kept++;
		}
	}

	e->count = kept;
}

static inline bool isLcBurst(const sfrBurst_t *b)
{
	return (b->kind == SFR_BURST_DATA) && ((b->type == DT_VOICE_LC_HEADER) || (b->type == DT_TERMINATOR_WITH_LC));
}

// Voice, its LC header or PI header: a voice call's. Anything else but idle (CSBK, MBC, data header, blocks): a data
// call's
static inline bool isVoiceCallBurst(const sfrBurst_t *b)
{
	return (b->kind == SFR_BURST_VOICE) || (b->type == DT_VOICE_LC_HEADER) || (b->type == DT_VOICE_PI_HEADER);
}

bool sfrLcKnown(const uint8_t *lc)
{
	return ((lc[3] | lc[4] | lc[5]) != 0) && ((lc[6] | lc[7] | lc[8]) != 0);
}

void sfrEngineInit(sfrEngine_t *e, const sfrConfig_t *cfg)
{
	memset(e, 0, sizeof(sfrEngine_t));
	e->cfg = *cfg;
	sfrDedupReset(&e->rfDedup);
}

// A burst heard on the inbound slot (repeater roles). cc: its colour code, sync: its sync type (sfrSync_t)
sfrRxResult_t sfrEngineRfBurst(sfrEngine_t *e, const sfrBurst_t *b, uint8_t cc, uint8_t sync, uint32_t nowMs)
{
	if ((cc != e->cfg.inboundCC) || ((b->kind == SFR_BURST_VOICE) ? (b->type > 5) : (b->type == DT_IDLE)) ||
			((b->kind == SFR_BURST_DATA) && !sfrInboundSyncAccepted(&e->cfg, sync)))
	{
		return SFR_RX_IGNORED;
	}

	bool terminator = (b->kind == SFR_BURST_DATA) && (b->type == DT_TERMINATOR_WITH_LC);
	bool newCall = (!e->rfCall || ((nowMs - e->rfLastMs) > SFR_RF_LOSS_MS));

	// Only a call's own terminator is repeated: the extra ones some radios send after it, or one heard alone, aren't
	if (newCall && terminator)
	{
		return SFR_RX_IGNORED;
	}

	// A new inbound call: the RF has the outbound slot, what the network had queued is dropped, and the rest of a
	// stream that was coming in refused
	if (newCall)
	{
		if (queueHas(e, SFR_BURST_FLAG_NET))
		{
			queueDropNet(e);
		}

		if (e->netStream)
		{
			e->netDropping = true;
		}

		e->rfCall = true;
		e->rfTot = false;
		e->rfTermQueued = false;
		e->rfStartMs = nowMs;
		sfrDedupReset(&e->rfDedup);
		// Nothing of the last call's carries over: its LC (on-air test 17: a header made with it for the HR-C6000's
		// voice reports after a data call), whether it was a voice call
		memset(e->rfLc, 0, SFR_LC_LENGTH);
		e->rfVoice = false;
		// A data call (CSBK, data header) is held until it's over: heard while the repeater sends, its bursts came
		// out with bit errors (on-air test 17: every CSBK and data header heard keyed failed its CRC)
		e->rfHold = !e->noDataHold && !isVoiceCallBurst(b);
	}

	if (isVoiceCallBurst(b))
	{
		e->rfVoice = true;
		e->rfHold = false;
	}

	e->rfLastMs = nowMs;
	e->rfBusy = true;

	if (e->rfTot || ((nowMs - e->rfStartMs) > e->cfg.totMs))
	{
		e->rfTot = true;
		return SFR_RX_TOT;
	}

	if (b->kind == SFR_BURST_VOICE)
	{
		if (!sfrDedupVoice(&e->rfDedup, b->type, nowMs))
		{
			return SFR_RX_DUPLICATE;
		}
	}
	else if (isLcBurst(b))
	{
		memcpy(e->rfLc, b->payload, SFR_LC_LENGTH);
		sfrDedupReset(&e->rfDedup);
	}

	if (!queuePush(e, b))
	{
		return SFR_RX_FULL;
	}

	queueAt(e, e->count - 1)->flags &= ~SFR_BURST_FLAG_NET;

	if (terminator)
	{
		e->rfTermQueued = true;
		e->rfCall = false;// over: the next burst heard is a new call
	}

	return SFR_RX_REPEATED;
}

// The inbound call's LC from its embedded signalling (late entry: no header heard)
void sfrEngineRfLc(sfrEngine_t *e, const uint8_t *lc)
{
	memcpy(e->rfLc, lc, SFR_LC_LENGTH);
}

// A burst from the network for the outbound slot (hotspot). lc: the stream's LC for a voice burst when the host's
// embedded LC gave one (NULL otherwise). Returns 0, or the NAK reason
uint8_t sfrEngineNetBurst(sfrEngine_t *e, const sfrBurst_t *b, const uint8_t *lc, uint32_t nowMs)
{
	bool header = (b->kind == SFR_BURST_DATA) && (b->type == DT_VOICE_LC_HEADER);
	bool terminator = (b->kind == SFR_BURST_DATA) && (b->type == DT_TERMINATOR_WITH_LC);

	if (e->rfCall && ((nowMs - e->rfLastMs) > SFR_RF_LOSS_MS))
	{
		e->rfCall = false;
	}

	// Busy until the inbound call's terminator is repeated; what the network sends once it's queued goes out after it
	bool busy = e->rfBusy && !e->rfTermQueued;

	// The stream an RF call cut into stays refused until it ends (its terminator, a gap) or a new one starts
	if (e->netDropping && (header || ((nowMs - e->netLastMs) > SFR_NET_LOSS_MS)) && !busy)
	{
		e->netDropping = false;
	}

	e->netLastMs = nowMs;

	if (busy || e->netDropping)
	{
		if (terminator)
		{
			e->netDropping = false;
			e->netStream = false;
		}
		else
		{
			e->netStream = true;
			e->netDropping = true;
		}

		return SFR_MMDVM_NAK_RF_BUSY;
	}

	if (e->count >= SFR_QUEUE_SIZE)
	{
		return SFR_MMDVM_NAK_FULL;
	}

	if (header || terminator)
	{
		memcpy(e->netLc, b->payload, SFR_LC_LENGTH);
	}
	else if (lc != NULL)
	{
		memcpy(e->netLc, lc, SFR_LC_LENGTH);
	}

	queuePush(e, b);
	queueAt(e, e->count - 1)->flags |= SFR_BURST_FLAG_NET;
	e->netStream = !terminator;

	return 0;
}

// MMDVM DMR_ABORT: what the network had queued is dropped
void sfrEngineNetAbort(sfrEngine_t *e)
{
	queueDropNet(e);
	e->netStream = false;
	e->netDropping = false;
}

void sfrEngineFlush(sfrEngine_t *e)
{
	e->head = 0;
	e->count = 0;
}

bool sfrEngineEndHang(sfrEngine_t *e)
{
	if (!e->keyed || (e->count > 0) || e->rfCall || e->netStream)
	{
		return false;
	}

	e->keyed = false;
	return true;
}

// Each time the outbound slot is next: what to send in it
sfrTxAction_t sfrEngineOutboundSlot(sfrEngine_t *e, uint32_t nowMs, sfrBurst_t *out)
{
	// An inbound call lost without its terminator
	if (e->rfCall && ((nowMs - e->rfLastMs) > SFR_RF_LOSS_MS))
	{
		e->rfCall = false;
		e->rfTot = false;
	}

	if (e->rfBusy && !e->rfCall && !queueHas(e, 0))
	{
		e->rfBusy = false;// all of it has been repeated
	}

	// A data call is sent once it's over (no burst for SFR_DATA_HOLD_GAP_MS), all of it, from its first burst. Until
	// then the repeater doesn't send (it stops if it was: its hang time, the network's traffic it dropped)
	if (e->rfHold && (!e->rfCall || ((nowMs - e->rfLastMs) > SFR_DATA_HOLD_GAP_MS)))
	{
		e->rfHold = false;
	}

	if (e->rfHold)
	{
		if (e->keyed)
		{
			e->keyed = false;
			return SFR_TX_END;
		}

		return SFR_TX_NONE;
	}

	if (e->netStream && ((nowMs - e->netLastMs) > SFR_NET_LOSS_MS))
	{
		e->netStream = false;
		e->netDropping = false;
	}

	if (e->count > 0)
	{
		sfrBurst_t *b = queueAt(e, 0);
		uint8_t origin = b->flags & SFR_BURST_FLAG_NET;

		memcpy(out, b, sizeof(sfrBurst_t));
		e->head = (e->head + 1) % SFR_QUEUE_SIZE;
		e->count--;

		if (out->kind == SFR_BURST_VOICE)
		{
			memcpy(e->txLc, (origin ? e->netLc : e->rfLc), SFR_LC_LENGTH);
			e->txLcOrigin = origin;
		}
		else if (isLcBurst(out))
		{
			memcpy(e->txLc, out->payload, SFR_LC_LENGTH);
			e->txLcOrigin = origin;
		}

		// The MMDVM busy flag ends when the inbound call's terminator has been repeated, not with the hang time
		if ((origin == 0) && (out->kind == SFR_BURST_DATA) && (out->type == DT_TERMINATOR_WITH_LC))
		{
			e->rfCall = false;
			e->rfBusy = false;
		}

		e->keyed = true;
		e->lastTrafficMs = nowMs;

		return SFR_TX_BURST;
	}

	if (e->keyed)
	{
		// Gaps in a call (a burst lost on the inbound slot, the host late) and the hang time are filled with idle bursts
		if ((e->rfCall && !e->rfTot) || e->netStream || ((nowMs - e->lastTrafficMs) < e->cfg.hangMs))
		{
			return SFR_TX_IDLE;
		}

		e->keyed = false;

		return SFR_TX_END;
	}

	return SFR_TX_NONE;
}

uint8_t sfrEngineSpace(const sfrEngine_t *e)
{
	return (uint8_t)(SFR_QUEUE_SIZE - e->count);
}

void sfrUplinkReset(sfrUplink_t *u)
{
	u->open = false;
	u->headerSent = false;
	u->lastMs = 0;
}

uint8_t sfrUplinkBurst(sfrUplink_t *u, const sfrBurst_t *b, bool lcKnown, uint32_t nowMs)
{
	bool header = (b->kind == SFR_BURST_DATA) && (b->type == DT_VOICE_LC_HEADER);
	bool terminator = (b->kind == SFR_BURST_DATA) && (b->type == DT_TERMINATOR_WITH_LC);
	uint8_t actions = SFR_UP_SEND;

	if (u->open && ((nowMs - u->lastMs) > SFR_UPLINK_LOST_MS))
	{
		u->open = false;// a new call (the old one's DMR_LOST is sfrUplinkLost()'s)
	}

	// The extra terminators radios send after the first go up as they are
	if (terminator && !u->open && ((nowMs - u->lastMs) <= SFR_UPLINK_LOST_MS))
	{
		u->lastMs = nowMs;
		return SFR_UP_SEND;
	}

	if (!u->open)
	{
		u->open = true;
		u->headerSent = false;
	}

	u->lastMs = nowMs;

	if (header)
	{
		u->headerSent = true;
	}
	else if ((b->kind == SFR_BURST_VOICE) || terminator)
	{
		// Voice and its terminator need the call's LC upstream: a header made from it, or wait for it
		if (!u->headerSent)
		{
			if (!lcKnown)
			{
				return 0;
			}

			actions |= SFR_UP_HEADER_FIRST;
			u->headerSent = true;
		}
	}

	if (terminator)
	{
		u->open = false;
	}

	return actions;
}

bool sfrUplinkLost(sfrUplink_t *u, uint32_t nowMs)
{
	if (u->open && ((nowMs - u->lastMs) > SFR_UPLINK_LOST_MS))
	{
		u->open = false;
		return true;
	}

	return false;
}

// GF(256) multiply, primitive polynomial 0x11D (ETSI TS 102 361-1 B.3.6)
static uint8_t gfMul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	while (b != 0)
	{
		if (b & 1)
		{
			p ^= a;
		}
		a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1D : 0x00));
		b >>= 1;
	}

	return p;
}

// RS(12,9): generator x^3 + 14x^2 + 56x + 64, the parity stored highest first and masked
static bool rs129Ok(const uint8_t *info, uint8_t mask)
{
	static const uint8_t POLY[3] = { 64, 56, 14 };
	uint8_t parity[3] = { 0, 0, 0 };

	for (int i = 0; i < 9; i++)
	{
		uint8_t t = info[i] ^ parity[2];

		parity[2] = parity[1] ^ gfMul(POLY[2], t);
		parity[1] = parity[0] ^ gfMul(POLY[1], t);
		parity[0] = gfMul(POLY[0], t);
	}

	return ((info[9] ^ mask) == parity[2]) && ((info[10] ^ mask) == parity[1]) && ((info[11] ^ mask) == parity[0]);
}

// CRC-CCITT of the first 10 octets (B.3.7), inverted, masked, stored MSB first
static bool crc16Ok(const uint8_t *info, uint8_t mask)
{
	uint16_t crc = 0;

	for (int i = 0; i < 10; i++)
	{
		crc ^= (uint16_t)(info[i] << 8);
		for (int bit = 0; bit < 8; bit++)
		{
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
		}
	}

	crc = (uint16_t)(~crc ^ ((mask << 8) | mask));

	return (info[10] == (crc >> 8)) && (info[11] == (crc & 0xFF));
}

// The checksum masked as on air, or without its mask (in case the HR-C6000 takes it off in its RX RAM)
bool sfrDataBurstCrcOk(uint8_t dataType, const uint8_t *info)
{
	switch (dataType)
	{
		case DT_VOICE_LC_HEADER:
			return rs129Ok(info, 0x96) || rs129Ok(info, 0x00);
		case DT_TERMINATOR_WITH_LC:
			return rs129Ok(info, 0x99) || rs129Ok(info, 0x00);
		case DT_VOICE_PI_HEADER:
			return crc16Ok(info, 0x69) || crc16Ok(info, 0x00);
		case DT_CSBK:
			return crc16Ok(info, 0xA5) || crc16Ok(info, 0x00);
		case DT_MBC_HEADER:
			return crc16Ok(info, 0xAA) || crc16Ok(info, 0x00);
		case DT_DATA_HEADER:
			return crc16Ok(info, 0xCC) || crc16Ok(info, 0x00);
		default:
			return true;
	}
}

// Bits 108-155 of a burst: the low nibble of byte 13, bytes 14-18, the high nibble of byte 19
static uint64_t rawMiddle(const uint8_t *frame)
{
	uint64_t v = (uint64_t)(frame[13] & 0x0F);

	for (int i = 14; i <= 18; i++)
	{
		v = (v << 8) | frame[i];
	}

	return (v << 4) | (frame[19] >> 4);
}

static int bitErrors(uint64_t a, uint64_t b)
{
	uint64_t x = a ^ b;
	int n = 0;

	while (x != 0)
	{
		x &= (x - 1);
		n++;
	}

	return n;
}

static inline bool rawBit(const uint8_t *frame, int n)
{
	return ((frame[n >> 3] >> (7 - (n & 7))) & 1) != 0;
}

// BPTC(196,96) without error correction (the checksum is checked after): the 96 data bits out of info bits 0-97 and
// 166-263. Data bit n is matrix row 0 columns 3-10 (n < 8), then rows 1-8 columns 0-10; matrix position m (1 + 15 *
// row + column) is sent as info bit (m * 181) mod 196.
static void bptcExtract(const uint8_t *frame, uint8_t *info)
{
	memset(info, 0, 12);

	for (int n = 0; n < 96; n++)
	{
		int row = (n < 8) ? 0 : (1 + ((n - 8) / 11));
		int column = (n < 8) ? (n + 3) : ((n - 8) % 11);
		int sent = ((1 + (15 * row) + column) * 181) % 196;

		if (rawBit(frame, (sent < 98) ? sent : (sent + 68)))
		{
			info[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
		}
	}
}

// Sync patterns (48 bits), in sfrSync_t order, voice and data
static const uint64_t RAW_VOICE_SYNC[4] = { 0x7F7D5DD57DFDULL, 0x755FD7DF75F7ULL, 0x5D577F7757FFULL, 0x7DFFD5F55D5FULL };
static const uint64_t RAW_DATA_SYNC[4]  = { 0xD5D7F77FD757ULL, 0xDFF57D75DF5DULL, 0xF7FDD5DDFD55ULL, 0xD7557F5FF7F5ULL };

uint8_t sfrRawClassify(const uint8_t *frame, uint8_t colourCode, uint8_t *dataType, uint8_t *info, uint8_t *sync)
{
	uint64_t middle = rawMiddle(frame);

	for (int s = 0; s < 4; s++)
	{
		if (bitErrors(middle, RAW_DATA_SYNC[s]) <= SFR_RAW_MAX_SYNC_ERRORS)
		{
			// The slot type's Golay(20,8) is systematic: colour code bits 98-101, data type 102-105
			uint8_t cc = (frame[12] >> 2) & 0x0F;
			uint8_t dt = (uint8_t)(((frame[12] & 0x03) << 2) | (frame[13] >> 6));

			*sync = (uint8_t)s;

			if ((cc != colourCode) || (dt > DT_RATE_1_DATA) || (dt == DT_RATE_34_DATA) || (dt == DT_RATE_1_DATA))
			{
				return SFR_RAW_NONE;// (rate 3/4 and 1 aren't BPTC: the HR-C6000's info bits are used for them)
			}

			bptcExtract(frame, info);

			if (!sfrDataBurstCrcOk(dt, info))
			{
				return SFR_RAW_NONE;
			}

			*dataType = dt;
			return SFR_RAW_DATA;
		}

		if (bitErrors(middle, RAW_VOICE_SYNC[s]) <= SFR_RAW_MAX_SYNC_ERRORS)
		{
			*sync = (uint8_t)s;
			return SFR_RAW_VOICE_A;
		}
	}

	// EMB: colour code in bits 108-111
	if (((frame[13] & 0x0F)) == colourCode)
	{
		return SFR_RAW_VOICE_EMB;
	}

	return SFR_RAW_NONE;
}

void sfrRawAmbe(const uint8_t *frame, uint8_t *ambe)
{
	memcpy(ambe, frame, 13);
	ambe[13] = (frame[13] & 0xF0) | (frame[19] & 0x0F);
	memcpy(ambe + 14, frame + 20, 13);
}

uint8_t sfrLcBurstGuess(uint8_t reportedType, const uint8_t *info, const uint8_t *callLc, bool voiceSinceHeader)
{
	if (!sfrLcKnown(callLc) || (reportedType > DT_TERMINATOR_WITH_LC) || (memcmp(info, callLc, 5) != 0))
	{
		return 0xFF;
	}

	if (sfrDataBurstCrcOk(reportedType, info) && (reportedType != DT_VOICE_PI_HEADER))
	{
		return 0xFF;// good as it is
	}

	return voiceSinceHeader ? DT_TERMINATOR_WITH_LC : DT_VOICE_LC_HEADER;
}

bool sfrMmdvmTrigger(bool simplexFlag, uint32_t rxFreq, uint32_t txFreq)
{
	return (!simplexFlag) && (rxFreq != 0) && (rxFreq == txFreq);
}

int sfrMmdvmVersionSuffix(const sfrConfig_t *cfg, char *buf, size_t size)
{
	return snprintf(buf, size, " SFR TS%d>TS%d %s", cfg->inboundSlot + 1, 2 - cfg->inboundSlot, sfrFramingName(cfg->framing));
}

#endif // HAS_SFR
