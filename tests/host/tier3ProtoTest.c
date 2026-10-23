// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * PROTOTYPE, not firmware code. Host-side encoders / decoders for the DMR Tier III (ETSI TS 102 361-4 V1.12.1)
 * PDUs that the minimal mobile station subset needs, written to check the field layouts and tables quoted in
 * docs/spec/dmr-tier3-plan.md before any firmware work starts. The real implementation (work package T3-1 in the
 * plan) goes to core/dmr/trunk/ and may reuse this code; this file stays a test.
 *
 * Checked here:
 *   - C_ALOHA, TV_GRANT, PV_GRANT, C_RAND (voice and registration), C_ACKD, C_AHOY, P_CLEAR, P_MAINT bit layouts
 *     (TS 102 361-4 clause 7.1.1), each a 64-bit field sum in octets 2-9, packed and unpacked round trip, with the
 *     CSBK CRC (mask 0xA5A5, TS 102 361-1 B.3.12) from core/dmr/call/dmr_lc.c
 *   - C_SYScode partition per network model (6.3.2.2.1.1, table 6.5), SYS_AREA with DMRLA (6.4.1.2), the PAR check
 *     (6.3.2.2.1.4), the 14-bit CACH subset (5.1.1)
 *   - the Mask applicability rule (6.1.3), the NRand_Wait and Backoff tables (7.2.3, 7.2.5)
 *   - C_SYS_Parms short LC layout (7.1.2.1)
 *   - the fixed channel plan and CdefParms absolute frequencies (Annex C)
 *   - the RC4 challenge / response authentication against the test vectors of tables 6.15 and 6.16 (6.4.8)
 *
 *   make -C tests/host
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "dmr/call/dmr_lc.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Bit packing: CSBK octets 0-9, MSB first (TS 102 361-1 clause 9.1.8: LB, PF, CSBKO, FID, then octets 2-9)

typedef struct {
	uint8_t *buf;
	unsigned pos;	// bit position from the MSB of buf[0]
} bitw_t;

static void put(bitw_t *w, unsigned width, uint32_t v)
{
	for (unsigned i = 0; i < width; i++) {
		unsigned bit = (v >> (width - 1 - i)) & 1u;
		unsigned p = w->pos++;
		if (bit) {
			w->buf[p / 8] |= (uint8_t)(0x80u >> (p % 8));
		} else {
			w->buf[p / 8] &= (uint8_t)~(0x80u >> (p % 8));
		}
	}
}

static uint32_t get(const uint8_t *buf, unsigned *pos, unsigned width)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < width; i++) {
		unsigned p = (*pos)++;
		v = (v << 1) | ((buf[p / 8] >> (7 - p % 8)) & 1u);
	}
	return v;
}

// Octets 0 and 1: LB, PF, CSBKO, FID
static void csbk_head(bitw_t *w, bool lb, uint8_t csbko, uint8_t fid)
{
	put(w, 1, lb);
	put(w, 1, 0);		// PF
	put(w, 6, csbko);
	put(w, 8, fid);
}

// Opcodes (Annex B.1)
#define CSBKO_C_ALOHA	25
#define CSBKO_C_AHOY	28
#define CSBKO_C_RAND	31
#define CSBKO_C_ACKD	32
#define CSBKO_C_ACKU	33
#define CSBKO_P_MAINT	42
#define CSBKO_P_CLEAR	46
#define CSBKO_PV_GRANT	48
#define CSBKO_TV_GRANT	49

// Gateways (A.4)
#define ID_REGI		0xFFFEC6u
#define ID_TSI		0xFFFECAu
#define ID_ALLMSI	0xFFFED4u
#define ID_ALLMSID	0xFFFFFFu

// ---------------------------------------------------------------------------------------------------------------
// C_ALOHA (7.1.1.1.4, table 7.19)

typedef struct {
	bool tsccas, site_ts_sync;
	uint8_t version;	// 3 bits
	bool offset, active_conn;
	uint8_t mask;		// 0..24
	uint8_t service_function;	// 2 bits, table 6.1
	uint8_t nrand_wait;	// 4 bits, table 6.2
	bool reg;
	uint8_t backoff;	// 4 bits, table 6.3
	uint16_t syscode;
	uint32_t ms_addr;
} aloha_t;

static void aloha_pack(const aloha_t *a, uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, CSBKO_C_ALOHA, 0);
	put(&w, 1, 0);
	put(&w, 1, a->tsccas);
	put(&w, 1, a->site_ts_sync);
	put(&w, 3, a->version);
	put(&w, 1, a->offset);
	put(&w, 1, a->active_conn);
	put(&w, 5, a->mask);
	put(&w, 2, a->service_function);
	put(&w, 4, a->nrand_wait);
	put(&w, 1, a->reg);
	put(&w, 4, a->backoff);
	put(&w, 16, a->syscode);
	put(&w, 24, a->ms_addr);
	CHECK(w.pos == 80, "C_ALOHA is %u bits, not 80", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

static bool aloha_unpack(const uint8_t in[12], aloha_t *a)
{
	uint8_t chk[12];
	memcpy(chk, in, 10);
	dmr_crc16_append(chk, DMR_CRC_MASK_CSBK);
	if (memcmp(chk + 10, in + 10, 2) != 0 || (in[0] & 0x3F) != CSBKO_C_ALOHA || in[1] != 0) {
		return false;
	}
	unsigned p = 16;
	get(in, &p, 1);
	a->tsccas = get(in, &p, 1);
	a->site_ts_sync = get(in, &p, 1);
	a->version = (uint8_t)get(in, &p, 3);
	a->offset = get(in, &p, 1);
	a->active_conn = get(in, &p, 1);
	a->mask = (uint8_t)get(in, &p, 5);
	a->service_function = (uint8_t)get(in, &p, 2);
	a->nrand_wait = (uint8_t)get(in, &p, 4);
	a->reg = get(in, &p, 1);
	a->backoff = (uint8_t)get(in, &p, 4);
	a->syscode = (uint16_t)get(in, &p, 16);
	a->ms_addr = get(in, &p, 24);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Voice channel grants (7.1.1.1.1.1.1 table 7.9, 7.1.1.1.1.2 table 7.11)

typedef struct {
	bool group;
	uint16_t lpcn;		// 12 bits: 0 invalid, 1..4094 logical channel, 4095 = absolute parameters follow (MBC)
	uint8_t slot;		// 1 or 2 (the IE is 0 = TDMA channel 1)
	bool late_entry;	// TV_GRANT only
	bool emergency, offset;
	uint32_t target, source;
} grant_t;

static void grant_pack(const grant_t *g, uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, g->group ? CSBKO_TV_GRANT : CSBKO_PV_GRANT, 0);
	put(&w, 12, g->lpcn);
	put(&w, 1, g->slot == 2);
	put(&w, 1, g->group ? g->late_entry : 0);
	put(&w, 1, g->emergency);
	put(&w, 1, g->offset);
	put(&w, 24, g->target);
	put(&w, 24, g->source);
	CHECK(w.pos == 80, "grant is %u bits", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

static bool grant_unpack(const uint8_t in[12], grant_t *g)
{
	uint8_t op = in[0] & 0x3F;
	if ((op != CSBKO_TV_GRANT && op != CSBKO_PV_GRANT) || in[1] != 0) {
		return false;
	}
	unsigned p = 16;
	g->group = op == CSBKO_TV_GRANT;
	g->lpcn = (uint16_t)get(in, &p, 12);
	g->slot = get(in, &p, 1) ? 2 : 1;
	g->late_entry = get(in, &p, 1) && g->group;
	g->emergency = get(in, &p, 1);
	g->offset = get(in, &p, 1);
	g->target = get(in, &p, 24);
	g->source = get(in, &p, 24);
	return true;
}

// Is a voice grant ours? (6.6.2.2.7)
static bool grant_applicable(const grant_t *g, uint32_t own_id, const uint32_t *groups, int ngroups)
{
	if (g->source == own_id) {
		return true;
	}
	if (!g->group) {
		return g->target == own_id;
	}
	if (g->target == ID_ALLMSID) {
		return true;
	}
	for (int i = 0; i < ngroups; i++) {
		if (groups[i] == g->target) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// C_RAND (7.1.1.2.1 table 7.25) for a voice call (6.6.2.2 table 6.40) and a registration (6.4.4.1 table 6.9)

#define SK_IND_VOICE	0x0
#define SK_GRP_VOICE	0x1
#define SK_REG		0xE

static void rand_voice_pack(bool group, bool emergency, uint8_t priority, uint32_t target, uint32_t source,
	uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, CSBKO_C_RAND, 0);
	// Service_Options: EMERG, privacy, SUPED_SV, BCAST_SV, reserved, PRIORITY (2); priority is 0 for emergency
	put(&w, 1, emergency);
	put(&w, 1, 0);
	put(&w, 1, 0);
	put(&w, 1, 0);
	put(&w, 1, 0);
	put(&w, 2, emergency ? 0 : priority);
	put(&w, 1, 0);		// proxy
	put(&w, 2, 0);		// appended supplementary data
	put(&w, 1, 0);		// ambient listening
	put(&w, 1, 0);		// reserved
	put(&w, 4, group ? SK_GRP_VOICE : SK_IND_VOICE);
	put(&w, 24, target);
	put(&w, 24, source);
	CHECK(w.pos == 80, "C_RAND voice is %u bits", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

// Registration (reg = true) or de-registration (6.4.6: IP_Inform 0, Reg_Dereg 0, PowerSave_RQ 0). Target =
// 0x00 + C_SYScode (TRGT_ADR_CNTS = 00)
static void rand_reg_pack(bool reg, uint16_t syscode, uint32_t source, uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, CSBKO_C_RAND, 0);
	put(&w, 1, 0);		// reserved
	put(&w, 1, 0);		// privacy
	put(&w, 1, 0);		// IP_INFORM
	put(&w, 3, 0);		// PS_RQ
	put(&w, 1, reg);	// REG_DEREG
	put(&w, 1, 0);		// proxy
	put(&w, 2, 0);		// TRGT_ADR_CNTS
	put(&w, 2, 0);		// appended UDT
	put(&w, 4, SK_REG);
	put(&w, 24, syscode);
	put(&w, 24, source);
	CHECK(w.pos == 80, "C_RAND reg is %u bits", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

// ---------------------------------------------------------------------------------------------------------------
// Acknowledgements (7.1.1.1.7 table 7.23; reasons 7.2.8)

#define REASON_REG_ACCEPTED	0x62	// 0110 0010
#define REASON_MS_ACCEPTED	0x44	// 0100 0100 (sent by an MS)
#define REASON_AUTH_RESPONSE	0x48	// 0100 1000 (sent by an MS)
#define REASON_REG_REFUSED	0x2A	// 0010 1010

static void ack_pack(uint8_t csbko, uint8_t response_info, uint8_t reason, uint32_t target, uint32_t addl,
	uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, csbko, 0);
	put(&w, 7, response_info);
	put(&w, 8, reason);
	put(&w, 1, 0);
	put(&w, 24, target);
	put(&w, 24, addl);
	CHECK(w.pos == 80, "ack is %u bits", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

// ---------------------------------------------------------------------------------------------------------------
// C_AHOY authentication challenge (6.4.8.2 table 6.17), P_CLEAR (7.1.1.3.2 table 7.30), P_MAINT (table 7.32)

static void ahoy_auth_pack(uint32_t ms, uint32_t challenge, uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, CSBKO_C_AHOY, 0);
	put(&w, 7, 0);		// Service_Options_Mirror (authentication poll)
	put(&w, 1, 0);		// Service_Kind_Flag
	put(&w, 1, 0);		// ALS
	put(&w, 1, 0);		// G/I: individual
	put(&w, 2, 0);		// appended blocks
	put(&w, 4, SK_REG);	// 1110 authentication service
	put(&w, 24, ms);
	put(&w, 24, challenge);
	CHECK(w.pos == 80, "C_AHOY is %u bits", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

static void p_clear_pack(uint16_t lpcn, bool group, uint32_t target, uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, CSBKO_P_CLEAR, 0);
	put(&w, 12, lpcn);
	put(&w, 1, 0);
	put(&w, 2, 0);
	put(&w, 1, group);
	put(&w, 24, target);
	put(&w, 24, ID_TSI);
	CHECK(w.pos == 80, "P_CLEAR is %u bits", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

static void p_maint_discon_pack(uint32_t ms, uint8_t out[12])
{
	bitw_t w = { out, 0 };
	csbk_head(&w, true, CSBKO_P_MAINT, 0);
	put(&w, 12, 0);
	put(&w, 3, 0);		// Maint_Kind DISCON
	put(&w, 1, 0);
	put(&w, 24, ID_TSI);
	put(&w, 24, ms);
	CHECK(w.pos == 80, "P_MAINT is %u bits", w.pos);
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

// ---------------------------------------------------------------------------------------------------------------
// C_SYScode (6.3.2.2.1.1): MODEL (2) + NET + SITE (12 together, split by the model) + PAR (2)

typedef struct { uint8_t model; uint16_t net, site; uint8_t par; } syscode_t;

static const uint8_t site_bits[4] = { 3, 5, 8, 10 };	// tiny, small, large, huge (table 6.5)

static uint16_t syscode_pack(const syscode_t *s)
{
	uint8_t sb = site_bits[s->model & 3];
	return (uint16_t)(((s->model & 3u) << 14) | ((uint32_t)s->net << (2 + sb)) | ((uint32_t)s->site << 2) | (s->par & 3u));
}

static void syscode_unpack(uint16_t v, syscode_t *s)
{
	s->model = (uint8_t)(v >> 14);
	uint8_t sb = site_bits[s->model];
	s->par = v & 3u;
	s->site = (uint16_t)((v >> 2) & ((1u << sb) - 1u));
	s->net = (uint16_t)((v >> (2 + sb)) & ((1u << (12 - sb)) - 1u));
}

// SYS_AREA: the DMRLA most significant bits of SITE (6.4.1.2)
static uint16_t sys_area(const syscode_t *s, uint8_t dmrla)
{
	uint8_t sb = site_bits[s->model];
	return dmrla >= sb ? s->site : (uint16_t)(s->site >> (sb - dmrla));
}

// PAR: 01 category A, 10 category B, 11 both, 00 reserved (6.3.2.2.1.4)
static bool par_permits(uint8_t par, bool category_b)
{
	return par == 3 || (par == 1 && !category_b) || (par == 2 && category_b);
}

// The Mask rule (6.1.3): the Mask LSBs of the PDU address and our address match
static bool mask_applies(uint8_t mask, uint32_t pdu_addr, uint32_t own)
{
	if (mask == 0) {
		return true;
	}
	if (mask > 24) {
		return false;
	}
	uint32_t m = (mask == 24) ? 0xFFFFFFu : ((1u << mask) - 1u);
	return ((pdu_addr ^ own) & m) == 0;
}

static unsigned nwait_frames(uint8_t nrand_wait)
{
	return nrand_wait < 14 ? nrand_wait : (nrand_wait == 14 ? 15u : 24u);
}

static unsigned backoff_frames(uint8_t n)
{
	static const uint8_t t[16] = { 0, 1, 2, 3, 4, 5, 8, 11, 15, 20, 26, 33, 41, 50, 70, 100 };
	return t[n & 15];
}

// C_SYS_Parms short LC (7.1.2.1): octet 0 bits 3-0 = SLCO 0010; octets 1-3 = MODEL, NET+SITE, Reg, counter
static void sys_parms_pack(uint8_t model, uint16_t netsite, bool reg, uint16_t counter, uint8_t out[4])
{
	uint32_t v = ((uint32_t)(model & 3u) << 22) | ((uint32_t)(netsite & 0xFFFu) << 10) | ((uint32_t)reg << 9) |
		(counter & 0x1FFu);
	out[0] = 0x02;
	out[1] = (uint8_t)(v >> 16);
	out[2] = (uint8_t)(v >> 8);
	out[3] = (uint8_t)v;
}

// ---------------------------------------------------------------------------------------------------------------
// Channel plans (Annex C): fixed plan and CdefParms absolute frequencies, in Hz

static const uint32_t sep_hz[8] = { 5000, 6250, 10000, 12500, 15000, 20000, 25000, 30000 };	// table C.1

// tx_low: TXRX_SPLIT = 1 (MS Tx is low of MS Rx). split in 2.5 kHz units (table C.3)
static void fixed_plan(uint32_t base_hz, uint8_t sep, uint16_t chan, bool tx_low, uint32_t split_units,
	uint32_t *tx_hz, uint32_t *rx_hz)
{
	*tx_hz = base_hz + (uint32_t)(chan - 1) * sep_hz[sep & 7];
	uint32_t split = split_units * 2500u;
	*rx_hz = tx_low ? *tx_hz + split : *tx_hz - split;
}

// CdefParms (7.2.42, C.1.1.4): MHz integer (10 bits) + 125 Hz steps (13 bits)
static uint32_t cdef_hz(uint16_t mhz, uint16_t khz_steps)
{
	return (uint32_t)mhz * 1000000u + (uint32_t)khz_steps * 125u;
}

// ---------------------------------------------------------------------------------------------------------------
// Authentication (6.4.8): RC4 keyed with challenge (3 octets) || K (16 octets); drop 256 octets; the next 3 are the
// response; optionally XORed with the PSN

static void auth_response(uint32_t challenge, const uint8_t k[16], const uint8_t *psn, uint8_t resp[3])
{
	uint8_t key[19], s[256];
	key[0] = (uint8_t)(challenge >> 16);
	key[1] = (uint8_t)(challenge >> 8);
	key[2] = (uint8_t)challenge;
	memcpy(key + 3, k, 16);
	for (int i = 0; i < 256; i++) {
		s[i] = (uint8_t)i;
	}
	for (int i = 0, j = 0; i < 256; i++) {
		j = (j + s[i] + key[i % 19]) & 0xFF;
		uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
	}
	uint8_t i = 0, j = 0, out = 0;
	for (int n = 0; n < 259; n++) {
		i++;
		j = (uint8_t)(j + s[i]);
		uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
		out = s[(uint8_t)(s[i] + s[j])];
		if (n >= 256) {
			resp[n - 256] = (uint8_t)(out ^ (psn != NULL ? psn[n - 256] : 0));
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------

static void test_aloha(void)
{
	aloha_t a = { .tsccas = false, .site_ts_sync = true, .version = 4, .offset = false, .active_conn = true,
		.mask = 0, .service_function = 0, .nrand_wait = 5, .reg = true, .backoff = 4, .syscode = 0x5A3D,
		.ms_addr = 0 }, b;
	uint8_t buf[12] = { 0 };
	aloha_pack(&a, buf);
	CHECK(buf[0] == 0x99 && buf[1] == 0x00, "C_ALOHA octets 0-1 %02X %02X (LB + 0x19)", buf[0], buf[1]);
	CHECK(aloha_unpack(buf, &b), "C_ALOHA unpack");
	CHECK(a.tsccas == b.tsccas && a.site_ts_sync == b.site_ts_sync && a.version == b.version && a.offset == b.offset &&
		a.active_conn == b.active_conn && a.mask == b.mask && a.service_function == b.service_function &&
		a.nrand_wait == b.nrand_wait && a.reg == b.reg && a.backoff == b.backoff && a.syscode == b.syscode &&
		a.ms_addr == b.ms_addr, "C_ALOHA round trip");
	// Octet 2: reserved 0, TSCCAS 0, site sync 1, version 100, offset 0, active connection 1 = 0011 0001
	CHECK(buf[2] == 0x31, "C_ALOHA octet 2 %02X", buf[2]);
	// Octet 3: mask 00000, service function 00, NRand_Wait MSB 0; octet 4: NRand_Wait 101, reg 1, backoff 0100
	CHECK(buf[3] == 0x00 && buf[4] == 0xB4, "C_ALOHA octets 3-4 %02X %02X", buf[3], buf[4]);
	CHECK(buf[5] == 0x5A && buf[6] == 0x3D, "C_ALOHA syscode octets 5-6");
	buf[7] ^= 1;
	CHECK(!aloha_unpack(buf, &b), "C_ALOHA with a bit error must fail its CRC");
}

static void test_grants(void)
{
	grant_t g = { .group = true, .lpcn = 17, .slot = 2, .late_entry = true, .emergency = false, .offset = false,
		.target = 3100, .source = 3109999 }, h;
	uint8_t buf[12] = { 0 };
	grant_pack(&g, buf);
	CHECK(buf[0] == 0xB1, "TV_GRANT octet 0 %02X", buf[0]);
	CHECK(grant_unpack(buf, &h) && h.group && h.lpcn == g.lpcn && h.slot == g.slot && h.late_entry && !h.emergency &&
		!h.offset && h.target == g.target && h.source == g.source, "TV_GRANT round trip");
	// LPCN 17 = 0000 0001 0001, then slot 1 (TS2), late 1, emergency 0, offset 0: octets 2-3 = 01 1C
	CHECK(buf[2] == 0x01 && buf[3] == 0x1C, "TV_GRANT octets 2-3 %02X %02X", buf[2], buf[3]);
	uint32_t groups[] = { 9, 3100 };
	CHECK(grant_applicable(&h, 1234567, groups, 2), "TV_GRANT to a listened group");
	h.target = 91;
	CHECK(!grant_applicable(&h, 1234567, groups, 2), "TV_GRANT to another group");
	CHECK(grant_applicable(&h, 3109999, groups, 2), "TV_GRANT from us");

	grant_t p = { .group = false, .lpcn = 4094, .slot = 1, .emergency = true, .offset = true, .target = 1234567,
		.source = 7654321 }, q;
	grant_pack(&p, buf);
	CHECK(buf[0] == 0xB0, "PV_GRANT octet 0 %02X", buf[0]);
	CHECK(grant_unpack(buf, &q) && q.lpcn == 4094 && q.slot == 1 && q.emergency && q.offset && !q.late_entry &&
		q.target == 1234567 && q.source == 7654321, "PV_GRANT round trip");
	CHECK(grant_applicable(&q, 1234567, NULL, 0), "PV_GRANT to us");
}

static void test_rand_ack(void)
{
	uint8_t buf[12] = { 0 };
	rand_voice_pack(true, false, 0, 3100, 1234567, buf);
	CHECK(buf[0] == 0x9F && buf[1] == 0, "C_RAND octets 0-1 %02X %02X", buf[0], buf[1]);
	CHECK(buf[2] == 0x00 && buf[3] == 0x01, "C_RAND group voice octets 2-3 %02X %02X", buf[2], buf[3]);
	CHECK(buf[4] == 0x00 && buf[5] == 0x0C && buf[6] == 0x1C, "C_RAND target 3100");
	rand_voice_pack(false, true, 3, 7654321, 1234567, buf);
	CHECK(buf[2] == 0x80 && buf[3] == 0x00, "C_RAND emergency individual octets 2-3 %02X %02X (priority forced 0)",
		buf[2], buf[3]);
	rand_reg_pack(true, 0x5A3D, 1234567, buf);
	CHECK(buf[2] == 0x02 && buf[3] == 0x0E, "C_RAND registration octets 2-3 %02X %02X", buf[2], buf[3]);
	CHECK(buf[4] == 0x00 && buf[5] == 0x5A && buf[6] == 0x3D, "C_RAND registration target 00 + syscode");
	rand_reg_pack(false, 0x5A3D, 1234567, buf);
	CHECK(buf[2] == 0x00, "C_RAND de-registration Reg_Dereg = 0");

	ack_pack(CSBKO_C_ACKD, 0, REASON_REG_ACCEPTED, 1234567, ID_REGI, buf);
	unsigned p = 16;
	CHECK(get(buf, &p, 7) == 0 && get(buf, &p, 8) == REASON_REG_ACCEPTED && get(buf, &p, 1) == 0 &&
		get(buf, &p, 24) == 1234567 && get(buf, &p, 24) == ID_REGI, "C_ACKD Reg_Accepted fields");
	// Reason layout t t d a a a a a: ACK (01), TS to MS (1)
	CHECK((REASON_REG_ACCEPTED >> 6) == 1 && ((REASON_REG_ACCEPTED >> 5) & 1) == 1, "Reg_Accepted is an ACK, TS to MS");
	CHECK((REASON_REG_REFUSED >> 6) == 0, "Reg_Refused is a NACK");
	CHECK((REASON_MS_ACCEPTED >> 6) == 1 && ((REASON_MS_ACCEPTED >> 5) & 1) == 0, "MS_Accepted is an ACK, MS to TS");
}

static void test_payload_pdus(void)
{
	uint8_t buf[12] = { 0 };
	p_clear_pack(0, false, ID_ALLMSI, buf);
	unsigned p = 0;
	CHECK(get(buf, &p, 8) == (0x80 | CSBKO_P_CLEAR), "P_CLEAR opcode");
	p = 16;
	CHECK(get(buf, &p, 12) == 0, "P_CLEAR LPCN 0: back to the last confirmed TSCC");
	p = 32;
	CHECK(get(buf, &p, 24) == ID_ALLMSI && get(buf, &p, 24) == ID_TSI, "P_CLEAR ALLMSI from TSI");
	p_maint_discon_pack(1234567, buf);
	p = 0;
	CHECK(get(buf, &p, 8) == (0x80 | CSBKO_P_MAINT), "P_MAINT opcode");
	p = 32;
	CHECK(get(buf, &p, 24) == ID_TSI && get(buf, &p, 24) == 1234567, "P_MAINT DISCON to TSI from us");
	ahoy_auth_pack(1234567, 0x7A17C0, buf);
	p = 28;
	CHECK(get(buf, &p, 4) == SK_REG && get(buf, &p, 24) == 1234567 && get(buf, &p, 24) == 0x7A17C0,
		"C_AHOY authentication challenge fields");
}

static void test_syscode(void)
{
	// Each model fills 16 bits exactly
	for (uint8_t m = 0; m < 4; m++) {
		uint8_t sb = site_bits[m];
		syscode_t s = { m, (uint16_t)((1u << (12 - sb)) - 1u), (uint16_t)((1u << sb) - 1u), 3 }, t;
		uint16_t v = syscode_pack(&s);
		CHECK(v == (uint16_t)((m << 14) | 0x3FFF), "model %u all-ones syscode %04X", m, v);
		syscode_unpack(v, &t);
		CHECK(s.model == t.model && s.net == t.net && s.site == t.site && s.par == t.par, "model %u round trip", m);
	}
	// Figure 6.19's example: large network (site 8 bits), DMRLA 6 -> the 6 MSBs of SITE
	syscode_t l = { 2, 5, 0xB7, 3 };
	CHECK(sys_area(&l, 6) == (0xB7 >> 2), "SYS_AREA large DMRLA 6");
	syscode_t tiny = { 0, 300, 5, 1 };
	CHECK(sys_area(&tiny, 3) == 5, "SYS_AREA tiny DMRLA 3 = whole site");
	// The CACH carries the 14 MSBs (no PAR)
	CHECK((syscode_pack(&tiny) >> 2) == ((0u << 12) | (300u << 3) | 5u), "CACH subset of a tiny syscode");
	CHECK(par_permits(3, false) && par_permits(3, true) && par_permits(1, false) && !par_permits(1, true) &&
		par_permits(2, true) && !par_permits(0, false), "PAR check");

	uint8_t slc[4];
	sys_parms_pack(2, (5u << 8) | 0xB7, true, 511, slc);
	CHECK(slc[0] == 0x02 && slc[1] == 0x96 && slc[2] == 0xDF && slc[3] == 0xFF, "C_SYS_Parms %02X %02X %02X %02X",
		slc[0], slc[1], slc[2], slc[3]);
}

static void test_tables(void)
{
	// Figure 6.1 / 6.2 / 6.3: MS 0000 0000 0010 1010 0001 1001, PDU address ...0001 1001 with bit 4 different
	uint32_t ms = 0x002A19, pdu = 0x002A19 ^ 0x10;
	CHECK(mask_applies(4, pdu, ms), "mask 4 applies");
	CHECK(!mask_applies(5, pdu, ms), "mask 5 doesn't");
	CHECK(mask_applies(0, 0xFFFFFF, ms), "mask 0 applies to all");
	CHECK(mask_applies(24, ms, ms) && !mask_applies(24, ms ^ 0x800000, ms), "mask 24: exactly one MS");
	CHECK(nwait_frames(0) == 0 && nwait_frames(13) == 13 && nwait_frames(14) == 15 && nwait_frames(15) == 24,
		"NRand_Wait table 6.2");
	CHECK(backoff_frames(1) == 1 && backoff_frames(6) == 8 && backoff_frames(9) == 20 && backoff_frames(15) == 100,
		"Backoff table 6.3");
}

static void test_channel_plans(void)
{
	uint32_t tx, rx;
	// 450 MHz base, 12.5 kHz (SEP 0011), MS Tx low of Rx by 5 MHz (2000 x 2.5 kHz)
	fixed_plan(450000000u, 3, 1, true, 2000, &tx, &rx);
	CHECK(tx == 450000000u && rx == 455000000u, "fixed plan CHAN 1: %u / %u", tx, rx);
	fixed_plan(450000000u, 3, 801, true, 2000, &tx, &rx);
	CHECK(tx == 460000000u && rx == 465000000u, "fixed plan CHAN 801: %u / %u", tx, rx);
	fixed_plan(150000000u, 1, 3, false, 240, &tx, &rx);
	CHECK(tx == 150012500u && rx == 149412500u, "fixed plan VHF Tx high: %u / %u", tx, rx);
	CHECK(cdef_hz(461, 5500) == 461687500u, "CdefParms 461 MHz + 5500 x 125 Hz");
	CHECK(cdef_hz(999, 0x1F3F) == 999999875u, "CdefParms top of the 125 Hz range");
}

static void test_auth(void)
{
	// Tables 6.15 / 6.16: K = 01..10, challenge 7A 17 C0 -> E8 CA 1D; with PSN 01 02 03 -> E9 C8 1E
	uint8_t k[16], r[3];
	for (int i = 0; i < 16; i++) {
		k[i] = (uint8_t)(i + 1);
	}
	auth_response(0x7A17C0, k, NULL, r);
	CHECK(r[0] == 0xE8 && r[1] == 0xCA && r[2] == 0x1D, "auth without PSN %02X %02X %02X", r[0], r[1], r[2]);
	const uint8_t psn[3] = { 1, 2, 3 };
	auth_response(0x7A17C0, k, psn, r);
	CHECK(r[0] == 0xE9 && r[1] == 0xC8 && r[2] == 0x1E, "auth with PSN %02X %02X %02X", r[0], r[1], r[2]);
	uint8_t buf[12] = { 0 };
	auth_response(0x7A17C0, k, NULL, r);
	ack_pack(CSBKO_C_ACKU, 0, REASON_AUTH_RESPONSE, ((uint32_t)r[0] << 16) | ((uint32_t)r[1] << 8) | r[2], 1234567,
		buf);
	CHECK(buf[0] == 0xA1 && buf[4] == 0xE8 && buf[5] == 0xCA && buf[6] == 0x1D, "C_ACKU carries the response");
}

int main(void)
{
	test_aloha();
	test_grants();
	test_rand_ack();
	test_payload_pdus();
	test_syscode();
	test_tables();
	test_channel_plans();
	test_auth();
	printf("tier3ProtoTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
