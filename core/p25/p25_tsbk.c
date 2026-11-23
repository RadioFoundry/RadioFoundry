// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 trunking signaling blocks and their messages (p25_tsbk.h, docs/spec/p25-trunking.md).
#include "../config.h"
#if CONFIG_P25_TRUNK	// the whole file
#include "p25_tsbk.h"

#include <string.h>

#include "../air/air_syms.h"
#include "../fec/trellis.h"

#define SS_EVERY	35	// information dibits between two status symbols
#define BLOCK_AT	112	// information bit of the first block: frame sync 48 + NID 64
#define BLOCK_BITS	196

// ── Geometry ──
// Information dibits to symbols: a status symbol after every 35
static uint16_t dibits_syms(uint16_t d)
{
	return d ? (uint16_t)(d + (d - 1) / SS_EVERY) : 0;
}

uint16_t p25_blocks_syms(uint8_t n)
{
	// Null bits fill the unit up to the next status symbol, which belongs to it
	uint16_t d = (uint16_t)(BLOCK_AT / 2 + n * (BLOCK_BITS / 2));

	d = (uint16_t)((d + SS_EVERY - 1) / SS_EVERY * SS_EVERY);
	return (uint16_t)(d + d / SS_EVERY);
}

uint16_t p25_block_end_sym(uint8_t k)
{
	return dibits_syms((uint16_t)(BLOCK_AT / 2 + (k + 1) * (BLOCK_BITS / 2)));
}

// ── CRC-CCITT of the first 80 bits: x^16 + x^12 + x^5 + 1, register from 0, the remainder inverted ──
uint16_t p25_tsbk_crc(const uint8_t blk[P25_TSBK_BYTES])
{
	uint16_t r = 0;

	for (unsigned i = 0; i < 10; i++) {
		r ^= (uint16_t)(blk[i] << 8);
		for (unsigned b = 0; b < 8; b++)
			r = (uint16_t)((r & 0x8000) ? (r << 1) ^ 0x1021 : r << 1);
	}
	return (uint16_t)~r;
}

void p25_tsbk_seal(uint8_t blk[P25_TSBK_BYTES])
{
	uint16_t c = p25_tsbk_crc(blk);

	blk[10] = (uint8_t)(c >> 8);
	blk[11] = (uint8_t)c;
}

// ── Blocks ──
int p25_block_decode(const uint8_t *unit, uint8_t k, uint8_t blk[P25_TSBK_BYTES])
{
	uint8_t in[FEC_TRELLIS_OUT], out[FEC_TRELLIS_IN];
	uint16_t at = (uint16_t)(BLOCK_AT + k * BLOCK_BITS);
	int errs;

	for (unsigned i = 0; i < FEC_TRELLIS_OUT; i++)
		in[i] = (uint8_t)p25_bits_get(unit, (uint16_t)(at + 2 * i), 2);
	errs = fec_trellis_decode(FEC_TRELLIS_HALF, in, out);
	for (unsigned i = 0; i < P25_TSBK_BYTES; i++)
		blk[i] = (uint8_t)(out[4 * i] << 6 | out[4 * i + 1] << 4 | out[4 * i + 2] << 2 | out[4 * i + 3]);
	return errs;
}

int p25_block_get(const uint8_t *unit, uint8_t k, uint8_t blk[P25_TSBK_BYTES])
{
	int errs = p25_block_decode(unit, k, blk);

	if (p25_tsbk_crc(blk) != (uint16_t)(blk[10] << 8 | blk[11]))
		return -1;
	return errs;
}

void p25_block_put(uint8_t *unit, uint8_t k, const uint8_t blk[P25_TSBK_BYTES])
{
	uint8_t in[FEC_TRELLIS_IN], out[FEC_TRELLIS_OUT];
	uint16_t at = (uint16_t)(BLOCK_AT + k * BLOCK_BITS);

	for (unsigned i = 0; i < FEC_TRELLIS_IN; i++)
		in[i] = (uint8_t)((blk[i / 4] >> (6 - 2 * (i & 3))) & 3);
	fec_trellis_encode(FEC_TRELLIS_HALF, in, out);
	for (unsigned i = 0; i < FEC_TRELLIS_OUT; i++)
		p25_bits_put(unit, (uint16_t)(at + 2 * i), 2, out[i]);
}

// ── Message fields: bit a of the arguments (octets 2-9), n bits, first bit first ──
static uint32_t arg(const uint8_t *blk, unsigned a, unsigned n)
{
	uint32_t v = 0;

	for (unsigned i = 0; i < n; i++)
		v = (v << 1) | air_bit_get(blk, (uint16_t)(16 + a + i));
	return v;
}

static void arg_put(uint8_t *blk, unsigned a, unsigned n, uint32_t v)
{
	for (unsigned i = 0; i < n; i++)
		air_bit_put(blk, (uint16_t)(16 + a + i), (uint8_t)((v >> (n - 1 - i)) & 1));
}

// The TDMA form's channel types: channels per carrier, and whether the type is the FDMA full-rate one (type 1) that
// the conventional voice path plays. Every other type is taken as not playable (unverified table)
static uint8_t tdma_slots(uint8_t type)
{
	static const uint8_t SLOTS[16] = { 1, 1, 1, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };

	return SLOTS[type & 15];
}

static int16_t offset(uint32_t sign, uint32_t mag)
{
	// Sign bit set: the subscriber transmits above its receive frequency
	return (int16_t)(sign ? (int32_t)mag : -(int32_t)mag);
}

bool p25_tsbk_parse(const uint8_t blk[P25_TSBK_BYTES], p25_tsbk_t *m)
{
	memset(m, 0, sizeof *m);
	m->last = (blk[0] & P25_TSBK_LB) != 0;
	m->prot = (blk[0] & P25_TSBK_P) != 0;
	m->opcode = blk[0] & 0x3F;
	m->mfid = blk[1];
	// 0x00 is the standard's; 0x01 is taken as the same table (unverified)
	if (m->mfid > 1) {
		m->kind = P25_TSBK_MFR;
		return false;
	}
	// A protected block's arguments are encrypted
	if (m->prot)
		return false;
	switch (m->opcode) {
	case P25_OP_GRP_V_GRANT:
		m->kind = P25_TSBK_GRANT;
		m->u.grant.has_svc = true;
		m->u.grant.svc = (uint8_t)arg(blk, 0, 8);
		m->u.grant.ch = (uint16_t)arg(blk, 8, 16);
		m->u.grant.tg = (uint16_t)arg(blk, 24, 16);
		m->u.grant.src = arg(blk, 40, 24);
		return true;
	case P25_OP_GRP_V_UPDT:
		m->kind = P25_TSBK_GRANT;
		m->u.grant.ch = (uint16_t)arg(blk, 0, 16);
		m->u.grant.tg = (uint16_t)arg(blk, 16, 16);
		m->u.grant.ch2 = (uint16_t)arg(blk, 32, 16);
		m->u.grant.tg2 = (uint16_t)arg(blk, 48, 16);
		return true;
	case P25_OP_GRP_V_UPDT_EXP:
		m->kind = P25_TSBK_GRANT;
		m->u.grant.has_svc = true;
		m->u.grant.svc = (uint8_t)arg(blk, 0, 8);
		m->u.grant.ch = (uint16_t)arg(blk, 16, 16);	// channel (T): the site transmits there
		m->u.grant.tg = (uint16_t)arg(blk, 48, 16);
		return true;
	case P25_OP_UU_V_GRANT:
	case P25_OP_UU_V_UPDT:
		m->kind = P25_TSBK_GRANT;
		m->u.grant.unit = true;
		m->u.grant.ch = (uint16_t)arg(blk, 0, 16);
		m->u.grant.dst = arg(blk, 16, 24);
		m->u.grant.src = arg(blk, 40, 24);
		return true;
	case P25_OP_IDEN_UP_TDMA:
	case P25_OP_IDEN_UP_VU:
	case P25_OP_IDEN_UP: {
		p25_iden_air_t *i = &m->u.iden;

		m->kind = P25_TSBK_IDEN;
		i->form = m->opcode;
		i->id = (uint8_t)arg(blk, 0, 4);
		i->spacing = (uint16_t)arg(blk, 22, 10);
		i->base = arg(blk, 32, 32);
		i->slots = 1;
		if (m->opcode == P25_OP_IDEN_UP) {
			i->bw_khz8 = (uint16_t)arg(blk, 4, 9);
			i->tx_off = offset(arg(blk, 13, 1), arg(blk, 14, 8));
		} else {
			uint8_t f = (uint8_t)arg(blk, 4, 4);

			i->tx_off = offset(arg(blk, 8, 1), arg(blk, 9, 13));
			if (m->opcode == P25_OP_IDEN_UP_VU) {
				i->bw_khz8 = f == 4 ? 50 : f == 5 ? 100 : 0;
			} else {
				i->type = f;
				i->slots = tdma_slots(f);
				i->tdma = f != 1;
			}
		}
		return true;
	}
	case P25_OP_NET_STS_BCST:
		m->kind = P25_TSBK_NET;
		m->u.net.lra = (uint8_t)arg(blk, 0, 8);
		m->u.net.wacn = arg(blk, 8, 20);
		m->u.net.sysid = (uint16_t)arg(blk, 28, 12);
		m->u.net.ch = (uint16_t)arg(blk, 40, 16);
		m->u.net.ssc = (uint8_t)arg(blk, 56, 8);
		return true;
	case P25_OP_RFSS_STS_BCST:
	case P25_OP_ADJ_STS_BCST:
		m->kind = m->opcode == P25_OP_RFSS_STS_BCST ? P25_TSBK_RFSS : P25_TSBK_ADJ;
		m->u.site.lra = (uint8_t)arg(blk, 0, 8);
		m->u.site.flags = (uint8_t)arg(blk, 8, 4);
		m->u.site.sysid = (uint16_t)arg(blk, 12, 12);
		m->u.site.rfss = (uint8_t)arg(blk, 24, 8);
		m->u.site.site = (uint8_t)arg(blk, 32, 8);
		m->u.site.ch = (uint16_t)arg(blk, 40, 16);
		m->u.site.ssc = (uint8_t)arg(blk, 56, 8);
		return true;
	case P25_OP_SCCB:
		m->kind = P25_TSBK_SCCB;
		m->u.sccb.rfss = (uint8_t)arg(blk, 0, 8);
		m->u.sccb.site = (uint8_t)arg(blk, 8, 8);
		m->u.sccb.ch[0] = (uint16_t)arg(blk, 16, 16);
		m->u.sccb.ssc[0] = (uint8_t)arg(blk, 32, 8);
		m->u.sccb.ch[1] = (uint16_t)arg(blk, 40, 16);
		m->u.sccb.ssc[1] = (uint8_t)arg(blk, 56, 8);
		return true;
	case P25_OP_SCCB_EXP:
		m->kind = P25_TSBK_SCCB;
		m->u.sccb.rfss = (uint8_t)arg(blk, 0, 8);
		m->u.sccb.site = (uint8_t)arg(blk, 8, 8);
		m->u.sccb.ch[0] = (uint16_t)arg(blk, 16, 16);	// channel (T)
		m->u.sccb.ssc[0] = (uint8_t)arg(blk, 48, 8);
		return true;
	case P25_OP_SYS_SRV_BCST:
		m->kind = P25_TSBK_SYS_SRV;
		m->u.svc.avail = arg(blk, 8, 24);
		m->u.svc.supp = arg(blk, 32, 24);
		m->u.svc.prio = (uint8_t)arg(blk, 56, 8);
		return true;
	case P25_OP_U_REG_RSP:
		m->kind = P25_TSBK_REG_RSP;
		m->u.reg.rv = (uint8_t)arg(blk, 2, 2);
		m->u.reg.sysid = (uint16_t)arg(blk, 4, 12);
		m->u.reg.id = arg(blk, 16, 24);
		m->u.reg.addr = arg(blk, 40, 24);
		return true;
	case P25_OP_GRP_AFF_RSP:
		m->kind = P25_TSBK_AFF_RSP;
		m->u.aff.global = arg(blk, 0, 1) != 0;
		m->u.aff.gav = (uint8_t)arg(blk, 6, 2);
		m->u.aff.ann = (uint16_t)arg(blk, 8, 16);
		m->u.aff.tg = (uint16_t)arg(blk, 24, 16);
		m->u.aff.dst = arg(blk, 40, 24);
		return true;
	case P25_OP_DENY_RSP:
	case P25_OP_QUE_RSP:
		m->kind = m->opcode == P25_OP_DENY_RSP ? P25_TSBK_DENY : P25_TSBK_QUEUED;
		m->u.rsp.aiv = arg(blk, 0, 1) != 0;
		m->u.rsp.ex = arg(blk, 1, 1) != 0;
		m->u.rsp.svc = (uint8_t)arg(blk, 2, 6);
		m->u.rsp.reason = (uint8_t)arg(blk, 8, 8);
		m->u.rsp.info = arg(blk, 16, 24);
		m->u.rsp.dst = arg(blk, 40, 24);
		return true;
	default:
		m->kind = P25_TSBK_OTHER;
		return false;
	}
}

void p25_tsbk_make(const p25_tsbk_t *m, uint8_t blk[P25_TSBK_BYTES])
{
	uint8_t op = m->opcode;

	memset(blk, 0, P25_TSBK_BYTES);
	switch (m->kind) {
	case P25_TSBK_GRANT: {
		const p25_grant_t *g = &m->u.grant;

		if (op == P25_OP_GRP_V_GRANT) {
			arg_put(blk, 0, 8, g->svc);
			arg_put(blk, 8, 16, g->ch);
			arg_put(blk, 24, 16, g->tg);
			arg_put(blk, 40, 24, g->src);
		} else if (op == P25_OP_GRP_V_UPDT) {
			arg_put(blk, 0, 16, g->ch);
			arg_put(blk, 16, 16, g->tg);
			arg_put(blk, 32, 16, g->ch2);
			arg_put(blk, 48, 16, g->tg2);
		} else if (op == P25_OP_GRP_V_UPDT_EXP) {
			arg_put(blk, 0, 8, g->svc);
			arg_put(blk, 16, 16, g->ch);
			arg_put(blk, 32, 16, g->ch2);	// channel (R), the inbound one: kept as given
			arg_put(blk, 48, 16, g->tg);
		} else {
			arg_put(blk, 0, 16, g->ch);
			arg_put(blk, 16, 24, g->dst);
			arg_put(blk, 40, 24, g->src);
		}
		break;
	}
	case P25_TSBK_IDEN: {
		const p25_iden_air_t *i = &m->u.iden;
		uint32_t mag = (uint32_t)(i->tx_off < 0 ? -i->tx_off : i->tx_off);

		op = i->form;
		arg_put(blk, 0, 4, i->id);
		if (op == P25_OP_IDEN_UP) {
			arg_put(blk, 4, 9, i->bw_khz8);
			arg_put(blk, 13, 1, i->tx_off >= 0);
			arg_put(blk, 14, 8, mag);
		} else {
			arg_put(blk, 4, 4, op == P25_OP_IDEN_UP_VU ? (i->bw_khz8 == 50 ? 4u : 5u) : i->type);
			arg_put(blk, 8, 1, i->tx_off >= 0);
			arg_put(blk, 9, 13, mag);
		}
		arg_put(blk, 22, 10, i->spacing);
		arg_put(blk, 32, 32, i->base);
		break;
	}
	case P25_TSBK_NET:
		op = P25_OP_NET_STS_BCST;
		arg_put(blk, 0, 8, m->u.net.lra);
		arg_put(blk, 8, 20, m->u.net.wacn);
		arg_put(blk, 28, 12, m->u.net.sysid);
		arg_put(blk, 40, 16, m->u.net.ch);
		arg_put(blk, 56, 8, m->u.net.ssc);
		break;
	case P25_TSBK_RFSS:
	case P25_TSBK_ADJ:
		op = m->kind == P25_TSBK_RFSS ? P25_OP_RFSS_STS_BCST : P25_OP_ADJ_STS_BCST;
		arg_put(blk, 0, 8, m->u.site.lra);
		arg_put(blk, 8, 4, m->u.site.flags);
		arg_put(blk, 12, 12, m->u.site.sysid);
		arg_put(blk, 24, 8, m->u.site.rfss);
		arg_put(blk, 32, 8, m->u.site.site);
		arg_put(blk, 40, 16, m->u.site.ch);
		arg_put(blk, 56, 8, m->u.site.ssc);
		break;
	case P25_TSBK_SCCB:
		op = P25_OP_SCCB;
		arg_put(blk, 0, 8, m->u.sccb.rfss);
		arg_put(blk, 8, 8, m->u.sccb.site);
		arg_put(blk, 16, 16, m->u.sccb.ch[0]);
		arg_put(blk, 32, 8, m->u.sccb.ssc[0]);
		arg_put(blk, 40, 16, m->u.sccb.ch[1]);
		arg_put(blk, 56, 8, m->u.sccb.ssc[1]);
		break;
	case P25_TSBK_SYS_SRV:
		op = P25_OP_SYS_SRV_BCST;
		arg_put(blk, 8, 24, m->u.svc.avail);
		arg_put(blk, 32, 24, m->u.svc.supp);
		arg_put(blk, 56, 8, m->u.svc.prio);
		break;
	case P25_TSBK_REG_RSP:
		op = P25_OP_U_REG_RSP;
		arg_put(blk, 2, 2, m->u.reg.rv);
		arg_put(blk, 4, 12, m->u.reg.sysid);
		arg_put(blk, 16, 24, m->u.reg.id);
		arg_put(blk, 40, 24, m->u.reg.addr);
		break;
	case P25_TSBK_AFF_RSP:
		op = P25_OP_GRP_AFF_RSP;
		arg_put(blk, 0, 1, m->u.aff.global);
		arg_put(blk, 6, 2, m->u.aff.gav);
		arg_put(blk, 8, 16, m->u.aff.ann);
		arg_put(blk, 24, 16, m->u.aff.tg);
		arg_put(blk, 40, 24, m->u.aff.dst);
		break;
	case P25_TSBK_DENY:
	case P25_TSBK_QUEUED:
		op = m->kind == P25_TSBK_DENY ? P25_OP_DENY_RSP : P25_OP_QUE_RSP;
		arg_put(blk, 0, 1, m->u.rsp.aiv);
		arg_put(blk, 1, 1, m->u.rsp.ex);
		arg_put(blk, 2, 6, m->u.rsp.svc);
		arg_put(blk, 8, 8, m->u.rsp.reason);
		arg_put(blk, 16, 24, m->u.rsp.info);
		arg_put(blk, 40, 24, m->u.rsp.dst);
		break;
	default:
		break;
	}
	blk[0] = (uint8_t)((m->last ? P25_TSBK_LB : 0) | (m->prot ? P25_TSBK_P : 0) | (op & 0x3F));
	blk[1] = m->mfid;
	p25_tsbk_seal(blk);
}

// ── Inbound signaling (layouts in docs/spec/p25-trunking.md 2.1; their argument places are unconfirmed) ──
void p25_isp_make(const p25_isp_t *m, uint8_t blk[P25_TSBK_BYTES])
{
	memset(blk, 0, P25_TSBK_BYTES);
	blk[0] = (uint8_t)(P25_TSBK_LB | (m->opcode & 0x3F));
	blk[1] = m->mfid;
	switch (m->opcode) {
	case P25_ISP_GRP_V_REQ:
		arg_put(blk, 0, 8, m->svc);
		arg_put(blk, 24, 16, m->tg);
		arg_put(blk, 40, 24, m->src);
		break;
	case P25_ISP_GRP_AFF_REQ:
		arg_put(blk, 12, 12, m->sysid);
		arg_put(blk, 24, 16, m->tg);
		arg_put(blk, 40, 24, m->src);
		break;
	case P25_ISP_U_REG_REQ:
		arg_put(blk, 8, 20, m->wacn);
		arg_put(blk, 28, 12, m->sysid);
		arg_put(blk, 40, 24, m->src);
		break;
	default:
		break;
	}
	p25_tsbk_seal(blk);
}

bool p25_isp_parse(const uint8_t blk[P25_TSBK_BYTES], p25_isp_t *m)
{
	memset(m, 0, sizeof *m);
	m->opcode = blk[0] & 0x3F;
	m->mfid = blk[1];
	if (m->mfid != 0 || (blk[0] & P25_TSBK_P))
		return false;
	switch (m->opcode) {
	case P25_ISP_GRP_V_REQ:
		m->svc = (uint8_t)arg(blk, 0, 8);
		m->tg = (uint16_t)arg(blk, 24, 16);
		m->src = arg(blk, 40, 24);
		break;
	case P25_ISP_GRP_AFF_REQ:
		m->sysid = (uint16_t)arg(blk, 12, 12);
		m->tg = (uint16_t)arg(blk, 24, 16);
		m->src = arg(blk, 40, 24);
		break;
	case P25_ISP_U_REG_REQ:
		m->wacn = arg(blk, 8, 20);
		m->sysid = (uint16_t)arg(blk, 28, 12);
		m->src = arg(blk, 40, 24);
		break;
	default:
		return false;
	}
	m->known = true;
	return true;
}

uint16_t p25_isp_unit(uint8_t *unit, uint16_t nac, const uint8_t blk[P25_TSBK_BYTES])
{
	uint16_t n = p25_blocks_syms(1);

	p25_unit_begin(unit, n, nac, P25_DUID_TSDU, P25_SS_UNKNOWN);
	p25_block_put(unit, 0, blk);
	return n;
}

// ── Multi-block trunking: the header block ──
bool p25_mbt_parse(const uint8_t hdr[P25_TSBK_BYTES], p25_mbt_t *h)
{
	h->format = hdr[0] & 0x1F;
	h->sap = hdr[1] & 0x3F;
	h->mfid = hdr[2];
	h->blocks = hdr[6] & 0x7F;
	h->opcode = hdr[7] & 0x3F;
	return (h->format == P25_PDU_FMT_AMBTC || h->format == P25_PDU_FMT_UMBTC) && (h->sap == 61 || h->sap == 63);
}

void p25_mbt_make(const p25_mbt_t *h, uint8_t hdr[P25_TSBK_BYTES])
{
	memset(hdr, 0, P25_TSBK_BYTES);
	hdr[0] = (uint8_t)(0x20 | (h->format & 0x1F));	// outbound, unconfirmed
	hdr[1] = h->sap & 0x3F;
	hdr[2] = h->mfid;
	hdr[6] = (uint8_t)(0x80 | (h->blocks & 0x7F));
	hdr[7] = h->opcode & 0x3F;
	p25_tsbk_seal(hdr);
}

uint32_t p25_packet_crc(const uint8_t *b, unsigned n)
{
	uint32_t crc = 0;

	// GM(x) = 0x04C11DB7, most significant bit first, from zero; the remainder inverted (IM(x))
	for (unsigned i = 0; i < n; i++) {
		crc ^= (uint32_t)b[i] << 24;
		for (unsigned k = 0; k < 8; k++)
			crc = crc & 0x80000000u ? crc << 1 ^ 0x04C11DB7u : crc << 1;
	}
	return ~crc;
}

// Header: the alternate format, a trunking SAP, MFID 0, one data block, the opcode; LRA in octet 3, the system in
// the low 12 bits of octets 4-5. Data: WACN 20 bits, 4 reserved, channel (T), channel (R) (not kept), service class,
// packet CRC
bool p25_mbt_net_parse(const uint8_t hdr[P25_TSBK_BYTES], const uint8_t data[P25_TSBK_BYTES], p25_tsbk_t *m)
{
	p25_mbt_t h;
	uint32_t crc = (uint32_t)data[8] << 24 | (uint32_t)data[9] << 16 | (uint32_t)data[10] << 8 | data[11];

	if (!p25_mbt_parse(hdr, &h) || h.format != P25_PDU_FMT_AMBTC || h.mfid != 0 || h.blocks != 1 ||
	    h.opcode != P25_OP_NET_STS_BCST || p25_packet_crc(data, 8) != crc)
		return false;
	memset(m, 0, sizeof *m);
	m->kind = P25_TSBK_NET;
	m->opcode = P25_OP_NET_STS_BCST;
	m->last = true;
	m->u.net.lra = hdr[3];
	m->u.net.sysid = (uint16_t)((hdr[4] & 0x0F) << 8 | hdr[5]);
	m->u.net.wacn = (uint32_t)data[0] << 12 | (uint32_t)data[1] << 4 | data[2] >> 4;
	m->u.net.ch = (uint16_t)(data[3] << 8 | data[4]);
	m->u.net.ssc = data[7];
	return true;
}

void p25_mbt_net_make(const p25_tsbk_t *m, uint8_t hdr[P25_TSBK_BYTES], uint8_t data[P25_TSBK_BYTES])
{
	p25_mbt_t h = { .format = P25_PDU_FMT_AMBTC, .sap = 61, .mfid = 0, .blocks = 1, .opcode = P25_OP_NET_STS_BCST };
	uint32_t crc;

	p25_mbt_make(&h, hdr);
	hdr[1] |= 0xC0;
	hdr[3] = m->u.net.lra;
	hdr[4] = (uint8_t)((m->u.net.sysid >> 8) & 0x0F);
	hdr[5] = (uint8_t)m->u.net.sysid;
	p25_tsbk_seal(hdr);
	memset(data, 0, P25_TSBK_BYTES);
	data[0] = (uint8_t)(m->u.net.wacn >> 12);
	data[1] = (uint8_t)(m->u.net.wacn >> 4);
	data[2] = (uint8_t)((m->u.net.wacn & 0x0F) << 4);
	data[3] = (uint8_t)(m->u.net.ch >> 8);
	data[4] = (uint8_t)m->u.net.ch;
	data[7] = m->u.net.ssc;
	crc = p25_packet_crc(data, 8);
	data[8] = (uint8_t)(crc >> 24);
	data[9] = (uint8_t)(crc >> 16);
	data[10] = (uint8_t)(crc >> 8);
	data[11] = (uint8_t)crc;
}

// ── Channels to frequencies ──
uint32_t p25_chan_hz(uint16_t ch, uint32_t base_hz, uint16_t spacing5, uint8_t slots)
{
	uint32_t n = ch & 0xFFFu, step;

	if (slots > 1)
		n /= slots;		// the carrier: its slots share one frequency
	step = n * spacing5 * 5u;	// at most 4095 x 65535 x 5: under 2^31
	if (!base_hz || step > UINT32_MAX - base_hz)
		return 0;
	return base_hz + step;
}

// Explicit status broadcasts: Aeroflex Advanced P25 Control Channel Functions, figures 5 and 6.
bool p25_mbt_status(const p25_rx_unit_t *u, uint8_t normalized[P25_TSBK_BYTES])
{
	uint8_t hdr[P25_TSBK_BYTES], data[P25_TSBK_BYTES];
	p25_mbt_t h;
	if (u->duid != P25_DUID_PDU || u->nsyms < p25_blocks_syms(2) ||
	    p25_block_get(u->air, 0, hdr) < 0 || !p25_mbt_parse(hdr, &h) ||
	    h.format != P25_PDU_FMT_AMBTC || h.blocks != 1 || (h.mfid != 0 && h.mfid != 1) ||
	    !(hdr[0] & 0x20) || (h.opcode != P25_OP_NET_STS_BCST && h.opcode != P25_OP_RFSS_STS_BCST))
		return false;
	if (p25_block_decode(u->air, 1, data) < 0)
		return false;
	// The data block carries a packet CRC-32, not the header's CRC-16.
	uint32_t crc = 0;
	for (unsigned i = 0; i < 8; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (unsigned b = 0; b < 8; b++)
			crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
	}
	uint32_t want = (uint32_t)data[8] << 24 | (uint32_t)data[9] << 16 | (uint32_t)data[10] << 8 | data[11];
	if (~crc != want)
		return false;
	p25_tsbk_t m = { .opcode = h.opcode };
	uint16_t sysid = (uint16_t)((hdr[4] & 15u) << 8 | hdr[5]);
	if (h.opcode == P25_OP_NET_STS_BCST) {
		m.kind = P25_TSBK_NET;
		m.u.net.lra = hdr[3];
		m.u.net.sysid = sysid;
		m.u.net.wacn = (uint32_t)data[0] << 12 | (uint32_t)data[1] << 4 | data[2] >> 4;
		m.u.net.ch = (uint16_t)(data[3] << 8 | data[4]);
		m.u.net.ssc = data[7];
	} else {
		m.kind = P25_TSBK_RFSS;
		m.u.site.lra = hdr[3];
		m.u.site.flags = (hdr[4] >> 4) & 1u;
		m.u.site.sysid = sysid;
		m.u.site.rfss = data[0];
		m.u.site.site = data[1];
		m.u.site.ch = (uint16_t)(data[2] << 8 | data[3]);
		m.u.site.ssc = data[6];
	}
	p25_tsbk_make(&m, normalized);
	return true;
}

#endif
