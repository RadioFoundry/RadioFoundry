// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III layer 3 PDUs (see t3_pdu.h). Field order and widths from ETSI TS 102 361-4 tables 7.9 - 7.34.
#include <string.h>
#include "dmr/trunk/t3_pdu.h"
#include "dmr/call/dmr_lc.h"

// --- Bit access, most significant bit of octet 0 first ---

typedef struct {
	uint8_t *b;
	unsigned pos;
} bw_t;

static void put(bw_t *w, unsigned width, uint32_t v)
{
	for (unsigned i = 0; i < width; i++) {
		unsigned p = w->pos++;
		uint8_t m = (uint8_t)(0x80u >> (p & 7u));
		if ((v >> (width - 1u - i)) & 1u) {
			w->b[p >> 3] |= m;
		} else {
			w->b[p >> 3] &= (uint8_t)~m;
		}
	}
}

typedef struct {
	const uint8_t *b;
	unsigned pos;
} br_t;

static uint32_t get(br_t *r, unsigned width)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < width; i++) {
		unsigned p = r->pos++;
		v = (v << 1) | ((r->b[p >> 3] >> (7u - (p & 7u))) & 1u);
	}
	return v;
}

static bool flag(br_t *r)
{
	return get(r, 1) != 0;
}

// --- CRC ---

void t3_crc_append(uint8_t pdu[T3_CSBK_BYTES], uint16_t mask)
{
	uint16_t crc = dmr_crc_ccitt(pdu, 10) ^ mask;
	pdu[10] = (uint8_t)(crc >> 8);
	pdu[11] = (uint8_t)crc;
}

bool t3_crc_ok(const uint8_t pdu[T3_CSBK_BYTES], uint16_t mask)
{
	uint16_t crc = dmr_crc_ccitt(pdu, 10) ^ mask;
	return pdu[10] == (uint8_t)(crc >> 8) && pdu[11] == (uint8_t)crc;
}

t3_ack_class_t t3_ack_class(uint8_t reason)
{
	switch (reason >> 4) {
	case 0x4: case 0x6:
		return T3_ACK;
	case 0x0: case 0x1: case 0x2: case 0x3:
		return T3_NACK;
	case 0xA:
		return T3_QACK;
	case 0xE:
		return T3_WACK;
	default:
		return T3_ACK_UNKNOWN;
	}
}

static bool is_grant(uint8_t op)
{
	// The multi-item data grants have the single-item layout (table 7.13); the duplex ones (53, 54) differ in two bits
	return op >= T3_OP_PV_GRANT && op <= T3_OP_TD_GRANT_MI;
}

static bool is_ack(uint8_t op)
{
	return op >= T3_OP_C_ACKD && op <= T3_OP_P_ACKU;
}

// --- Parse ---

bool t3_csbk_parse(const uint8_t pdu[T3_CSBK_BYTES], t3_csbk_t *out)
{
	memset(out, 0, sizeof(*out));
	br_t r = { pdu, 0 };
	out->lb = flag(&r);
	out->pf = flag(&r);
	out->op = (uint8_t)get(&r, 6);
	out->fid = (uint8_t)get(&r, 8);
	memcpy(out->raw, pdu + 2, sizeof(out->raw));
	if (out->fid != 0) {
		return false;
	}
	uint8_t op = out->op;
	if (op == T3_OP_C_ALOHA) {
		t3_aloha_t *a = &out->u.aloha;
		get(&r, 1);
		a->tsccas = flag(&r);
		a->site_ts_sync = flag(&r);
		a->version = (uint8_t)get(&r, 3);
		a->offset = flag(&r);
		a->active_conn = flag(&r);
		a->mask = (uint8_t)get(&r, 5);
		a->service_fn = (uint8_t)get(&r, 2);
		a->nrand_wait = (uint8_t)get(&r, 4);
		a->reg = flag(&r);
		a->backoff = (uint8_t)get(&r, 4);
		a->syscode = (uint16_t)get(&r, 16);
		a->ms = get(&r, 24);
		return true;
	}
	if (is_grant(op)) {
		t3_grant_t *g = &out->u.grant;
		g->lpcn = (uint16_t)get(&r, 12);
		g->slot = flag(&r) ? 2 : 1;
		bool b13 = flag(&r);
		if (op == T3_OP_TV_GRANT || op == T3_OP_BTV_GRANT) {
			g->late_entry = b13;
		} else if (op == T3_OP_PD_GRANT || op == T3_OP_TD_GRANT || op == T3_OP_PD_GRANT_MI ||
			   op == T3_OP_TD_GRANT_MI) {
			g->hi_rate = b13;
		}
		g->emergency = flag(&r);
		if (op == T3_OP_PV_GRANT_DX || op == T3_OP_PD_GRANT_DX) {
			// Call Direction in place of Offset: duplex payload channels always use offset timing
			g->dx_calling = flag(&r);
			g->offset = true;
		} else {
			g->offset = flag(&r);
		}
		g->dst = get(&r, 24);
		g->src = get(&r, 24);
		return true;
	}
	switch (op) {
	case T3_OP_C_BCAST: {
		t3_bcast_t *b = &out->u.bcast;
		b->type = (uint8_t)get(&r, 5);
		b->parms1 = (uint16_t)get(&r, 14);
		b->reg = flag(&r);
		b->backoff = (uint8_t)get(&r, 4);
		b->syscode = (uint16_t)get(&r, 16);
		b->parms2 = get(&r, 24);
		return true;
	}
	case T3_OP_C_AHOY:
	case T3_OP_C_ACKVIT: {	// the same layout (table 6.21: ALS and G/I reserved)
		t3_ahoy_t *a = &out->u.ahoy;
		a->options = (uint8_t)get(&r, 7);
		a->kind_flag = flag(&r);
		a->als = flag(&r);
		a->group = flag(&r);
		a->blocks = (uint8_t)get(&r, 2);
		a->kind = (uint8_t)get(&r, 4);
		a->dst = get(&r, 24);
		a->src = get(&r, 24);
		return true;
	}
	case T3_OP_C_RAND: {
		t3_rand_t *q = &out->u.rand;
		q->options = (uint8_t)get(&r, 7);
		q->proxy = flag(&r);
		q->extra = (uint8_t)get(&r, 4);
		q->kind = (uint8_t)get(&r, 4);
		q->dst = get(&r, 24);
		q->src = get(&r, 24);
		return true;
	}
	case T3_OP_P_CLEAR: {
		t3_clear_t *c = &out->u.clear;
		c->lpcn = (uint16_t)get(&r, 12);
		get(&r, 3);
		c->group = flag(&r);
		c->dst = get(&r, 24);
		c->src = get(&r, 24);
		return true;
	}
	case T3_OP_C_MOVE: {
		t3_move_t *m = &out->u.move;
		get(&r, 9);
		m->mask = (uint8_t)get(&r, 5);
		get(&r, 5);
		m->reg = flag(&r);
		m->backoff = (uint8_t)get(&r, 4);
		get(&r, 4);
		m->lpcn = (uint16_t)get(&r, 12);
		m->ms = get(&r, 24);
		return true;
	}
	case T3_OP_P_PROTECT: {
		t3_protect_t *p = &out->u.protect;
		get(&r, 12);
		p->kind = (uint8_t)get(&r, 3);
		p->group = flag(&r);
		p->dst = get(&r, 24);
		p->src = get(&r, 24);
		return true;
	}
	case T3_OP_P_MAINT: {
		t3_maint_t *m = &out->u.maint;
		get(&r, 12);
		m->kind = (uint8_t)get(&r, 3);
		get(&r, 1);
		m->dst = get(&r, 24);
		m->src = get(&r, 24);
		return true;
	}
	default:
		break;
	}
	if (is_ack(op)) {
		t3_ack_t *a = &out->u.ack;
		a->info = (uint8_t)get(&r, 7);
		a->reason = (uint8_t)get(&r, 8);
		get(&r, 1);
		a->dst = get(&r, 24);
		a->src = get(&r, 24);
		return true;
	}
	return false;
}

// --- Build ---

bool t3_csbk_build(const t3_csbk_t *in, uint16_t mask, uint8_t pdu[T3_CSBK_BYTES])
{
	memset(pdu, 0, T3_CSBK_BYTES);
	bw_t w = { pdu, 0 };
	uint8_t op = in->op;
	put(&w, 1, in->lb);
	put(&w, 1, in->pf);
	put(&w, 6, op);
	put(&w, 8, 0);
	if (op == T3_OP_C_ALOHA) {
		const t3_aloha_t *a = &in->u.aloha;
		put(&w, 1, 0);
		put(&w, 1, a->tsccas);
		put(&w, 1, a->site_ts_sync);
		put(&w, 3, a->version);
		put(&w, 1, a->offset);
		put(&w, 1, a->active_conn);
		put(&w, 5, a->mask);
		put(&w, 2, a->service_fn);
		put(&w, 4, a->nrand_wait);
		put(&w, 1, a->reg);
		put(&w, 4, a->backoff);
		put(&w, 16, a->syscode);
		put(&w, 24, a->ms);
	} else if (is_grant(op)) {
		const t3_grant_t *g = &in->u.grant;
		put(&w, 12, g->lpcn);
		put(&w, 1, g->slot == 2);
		if (op == T3_OP_TV_GRANT || op == T3_OP_BTV_GRANT) {
			put(&w, 1, g->late_entry);
		} else if (op == T3_OP_PD_GRANT || op == T3_OP_TD_GRANT || op == T3_OP_PD_GRANT_MI ||
			   op == T3_OP_TD_GRANT_MI) {
			put(&w, 1, g->hi_rate);
		} else {
			put(&w, 1, 0);
		}
		put(&w, 1, g->emergency);
		put(&w, 1, op == T3_OP_PV_GRANT_DX || op == T3_OP_PD_GRANT_DX ? g->dx_calling : g->offset);
		put(&w, 24, g->dst);
		put(&w, 24, g->src);
	} else if (op == T3_OP_C_BCAST) {
		const t3_bcast_t *b = &in->u.bcast;
		put(&w, 5, b->type);
		put(&w, 14, b->parms1);
		put(&w, 1, b->reg);
		put(&w, 4, b->backoff);
		put(&w, 16, b->syscode);
		put(&w, 24, b->parms2);
	} else if (op == T3_OP_C_AHOY || op == T3_OP_C_ACKVIT) {
		const t3_ahoy_t *a = &in->u.ahoy;
		put(&w, 7, a->options);
		put(&w, 1, a->kind_flag);
		put(&w, 1, a->als);
		put(&w, 1, a->group);
		put(&w, 2, a->blocks);
		put(&w, 4, a->kind);
		put(&w, 24, a->dst);
		put(&w, 24, a->src);
	} else if (op == T3_OP_C_RAND) {
		const t3_rand_t *q = &in->u.rand;
		put(&w, 7, q->options);
		put(&w, 1, q->proxy);
		put(&w, 4, q->extra);
		put(&w, 4, q->kind);
		put(&w, 24, q->dst);
		put(&w, 24, q->src);
	} else if (op == T3_OP_P_CLEAR) {
		const t3_clear_t *c = &in->u.clear;
		put(&w, 12, c->lpcn);
		put(&w, 3, 0);
		put(&w, 1, c->group);
		put(&w, 24, c->dst);
		put(&w, 24, c->src);
	} else if (op == T3_OP_C_MOVE) {
		const t3_move_t *m = &in->u.move;
		put(&w, 9, 0);
		put(&w, 5, m->mask);
		put(&w, 5, 0);
		put(&w, 1, m->reg);
		put(&w, 4, m->backoff);
		put(&w, 4, 0);
		put(&w, 12, m->lpcn);
		put(&w, 24, m->ms);
	} else if (op == T3_OP_P_PROTECT) {
		const t3_protect_t *p = &in->u.protect;
		put(&w, 12, 0);
		put(&w, 3, p->kind);
		put(&w, 1, p->group);
		put(&w, 24, p->dst);
		put(&w, 24, p->src);
	} else if (op == T3_OP_P_MAINT) {
		const t3_maint_t *m = &in->u.maint;
		put(&w, 12, 0);
		put(&w, 3, m->kind);
		put(&w, 1, 0);
		put(&w, 24, m->dst);
		put(&w, 24, m->src);
	} else if (is_ack(op)) {
		const t3_ack_t *a = &in->u.ack;
		put(&w, 7, a->info);
		put(&w, 8, a->reason);
		put(&w, 1, 0);
		put(&w, 24, a->dst);
		put(&w, 24, a->src);
	} else {
		return false;
	}
	t3_crc_append(pdu, mask);
	return true;
}

// --- MBC appended block with CdefParms ---

uint32_t t3_cdef_hz(uint16_t mhz, uint16_t khz125)
{
	return (uint32_t)mhz * 1000000u + (uint32_t)khz125 * 125u;
}

static void hz_split(uint32_t hz, uint16_t *mhz, uint16_t *k)
{
	*mhz = (uint16_t)(hz / 1000000u);
	*k = (uint16_t)((hz % 1000000u) / 125u);
}

bool t3_cdef_parse(const uint8_t blk[T3_CSBK_BYTES], uint8_t op, t3_cdef_t *out)
{
	memset(out, 0, sizeof(*out));
	br_t r = { blk, 0 };
	if (!flag(&r)) {
		return false;	// an intermediate block: this module knows only two-block MBCs
	}
	get(&r, 1);
	if (get(&r, 6) != op) {
		return false;
	}
	if (op == T3_OP_C_BCAST) {
		get(&r, 8);
	} else {
		get(&r, 4);
		out->cc = (uint8_t)get(&r, 4);
	}
	out->cdeftype = (uint8_t)get(&r, 4);
	get(&r, 2);
	if (out->cdeftype != 0) {
		return false;
	}
	out->chan = (uint16_t)get(&r, 12);
	uint16_t m = (uint16_t)get(&r, 10);
	uint16_t k = (uint16_t)get(&r, 13);
	out->tx_hz = t3_cdef_hz(m, k);
	m = (uint16_t)get(&r, 10);
	k = (uint16_t)get(&r, 13);
	out->rx_hz = t3_cdef_hz(m, k);
	return true;
}

void t3_cdef_build(const t3_cdef_t *in, uint8_t op, uint8_t blk[T3_CSBK_BYTES])
{
	memset(blk, 0, T3_CSBK_BYTES);
	bw_t w = { blk, 0 };
	put(&w, 1, 1);
	put(&w, 1, 0);
	put(&w, 6, op);
	if (op == T3_OP_C_BCAST) {
		put(&w, 8, 0);
	} else {
		put(&w, 4, 0);
		put(&w, 4, in->cc);
	}
	put(&w, 4, in->cdeftype);
	put(&w, 2, 0);
	uint16_t m, k;
	put(&w, 12, in->chan);
	hz_split(in->tx_hz, &m, &k);
	put(&w, 10, m);
	put(&w, 13, k);
	hz_split(in->rx_hz, &m, &k);
	put(&w, 10, m);
	put(&w, 13, k);
	t3_crc_append(blk, 0);
}

// --- Short LC system parameters ---

bool t3_sysparms_parse(uint32_t slc, t3_sysparms_t *out)
{
	uint8_t slco = (uint8_t)((slc >> 24) & 0xFu);
	if (slco != T3_SLCO_C_SYS_PARMS && slco != T3_SLCO_P_SYS_PARMS) {
		return false;
	}
	out->control = slco == T3_SLCO_C_SYS_PARMS;
	out->model = (uint8_t)((slc >> 22) & 0x3u);
	out->netsite = (uint16_t)((slc >> 10) & 0xFFFu);
	out->flag = ((slc >> 9) & 1u) != 0;
	out->csc = (uint16_t)(slc & 0x1FFu);
	return true;
}

uint32_t t3_sysparms_build(const t3_sysparms_t *in)
{
	return (uint32_t)(in->control ? T3_SLCO_C_SYS_PARMS : T3_SLCO_P_SYS_PARMS) << 24 |
	       (uint32_t)(in->model & 0x3u) << 22 | (uint32_t)(in->netsite & 0xFFFu) << 10 |
	       (uint32_t)(in->flag ? 1u : 0u) << 9 | (in->csc & 0x1FFu);
}

// --- UDT ---

void t3_udt_build(const t3_udt_t *h, uint8_t out[T3_CSBK_BYTES])
{
	uint8_t blocks = h->blocks < 1u ? 1u : (h->blocks > T3_UDT_BLOCKS_MAX ? T3_UDT_BLOCKS_MAX : h->blocks);
	out[0] = (uint8_t)((h->group ? 0x80u : 0u) | (h->a ? 0x40u : 0u) | (h->emergency ? 0x20u : 0u) |
			   (h->opt ? 0x10u : 0u));	// DPF 0000: UDT
	out[1] = (uint8_t)(h->format & 0x0Fu);		// SAP 0000: UDT
	out[2] = (uint8_t)(h->dst >> 16);
	out[3] = (uint8_t)(h->dst >> 8);
	out[4] = (uint8_t)h->dst;
	out[5] = (uint8_t)(h->src >> 16);
	out[6] = (uint8_t)(h->src >> 8);
	out[7] = (uint8_t)h->src;
	out[8] = (uint8_t)(((h->pad & 0x1Fu) << 3) | ((blocks - 1u) & 3u));
	out[9] = (uint8_t)((h->sf ? 0x80u : 0u) | (h->op & 0x3Fu));
	t3_crc_append(out, T3_CRC_MASK_DATA_HDR);
}

bool t3_udt_parse(const uint8_t in[T3_CSBK_BYTES], t3_udt_t *h)
{
	if (!t3_crc_ok(in, T3_CRC_MASK_DATA_HDR) || (in[0] & 0x0Fu) != 0 || (in[1] & 0xF0u) != 0) {
		return false;
	}
	uint8_t op = in[9] & 0x3Fu;
	if (op != T3_OP_C_UDTHD && op != T3_OP_C_UDTHU && op != T3_OP_C_DGNAHD) {
		return false;
	}
	h->op = op;
	h->group = (in[0] & 0x80u) != 0;
	h->a = (in[0] & 0x40u) != 0;
	h->emergency = (in[0] & 0x20u) != 0;
	h->opt = (in[0] & 0x10u) != 0;
	h->format = in[1] & 0x0Fu;
	h->dst = ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 8) | in[4];
	h->src = ((uint32_t)in[5] << 16) | ((uint32_t)in[6] << 8) | in[7];
	h->pad = in[8] >> 3;
	h->blocks = (uint8_t)((in[8] & 3u) + 1u);
	h->sf = (in[9] & 0x80u) != 0;
	return true;
}

uint8_t t3_udt_pack(const uint8_t *data, uint8_t len, uint8_t fill, uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK],
		    uint8_t *pad_nibbles)
{
	if (len == 0 || len > T3_UDT_MAX) {
		return 0;
	}
	uint8_t n = (uint8_t)((len + 2u + T3_UDT_BLOCK - 1u) / T3_UDT_BLOCK);
	uint8_t room = (uint8_t)(n * T3_UDT_BLOCK - 2u);
	uint8_t *flat = &blk[0][0];
	memcpy(flat, data, len);
	memset(flat + len, fill, (size_t)(room - len));
	uint16_t crc = dmr_crc_ccitt(flat, room);
	flat[room] = (uint8_t)(crc >> 8);
	flat[room + 1u] = (uint8_t)crc;
	*pad_nibbles = (uint8_t)(2u * (room - len));
	return n;
}

bool t3_udt_crc_ok(const uint8_t blk[][T3_UDT_BLOCK], uint8_t n)
{
	if (n < 1u || n > T3_UDT_BLOCKS_MAX) {
		return false;
	}
	const uint8_t *flat = &blk[0][0];
	uint8_t room = (uint8_t)(n * T3_UDT_BLOCK - 2u);
	uint16_t crc = dmr_crc_ccitt(flat, room);
	return flat[room] == (uint8_t)(crc >> 8) && flat[room + 1u] == (uint8_t)crc;
}
