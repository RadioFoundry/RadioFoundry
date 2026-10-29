// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The split duplex TX half's continuous BS outbound (see bsOutbound.h; docs/spec/split-duplex.md 5)
#include <string.h>
#include "dmr/bsOutbound.h"
#include "dmr/fec/dmrCach.h"
#include "dmr/fec/dmrFec.h"
#include "dmr/call/dmr_lc.h"
#include "hotspot/dmrDataFrame.h"

#define NAK_FULL	5	// MMDVM_NAK_FULL
#define BS_DATA_SYNC	0xDFF57D75DF5DULL	// ETSI TS 102 361-1 9.1.1
#define BS_VOICE_SYNC	0x755FD7DF75F7ULL
#define SYNC_FIRST_BIT	108

const uint8_t BS_IDLE_INFO[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91 };

// Three AMBE+2 silence frames, as the HR_C6000 takes a voice burst (the same bits as HOTSPOT_UPLINK_SILENCE)
static const uint8_t SILENCE[DMR_VOICE_BYTES] = {
	0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B, 0xB9, 0xE8, 0x81, 0x52,
	0x61, 0x73, 0x00, 0x2A, 0x6B, 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
};

static bsSlot_t *slot_of(bsOutbound_t *o, int8_t ts)
{
	return &o->slot[(ts == 1) ? 0 : 1];
}

static const bsSlot_t *cslot_of(const bsOutbound_t *o, int8_t ts)
{
	return &o->slot[(ts == 1) ? 0 : 1];
}

void bsOutboundInit(bsOutbound_t *o, uint8_t prebuffer)
{
	memset(o, 0, sizeof(*o));
	bsOutboundSetPrebuffer(o, prebuffer);
	dmrShortLcEncode(dmrShortLcNull(), o->slc_cur);
}

void bsOutboundSetPrebuffer(bsOutbound_t *o, uint8_t prebuffer)
{
	if (prebuffer == 0) {
		prebuffer = BS_OUT_PREBUFFER;
	}
	o->prebuffer = (prebuffer > BS_OUT_PREBUFFER_MAX) ? BS_OUT_PREBUFFER_MAX : prebuffer;
}

static void slot_clear(bsOutbound_t *o, bsSlot_t *s)
{
	o->st.dropped = (uint16_t)(o->st.dropped + s->count);
	s->head = 0;
	s->count = 0;
	s->terms = 0;
	s->playing = false;
}

void bsOutboundReset(bsOutbound_t *o)
{
	for (int i = 0; i < 2; i++) {
		bsSlot_t *s = &o->slot[i];

		slot_clear(o, s);
		s->voice_open = false;
		s->lc_valid = false;
	}
}

uint8_t bsOutboundPush(bsOutbound_t *o, int8_t ts, const dmr_tx_burst_t *b, const uint8_t *lc, uint32_t now_ms)
{
	bsSlot_t *s = slot_of(o, ts);

	if (s->count >= BS_OUT_DEPTH) {
		return NAK_FULL;
	}
	bsOutBurst_t *e = &s->q[(s->head + s->count) % BS_OUT_DEPTH];

	e->b = *b;
	e->lc_valid = (lc != NULL);
	if (lc != NULL) {
		memcpy(e->lc, lc, 9);
	}
	if (s->count == 0) {
		s->first_ms = now_ms;
	}
	s->count++;
	if (b->kind == DMR_BURST_TERMINATOR_LC) {
		s->terms++;
	}
	s->last_push_ms = now_ms;
	return 0;
}

void bsOutboundDrop(bsOutbound_t *o, int8_t ts)
{
	slot_clear(o, slot_of(o, ts));
	// A voice stream on air ends with a terminator from Next() once the buffer is empty and the host has gone
	// quiet; an abort means that now
	slot_of(o, ts)->last_push_ms -= BS_OUT_STREAM_LOST_MS;
}

uint8_t bsOutboundCount(const bsOutbound_t *o, int8_t ts)
{
	return cslot_of(o, ts)->count;
}

uint8_t bsOutboundSpace(const bsOutbound_t *o, int8_t ts)
{
	return (uint8_t)(BS_OUT_DEPTH - cslot_of(o, ts)->count);
}

bool bsOutboundBusy(const bsOutbound_t *o)
{
	for (int i = 0; i < 2; i++) {
		if ((o->slot[i].count > 0) || o->slot[i].voice_open) {
			return true;
		}
	}
	return false;
}

void bsOutboundSetAt(bsOutbound_t *o, uint8_t mask, uint8_t mode, uint32_t now_ms)
{
	o->at_mask = mask & 3u;
	o->at_mode = (mode == BS_AT_MODE_MASK) ? BS_AT_MODE_MASK : BS_AT_MODE_OR;
	o->at_ms = now_ms;
	o->at_set = true;
}

uint8_t bsOutboundAtMask(const bsOutbound_t *o, uint32_t now_ms)
{
	if (!o->at_set || ((uint32_t)(now_ms - o->at_ms) >= BS_OUT_AT_HOLD_MS)) {
		return 0;
	}
	return o->at_mask;
}

static uint32_t next_s(const bsOutbound_t *o, int i)
{
	return 2u * o->slot[i].n + (uint32_t)i;
}

void bsOutboundSetShortLc(bsOutbound_t *o, const uint32_t rows[4])
{
	uint32_t s1 = next_s(o, 0), s2 = next_s(o, 1);

	if (o->slc_pending && ((s1 >= o->switch_s) || (s2 >= o->switch_s))) {
		memcpy(o->slc_next, rows, sizeof(o->slc_next));	// one slot already sends the new one: after it
		o->slc_queued = true;
		return;
	}
	// The first multiple of 4 after every slot already produced (s1 - 2 and s2 - 2 were the last ones)
	uint32_t m = 0;

	if (o->slot[0].n > 0) {
		m = s1 - 1u;
	}
	if ((o->slot[1].n > 0) && (s2 - 1u > m)) {
		m = s2 - 1u;
	}
	memcpy(o->slc_new, rows, sizeof(o->slc_new));
	o->switch_s = (m + 3u) & ~3u;
	o->slc_pending = true;
}

static uint32_t slc_row(bsOutbound_t *o, uint32_t s, const uint32_t **set)
{
	if (o->slc_pending && (next_s(o, 0) >= o->switch_s) && (next_s(o, 1) >= o->switch_s)) {
		memcpy(o->slc_cur, o->slc_new, sizeof(o->slc_cur));	// both slots are past the switch
		o->slc_pending = false;
		if (o->slc_queued) {
			o->slc_queued = false;
			bsOutboundSetShortLc(o, o->slc_next);
		}
	}
	const uint32_t *rows = (o->slc_pending && (s >= o->switch_s)) ? o->slc_new : o->slc_cur;

	*set = rows;
	return rows[s % 4u];
}

static void make_idle(dmr_tx_burst_t *out)
{
	memset(out, 0, sizeof(*out));
	out->kind = DMR_BURST_IDLE;
	out->sync_override = DMR_SYNC_BS;
	out->host_crc = false;		// the idle message has no CRC
	out->len = sizeof(BS_IDLE_INFO);
	memcpy(out->payload, BS_IDLE_INFO, sizeof(BS_IDLE_INFO));
}

void bsOutboundNext(bsOutbound_t *o, int8_t ts, uint32_t now_ms, dmr_tx_burst_t *out, dmr_cach_t *cach,
		    const uint8_t **emb_lc)
{
	bsSlot_t *s = slot_of(o, ts);
	uint32_t sidx = 2u * s->n + ((ts == 1) ? 0u : 1u);

	*emb_lc = NULL;
	if (!s->playing && (s->count > 0) &&
	    ((s->count >= o->prebuffer) || (s->terms > 0) || ((uint32_t)(now_ms - s->first_ms) >= BS_OUT_WAIT_MS))) {
		s->playing = true;
	}
	if (s->playing && (s->count > 0)) {
		bsOutBurst_t *e = &s->q[s->head];

		*out = e->b;
		out->sync_override = DMR_SYNC_BS;
		s->head = (uint8_t)((s->head + 1u) % BS_OUT_DEPTH);
		s->count--;
		switch (out->kind) {
		case DMR_BURST_VOICE_LC_HEADER:
			memcpy(s->lc, out->payload, 9);
			s->lc_valid = true;
			s->voice_open = true;
			s->last_index = 5;
			break;
		case DMR_BURST_VOICE:
			s->voice_open = true;
			s->last_index = out->voice_index;
			if (out->voice_index == 0) {
				*emb_lc = e->lc_valid ? e->lc : (s->lc_valid ? s->lc : NULL);
			}
			break;
		case DMR_BURST_TERMINATOR_LC:
			s->terms--;
			s->voice_open = false;
			s->playing = false;	// the next stream prebuffers again
			break;
		default:
			break;
		}
	} else if (s->voice_open) {
		if (s->playing) {
			s->underruns++;		// a playing stream ran dry: count once, then wait for the prebuffer again
			s->playing = false;
		}
		if (((uint32_t)(now_ms - s->last_push_ms) >= BS_OUT_STREAM_LOST_MS) && s->lc_valid) {
			// The host stopped mid stream: end it properly
			memset(out, 0, sizeof(*out));
			out->kind = DMR_BURST_TERMINATOR_LC;
			out->sync_override = DMR_SYNC_BS;
			out->len = 9;
			memcpy(out->payload, s->lc, 9);
			s->voice_open = false;
			o->st.closed++;
		} else if ((uint32_t)(now_ms - s->last_push_ms) >= BS_OUT_STREAM_LOST_MS) {
			s->voice_open = false;
			make_idle(out);
			o->st.idle++;
		} else {
			memset(out, 0, sizeof(*out));
			out->kind = DMR_BURST_VOICE;
			out->sync_override = DMR_SYNC_BS;
			out->voice_index = (uint8_t)((s->last_index + 1u) % 6u);
			out->len = DMR_VOICE_BYTES;
			memcpy(out->payload, SILENCE, DMR_VOICE_BYTES);
			s->last_index = out->voice_index;
			if (out->voice_index == 0) {
				*emb_lc = s->lc_valid ? s->lc : NULL;
			}
			o->st.silence++;
		}
	} else {
		if (s->playing) {
			s->playing = false;	// data ran dry: idle until the prebuffer again
		}
		make_idle(out);
		o->st.idle++;
	}

	// The CACH before this burst (split-duplex.md 5.2). Its AT speaks for the inbound slot a slot later (-1 4.5 figure
	// 4.8): with aligned timing (-1 5.1.1 table 5.1) that's the other channel's
	uint8_t bit = (ts == 1) ? 1u : 2u;
	uint8_t other = (uint8_t)(bit ^ 3u);
	bool busy = out->kind != DMR_BURST_IDLE;

	o->at_auto = (uint8_t)((o->at_auto & (uint8_t)~bit) | (busy ? bit : 0u));
	uint8_t at = (uint8_t)(((bsOutboundAtMask(o, now_ms) & other) != 0) ||
			       (((o->at_mode == BS_AT_MODE_OR) || !o->at_set) && ((o->at_auto & other) != 0)));
	uint8_t row = (uint8_t)(sidx % 4u);
	static const uint8_t LCSS[4] = { DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST };

	cach->at = at;
	cach->tc = (ts == 1) ? 0u : 1u;
	cach->lcss = LCSS[row];
	const uint32_t *set;

	cach->payload = slc_row(o, sidx, &set);
	cach->bits = dmrCachBuild(cach->at, cach->tc, cach->lcss, cach->payload);
	cach->slc = 0;
	cach->slc_crc = 0;
	if ((row == 0) && dmrShortLcDecode(set, &cach->slc)) {	// the HR_C6000 takes the short LC whole (page 2 0x24)
		cach->slc_crc = dmrShortLcCrc8(cach->slc);
	}
	o->at_on_air = (uint8_t)((o->at_on_air & (uint8_t)~other) | (at ? other : 0u));
	o->st.cach++;
	s->n++;
}

static void put_sync(uint8_t *burst, uint64_t sync)
{
	for (int i = 0; i < 48; i++) {
		uint8_t m = (uint8_t)(0x80u >> ((SYNC_FIRST_BIT + i) % 8));

		if (((sync >> (47 - i)) & 1u) != 0) {
			burst[(SYNC_FIRST_BIT + i) / 8] |= m;
		} else {
			burst[(SYNC_FIRST_BIT + i) / 8] &= (uint8_t)~m;
		}
	}
}

// A voice burst (ETSI TS 102 361-1 9.1): 108 + 108 AMBE bits around the middle 48, which is the BS voice sync on A,
// else the EMB (8 + 8 bits, LCSS by position) around 32 embedded bits: the fragments of emb_lc on B-E, null on F
static void raw_voice(const dmr_tx_burst_t *b, uint8_t cc, const uint8_t *emb_lc, uint8_t *burst)
{
	const uint8_t *v = (b->len >= DMR_VOICE_BYTES) ? b->payload : SILENCE;
	uint8_t index = (uint8_t)(b->voice_index % 6u), middle[6] = { 0, 0, 0, 0, 0, 0 };

	if (index > 0) {
		uint16_t emb = dmrFecEmbEncode(cc, false, dmr_emb_lcss(index));

		middle[0] = (uint8_t)(emb >> 8);
		if ((index <= 4) && (emb_lc != NULL)) {
			uint8_t frag[4][4];

			dmr_emb_lc_encode(emb_lc, frag);
			memcpy(middle + 1, frag[index - 1], 4);
		}
		middle[5] = (uint8_t)emb;
	}
	memcpy(burst, v, 13);
	burst[13] = (uint8_t)((v[13] & 0xF0) | (middle[0] >> 4));
	for (int i = 0; i < 5; i++) {
		burst[14 + i] = (uint8_t)((middle[i] << 4) | (middle[i + 1] >> 4));
	}
	burst[19] = (uint8_t)((middle[5] << 4) | (v[13] & 0x0F));
	memcpy(burst + 20, v + 14, 13);
	if (index == 0) {
		put_sync(burst, BS_VOICE_SYNC);
	}
}

// A data / control burst through the hotspot's frame coder (BPTC, trellis, rate 1), BS data sync. A voice LC header
// or terminator without its parity (the core's own terminator) gets RS(12,9) here; anything it can't code: false
static bool raw_data(const dmr_tx_burst_t *b, uint8_t cc, uint8_t *burst)
{
	dmrBurst_t d;

	memset(&d, 0, sizeof(d));
	d.dataType = b->kind;
	d.length = (b->len <= DMR_BURST_PAYLOAD_MAX) ? b->len : DMR_BURST_PAYLOAD_MAX;
	memcpy(d.payload, b->payload, d.length);
	if (!b->host_crc && (b->len >= DMR_LC_BYTES) &&
	    ((b->kind == DMR_BURST_VOICE_LC_HEADER) || (b->kind == DMR_BURST_TERMINATOR_LC))) {
		dmr_lc_encode_full(b->payload, (b->kind == DMR_BURST_VOICE_LC_HEADER) ? DMR_RS_MASK_VOICE_HEADER :
				   DMR_RS_MASK_TERMINATOR, d.payload);
		d.length = DMR_LC_FULL_BYTES;
	} else if (!b->host_crc && (b->kind != DMR_BURST_IDLE)) {
		return false;		// the chip would have added the CRC: the core's outbound never asks for that
	}
	if (b->kind == DMR_BURST_IDLE) {
		memcpy(d.payload, (b->len >= 12) ? b->payload : BS_IDLE_INFO, 12);
		d.length = 12;
	}
	if (!dmrDataBurstToFrame(&d, cc, burst)) {
		return false;
	}
	put_sync(burst, BS_DATA_SYNC);
	return true;
}

bool bsOutboundRawSlot(const dmr_tx_burst_t *b, const dmr_cach_t *cach, int8_t ts, uint8_t cc, const uint8_t *emb_lc,
		       uint8_t *out)
{
	uint8_t *burst = out + 3;
	// Without the core's CACH (a driver underrun): TC from the slot, AT idle, a null payload
	uint32_t c = (cach != NULL) ? cach->bits : dmrCachBuild(0, (ts == 2) ? 1u : 0u, DMR_LCSS_LAST, 0);
	bool ok = true;

	memset(out, 0, BS_RAW_SLOT_BYTES);
	out[0] = (uint8_t)(c >> 16);
	out[1] = (uint8_t)(c >> 8);
	out[2] = (uint8_t)c;
	if (b->kind == DMR_BURST_VOICE) {
		raw_voice(b, cc, emb_lc, burst);
	} else if (!raw_data(b, cc, burst)) {
		dmr_tx_burst_t idle;

		memset(&idle, 0, sizeof(idle));
		idle.kind = DMR_BURST_IDLE;
		memset(burst, 0, DMR_FEC_BURST_BYTES);
		(void)raw_data(&idle, cc, burst);
		ok = false;
	}
	return ok;
}
