// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR receive (dpmr.h): the framer's table for the mode and its hooks. Section numbers are ETSI TS 102 490's.
//
// Units, each opened by a pattern of the profile, so a lock is always on a unit's first symbol:
//   a header from its FS1 (156 symbols),
//   a pair of payload frames from the FS2 of frame 1 or 3 (384 symbols, in two pieces of one frame each),
//   an end frame from its FS3 (48 symbols),
//   and between two headers the second's preamble (36 symbols), which no pattern opens: the head hook knows it by
//   what it holds and where it is. Taking it into the header before it instead would need the header's length before
//   the 12 symbols behind it are in, and a unit can't end before the symbols the hook has seen.
// Payload frames 2 and 4 open with the channel code instead of FS2. The code belongs to the channel, not the mode, so
// it can't be a pattern of a profile shared by every channel; cutting pairs puts an FS2 at the head of every payload
// unit instead, and the pair's second frame is placed by counting, as the radio that sent it placed it.
//
// FS2 and FS3 are twelve symbols and also turn up in payload and noise; a code of table 6.1 is two symbols from FS2
// (0x392). The head hook takes a pair only when the CCH behind its FS2 decodes and names frame 1 or 3 (a channel code
// read as FS2 is followed by the CCH of frame 2 or 4), and an end frame only inside a call.
#include "dpmr.h"

#include "../air/air_syms.h"
#include "../../hal/hal_radio.h"

// Pairs in a row taken with neither a sync nor a readable CCH before the call counts as gone (with the framer's
// lost_frames: a fade of up to 160 ms keeps the call)
#define COAST_PAIRS	1
// A preamble's +3 +3 -3 -3 in the first 12 symbols of a unit behind a header: this many right says another header
// follows
#define PRE_MIN		9

static uint8_t preamble_right(const uint8_t *hard)
{
	uint8_t n = 0;

	for (uint16_t i = 0; i < DPMR_SYNC_SYMS; i++)
		n += air_sym_get(hard, i) == ((DPMR_PREAMBLE_BYTE >> (6u - 2u * (i & 3u))) & 3u);
	return n;
}

static uint16_t take(dpmr_rx_t *rx, uint8_t kind, uint16_t len)
{
	rx->kind = kind;
	rx->locked = true;
	return len;
}

static uint16_t reject(dpmr_rx_t *rx)
{
	rx->rejects++;
	rx->locked = false;
	return 0;
}

static void unit_from(dpmr_rx_unit_t *u, const air_frame_t *f, uint8_t kind)
{
	u->kind = kind;
	u->flags = f->flags;
	u->second = false;
	u->t_us = f->t_us;
	u->rssi_dbm = f->rssi_dbm;
	u->hard = f->hard;
	u->cc = 0;
	u->cc_bad = 12;
}

// Called at the unit's first 12 symbols (its sync field), and again as the unit's kind needs: 24 for an FS1 (the
// framer checks it at its last symbol), 48 for a pair's CCH, 156 for a header's information
static uint16_t rx_head(const air_frame_t *f, void *ctx)
{
	dpmr_rx_t *rx = ctx;
	uint8_t id = (f->flags & AIR_FRAME_NO_SYNC) ? 0xFF : f->sync_id;
	dpmr_rx_unit_t *u = &rx->head;

	if (id == DPMR_SYNC_FS1) {
		if (f->nsyms < DPMR_HDR_SYMS)
			return AIR_HEAD_AGAIN | DPMR_HDR_SYMS;
		// A 24-symbol pattern seldom turns up by chance: a header whose two copies both fail is still one inside a
		// call (a fade), and keeps the count
		if (!dpmr_header_get(f->hard, &u->u.hi, &u->res) && !rx->locked)
			return reject(rx);
		return take(rx, DPMR_UNIT_HEADER, DPMR_HDR_SYMS);
	}
	if (id == DPMR_SYNC_FS3)
		return rx->locked ? take(rx, DPMR_UNIT_END, DPMR_END_SYMS) : reject(rx);
	if (id != DPMR_SYNC_FS2) {
		if (!rx->locked)
			return reject(rx);
		// Behind a header, another header's preamble is a unit of its own: the header after it then opens with its
		// FS1 where the framer looks for one, and a run of extended wake-up headers stays one lock
		if (rx->last == DPMR_UNIT_HEADER && f->nsyms <= DPMR_SYNC_SYMS && preamble_right(f->hard) >= PRE_MIN)
			return take(rx, DPMR_UNIT_PREAMBLE, DPMR_PREAMBLE_SYMS);
		if (f->nsyms < DPMR_FS1_SYMS)
			return AIR_HEAD_AGAIN | DPMR_FS1_SYMS;		// an FS1 (behind a preamble), or a pair whose FS2 faded
	}
	if (f->nsyms < DPMR_HEAD_SYMS)
		return AIR_HEAD_AGAIN | DPMR_HEAD_SYMS;
	// A pair: by its FS2, or by counting inside a call
	unit_from(u, f, DPMR_UNIT_PAYLOAD);
	u->ok = dpmr_cch_get(f->hard, &u->u.cch, &u->res);
	if (u->ok && (u->u.cch.fn & 1u)) {
		// The CCH of frame 2 or 4: what opened this unit was a channel code, not FS2. The search goes on, and finds
		// the real FS2 a frame further on
		u->ok = false;
		if (!rx->locked || id == DPMR_SYNC_FS2)
			return reject(rx);
	}
	if (u->ok || (id == DPMR_SYNC_FS2 && rx->locked)) {
		rx->coasted = 0;
		return take(rx, DPMR_UNIT_PAYLOAD, DPMR_PAIR_SYMS);
	}
	// Neither its FS2 nor its CCH read: a fade inside a call keeps its place for a while
	if (rx->locked && (rx->last == DPMR_UNIT_PAYLOAD || rx->last == DPMR_UNIT_HEADER) && rx->coasted < COAST_PAIRS) {
		rx->coasted++;
		return take(rx, DPMR_UNIT_PAYLOAD, DPMR_PAIR_SYMS);
	}
	return reject(rx);
}

static void rx_frame(const air_frame_t *f, void *ctx)
{
	dpmr_rx_t *rx = ctx;
	dpmr_rx_unit_t u;

	switch (rx->kind) {
	case DPMR_UNIT_PREAMBLE:
		rx->last = rx->kind;
		return;			// nothing in it for the call
	case DPMR_UNIT_HEADER:
		unit_from(&u, f, DPMR_UNIT_HEADER);
		u.ok = dpmr_header_get(f->hard, &u.u.hi, &u.res);
		u.cc = dpmr_cc_from_air(dpmr_get(f->hard, 2u * DPMR_HDR_CC_SYM, 24u), &u.cc_bad);
		break;
	case DPMR_UNIT_END:
		unit_from(&u, f, DPMR_UNIT_END);
		u.ok = dpmr_end_get(f->hard, &u.u.end, &u.res);
		break;
	default:
		if (f->offset == 0) {
			// The pair's first frame: its CCH was read by the head hook
			u = rx->head;
			u.flags = f->flags;
			u.hard = f->hard;
		} else {
			unit_from(&u, f, DPMR_UNIT_PAYLOAD);
			u.second = true;
			u.flags = (uint8_t)(f->flags | AIR_FRAME_NO_SYNC);	// placed by counting: its sync is the code
			u.ok = dpmr_cch_get(f->hard, &u.u.cch, &u.res);
			u.cc = dpmr_cc_from_air(dpmr_sync_field(f->hard), &u.cc_bad);
		}
		break;
	}
	if (!(f->flags & AIR_FRAME_MORE))
		rx->last = rx->kind;
	if (rx->unit)
		rx->unit(&u, rx->ctx);
}

static void rx_lost(void *ctx)
{
	dpmr_rx_t *rx = ctx;

	rx->locked = false;
	rx->coasted = 0;
	rx->last = DPMR_UNIT_END;
	if (rx->lost)
		rx->lost(rx->ctx);
}

// 6.1. FS1 is the one 24-symbol pattern: a chip correlator takes it (all outer), but it opens only headers, so nothing
// recurs at 24 symbols inside a call. FS2 opens every second payload frame and so every pair; FS3 the end frame. One
// of their twelve symbols may be wrong: with two, FS2 is found one symbol into FS1 and FS3 across a preamble's end
// (a lock there would cost the header), and two of table 6.1's codes read as FS2. A pair whose FS2 is lost is still
// placed by counting and read by its CCH (the head hook)
const air_mode_t dpmr_air_mode = {
	.name = "dPMR",
	.profile = {
		.sym_rate = 2400,
		.levels = 4,
		.shape = AIR_SHAPE_RRC,		// 12.2: root raised cosine, roll-off 0.2
		.shape_param = 20,
		.dev_outer_hz = 1050,		// 12.2: +-1050 Hz outer, +-350 Hz inner
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,		// the narrowest the radios have; the channel is 6.25 kHz
		.sync_every_syms = 0,		// FS1 does not recur inside a call
		.sync_any_syms = DPMR_PAIR_SYMS,	// FS2 opens every pair
		.idle = DPMR_PREAMBLE_BYTE,	// +3 +3 -3 -3
		.nsyncs = 3,
		.sync = {
			{ .id = DPMR_SYNC_FS2, .nsyms = 12, .max_errs = 1, .frame_at = 0, .syms = { 0x5F, 0xF7, 0x7D } },
			{ .id = DPMR_SYNC_FS3, .nsyms = 12, .max_errs = 1, .frame_at = 0, .syms = { 0x7D, 0xDF, 0xF5 } },
			{ .id = DPMR_SYNC_FS1, .nsyms = 24, .max_errs = 3, .frame_at = 0,
			  .syms = { 0x57, 0xFF, 0x5F, 0x75, 0xD5, 0x77 } },
		},
	},
	.frame_syms = DPMR_PAIR_SYMS,
	.head_syms = DPMR_SYNC_SYMS,
	.lost_frames = 2,
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

void dpmr_rx_init(dpmr_rx_t *rx, void (*unit)(const dpmr_rx_unit_t *u, void *ctx), void (*lost)(void *ctx), void *ctx)
{
	rx->unit = unit;
	rx->lost = lost;
	rx->ctx = ctx;
	rx->locked = false;
	rx->kind = rx->last = DPMR_UNIT_END;
	rx->coasted = 0;
	rx->rejects = 0;
}
