// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The receive-only P25 trunking follower (core/p25/p25_follow.c) and the signaling it reads (core/p25/p25_tsbk.c):
// message layouts from bytes set by hand, the CRC and the trellis under bit errors, channel arithmetic, the follower's
// states on blocks fed by hand, random input, and whole systems on the air: control channels and voice channels
// modulated as C4FM into a discriminator tap with noise, through the soft modem, the framer and the P25 engine, with
// the receiver retuned wherever the follower asks.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p25/p25_engine.h"
#include "p25/p25_follow.h"
#include "air/air_softmodem.h"
#include "air/air_syms.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x5EED25;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static double gauss(void)
{
	double u = (rng() % 1000000 + 1) / 1000001.0, v = (rng() % 1000000) / 1000000.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

// Bits into a block by hand, first bit of octet 0 is bit 0 (independent of the code under test)
static void put(uint8_t *b, unsigned at, unsigned n, uint32_t v)
{
	for (unsigned i = 0; i < n; i++) {
		unsigned k = at + i;
		uint8_t m = (uint8_t)(0x80 >> (k & 7));

		if ((v >> (n - 1 - i)) & 1)
			b[k / 8] |= m;
		else
			b[k / 8] &= (uint8_t)~m;
	}
}

static void blk_hand(uint8_t b[P25_TSBK_BYTES], uint8_t op0, uint8_t mfid)
{
	memset(b, 0, P25_TSBK_BYTES);
	b[0] = op0;
	b[1] = mfid;
}

// ── Message layouts ──
static void testLayouts(void)
{
	uint8_t b[P25_TSBK_BYTES], c[P25_TSBK_BYTES];
	p25_tsbk_t m;

	// Group voice channel grant: service options, channel, group, source
	blk_hand(b, 0x80 | P25_OP_GRP_V_GRANT, 0x00);
	b[2] = 0xC3;
	b[3] = 0x12; b[4] = 0x34;
	b[5] = 0x0B; b[6] = 0xCD;
	b[7] = 0x12; b[8] = 0x34; b[9] = 0x56;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_GRANT && m.last && !m.prot && !m.u.grant.unit && m.u.grant.has_svc &&
	      m.u.grant.svc == 0xC3 && m.u.grant.ch == 0x1234 && m.u.grant.tg == 0x0BCD && m.u.grant.src == 0x123456,
	      "group grant: ch %04x tg %04x src %06x", m.u.grant.ch, m.u.grant.tg, m.u.grant.src);
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "group grant made back");

	// Group voice channel grant update: two channel / group pairs
	blk_hand(b, P25_OP_GRP_V_UPDT, 0x00);
	b[2] = 0x10; b[3] = 0x05; b[4] = 0x00; b[5] = 0x64;
	b[6] = 0x20; b[7] = 0x07; b[8] = 0x00; b[9] = 0xC8;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_GRANT && !m.last && !m.u.grant.has_svc && m.u.grant.ch == 0x1005 &&
	      m.u.grant.tg == 100 && m.u.grant.ch2 == 0x2007 && m.u.grant.tg2 == 200, "update: %04x/%u %04x/%u",
	      m.u.grant.ch, m.u.grant.tg, m.u.grant.ch2, m.u.grant.tg2);
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "update made back");

	// The explicit form: service options, a reserved octet, channel (T), channel (R), group. The listener tunes to T
	blk_hand(b, P25_OP_GRP_V_UPDT_EXP, 0x00);
	b[2] = 0x40;
	b[4] = 0x30; b[5] = 0x11;
	b[6] = 0x31; b[7] = 0x22;
	b[8] = 0x01; b[9] = 0x2C;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_GRANT && m.u.grant.svc == 0x40 && m.u.grant.ch == 0x3011 &&
	      m.u.grant.tg == 300, "explicit update: ch %04x tg %u", m.u.grant.ch, m.u.grant.tg);

	// Unit to unit voice channel grant: channel, target, source
	blk_hand(b, P25_OP_UU_V_GRANT, 0x00);
	b[2] = 0x10; b[3] = 0x09;
	b[4] = 0x00; b[5] = 0x27; b[6] = 0x11;
	b[7] = 0x0F; b[8] = 0x42; b[9] = 0x40;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_GRANT && m.u.grant.unit && m.u.grant.ch == 0x1009 &&
	      m.u.grant.dst == 10001 && m.u.grant.src == 1000000, "unit grant: dst %u src %u", m.u.grant.dst, m.u.grant.src);
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "unit grant made back");

	// Network status: LRA, WACN 20, system 12, channel, service class
	blk_hand(b, 0x80 | P25_OP_NET_STS_BCST, 0x00);
	b[2] = 0x01;
	put(b, 24, 20, 0xBEE00);
	put(b, 44, 12, 0x2F3);
	b[7] = 0x10; b[8] = 0x04;
	b[9] = 0x70;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_NET && m.u.net.lra == 1 && m.u.net.wacn == 0xBEE00 &&
	      m.u.net.sysid == 0x2F3 && m.u.net.ch == 0x1004 && m.u.net.ssc == 0x70, "net: wacn %05x sys %03x",
	      m.u.net.wacn, m.u.net.sysid);
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "net made back");

	// RFSS status: LRA, 4 flag bits, system, RFSS, site, channel, service class; the adjacent site's the same
	blk_hand(b, P25_OP_RFSS_STS_BCST, 0x00);
	b[2] = 0x01;
	put(b, 24, 4, 0x1);
	put(b, 28, 12, 0x2F3);
	b[5] = 0x02; b[6] = 0x05;
	b[7] = 0x10; b[8] = 0x04;
	b[9] = 0x70;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_RFSS && m.u.site.flags == 1 && m.u.site.sysid == 0x2F3 &&
	      m.u.site.rfss == 2 && m.u.site.site == 5 && m.u.site.ch == 0x1004, "rfss: sys %03x rfss %u site %u",
	      m.u.site.sysid, m.u.site.rfss, m.u.site.site);
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "rfss made back");
	b[0] = P25_OP_ADJ_STS_BCST;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_ADJ && m.u.site.site == 5, "adjacent site");

	// Identifier update (the 800 MHz form): id 4, bandwidth 9, transmit offset sign + 8, spacing 10, base 32.
	// 851.00625 MHz, 6.25 kHz steps, 12.5 kHz wide, mobiles transmit 45 MHz below
	blk_hand(b, P25_OP_IDEN_UP, 0x00);
	put(b, 16, 4, 1);
	put(b, 20, 9, 100);
	put(b, 29, 1, 0);
	put(b, 30, 8, 180);
	put(b, 38, 10, 50);
	put(b, 48, 32, 851006250 / 5);
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_IDEN && m.u.iden.id == 1 && m.u.iden.bw_khz8 == 100 &&
	      m.u.iden.tx_off == -180 && m.u.iden.spacing == 50 && m.u.iden.base == 851006250 / 5 && !m.u.iden.tdma &&
	      m.u.iden.slots == 1, "iden: id %u bw %u off %d sp %u base %u", m.u.iden.id, m.u.iden.bw_khz8,
	      m.u.iden.tx_off, m.u.iden.spacing, m.u.iden.base);
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "iden made back");

	// The TDMA form: id 4, channel type 4, offset sign + 13, spacing 10, base 32. Type 3 is two slots: not playable
	blk_hand(b, P25_OP_IDEN_UP_TDMA, 0x00);
	put(b, 16, 4, 2);
	put(b, 20, 4, 3);
	put(b, 24, 1, 1);
	put(b, 25, 13, 120);
	put(b, 38, 10, 100);
	put(b, 48, 32, 769006250 / 5);
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_IDEN && m.u.iden.id == 2 && m.u.iden.tdma && m.u.iden.slots == 2 &&
	      m.u.iden.type == 3 && m.u.iden.tx_off == 120 && m.u.iden.spacing == 100, "tdma iden: slots %u type %u off %d",
	      m.u.iden.slots, m.u.iden.type, m.u.iden.tx_off);
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "tdma iden made back");
	put(b, 20, 4, 1);	// type 1: single slot, full rate (FDMA)
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && !m.u.iden.tdma && m.u.iden.slots == 1, "tdma form, FDMA type");

	// The VHF/UHF form: bandwidth code 4 is 6.25 kHz, 5 is 12.5 kHz
	blk_hand(b, P25_OP_IDEN_UP_VU, 0x00);
	put(b, 16, 4, 3);
	put(b, 20, 4, 5);
	put(b, 38, 10, 100);
	put(b, 48, 32, 136000000 / 5);
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_IDEN && m.u.iden.bw_khz8 == 100 && m.u.iden.base == 136000000 / 5,
	      "vu iden: bw %u", m.u.iden.bw_khz8);

	// Secondary control channels, system services
	blk_hand(b, P25_OP_SCCB, 0x00);
	b[2] = 1; b[3] = 3;
	b[4] = 0x10; b[5] = 0x06; b[6] = 0x70;
	b[7] = 0x10; b[8] = 0x07; b[9] = 0x70;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_SCCB && m.u.sccb.ch[0] == 0x1006 && m.u.sccb.ch[1] == 0x1007,
	      "sccb");
	p25_tsbk_make(&m, c);
	CHECK(!memcmp(b, c, P25_TSBK_BYTES), "sccb made back");
	blk_hand(b, P25_OP_SYS_SRV_BCST, 0x00);
	put(b, 24, 24, 0xABCDEF);
	put(b, 48, 24, 0x123456);
	b[9] = 7;
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_SYS_SRV && m.u.svc.avail == 0xABCDEF && m.u.svc.supp == 0x123456 &&
	      m.u.svc.prio == 7, "system services");

	// Not decoded: a manufacturer's own block (even with a grant's opcode), a protected one, another opcode
	blk_hand(b, P25_OP_GRP_V_GRANT, 0x90);
	p25_tsbk_seal(b);
	CHECK(!p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_MFR && m.mfid == 0x90, "manufacturer's block");
	blk_hand(b, P25_TSBK_P | P25_OP_GRP_V_GRANT, 0x00);
	p25_tsbk_seal(b);
	CHECK(!p25_tsbk_parse(b, &m) && m.prot, "protected block");
	blk_hand(b, 0x31, 0x00);
	p25_tsbk_seal(b);
	CHECK(!p25_tsbk_parse(b, &m) && m.kind == P25_TSBK_OTHER && m.opcode == 0x31, "another opcode");

	// The header of a multi-block trunking packet
	{
		p25_mbt_t h = { .format = P25_PDU_FMT_AMBTC, .sap = 61, .mfid = 0, .blocks = 2, .opcode = 0x3C }, g;

		p25_mbt_make(&h, b);
		CHECK(p25_mbt_parse(b, &g) && g.format == h.format && g.sap == 61 && g.blocks == 2 && g.opcode == 0x3C &&
		      p25_tsbk_crc(b) == (uint16_t)(b[10] << 8 | b[11]), "MBT header");
		h.sap = 0;	// a packet of another service
		p25_mbt_make(&h, b);
		CHECK(!p25_mbt_parse(b, &g), "not a trunking packet");
	}
}

// ── The CRC, the blocks in a unit, the trellis ──
static void testBlocks(void)
{
	uint8_t b[P25_TSBK_BYTES], got[P25_TSBK_BYTES], unit[P25_UNIT_BYTES_MAX], blks[3][P25_TSBK_BYTES];
	unsigned missed = 0, wrong = 0, fixed[7] = { 0 }, tries = 0;

	// Every single bit error and every burst of up to 16 is seen
	for (unsigned i = 0; i < 10; i++)
		b[i] = (uint8_t)rng();
	p25_tsbk_seal(b);
	CHECK(p25_tsbk_crc(b) == (uint16_t)(b[10] << 8 | b[11]), "sealed");
	for (unsigned k = 0; k < 96; k++) {
		for (unsigned len = 1; len <= 16 && k + len <= 96; len++) {
			memcpy(got, b, sizeof(got));
			for (unsigned j = 0; j < len; j++)
				if (j == 0 || j == len - 1 || (rng() & 1))
					got[(k + j) / 8] ^= (uint8_t)(0x80 >> ((k + j) & 7));
			if (p25_tsbk_crc(got) == (uint16_t)(got[10] << 8 | got[11]))
				missed++;
		}
	}
	CHECK(missed == 0, "CRC: %u bursts missed", missed);

	CHECK(p25_blocks_syms(1) == 180 && p25_blocks_syms(2) == 288 && p25_blocks_syms(3) == 360 &&
	      p25_blocks_syms(P25_PDU_BLOCKS_MAX) == P25_UNIT_SYMS_MAX, "unit lengths %u %u %u", p25_blocks_syms(1),
	      p25_blocks_syms(2), p25_blocks_syms(3));
	CHECK(p25_block_end_sym(0) == 158 && p25_block_end_sym(1) == 259 && p25_block_end_sym(2) == 359, "block ends");

	// Three blocks in a TSDU, each back clean, then with dibits in error spread over each block
	p25_unit_begin(unit, p25_blocks_syms(3), 0x293, P25_DUID_TSDU, P25_SS_IDLE);
	for (unsigned k = 0; k < 3; k++) {
		for (unsigned i = 0; i < 10; i++)
			blks[k][i] = (uint8_t)rng();
		blks[k][0] = (uint8_t)((k == 2 ? P25_TSBK_LB : 0) | (blks[k][0] & 0x3F));
		p25_tsbk_seal(blks[k]);
		p25_block_put(unit, (uint8_t)k, blks[k]);
	}
	{
		uint16_t nac;
		uint8_t duid;

		CHECK(p25_nid_get(unit, &nac, &duid) == 0 && nac == 0x293 && duid == P25_DUID_TSDU, "TSDU NID");
	}
	for (unsigned k = 0; k < 3; k++)
		CHECK(p25_block_get(unit, (uint8_t)k, got) == 0 && !memcmp(got, blks[k], P25_TSBK_BYTES), "block %u clean", k);
	for (unsigned errs = 1; errs <= 6; errs++) {
		for (unsigned t = 0; t < 40; t++) {
			uint8_t u2[P25_UNIT_BYTES_MAX];
			unsigned k = t % 3;
			int r;

			memcpy(u2, unit, sizeof(u2));
			// errs dibits in error, one in every 98 / errs of the block's
			for (unsigned e = 0; e < errs; e++) {
				uint16_t at = (uint16_t)(112 + k * 196 + 2 * (e * (98 / errs) + rng() % (98 / errs)));

				p25_bits_put(u2, at, 2, (p25_bits_get(u2, at, 2) + 1 + rng() % 3) & 3);
			}
			r = p25_block_get(u2, (uint8_t)k, got);
			if (r >= 0 && !memcmp(got, blks[k], P25_TSBK_BYTES))
				fixed[errs]++;
			else if (r >= 0)
				wrong++;	// decoded to another block whose CRC held
		}
	}
	tries = 40;
	printf("  trellis, blocks of %u corrected with 1..6 dibits in error: %u %u %u %u %u %u\n", tries, fixed[1], fixed[2],
	       fixed[3], fixed[4], fixed[5], fixed[6]);
	CHECK(fixed[1] == tries && fixed[2] >= tries * 9 / 10 && fixed[3] >= tries * 3 / 4 && wrong == 0,
	      "trellis: %u %u %u of %u corrected, %u wrong", fixed[1], fixed[2], fixed[3], tries, wrong);
	// A block of noise: the CRC catches it
	for (unsigned i = 0; i < sizeof(unit); i++)
		unit[i] = (uint8_t)rng();
	missed = 0;
	for (unsigned t = 0; t < 2000; t++) {
		for (unsigned i = 14; i < 14 + 25; i++)
			unit[i] = (uint8_t)rng();
		if (p25_block_get(unit, 0, got) >= 0)
			missed++;
	}
	CHECK(missed <= 1, "noise blocks passing the CRC: %u of 2000", missed);
}

// ── Channel numbers to frequencies ──
static void testChannels(void)
{
	// 851.00625 MHz, 6.25 kHz steps
	CHECK(p25_chan_hz(0x1000, 851006250, 1250, 1) == 851006250, "channel 0");
	CHECK(p25_chan_hz(0x1001, 851006250, 1250, 1) == 851012500, "channel 1");
	CHECK(p25_chan_hz(0x1FFF, 851006250, 1250, 1) == 876600000, "channel 4095: %u", p25_chan_hz(0x1FFF, 851006250, 1250, 1));
	// 136 MHz, 2.5 kHz steps (20 x 125 Hz on air)
	CHECK(p25_chan_hz(0x0010, 136000000, 500, 1) == 136040000, "VHF channel 16");
	// Two slots per carrier, 12.5 kHz: channels 6 and 7 are both on carrier 3
	CHECK(p25_chan_hz(0x2006, 769006250, 2500, 2) == 769043750 && p25_chan_hz(0x2007, 769006250, 2500, 2) == 769043750,
	      "TDMA carrier");
	// Past 32 bits, or no base: none
	CHECK(p25_chan_hz(0x0FFF, 0xFFFFFF00u, 1250, 1) == 0 && p25_chan_hz(0x0001, 0, 1250, 1) == 0, "out of range");
}

// ── The follower on blocks fed by hand ──
#define HZ_CC1		851031250u	// iden 1 channel 4
#define HZ_CC2		851050000u	// programmed only by frequency
#define HZ_VA		851068750u	// iden 1 channel 10
#define WACN		0xBEE00u
#define SYSID		0x2F3u
#define NAC		0x293u
#define OUR_UNIT	10001u

static struct { uint32_t hz; uint8_t what; uint32_t id; uint32_t at; } tunes[400];
static unsigned ntunes, nfallback;
static uint32_t now_ms;
static bool tune_pending;

static void h_tune(uint32_t hz, uint8_t what, uint32_t id, void *ctx)
{
	(void)ctx;
	if (ntunes < 400)
		tunes[ntunes++] = (typeof(tunes[0])){ hz, what, id, now_ms };
	tune_pending = true;
}
static void h_fallback(void *ctx) { (void)ctx; nfallback++; }
static const p25_follow_hooks_t fhooks = { h_tune, h_fallback, NULL };

static void mk_net(uint8_t b[P25_TSBK_BYTES], uint32_t wacn, uint16_t sysid)
{
	p25_tsbk_t m = { .kind = P25_TSBK_NET, .last = true, .u.net = { .lra = 1, .wacn = wacn, .sysid = sysid, .ch = 0x1004 } };

	p25_tsbk_make(&m, b);
}
static void mk_rfss(uint8_t b[P25_TSBK_BYTES], uint8_t kind, uint16_t sysid, uint8_t site, uint16_t ch)
{
	p25_tsbk_t m = { .kind = kind, .u.site = { .lra = 1, .sysid = sysid, .rfss = 1, .site = site, .ch = ch, .ssc = 0x70 } };

	p25_tsbk_make(&m, b);
}
static void mk_iden(uint8_t b[P25_TSBK_BYTES], uint8_t form, uint8_t id, uint8_t type, uint32_t base_hz, uint16_t spacing)
{
	p25_tsbk_t m = { .kind = P25_TSBK_IDEN, .u.iden = { .id = id, .form = form, .type = type, .base = base_hz / 5,
				     .spacing = spacing, .tx_off = -180, .bw_khz8 = 100 } };

	p25_tsbk_make(&m, b);
}
static void mk_grant(uint8_t b[P25_TSBK_BYTES], uint8_t op, uint8_t svc, uint16_t ch, uint16_t tg, uint32_t src)
{
	p25_tsbk_t m = { .kind = P25_TSBK_GRANT, .opcode = op };

	m.u.grant.svc = svc;
	m.u.grant.ch = ch;
	if (op == P25_OP_UU_V_GRANT || op == P25_OP_UU_V_UPDT)
		m.u.grant.dst = tg;
	else
		m.u.grant.tg = tg;
	m.u.grant.src = src;
	p25_tsbk_make(&m, b);
}
static void mk_sccb(uint8_t b[P25_TSBK_BYTES], uint16_t ch)
{
	p25_tsbk_t m = { .kind = P25_TSBK_SCCB, .u.sccb = { .rfss = 1, .site = 3, .ch = { ch, 0 } } };

	p25_tsbk_make(&m, b);
}

static void retune(p25_follow_t *f)
{
	tune_pending = false;
	p25_follow_retuned(f, now_ms);
}
static void unit_duid(p25_follow_t *f, uint8_t duid)
{
	static uint8_t air[P25_UNIT_BYTES_MAX];
	p25_rx_unit_t u = { .duid = duid, .nac = NAC, .nsyms = P25_LDU_SYMS, .air = air };

	p25_follow_unit(f, &u, now_ms);
}
static bool cc_alive;	// the control channel keeps sending (a network status block every 200 ms)
static void run_ms(p25_follow_t *f, uint32_t ms)
{
	for (uint32_t end = now_ms + ms; now_ms < end;) {
		now_ms += 10;
		if (cc_alive && now_ms % 200 == 0 && f->view.state == P25_FOLLOW_CONTROL) {
			uint8_t b[P25_TSBK_BYTES];

			mk_net(b, WACN, SYSID);
			p25_follow_tsbk(f, b, NAC, now_ms);
		}
		p25_follow_tick(f, now_ms);
		if (tune_pending)
			retune(f);
	}
}
static p25_follow_view_t view(const p25_follow_t *f)
{
	p25_follow_view_t v;

	p25_follow_view(f, &v);
	return v;
}

// ── Inbound signaling: the radio's blocks, and the answers to them ──
static void testInbound(void)
{
	static uint8_t unit[P25_UNIT_BYTES_MAX];
	p25_isp_t m = { .opcode = P25_ISP_GRP_V_REQ, .svc = 0x04, .tg = 0x1234, .src = 0xABCDEF }, g;
	uint8_t b[P25_TSBK_BYTES], c[P25_TSBK_BYTES];
	p25_tsbk_t r, q;
	uint16_t nac, n;
	uint8_t duid;

	// Layout by hand: a voice request is the outbound grant with the channel reserved
	p25_isp_make(&m, b);
	CHECK(b[0] == (P25_TSBK_LB | 0x00) && b[1] == 0 && b[2] == 0x04 && b[3] == 0 && b[4] == 0 && b[5] == 0x12 &&
	      b[6] == 0x34 && b[7] == 0xAB && b[8] == 0xCD && b[9] == 0xEF && p25_tsbk_crc(b) == (uint16_t)(b[10] << 8 | b[11]),
	      "voice request layout");
	CHECK(p25_isp_parse(b, &g) && g.known && g.svc == 0x04 && g.tg == 0x1234 && g.src == 0xABCDEF, "voice request back");
	m = (p25_isp_t){ .opcode = P25_ISP_GRP_AFF_REQ, .emergency = true, .sysid = SYSID, .tg = 201, .src = OUR_UNIT };
	p25_isp_make(&m, b);
	CHECK(p25_isp_parse(b, &g) && g.emergency && g.sysid == SYSID && g.tg == 201 && g.src == OUR_UNIT, "affiliation request");
	m = (p25_isp_t){ .opcode = P25_ISP_U_REG_REQ, .wacn = WACN, .sysid = SYSID, .src = OUR_UNIT };
	p25_isp_make(&m, b);
	CHECK(p25_isp_parse(b, &g) && g.wacn == WACN && g.sysid == SYSID && g.src == OUR_UNIT && g.opcode == 0x2C,
	      "registration request");
	b[1] = 0x90;
	p25_tsbk_seal(b);
	CHECK(!p25_isp_parse(b, &g) && g.mfid == 0x90, "inbound manufacturer's block not taken");

	// The unit: a one-block TSDU, status symbols "unknown"; through the receive side's decoder, clean and with errors
	p25_isp_make(&m, b);
	n = p25_isp_unit(unit, NAC, b);
	CHECK(n == p25_blocks_syms(1) && n == 180, "inbound unit length %u", n);
	CHECK(p25_nid_get(unit, &nac, &duid) == 0 && nac == NAC && duid == P25_DUID_TSDU, "inbound NID");
	CHECK(p25_status_get(unit, 0) == P25_SS_UNKNOWN && p25_status_get(unit, 4) == P25_SS_UNKNOWN, "inbound status symbols");
	CHECK(p25_block_get(unit, 0, c) == 0 && memcmp(b, c, P25_TSBK_BYTES) == 0, "inbound block round trip");
	for (unsigned k = 0; k < 4; k++)
		p25_bits_put(unit, (uint16_t)(112 + 30 + 40 * k), 1, p25_bits_get(unit, (uint16_t)(112 + 30 + 40 * k), 1) ^ 1);
	CHECK(p25_block_get(unit, 0, c) > 0 && memcmp(b, c, P25_TSBK_BYTES) == 0, "inbound block with 4 bit errors");
	CHECK(p25_isp_parse(c, &g) && g.src == OUR_UNIT, "inbound block parsed after the errors");

	// The answers
	r = (p25_tsbk_t){ .kind = P25_TSBK_REG_RSP, .last = true, .u.reg = { .rv = P25_RV_DENY, .sysid = SYSID, .id = OUR_UNIT, .addr = 0x123456 } };
	p25_tsbk_make(&r, b);
	CHECK(p25_tsbk_parse(b, &q) && q.kind == P25_TSBK_REG_RSP && q.opcode == 0x2C && q.u.reg.rv == P25_RV_DENY &&
	      q.u.reg.sysid == SYSID && q.u.reg.id == OUR_UNIT && q.u.reg.addr == 0x123456, "registration response");
	r = (p25_tsbk_t){ .kind = P25_TSBK_AFF_RSP, .u.aff = { .global = true, .gav = P25_RV_ACCEPT, .ann = 77, .tg = 201, .dst = OUR_UNIT } };
	p25_tsbk_make(&r, b);
	CHECK(p25_tsbk_parse(b, &q) && q.kind == P25_TSBK_AFF_RSP && q.opcode == 0x28 && q.u.aff.global && q.u.aff.gav == 0 &&
	      q.u.aff.ann == 77 && q.u.aff.tg == 201 && q.u.aff.dst == OUR_UNIT, "affiliation response");
	r = (p25_tsbk_t){ .kind = P25_TSBK_DENY, .u.rsp = { .aiv = true, .svc = P25_ISP_GRP_V_REQ, .reason = 0x59, .info = 201, .dst = OUR_UNIT } };
	p25_tsbk_make(&r, b);
	CHECK(p25_tsbk_parse(b, &q) && q.kind == P25_TSBK_DENY && q.opcode == 0x27 && q.u.rsp.aiv && q.u.rsp.svc == 0 &&
	      q.u.rsp.reason == 0x59 && q.u.rsp.info == 201 && q.u.rsp.dst == OUR_UNIT, "deny response");
	CHECK(b[2] == 0x80 && b[3] == 0x59 && b[7] == (uint8_t)(OUR_UNIT >> 16), "deny response layout");
	r.kind = P25_TSBK_QUEUED;
	p25_tsbk_make(&r, b);
	CHECK(p25_tsbk_parse(b, &q) && q.kind == P25_TSBK_QUEUED && q.opcode == 0x21 && q.u.rsp.reason == 0x59, "queued response");
}

static void testFollowHand(void)
{
	static const uint16_t groups[] = { 200 };
	static const p25_follow_cc_t cc[] = { { .hz = HZ_CC1, .site = 3 }, { .hz = HZ_CC2, .site = 3 } };
	static p25_follow_t f;
	p25_follow_cfg_t cfg = { .wacn = WACN, .sysid = SYSID, .nac = NAC, .flags = P25_FOLLOW_SKIP_ENC, .unit_id = OUR_UNIT,
				 .talk_group = 100, .ngroups = 1, .groups = groups, .ncc = 2, .cc = cc, .fallback_ms = 5000 };
	uint8_t b[P25_TSBK_BYTES];
	p25_follow_view_t v;
	unsigned n;

	ntunes = nfallback = 0;
	now_ms = 1000;
	p25_follow_begin(&f, &cfg, &fhooks, NULL, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_HUNT && ntunes == 1 && tunes[0].hz == HZ_CC1 && tunes[0].what == P25_FOLLOW_TUNE_CONTROL,
	      "begin: hunting on the first: %u tunes", ntunes);
	// Before the receiver is there, nothing counts
	mk_net(b, WACN, SYSID);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_HUNT && view(&f).blocks == 0, "units of the old channel ignored");
	retune(&f);
	// Nothing heard: the next after the dwell
	run_ms(&f, P25_FOLLOW_DWELL_MS + 20);
	CHECK(ntunes == 2 && tunes[1].hz == HZ_CC2, "dwell: the second: %u tunes", ntunes);
	// Another system there: back to the first at once
	mk_net(b, 0x12345, SYSID);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(ntunes == 3 && tunes[2].hz == HZ_CC1 && v.wrong_sys == 1 && v.state == P25_FOLLOW_HUNT, "wrong WACN: next");
	retune(&f);
	// (that was the pass: it waits on the first). A block of another NAC: not ours either
	mk_rfss(b, P25_TSBK_RFSS, SYSID, 3, 0x1004);
	p25_follow_tsbk(&f, b, 0x111, now_ms);
	CHECK(ntunes == 3 && view(&f).wrong_sys == 2, "another NAC");
	run_ms(&f, P25_FOLLOW_BACKOFF_MS + 20);
	CHECK(ntunes == 4 && tunes[3].hz == HZ_CC1, "the next pass: %u", ntunes);
	// RFSS status names the system but not its WACN: we wait for the identity, which then comes
	p25_follow_tsbk(&f, b, NAC, now_ms);
	run_ms(&f, P25_FOLLOW_DWELL_MS + 100);
	CHECK(view(&f).state == P25_FOLLOW_HUNT && ntunes == 4, "signaling heard: the identity is waited for past the dwell");
	mk_net(b, WACN, SYSID);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_CONTROL && v.cc == 0 && v.cc_hz == HZ_CC1 && v.wacn == WACN && v.sysid == SYSID &&
	      v.nac == NAC && v.op[P25_OP_NET_STS_BCST] == 2 && v.op[P25_OP_RFSS_STS_BCST] == 1, "control channel held");
	n = ntunes;
	cc_alive = true;

	// A grant for our group before its identifier is known: refused
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, 5555);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_CONTROL && v.refused == P25_REFUSE_NO_IDEN && v.no_iden == 1 && ntunes == n, "no identifier");
	mk_iden(b, P25_OP_IDEN_UP, 1, 0, 851006250, 50);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).idens == 0x0002, "identifier 1 learned");
	// A group not ours: nothing
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 300, 5555);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL && ntunes == n && view(&f).grants == 2, "another group: not followed");
	// Ours: to the voice channel, iden 1 channel 10
	mk_grant(b, P25_OP_GRP_V_GRANT, P25_SVC_EMERGENCY, 0x100A, 100, 5555);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_VOICE && ntunes == n + 1 && tunes[n].hz == HZ_VA && tunes[n].what == P25_FOLLOW_TUNE_GROUP &&
	      tunes[n].id == 100 && v.tg == 100 && v.src == 5555 && !v.enc, "followed to %u", tunes[n].hz);
	retune(&f);
	// Voice comes and goes for a while; the terminator brings us back
	for (unsigned k = 0; k < 10; k++) {
		run_ms(&f, 180);
		unit_duid(&f, k & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1);
	}
	CHECK(view(&f).state == P25_FOLLOW_VOICE, "on the voice channel while voice comes");
	unit_duid(&f, P25_DUID_TDULC);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_CONTROL && ntunes == n + 2 && tunes[n + 1].hz == HZ_CC1 && v.follows == 1, "back on the terminator");
	retune(&f);
	// The call's updates go on a moment: not joined again; a new grant after that is
	mk_grant(b, P25_OP_GRP_V_UPDT, 0, 0x100A, 100, 0);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL && ntunes == n + 2, "trailing update skipped");
	run_ms(&f, P25_FOLLOW_REJOIN_MS);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_VOICE && ntunes == n + 3, "a later update is followed");
	retune(&f);
	// The voice channel carries another group's call: back
	{
		p25_call_t c = { .nac = NAC, .have_lc = true, .lc = { .lcf = P25_LCF_GROUP, .tgid = 999, .src = 1 } };

		p25_follow_call(&f, &c, now_ms);
		CHECK(view(&f).state == P25_FOLLOW_CONTROL && view(&f).wrong_call == 1 && ntunes == n + 4, "wrong call: back");
		retune(&f);
	}
	// Voice lost without a terminator
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 200, 6666);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_VOICE && view(&f).tg == 200 && ntunes == n + 5, "group list followed");
	retune(&f);
	run_ms(&f, P25_FOLLOW_VOICE_LOST_MS - 100);
	CHECK(view(&f).state == P25_FOLLOW_VOICE, "voice lost: not yet");
	run_ms(&f, 200);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_CONTROL && v.voice_lost == 1 && ntunes == n + 6 && tunes[n + 5].hz == HZ_CC1, "voice lost: back");
	retune(&f);
	// An encrypted grant is skipped, and its updates with it, for as long as they come
	mk_grant(b, P25_OP_GRP_V_GRANT, P25_SVC_PROTECTED, 0x100B, 100, 7777);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_CONTROL && v.refused == P25_REFUSE_ENC && v.enc_skip == 1 && ntunes == n + 6, "encrypted grant");
	mk_grant(b, P25_OP_GRP_V_UPDT, 0, 0x100B, 100, 0);
	for (unsigned k = 0; k < 5; k++) {
		run_ms(&f, 900);
		p25_follow_tsbk(&f, b, NAC, now_ms);
	}
	CHECK(view(&f).state == P25_FOLLOW_CONTROL && ntunes == n + 6, "its updates skipped while they come");
	// A call found encrypted on the voice channel (grant without the flag): back, its updates skipped
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100C, 100, 7778);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_VOICE && ntunes == n + 7, "a grant without the flag is followed");
	retune(&f);
	{
		p25_call_t c = { .nac = NAC, .have_alg = true, .algid = 0x84 };

		p25_follow_call(&f, &c, now_ms);
		v = view(&f);
		CHECK(v.state == P25_FOLLOW_CONTROL && v.enc && v.enc_skip == 2 && ntunes == n + 8, "found encrypted: back");
		retune(&f);
		mk_grant(b, P25_OP_GRP_V_UPDT, 0, 0x100C, 100, 0);
		p25_follow_tsbk(&f, b, NAC, now_ms);
		CHECK(view(&f).state == P25_FOLLOW_CONTROL && ntunes == n + 8, "its updates skipped");
	}
	// A two-slot channel: refused
	mk_iden(b, P25_OP_IDEN_UP_TDMA, 2, 3, 769006250, 100);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x2003, 100, 1);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_CONTROL && v.refused == P25_REFUSE_TDMA && v.tdma == 1 && v.idens == 0x0006 && ntunes == n + 8,
	      "two-slot channel refused");
	// A unit to unit call to us
	mk_grant(b, P25_OP_UU_V_GRANT, 0, 0x100D, OUR_UNIT, 4242);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_VOICE && v.unit_call && v.dst == OUR_UNIT && v.src == 4242 && ntunes == n + 9 &&
	      tunes[n + 8].what == P25_FOLLOW_TUNE_UNIT && tunes[n + 8].id == OUR_UNIT, "unit call to us");
	retune(&f);
	unit_duid(&f, P25_DUID_TDU);
	retune(&f);
	// ... and one between two others: not ours
	mk_grant(b, P25_OP_UU_V_GRANT, 0, 0x100D, 1, 2);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL && ntunes == n + 10, "unit call between others");

	// Control channels learned off air: an adjacent site and a secondary channel
	mk_rfss(b, P25_TSBK_ADJ, SYSID, 4, 0x1010);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	mk_rfss(b, P25_TSBK_ADJ, 0x111, 4, 0x1011);	// another system's: not
	p25_follow_tsbk(&f, b, NAC, now_ms);
	mk_sccb(b, 0x1012);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	p25_follow_tsbk(&f, b, NAC, now_ms);	// once only
	CHECK(view(&f).learned == 2, "learned: %u", view(&f).learned);
	cc_alive = false;
	// The control channel goes: hunt from it, then the second, then the learned ones; a whole pass, then a wait
	n = ntunes;
	run_ms(&f, P25_FOLLOW_CC_LOST_MS + 20);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_HUNT && v.cc_lost == 1 && ntunes == n + 1 && tunes[n].hz == HZ_CC1, "lost: hunting");
	run_ms(&f, 3 * P25_FOLLOW_DWELL_MS + 50);
	CHECK(ntunes == n + 4 && tunes[n + 1].hz == HZ_CC2 && tunes[n + 2].hz == 851006250u + 16 * 6250 &&
	      tunes[n + 3].hz == 851006250u + 18 * 6250, "hunt order: %u %u %u", tunes[n + 1].hz, tunes[n + 2].hz,
	      tunes[n + 3].hz);
	run_ms(&f, P25_FOLLOW_DWELL_MS);
	CHECK(ntunes == n + 5 && tunes[n + 4].hz == HZ_CC1, "after a pass: wait on the last good one");
	run_ms(&f, P25_FOLLOW_BACKOFF_MS - 100);
	CHECK(ntunes == n + 5 && nfallback == 0, "waiting");
	run_ms(&f, 200);
	CHECK(ntunes == n + 6, "the next pass");
	run_ms(&f, 3000);
	CHECK(nfallback == 1, "fallback after %u ms: %u", cfg.fallback_ms, nfallback);
	// The waits double: count the tunes over a minute
	n = ntunes;
	run_ms(&f, 60000);
	CHECK(nfallback == 1 && ntunes - n < 40, "backoff: %u tunes in a minute", ntunes - n);
	// It answers again
	mk_net(b, WACN, SYSID);
	{
		uint32_t t0 = now_ms;

		while (view(&f).state != P25_FOLLOW_CONTROL && now_ms - t0 < 40000) {
			run_ms(&f, 100);
			if (!f.tuning && f.view.cc_hz == HZ_CC1)
				p25_follow_tsbk(&f, b, NAC, now_ms);
		}
		CHECK(view(&f).state == P25_FOLLOW_CONTROL && !view(&f).fallback, "found again after %u ms", now_ms - t0);
	}
	p25_follow_end(&f);
	CHECK(view(&f).state == P25_FOLLOW_OFF, "off");
}

// Hold, a plan only, programmed identifiers and control channels by number, any system
// ── The transmit side: registration, affiliation, voice requests ──
static struct { uint32_t hz, at; uint8_t blk[P25_TSBK_BYTES]; } sends[64];
static unsigned nsends;
static bool send_refuse;
static bool h_send(uint32_t hz, const uint8_t blk[P25_TSBK_BYTES], void *ctx)
{
	(void)ctx;
	if (send_refuse)
		return false;
	if (nsends < 64) {
		sends[nsends].hz = hz;
		sends[nsends].at = now_ms;
		memcpy(sends[nsends].blk, blk, P25_TSBK_BYTES);
		nsends++;
	}
	return true;
}
static const p25_follow_hooks_t ahooks = { h_tune, NULL, h_send };

static p25_isp_t last_isp(void)
{
	p25_isp_t m = { 0 };

	if (nsends)
		p25_isp_parse(sends[nsends - 1].blk, &m);
	return m;
}
static void answer(p25_follow_t *f, const p25_tsbk_t *m)
{
	uint8_t b[P25_TSBK_BYTES];

	p25_tsbk_make(m, b);
	p25_follow_tsbk(f, b, NAC, now_ms);
}
static void answer_reg(p25_follow_t *f, uint8_t rv)
{
	p25_tsbk_t m = { .kind = P25_TSBK_REG_RSP, .u.reg = { .rv = rv, .sysid = SYSID, .id = OUR_UNIT, .addr = OUR_UNIT } };

	answer(f, &m);
}
static void answer_aff(p25_follow_t *f, uint8_t gav, uint16_t tg)
{
	p25_tsbk_t m = { .kind = P25_TSBK_AFF_RSP, .u.aff = { .gav = gav, .ann = tg, .tg = tg, .dst = OUR_UNIT } };

	answer(f, &m);
}
static void answer_rsp(p25_follow_t *f, uint8_t kind, uint8_t svc, uint8_t reason)
{
	p25_tsbk_t m = { .kind = kind, .u.rsp = { .svc = svc, .reason = reason, .dst = OUR_UNIT } };

	answer(f, &m);
}
static void site_ident(p25_follow_t *f, uint8_t site)
{
	uint8_t b[P25_TSBK_BYTES];

	mk_net(b, WACN, SYSID);
	p25_follow_tsbk(f, b, NAC, now_ms);
	mk_rfss(b, P25_TSBK_RFSS, SYSID, site, 0x1004);
	p25_follow_tsbk(f, b, NAC, now_ms);
}
// A TSDU of one network status block whose status symbols are ss, through the unit entry
static void cc_unit_ss(p25_follow_t *f, uint8_t ss)
{
	static uint8_t air[P25_UNIT_BYTES_MAX];
	uint8_t b[P25_TSBK_BYTES];
	p25_rx_unit_t u = { .duid = P25_DUID_TSDU, .nac = NAC, .nsyms = 180, .air = air };

	mk_net(b, WACN, SYSID);
	p25_unit_begin(air, 180, NAC, P25_DUID_TSDU, ss);
	p25_block_put(air, 0, b);
	p25_follow_unit(f, &u, now_ms);
}

#define HZ_CC1_IN	(HZ_CC1 - 45000000u)
#define HZ_VA_IN	(HZ_VA - 45000000u)

// An answer heard while the request still waits for its turn is not ours: nothing is taken before it went out
static void hold_request(p25_follow_t *f)
{
	while (f->acc.pend == 0xFF)	// (NO_OP)
		run_ms(f, 1);
	f->acc.t_next = now_ms + 150;
}

static void testAccessEarlyAnswers(void)
{
	static const p25_follow_cc_t cc[] = { { .hz = HZ_CC1, .chan = 0x1004, .site = 3 } };
	static const p25_follow_iden_t iden[P25_FOLLOW_IDENS] = {
		[1] = { .base_hz = 851006250u, .spacing_hz = 6250, .tx_off_hz = -45000000 } };
	static p25_follow_t f;
	p25_follow_cfg_t cfg = { .wacn = WACN, .sysid = SYSID, .nac = NAC, .flags = P25_FOLLOW_TX, .unit_id = OUR_UNIT,
				 .talk_group = 100, .ncc = 1, .cc = cc, .iden_set = 1u << 1, .iden = iden };
	uint8_t b[P25_TSBK_BYTES];
	p25_follow_view_t v;
	unsigned n;

	ntunes = nsends = 0;
	now_ms = 9000;
	cc_alive = true;
	p25_follow_begin(&f, &cfg, &ahooks, NULL, now_ms);
	retune(&f);
	site_ident(&f, 3);
	hold_request(&f);
	CHECK(nsends == 0 && view(&f).reg == P25_REG_WAIT, "registration queued, not sent: %u", nsends);
	answer_reg(&f, P25_RV_ACCEPT);
	answer_rsp(&f, P25_TSBK_DENY, P25_ISP_U_REG_REQ, 0x10);
	v = view(&f);
	CHECK(v.reg == P25_REG_WAIT && v.reg_ok == 0 && v.denials == 0, "answers before the request: not ours (%u)", v.reg);
	run_ms(&f, 200);
	CHECK(nsends == 1 && last_isp().opcode == P25_ISP_U_REG_REQ, "the request goes out: %u", nsends);
	answer_reg(&f, P25_RV_ACCEPT);
	hold_request(&f);
	CHECK(nsends == 1 && view(&f).reg == P25_REG_AFF, "affiliation queued, not sent: %u", nsends);
	answer_aff(&f, P25_RV_ACCEPT, 100);
	v = view(&f);
	CHECK(v.reg == P25_REG_AFF && v.aff_ok == 0, "an affiliation answer before the request: not ours");
	run_ms(&f, 200);
	CHECK(nsends == 2 && last_isp().opcode == P25_ISP_GRP_AFF_REQ, "the affiliation goes out: %u", nsends);
	answer_aff(&f, P25_RV_ACCEPT, 100);
	CHECK(view(&f).reg == P25_REG_READY, "ready");
	// A group call granted to our own unit while we ask for nothing: not followed (not a call to listen to)
	n = ntunes;
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, OUR_UNIT);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(ntunes == n && view(&f).state == P25_FOLLOW_CONTROL && view(&f).follows == 0, "our own unit's grant: not followed");
	// A grant that names us before our request went out is not its answer: no voice channel, nothing keyed
	p25_follow_ptt(&f, true, now_ms);
	hold_request(&f);
	n = ntunes;
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, OUR_UNIT);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	answer_rsp(&f, P25_TSBK_QUEUED, P25_ISP_GRP_V_REQ, 0x2F);
	v = view(&f);
	CHECK(nsends == 2 && ntunes == n && v.req == P25_REQ_WAIT && v.req_grant == 0 && v.queued == 0 &&
	      v.state == P25_FOLLOW_CONTROL, "a grant before the request: not ours (%u)", v.req);
	run_ms(&f, 200);
	CHECK(nsends == 3 && last_isp().opcode == P25_ISP_GRP_V_REQ, "the voice request goes out: %u", nsends);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(v.req == P25_REQ_TALK && v.req_grant == 1 && ntunes == n + 1, "the grant after it: ours");
	p25_follow_end(&f);
}

static void testAccess(void)
{
	static const p25_follow_cc_t cc[] = { { .hz = HZ_CC1, .chan = 0x1004, .site = 3 } };
	static const p25_follow_iden_t iden[P25_FOLLOW_IDENS] = {
		[1] = { .base_hz = 851006250u, .spacing_hz = 6250, .tx_off_hz = -45000000 } };
	static p25_follow_t f;
	p25_follow_cfg_t cfg = { .wacn = WACN, .sysid = SYSID, .nac = NAC, .flags = P25_FOLLOW_TX, .unit_id = OUR_UNIT,
				 .talk_group = 100, .ncc = 1, .cc = cc, .iden_set = 1u << 1, .iden = iden };
	uint8_t b[P25_TSBK_BYTES];
	p25_follow_view_t v;
	p25_isp_t m;
	unsigned n;

	// Registration: once the control channel is held and its site heard, at its inbound frequency
	ntunes = nsends = 0;
	now_ms = 5000;
	cc_alive = true;
	p25_follow_begin(&f, &cfg, &ahooks, NULL, now_ms);
	retune(&f);
	CHECK(view(&f).reg == P25_REG_IDLE && p25_follow_can_talk(&f.view, cfg.flags) == P25_TALK_NO_CC, "begin: to register");
	site_ident(&f, 3);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL, "control channel held");
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 300);
	m = last_isp();
	CHECK(nsends == 1 && sends[0].hz == HZ_CC1_IN && m.opcode == P25_ISP_U_REG_REQ && m.src == OUR_UNIT &&
	      m.wacn == WACN && m.sysid == SYSID, "registration request sent: %u at %u", nsends, nsends ? sends[0].hz : 0);
	v = view(&f);
	CHECK(v.reg == P25_REG_WAIT && v.req == P25_REQ_IDLE && v.reg_tx == 1, "PTT before registration asks nothing");
	CHECK(p25_follow_can_talk(&v, cfg.flags) == P25_TALK_NOT_REG, "not registered");
	p25_follow_ptt(&f, false, now_ms);
	// No answer: sent again twice, then a pause of P25_ACC_RETRY_MS, then a new round
	run_ms(&f, 2 * (P25_ACC_ANSWER_MS + 4 * P25_ACC_GAP_MS));
	CHECK(nsends == 3 && view(&f).reg == P25_REG_WAIT, "two resends: %u", nsends);
	run_ms(&f, P25_ACC_ANSWER_MS + 20);
	v = view(&f);
	CHECK(nsends == 3 && v.reg == P25_REG_RETRY && v.timeouts == 1, "no answer: retry later (%u)", v.reg);
	run_ms(&f, P25_ACC_RETRY_MS - 1200);
	CHECK(nsends == 3, "the pause");
	run_ms(&f, 1600);
	CHECK(nsends == 4 && view(&f).reg == P25_REG_WAIT, "the next round: %u", nsends);
	// Accepted: affiliation to the talk group, then ready
	answer_reg(&f, P25_RV_ACCEPT);
	run_ms(&f, 200);
	m = last_isp();
	CHECK(nsends == 5 && m.opcode == P25_ISP_GRP_AFF_REQ && m.tg == 100 && m.src == OUR_UNIT && m.sysid == SYSID &&
	      view(&f).reg == P25_REG_AFF, "affiliation request");
	answer_aff(&f, P25_RV_ACCEPT, 300);	// another group's answer: not ours
	CHECK(view(&f).reg == P25_REG_AFF, "another group's affiliation ignored");
	answer_aff(&f, P25_RV_ACCEPT, 100);
	v = view(&f);
	CHECK(v.reg == P25_REG_READY && v.aff_tg == 100 && v.reg_ok == 1 && v.aff_ok == 1 &&
	      p25_follow_can_talk(&v, cfg.flags) == P25_TALK_OK, "ready");
	run_ms(&f, 3000);
	CHECK(nsends == 5, "ready: quiet");

	// A voice request granted: our voice channel, its transmit frequency, held without voice until the end
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 200);
	m = last_isp();
	CHECK(nsends == 6 && m.opcode == P25_ISP_GRP_V_REQ && m.tg == 100 && m.src == OUR_UNIT && view(&f).req == P25_REQ_WAIT,
	      "voice request");
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, 4444);	// another unit's grant for our group: not ours
	n = ntunes;
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(ntunes == n && view(&f).state == P25_FOLLOW_CONTROL, "another unit's grant: not followed while we ask");
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, OUR_UNIT);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	v = view(&f);
	CHECK(ntunes == n + 1 && tunes[n].hz == HZ_VA && tunes[n].what == P25_FOLLOW_TUNE_TALK && tunes[n].id == 100 &&
	      v.req == P25_REQ_TALK && v.state == P25_FOLLOW_VOICE && v.talk_hz == HZ_VA_IN && v.req_grant == 1,
	      "granted: %u tunes, talk at %u", ntunes - n, v.talk_hz);
	retune(&f);
	run_ms(&f, 5000);
	CHECK(view(&f).state == P25_FOLLOW_VOICE && view(&f).voice_lost == 0, "transmitting: no voice-lost timer");
	unit_duid(&f, P25_DUID_TDU);
	CHECK(view(&f).state == P25_FOLLOW_VOICE, "a terminator heard while we talk: ours to end");
	p25_follow_ptt(&f, false, now_ms);
	CHECK(view(&f).req == P25_REQ_TALK, "release while talking: the caller ends it");
	p25_follow_talk_end(&f, now_ms);
	v = view(&f);
	CHECK(v.req == P25_REQ_IDLE && v.state == P25_FOLLOW_CONTROL && tunes[ntunes - 1].hz == HZ_CC1 &&
	      tunes[ntunes - 1].what == P25_FOLLOW_TUNE_CONTROL, "talk end: back to the control channel");
	retune(&f);
	run_ms(&f, 500);

	// Queued, then granted
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 200);
	n = nsends;
	answer_rsp(&f, P25_TSBK_QUEUED, P25_ISP_GRP_V_REQ, 0x2F);
	CHECK(view(&f).req == P25_REQ_QUEUED && view(&f).queued == 1, "queued");
	run_ms(&f, 4000);
	CHECK(nsends == n && view(&f).req == P25_REQ_QUEUED, "queued: not sent again");
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).req == P25_REQ_TALK, "queued, then granted");
	retune(&f);
	p25_follow_talk_end(&f, now_ms);
	retune(&f);
	p25_follow_ptt(&f, false, now_ms);
	run_ms(&f, 500);

	// Denied
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 200);
	answer_rsp(&f, P25_TSBK_DENY, P25_ISP_U_REG_REQ, 0x10);	// for another request: not this one's answer
	CHECK(view(&f).req == P25_REQ_WAIT, "a deny of another request ignored");
	answer_rsp(&f, P25_TSBK_DENY, P25_ISP_GRP_V_REQ, 0x59);
	v = view(&f);
	CHECK(v.req == P25_REQ_DENIED && v.deny_svc == 0 && v.deny_reason == 0x59 && v.reg == P25_REG_READY, "denied");
	n = nsends;
	run_ms(&f, 3000);
	CHECK(nsends == n, "denied: not sent again");
	p25_follow_ptt(&f, false, now_ms);
	CHECK(view(&f).req == P25_REQ_IDLE, "released: idle");

	// No answer at all: three sends, then timed out
	n = nsends;
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 3 * (P25_ACC_ANSWER_MS + 4 * P25_ACC_GAP_MS) + 100);
	v = view(&f);
	CHECK(nsends == n + 3 && v.req == P25_REQ_TIMEOUT, "voice request timed out after %u sends", nsends - n);
	p25_follow_ptt(&f, false, now_ms);
	// Released before the grant: the grant that comes later is not taken
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 200);
	p25_follow_ptt(&f, false, now_ms);
	n = ntunes;
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, OUR_UNIT);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	CHECK(view(&f).req == P25_REQ_IDLE && tunes[ntunes - 1].what != P25_FOLLOW_TUNE_TALK, "a grant after release not taken");
	if (view(&f).state == P25_FOLLOW_VOICE) {	// (followed as a listener: back)
		retune(&f);
		unit_duid(&f, P25_DUID_TDU);
		retune(&f);
	}
	run_ms(&f, 500);

	// PTT while following a call of our group: back to the control channel, the request from there
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, 4444);
	run_ms(&f, P25_FOLLOW_REJOIN_MS);
	p25_follow_tsbk(&f, b, NAC, now_ms);
	retune(&f);
	CHECK(view(&f).state == P25_FOLLOW_VOICE && view(&f).req == P25_REQ_IDLE, "following our group's call");
	n = nsends;
	p25_follow_ptt(&f, true, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL && tunes[ntunes - 1].hz == HZ_CC1 && view(&f).req == P25_REQ_WAIT,
	      "PTT on a voice channel: back to ask");
	run_ms(&f, 100);
	CHECK(nsends == n, "nothing sent before the control channel is heard again");
	retune(&f);
	run_ms(&f, 400);
	CHECK(nsends == n + 1 && last_isp().opcode == P25_ISP_GRP_V_REQ, "then the request");
	p25_follow_ptt(&f, false, now_ms);

	// The inbound channel busy (status symbols): the send waits until they say idle
	n = nsends;
	cc_alive = false;
	cc_unit_ss(&f, P25_SS_BUSY);
	p25_follow_ptt(&f, true, now_ms);
	for (unsigned k = 0; k < 20; k++) {
		run_ms(&f, 20);
		cc_unit_ss(&f, P25_SS_BUSY);
	}
	CHECK(nsends == n && view(&f).inb_busy > 0, "busy inbound: held back (%u)", view(&f).inb_busy);
	cc_unit_ss(&f, P25_SS_IDLE);
	run_ms(&f, 100);
	CHECK(nsends == n + 1, "idle inbound: sent");
	p25_follow_ptt(&f, false, now_ms);

	// The control channel lost and found on the same site: still registered. On another site: registers again
	run_ms(&f, P25_FOLLOW_CC_LOST_MS + 50);
	CHECK(view(&f).state == P25_FOLLOW_HUNT && view(&f).reg == P25_REG_READY, "lost: registration stands");
	retune(&f);
	n = nsends;
	site_ident(&f, 3);
	cc_alive = true;
	run_ms(&f, 1000);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL && view(&f).reg == P25_REG_READY && nsends == n, "same site: no registration");
	cc_alive = false;
	run_ms(&f, P25_FOLLOW_CC_LOST_MS + 50);
	retune(&f);
	site_ident(&f, 4);
	cc_alive = true;
	run_ms(&f, 500);
	CHECK(view(&f).reg == P25_REG_WAIT && nsends == n + 1 && last_isp().opcode == P25_ISP_U_REG_REQ, "another site: registers");
	// Denied there: no talking, no more sends
	answer_reg(&f, P25_RV_DENY);
	v = view(&f);
	CHECK(v.reg == P25_REG_DENIED && v.deny_svc == P25_ISP_U_REG_REQ && v.deny_reason == P25_RV_DENY &&
	      p25_follow_can_talk(&v, cfg.flags) == P25_TALK_NOT_REG, "registration denied");
	n = nsends;
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 15000);
	CHECK(nsends == n && view(&f).req == P25_REQ_IDLE, "denied: silent");
	p25_follow_ptt(&f, false, now_ms);
	p25_follow_end(&f);

	// Requests without registration (P25_FOLLOW_NO_REG); a send the caller refuses counts and is retried
	cfg.flags = P25_FOLLOW_TX | P25_FOLLOW_NO_REG;
	nsends = 0;
	p25_follow_begin(&f, &cfg, &ahooks, NULL, now_ms);
	retune(&f);
	site_ident(&f, 3);
	run_ms(&f, 3000);
	v = view(&f);
	CHECK(nsends == 0 && v.reg == P25_REG_OFF && p25_follow_can_talk(&v, cfg.flags) == P25_TALK_OK, "no registration");
	send_refuse = true;
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 200);
	CHECK(view(&f).send_fail == 1 && nsends == 0, "send refused");
	send_refuse = false;
	run_ms(&f, P25_ACC_ANSWER_MS + 4 * P25_ACC_GAP_MS);
	CHECK(nsends == 1 && last_isp().opcode == P25_ISP_GRP_V_REQ, "sent again");
	p25_follow_ptt(&f, false, now_ms);
	p25_follow_end(&f);

	// The transmit side off (a listen-only system): nothing sent, PTT can't talk
	cfg.flags = 0;
	nsends = 0;
	p25_follow_begin(&f, &cfg, &ahooks, NULL, now_ms);
	retune(&f);
	site_ident(&f, 3);
	p25_follow_ptt(&f, true, now_ms);
	run_ms(&f, 5000);
	v = view(&f);
	CHECK(nsends == 0 && v.reg == P25_REG_OFF && v.req == P25_REQ_IDLE && p25_follow_can_talk(&v, cfg.flags) == P25_TALK_OFF,
	      "listen only");
	p25_follow_end(&f);

	// A control channel with no inbound frequency (no offset known): no transmit side there
	static const p25_follow_iden_t iden0[P25_FOLLOW_IDENS] = { [1] = { .base_hz = 851006250u, .spacing_hz = 6250 } };
	cfg.flags = P25_FOLLOW_TX;
	cfg.iden = iden0;
	nsends = 0;
	p25_follow_begin(&f, &cfg, &ahooks, NULL, now_ms);
	retune(&f);
	site_ident(&f, 3);
	run_ms(&f, 1000);
	v = view(&f);
	CHECK(nsends == 0 && v.reg == P25_REG_NO_TX && p25_follow_can_talk(&v, cfg.flags) == P25_TALK_OFF, "no inbound frequency");
	// ... until an identifier update off air gives the offset (the VHF/UHF form: in channel spacings)
	{
		p25_tsbk_t im = { .kind = P25_TSBK_IDEN, .u.iden = { .id = 1, .form = P25_OP_IDEN_UP_VU, .base = 851006250u / 5,
								    .spacing = 50, .tx_off = -7200, .bw_khz8 = 100 } };
		answer(&f, &im);
	}
	cc_alive = false;
	run_ms(&f, P25_FOLLOW_CC_LOST_MS + 50);
	retune(&f);
	site_ident(&f, 3);
	cc_alive = true;
	run_ms(&f, 500);
	CHECK(nsends == 1 && sends[0].hz == HZ_CC1_IN, "offset learned: registers at %u", nsends ? sends[0].hz : 0);
	p25_follow_end(&f);
	cc_alive = false;
}

static void testFollowOptions(void)
{
	static const uint16_t groups[] = { 200 };
	static p25_follow_iden_t idens[P25_FOLLOW_IDENS];
	static const p25_follow_cc_t cc[] = { { .chan = 0x1004 } };
	static p25_follow_t f;
	p25_follow_cfg_t cfg = { .wacn = P25_FOLLOW_WACN_ANY, .sysid = P25_FOLLOW_SYSID_ANY, .nac = P25_NAC_ANY,
				 .flags = P25_FOLLOW_PLAN_ONLY, .talk_group = 100, .ngroups = 1, .groups = groups, .ncc = 1,
				 .cc = cc, .iden_set = 0x0002, .iden = idens, .hold_ms = 3000 };
	uint8_t b[P25_TSBK_BYTES];
	p25_follow_view_t v;

	idens[1] = (p25_follow_iden_t){ .base_hz = 851006250, .spacing_hz = 6250 };
	ntunes = 0;
	now_ms = 0;
	p25_follow_begin(&f, &cfg, &fhooks, NULL, now_ms);
	CHECK(ntunes == 1 && tunes[0].hz == HZ_CC1, "control channel by number: %u", tunes[0].hz);
	retune(&f);
	// Any system: the first block decoded holds it
	mk_iden(b, P25_OP_IDEN_UP, 1, 0, 852006250, 50);	// the air says otherwise: the programmed plan stands
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	mk_iden(b, P25_OP_IDEN_UP, 3, 0, 852006250, 50);	// and a plan only learns nothing
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	v = view(&f);
	CHECK(v.state == P25_FOLLOW_CONTROL && v.nac == 0x5A5 && v.idens == 0x0002, "any system held; plan only");
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x300A, 100, 1);
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	CHECK(view(&f).refused == P25_REFUSE_NO_IDEN, "identifier 3 not learned");
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, 1);
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_VOICE && tunes[ntunes - 1].hz == HZ_VA, "programmed plan used");
	retune(&f);
	unit_duid(&f, P25_DUID_TDU);
	retune(&f);
	// Hold: only group 100 for 3 s
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100B, 200, 1);
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL, "hold: another group of ours waits");
	run_ms(&f, 500);
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL, "hold: still");
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100A, 100, 1);
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_VOICE, "hold: the held group is followed");
	retune(&f);
	unit_duid(&f, P25_DUID_TDU);
	retune(&f);
	run_ms(&f, 3100);
	// (the control channel was lost meanwhile: nothing came; feed it again)
	mk_net(b, 1, 1);
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	mk_grant(b, P25_OP_GRP_V_GRANT, 0, 0x100B, 200, 1);
	p25_follow_tsbk(&f, b, 0x5A5, now_ms);
	CHECK(view(&f).state == P25_FOLLOW_VOICE && view(&f).tg == 200, "hold over");
	retune(&f);
	// The longest call
	cfg.call_max_ms = 2000;
	run_ms(&f, 800);
	unit_duid(&f, P25_DUID_LDU1);
	run_ms(&f, 800);
	unit_duid(&f, P25_DUID_LDU1);
	CHECK(view(&f).state == P25_FOLLOW_VOICE, "a long call goes on");
	run_ms(&f, 500);
	CHECK(view(&f).state == P25_FOLLOW_CONTROL, "the longest call ends");
	p25_follow_end(&f);
}

// Random input: never a crash, never a tune to nothing
static void testFuzz(void)
{
	static p25_follow_iden_t idens[P25_FOLLOW_IDENS];
	static const p25_follow_cc_t cc[] = { { .hz = HZ_CC1 }, { .chan = 0x1005 } };
	static p25_follow_t f;
	static uint8_t air[P25_UNIT_BYTES_MAX];
	p25_follow_cfg_t cfg = { .wacn = P25_FOLLOW_WACN_ANY, .sysid = P25_FOLLOW_SYSID_ANY, .nac = P25_NAC_ANY,
				 .flags = P25_FOLLOW_ALL_GROUPS, .unit_id = 7, .ncc = 2, .cc = cc, .iden_set = 0x8001,
				 .iden = idens, .hold_ms = 500, .fallback_ms = 2000 };
	unsigned zero = 0, bad_state = 0, voice = 0, tunes_seen = 0;

	idens[0] = (p25_follow_iden_t){ .base_hz = 851006250, .spacing_hz = 12500 };
	idens[15] = (p25_follow_iden_t){ .base_hz = 0xFFFF0000u, .spacing_hz = 0x7FFFF, .slots = 4, .tdma = true };
	for (unsigned pass = 0; pass < 2; pass++) {
		ntunes = 0;
		now_ms = 0;
		p25_follow_begin(&f, &cfg, &fhooks, NULL, now_ms);
		for (unsigned i = 0; i < 200000; i++) {
			unsigned r = rng() % 16;

			now_ms += rng() % 50;
			if (tune_pending && (rng() & 3)) {
				retune(&f);
			} else if (r < 6) {
				// A sealed block: a valid CRC, random content (most of the standard's opcodes)
				uint8_t b[P25_TSBK_BYTES];

				for (unsigned k = 0; k < 10; k++)
					b[k] = (uint8_t)rng();
				if (rng() & 1)
					b[1] = 0;
				p25_tsbk_seal(b);
				p25_follow_tsbk(&f, b, (uint16_t)(rng() & 0xFFF), now_ms);
			} else if (r < 10) {
				// A unit of noise
				p25_rx_unit_t u = { .duid = (uint8_t)(rng() & 15), .nac = (uint16_t)(rng() & 0xFFF), .air = air };

				u.nsyms = (uint16_t)(rng() % (P25_UNIT_SYMS_MAX + 1));
				for (unsigned k = 0; k < sizeof(air); k++)
					air[k] = (uint8_t)rng();
				p25_follow_unit(&f, &u, now_ms);
			} else if (r < 12) {
				p25_call_t c = { .have_lc = rng() & 1, .have_alg = rng() & 1, .algid = (uint8_t)rng() };

				c.lc.lcf = (uint8_t)(rng() & 1 ? P25_LCF_GROUP : P25_LCF_UNIT);
				c.lc.tgid = (uint16_t)rng();
				c.lc.src = rng() & 0xFFFFFF;
				p25_follow_call(&f, &c, now_ms);
			} else {
				p25_follow_tick(&f, now_ms);
			}
			if (f.view.state > P25_FOLLOW_VOICE || f.view.cc == 0xFF)
				bad_state++;
			if (f.view.state == P25_FOLLOW_VOICE)
				voice++;
			for (; tunes_seen < ntunes; tunes_seen++)
				if (!tunes[tunes_seen].hz)
					zero++;
			if (ntunes >= 300)
				ntunes = tunes_seen = 0;
		}
		p25_follow_end(&f);
		cfg.flags = P25_FOLLOW_SKIP_ENC;
		cfg.talk_group = 1;
	}
	CHECK(zero == 0 && bad_state == 0, "fuzz: %u tunes to 0 Hz, %u bad states", zero, bad_state);
	CHECK(voice > 0, "fuzz reached voice channels");
}

// ── Whole systems on the air ──
// Symbols are on one clock for every channel: symbol k at k / 4800 s. The tap runs at 8 kHz: sample n at n / 8000 s,
// so sample n sits at 3n - 5k in 1/24000 s from symbol k's center
#define RATE		8000
#define HSPAN		8		// symbols of pulse each side
#define RING		2048
static double pulse[2 * HSPAN * 5 + 1];

static void pulse_init(void)
{
	double alpha = p25_air_mode.profile.shape_param / 100.0, f1 = 4800 * (1 - alpha) / 2, f2 = 4800 * (1 + alpha) / 2;
	double sum = 0;

	// TIA-102.BAAA-A 9.3: the raised cosine times the shaper (pi f T) / sin(pi f T), sampled at 24 kHz
	for (int i = 0; i <= 2 * HSPAN * 5; i++) {
		double t = (double)(i - HSPAN * 5) / 24000, v = 0;

		for (double f = 2.5; f < f2; f += 5) {
			double hf = f < f1 ? 1 : 0.5 + 0.5 * cos(M_PI * (f - f1) / (f2 - f1)), x = M_PI * f / 4800;

			v += hf * x / sin(x) * cos(2 * M_PI * f * t);
		}
		pulse[i] = v;
		sum += v;
	}
	for (int i = 0; i <= 2 * HSPAN * 5; i++)
		pulse[i] *= 5 / sum;
}

// A system's control channel content
typedef struct {
	uint16_t nac;
	uint32_t wacn;
	uint16_t sysid;
	uint8_t site;
	uint16_t adj_ch, sccb_ch;	// advertised control channels, 0 none
} sys_t;

// A call on a system: granted at t0, voice on its channel from t0 + 100 ms to t1, a terminator at t1 if term
typedef struct {
	const sys_t *sys;
	uint16_t ch;
	uint16_t tg;
	uint32_t src;
	uint8_t svc;
	bool enc, term;
	uint32_t t0, t1;
	// What the air did
	uint32_t ldus;
} call_t;

enum { TX_CC, TX_VOICE };
typedef struct {
	uint32_t hz;
	uint8_t kind;
	const sys_t *sys;
	uint32_t off_ms;	// a control channel stops at this time (0: never)
	uint32_t made;		// symbols made
	uint8_t sym[RING];
	bool on[RING];		// a carrier under the symbol
	uint32_t units, rot;
	int call;		// voice: the call on air, -1 none
	uint8_t ldu;
} tx_t;

static call_t *calls;
static unsigned ncalls;

static uint32_t sym_ms(uint32_t k) { return (uint32_t)((uint64_t)k * 5 / 24); }

static void tx_add(tx_t *t, const uint8_t *unit, uint16_t nsyms, bool on)
{
	for (uint16_t i = 0; i < nsyms; i++, t->made++) {
		t->sym[t->made % RING] = unit ? air_sym_get(unit, i) : (uint8_t)(rng() & 3);
		t->on[t->made % RING] = on;
	}
}

static void cc_unit(tx_t *t)
{
	static uint8_t unit[P25_UNIT_BYTES_MAX];
	const sys_t *s = t->sys;
	uint32_t ms = sym_ms(t->made);
	uint8_t b[3][P25_TSBK_BYTES];
	unsigned n = 0;

	if (t->off_ms && ms >= t->off_ms) {
		tx_add(t, NULL, 48, false);
		return;
	}
	t->units++;
	if (t->units % 13 == 0) {
		// A multi-block trunking packet: its header and two data blocks
		p25_mbt_t h = { .format = P25_PDU_FMT_AMBTC, .sap = 61, .mfid = 0, .blocks = 2, .opcode = 0x3C };

		p25_unit_begin(unit, p25_blocks_syms(3), s->nac, P25_DUID_PDU, P25_SS_IDLE);
		p25_mbt_make(&h, b[0]);
		p25_block_put(unit, 0, b[0]);
		for (unsigned k = 1; k < 3; k++) {
			for (unsigned i = 0; i < P25_TSBK_BYTES; i++)
				b[k][i] = (uint8_t)rng();
			p25_block_put(unit, (uint8_t)k, b[k]);
		}
		tx_add(t, unit, p25_blocks_syms(3), true);
		return;
	}
	// Grants for the calls being set up, updates for the ones under way
	for (unsigned i = 0; i < ncalls && n < 3; i++) {
		const call_t *c = &calls[i];

		if (c->sys != s || ms < c->t0 || ms >= c->t1)
			continue;
		if (ms < c->t0 + 400)
			mk_grant(b[n++], P25_OP_GRP_V_GRANT, c->svc, c->ch, c->tg, c->src);
		else if (t->units % 3 == 0)
			mk_grant(b[n++], P25_OP_GRP_V_UPDT, 0, c->ch, c->tg, 0);
	}
	// The broadcasts in turn
	while (n < 1 + t->units % 3) {
		switch (t->rot++ % 10) {
		case 0: mk_net(b[n], s->wacn, s->sysid); break;
		case 1: mk_rfss(b[n], P25_TSBK_RFSS, s->sysid, s->site, 0x1004); break;
		case 2: mk_iden(b[n], P25_OP_IDEN_UP, 1, 0, 851006250, 50); break;
		case 3: mk_iden(b[n], P25_OP_IDEN_UP_TDMA, 2, 3, 852006250, 100); break;
		case 4:
			if (!s->adj_ch)
				continue;
			mk_rfss(b[n], P25_TSBK_ADJ, s->sysid, (uint8_t)(s->site + 1), s->adj_ch);
			break;
		case 5:
			if (!s->sccb_ch)
				continue;
			mk_sccb(b[n], s->sccb_ch);
			break;
		case 6: {
			p25_tsbk_t m = { .kind = P25_TSBK_SYS_SRV, .u.svc = { .avail = 0x1F, .supp = 0x1F } };

			p25_tsbk_make(&m, b[n]);
			break;
		}
		case 7:	// a manufacturer's block, with a grant's opcode and our group in it: not a grant
			mk_grant(b[n], P25_OP_GRP_V_GRANT, 0, 0x100A, 100, 1);
			b[n][1] = 0x90;
			p25_tsbk_seal(b[n]);
			break;
		case 8:	// a protected one
			mk_grant(b[n], P25_OP_GRP_V_GRANT, 0, 0x100A, 100, 1);
			b[n][0] |= P25_TSBK_P;
			p25_tsbk_seal(b[n]);
			break;
		default:	// one not decoded here
			blk_hand(b[n], 0x28, 0);
			p25_tsbk_seal(b[n]);
			break;
		}
		n++;
	}
	for (unsigned k = 0; k < n; k++) {
		b[k][0] = (uint8_t)((b[k][0] & 0x7F) | (k == n - 1 ? P25_TSBK_LB : 0));
		p25_tsbk_seal(b[k]);
	}
	p25_unit_begin(unit, p25_blocks_syms((uint8_t)n), s->nac, P25_DUID_TSDU, P25_SS_IDLE);
	for (unsigned k = 0; k < n; k++)
		p25_block_put(unit, (uint8_t)k, b[k]);
	tx_add(t, unit, p25_blocks_syms((uint8_t)n), true);
}

static void voice_unit(tx_t *t)
{
	static uint8_t unit[P25_UNIT_BYTES_MAX];
	uint32_t ms = sym_ms(t->made);
	uint8_t lcraw[P25_LC_BYTES], v[P25_IMBE_BYTES];

	if (t->call < 0) {
		for (unsigned i = 0; i < ncalls; i++) {
			const call_t *c = &calls[i];

			if (c->ch != (t->hz == HZ_VA ? 0x100A : 0x100C) || ms < c->t0 + 100 || ms >= c->t1)
				continue;
			// Its header
			uint8_t hdr[P25_HDR_BYTES] = { 0 };

			t->call = (int)i;
			t->ldu = 0;
			hdr[10] = c->enc ? 0x84 : P25_ALGID_CLEAR;
			hdr[13] = (uint8_t)(c->tg >> 8);
			hdr[14] = (uint8_t)c->tg;
			p25_unit_begin(unit, P25_HDU_SYMS, c->sys->nac, P25_DUID_HDU, P25_SS_UNKNOWN);
			p25_hdu_put(unit, hdr);
			tx_add(t, unit, P25_HDU_SYMS, true);
			return;
		}
		tx_add(t, NULL, 48, false);
		return;
	}
	call_t *c = &calls[t->call];

	if (ms >= c->t1) {
		if (c->term) {
			p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = c->tg, .src = c->src };

			p25_lc_build(&lc, lcraw);
			p25_unit_begin(unit, P25_TDULC_SYMS, c->sys->nac, P25_DUID_TDULC, P25_SS_UNKNOWN);
			p25_tdulc_put(unit, lcraw);
			tx_add(t, unit, P25_TDULC_SYMS, true);
		} else {
			tx_add(t, NULL, 48, false);
		}
		t->call = -1;
		return;
	}
	p25_unit_begin(unit, P25_LDU_SYMS, c->sys->nac, t->ldu & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1, P25_SS_UNKNOWN);
	for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
		for (unsigned k = 0; k < P25_IMBE_BYTES; k++)
			v[k] = (uint8_t)rng();
		p25_ldu_voice_put(unit, n, v);
	}
	if (t->ldu & 1) {
		uint8_t es[P25_ES_BYTES] = { 0 };

		es[9] = c->enc ? 0x84 : P25_ALGID_CLEAR;
		p25_ldu_es_put(unit, es);
	} else {
		p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = c->tg, .src = c->src };

		p25_lc_build(&lc, lcraw);
		p25_ldu_lc_put(unit, lcraw);
	}
	t->ldu++;
	c->ldus++;
	tx_add(t, unit, P25_LDU_SYMS, true);
}

static tx_t txs[4];
static unsigned ntx;

static tx_t *tx_at(uint32_t hz)
{
	for (unsigned i = 0; i < ntx; i++)
		if (txs[i].hz == hz)
			return &txs[i];
	return NULL;
}

// Every transmitter runs whether we listen or not
static void tx_run(uint32_t k_to)
{
	for (unsigned i = 0; i < ntx; i++)
		while (txs[i].made <= k_to)
			(txs[i].kind == TX_CC ? cc_unit : voice_unit)(&txs[i]);
}

// ── The receiver: a provider on the soft modem, tuned where the follower says ──
static air_softmodem_t modem;
static air_event_cb_t prov_cb;
static void *prov_ctx;
static uint32_t prov_us;
static uint16_t told_cpk;

static void modem_out(const air_chunk_t *c, void *ctx)
{
	air_event_t e = { .kind = AIR_EV_RX, .t_us = prov_us, .chunk = c };

	(void)ctx;
	if (prov_cb)
		prov_cb(&e, prov_ctx);
}
static uint32_t pr_fit(const air_profile_t *p) { return p->sym_rate == 4800 ? AIR_FIT_RX | AIR_FIT_SOFT : 0; }
static bool pr_open(const air_profile_t *p, air_event_cb_t cb, void *ctx)
{
	prov_cb = cb;
	prov_ctx = ctx;
	return air_softmodem_rx_open(&modem, p, RATE, told_cpk, modem_out, NULL);
}
static void pr_close(void) { prov_cb = NULL; }
static void pr_restart(void) { air_softmodem_rx_reset(&modem); }
static bool pr_no(void) { return false; }
static bool pr_queue(const air_tx_chunk_t *c) { (void)c; return false; }
static uint8_t pr_space(void) { return 0; }
static void pr_nothing(void) { }
static uint32_t pr_now(void) { return prov_us; }
static const hal_air_t prov = { .fit = pr_fit, .open = pr_open, .close = pr_close, .rx_restart = pr_restart,
				.tx_begin = pr_no, .tx_queue = pr_queue, .tx_space = pr_space, .tx_end = pr_nothing,
				.tx_abort = pr_nothing, .now_us = pr_now, .tx_chunk_syms = 144, .tx_queue_depth = 4,
				.tx_lead_ms = 60, .time_accuracy_us = 100 };

typedef struct {
	p25_engine_t eng;
	p25_engine_cfg_t ecfg;
	p25_follow_t fol;
	uint32_t starts, frames, bad_frames, ends;
	uint16_t last_tg;
	uint32_t last_src;
} rig_t;
static rig_t rig;

static void e_start(const p25_call_t *c, void *ctx) { (void)ctx; rig.starts++; rig.last_tg = c->lc.tgid; rig.last_src = c->lc.src; }
static void e_update(const p25_call_t *c, void *ctx) { (void)ctx; rig.last_tg = c->lc.tgid; rig.last_src = c->lc.src; }
static void e_voice(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx)
{
	(void)imbe;
	(void)ctx;
	rig.frames++;
	if (P25_VOICE_ERRS_BAD(errs))
		rig.bad_frames++;
}
static void e_end(void *ctx) { (void)ctx; rig.ends++; }
static void e_unit(const p25_rx_unit_t *u, void *ctx) { (void)ctx; p25_follow_unit(&rig.fol, u, now_ms); }
static void e_call(const p25_call_t *c, void *ctx) { (void)ctx; p25_follow_call(&rig.fol, c, now_ms); }
static const p25_engine_hooks_t ehooks = { .rx_start = e_start, .rx_update = e_update, .rx_voice = e_voice,
					   .rx_end = e_end, .rx_unit = e_unit, .rx_call = e_call };

// Run the air from now_ms to end_ms, tuned to tuned_hz; sigma: noise in counts at cpk counts per kHz
static uint32_t tuned_hz;
static uint64_t sample_n;

static void air_run(uint32_t end_ms, double cpk, double sigma)
{
	static int16_t buf[240];

	while (now_ms < end_ms) {
		tx_t *t = tx_at(tuned_hz);
		uint32_t t_us = prov_us;

		for (unsigned i = 0; i < 240; i++, sample_n++) {
			int64_t c3 = (int64_t)(3 * sample_n);
			uint32_t kc = (uint32_t)((c3 + 2) / 5), klo = (uint32_t)(c3 / 5 > HSPAN ? c3 / 5 - HSPAN : 0);
			double v = 0;

			tx_run(kc + HSPAN + 1);
			if (!t || !t->on[kc % RING]) {
				v = 2.5 * cpk * gauss();	// no carrier: discriminator noise
			} else {
				for (uint32_t k = klo; k <= kc + HSPAN; k++) {
					int64_t d = c3 - 5 * (int64_t)k;

					if (d >= -5 * HSPAN && d <= 5 * HSPAN)
						v += air_sym_level(t->sym[k % RING]) * 600.0 * pulse[d + 5 * HSPAN];
				}
				v = v / 1000 * cpk + sigma * gauss();
			}
			buf[i] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		prov_us += 30000;
		now_ms += 30;
		air_softmodem_rx(&modem, buf, 240, t_us);
		p25_follow_tick(&rig.fol, now_ms);
		if (tune_pending) {
			// The radio core's task: retune, set the squelch for the call, then tell the engine and the follower
			const typeof(tunes[0]) *w = &tunes[ntunes - 1];

			tune_pending = false;
			tuned_hz = w->hz;
			rig.ecfg.rx_tgid = w->what == P25_FOLLOW_TUNE_GROUP ? (uint16_t)w->id : 0;
			p25_engine_set(&rig.eng, &rig.ecfg);
			p25_engine_retuned(&rig.eng);
			p25_follow_retuned(&rig.fol, now_ms);
		}
	}
}

static void rig_begin(const p25_follow_cfg_t *cfg, uint16_t told)
{
	memset(&rig, 0, sizeof(rig));
	told_cpk = told;
	ntunes = nfallback = 0;
	tune_pending = false;
	now_ms = 0;
	sample_n = 0;
	prov_us = 0xFFFF0000u;	// the clock wraps inside the run
	rig.ecfg = (p25_engine_cfg_t){ .rx_nac = cfg->nac, .tx_nac = cfg->nac, .unit_id = cfg->unit_id, .trunk = true };
	CHECK(p25_engine_begin(&rig.eng, &prov, &rig.ecfg, &ehooks, NULL), "engine begins");
	p25_follow_begin(&rig.fol, cfg, &fhooks, NULL, now_ms);
	// The first tune: at once
	tune_pending = false;
	tuned_hz = tunes[0].hz;
	p25_engine_retuned(&rig.eng);
	p25_follow_retuned(&rig.fol, now_ms);
}

static void rig_end(void)
{
	p25_follow_end(&rig.fol);
	p25_engine_end(&rig.eng);
}

static unsigned tunes_to(uint32_t hz)
{
	unsigned n = 0;

	for (unsigned i = 0; i < ntunes; i++)
		n += tunes[i].hz == hz;
	return n;
}

#define HZ_DEAD		851012500u
#define HZ_VB		851081250u	// iden 1 channel 12
#define HZ_CC3		851043750u	// iden 1 channel 6: a secondary control channel

static const struct { double cpk, sigma; uint16_t told; const char *what; } levels[] = {
	{ 2000, 0, 2000, "clean" },	// counts per kHz: the outer symbol is 3600, the noise of no carrier 5000 rms
	{ 2000, 250, 2000, "noise 250" },
	{ 2000, 500, 2000, "noise 500" },
	{ 2000, 800, 2000, "noise 800" },	// some blocks lost: near the edge (at 1500 nothing locks)
	{ 1700, 250, 0, "unknown scale, noise 250" },
};

// A system with calls of every kind: hunt past a dead channel, follow ours, skip the rest
static void testAirSystem(void)
{
	static const sys_t sys = { .nac = NAC, .wacn = WACN, .sysid = SYSID, .site = 3, .adj_ch = 0x1010, .sccb_ch = 0x1006 };
	static const p25_follow_cc_t cc[] = { { .hz = HZ_DEAD }, { .hz = HZ_CC1 } };
	static const uint16_t groups[] = { 300 };
	p25_follow_cfg_t cfg = { .wacn = WACN, .sysid = SYSID, .nac = NAC, .flags = P25_FOLLOW_SKIP_ENC, .unit_id = OUR_UNIT,
				 .talk_group = 100, .ngroups = 1, .groups = groups, .ncc = 2, .cc = cc };
	static call_t script[6];

	for (unsigned l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
		p25_follow_view_t v;
		uint32_t back_ms = 0, frames1;

		script[0] = (call_t){ &sys, 0x100A, 100, 1001, 0, false, true, 4000, 7000, 0 };	// ours: followed
		script[1] = (call_t){ &sys, 0x100C, 200, 1002, 0, false, true, 7500, 9500, 0 };	// another group
		script[2] = (call_t){ &sys, 0x100A, 300, 1003, P25_SVC_PROTECTED, true, true, 10000, 12000, 0 };	// encrypted
		script[3] = (call_t){ &sys, 0x100C, 100, 1004, 0, true, true, 12500, 14500, 0 };	// encrypted, unflagged
		script[4] = (call_t){ &sys, 0x2003, 100, 1005, 0, false, true, 15000, 16000, 0 };	// two-slot
		script[5] = (call_t){ &sys, 0x100A, 300, 1006, 0, false, false, 17000, 20000, 0 };	// fades out
		calls = script;
		ncalls = 6;
		memset(txs, 0, sizeof(txs));
		txs[0] = (tx_t){ .hz = HZ_CC1, .kind = TX_CC, .sys = &sys, .call = -1 };
		txs[1] = (tx_t){ .hz = HZ_VA, .kind = TX_VOICE, .sys = &sys, .call = -1 };
		txs[2] = (tx_t){ .hz = HZ_VB, .kind = TX_VOICE, .sys = &sys, .call = -1 };
		ntx = 3;
		rig_begin(&cfg, levels[l].told);
		air_run(3900, levels[l].cpk, levels[l].sigma);
		v = view(&rig.fol);
		CHECK(v.state == P25_FOLLOW_CONTROL && v.cc_hz == HZ_CC1 && v.wacn == WACN && v.sysid == SYSID && v.rfss == 1 &&
		      v.site == 3 && (v.idens & 0x0006) == 0x0006 && v.learned == 2, "%s: control channel held, %u tunes",
		      levels[l].what, ntunes);
		CHECK(ntunes >= 2 && tunes[0].hz == HZ_DEAD && tunes[1].hz == HZ_CC1, "%s: the dead one first", levels[l].what);
		// Our call
		air_run(7000, levels[l].cpk, levels[l].sigma);
		v = view(&rig.fol);
		frames1 = rig.frames;
		CHECK(v.follows == 1 && rig.starts == 1 && rig.last_tg == 100 && rig.last_src == 1001, "%s: our call heard: follows %u starts %u tg %u",
		      levels[l].what, v.follows, rig.starts, rig.last_tg);
		while (now_ms < 8000 && view(&rig.fol).state == P25_FOLLOW_VOICE)
			air_run(now_ms + 30, levels[l].cpk, levels[l].sigma);
		back_ms = now_ms - 7000;
		// The rest
		air_run(23000, levels[l].cpk, levels[l].sigma);
		v = view(&rig.fol);
		printf("  air, %s: on the control channel %u ms after the terminator; frames %u of %u LDUs (bad frames in the run %u); blocks %u, CRC failed %u; "
		       "follows %u, encrypted skipped %u, two-slot %u, voice lost %u, packets %u, mfr %u\n", levels[l].what,
		       back_ms, frames1, script[0].ldus, rig.bad_frames, v.blocks, v.crc_fail, v.follows, v.enc_skip, v.tdma,
		       v.voice_lost, v.mbt, v.mfr);
		CHECK(back_ms <= 300, "%s: back %u ms after the terminator", levels[l].what, back_ms);
		CHECK(frames1 >= (script[0].ldus - 3) * 9 && frames1 <= script[0].ldus * 9, "%s: frames %u of %u LDUs",
		      levels[l].what, frames1, script[0].ldus);
		CHECK(tunes_to(HZ_VB) == 1 && tunes_to(HZ_VA) == 2, "%s: tunes VA %u VB %u", levels[l].what, tunes_to(HZ_VA),
		      tunes_to(HZ_VB));
		// Only the unflagged encrypted call took us to its channel, and nothing was played of it
		CHECK(v.follows == 3 && v.enc_skip >= 2 && v.tdma >= 1 && v.voice_lost == 1 && v.state == P25_FOLLOW_CONTROL &&
		      v.cc_hz == HZ_CC1 && v.cc_lost == 0 && v.hunts == 1, "%s: follows %u enc %u tdma %u lost %u cc_lost %u hunts %u",
		      levels[l].what, v.follows, v.enc_skip, v.tdma, v.voice_lost, v.cc_lost, v.hunts);
		// (a fading call can be started again by the call layer as its units come and go)
		CHECK(rig.starts >= 2 && rig.last_tg == 300 && rig.last_src == 1006, "%s: calls played %u", levels[l].what, rig.starts);
		CHECK(v.mbt > 0 && v.mfr > 0 && v.op[0x28] > 0 && v.grants > 0 && v.op[P25_OP_IDEN_UP_TDMA] > 0,
		      "%s: signaling counted", levels[l].what);
		CHECK(v.crc_fail * 25 <= v.blocks, "%s: CRC failed %u of %u", levels[l].what, v.crc_fail, v.blocks);
		rig_end();
	}
}

// The control channel goes off the air: a secondary one learned off air takes over. Then another system only, and
// the fallback
static void testAirLoss(void)
{
	static const sys_t sys = { .nac = NAC, .wacn = WACN, .sysid = SYSID, .site = 3, .sccb_ch = 0x1006 };
	static const sys_t other = { .nac = NAC, .wacn = WACN, .sysid = 0x111, .site = 9 };
	static const p25_follow_cc_t cc[] = { { .hz = HZ_CC1 } };
	p25_follow_cfg_t cfg = { .wacn = WACN, .sysid = SYSID, .nac = NAC, .talk_group = 100, .ncc = 1, .cc = cc,
				 .fallback_ms = 4000 };
	static call_t script[1];

	for (unsigned l = 0; l < 3; l++) {
		p25_follow_view_t v;

		script[0] = (call_t){ &sys, 0x100A, 100, 1001, 0, false, true, 6000, 8000, 0 };
		calls = script;
		ncalls = 1;
		memset(txs, 0, sizeof(txs));
		txs[0] = (tx_t){ .hz = HZ_CC1, .kind = TX_CC, .sys = &sys, .off_ms = 3000, .call = -1 };
		txs[1] = (tx_t){ .hz = HZ_CC3, .kind = TX_CC, .sys = &sys, .call = -1 };
		txs[2] = (tx_t){ .hz = HZ_VA, .kind = TX_VOICE, .sys = &sys, .call = -1 };
		ntx = 3;
		rig_begin(&cfg, levels[l].told);
		air_run(2900, levels[l].cpk, levels[l].sigma);
		v = view(&rig.fol);
		CHECK(v.state == P25_FOLLOW_CONTROL && v.cc_hz == HZ_CC1 && v.learned == 1, "%s: held, secondary learned",
		      levels[l].what);
		air_run(5500, levels[l].cpk, levels[l].sigma);
		v = view(&rig.fol);
		CHECK(v.state == P25_FOLLOW_CONTROL && v.cc_hz == HZ_CC3 && v.cc_lost == 1, "%s: the secondary took over: %u Hz",
		      levels[l].what, v.cc_hz);
		air_run(9000, levels[l].cpk, levels[l].sigma);
		v = view(&rig.fol);
		CHECK(v.follows == 1 && rig.starts == 1 && rig.last_tg == 100 && v.state == P25_FOLLOW_CONTROL && v.cc_hz == HZ_CC3,
		      "%s: a call from the secondary: follows %u", levels[l].what, v.follows);
		CHECK(nfallback == 0, "%s: no fallback", levels[l].what);
		rig_end();
	}

	// Only another system on the air: never held, the fallback once
	script[0].sys = &other;
	memset(txs, 0, sizeof(txs));
	txs[0] = (tx_t){ .hz = HZ_CC1, .kind = TX_CC, .sys = &other, .call = -1 };
	txs[1] = (tx_t){ .hz = HZ_VA, .kind = TX_VOICE, .sys = &other, .call = -1 };
	ntx = 2;
	rig_begin(&cfg, 2000);
	air_run(12000, 2000, 250);
	{
		p25_follow_view_t v = view(&rig.fol);

		CHECK(v.state == P25_FOLLOW_HUNT && v.wrong_sys > 0 && v.follows == 0 && nfallback == 1 && tunes_to(HZ_VA) == 0 &&
		      rig.starts == 0, "another system: state %u wrong %u fallback %u", v.state, v.wrong_sys, nfallback);
	}
	rig_end();
}

int main(void)
{
	pulse_init();
	testLayouts();
	testInbound();
	testBlocks();
	testChannels();
	testFollowHand();
	testFollowOptions();
	testAccess();
	testAccessEarlyAnswers();
	testFuzz();
	testAirSystem();
	testAirLoss();
	printf("p25TrunkTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
