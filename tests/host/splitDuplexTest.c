// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Host test of the split duplex extension (docs/spec/split-duplex.md):
//   - the CACH and short LC coding (core/dmr/fec/dmrCach.c) against ETSI TS 102 361-1 itself: the generator matrices
//     of tables B.13 and B.17 multiplied out here, the bit placement of figures B.6 and B.9 written out as tables, the
//     CRC-8 of B.3.7 by long division, the idle message bits of table D.2
//   - the continuous outbound's buffer, prebuffer, underrun fill, AT and short LC switching (core/dmr/bsOutbound.c)
//   - the RX half's labels and tracking (core/dmr/slotEpoch.c), with clock offsets, rate errors and wrap-around
//   - the whole extension through mmdvm.c and hotspot.c on the simulated split modem (tests/host/sim/split_sim_modem.c):
//     command 0xD6, the roles and their standard commands, the lease, the guard, the RX half's dual-slot uplink
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dmr/fec/dmrCach.h"
#include "dmr/fec/dmrFec.h"
#include "dmr/bsOutbound.h"
#include "dmr/slotEpoch.h"
#include "dmr/dmrData.h"
#include "dmr/call/dmr_lc.h"
#include "hotspot/mmdvm.h"
#include "hotspot/hotspot.h"
#include "hotspot/hotspotSplit.h"
#include "hotspot/dmrDataFrame.h"
#include "sim/split_sim_modem.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

// ---- The standard's tables ----

// Table B.17: Hamming (7,4,3) generator matrix, rows for data bits 3..0 (the first data bit on top)
static const char *B17[4] = { "1000101", "0100111", "0010110", "0001011" };
// Table B.13: Hamming (17,12,3) generator matrix
static const char *B13[12] = {
	"10000000000011011", "01000000000011111", "00100000000011101", "00010000000011100",
	"00001000000001110", "00000100000000111", "00000010000010001", "00000001000011010",
	"00000000100001101", "00000000010010100", "00000000001001010", "00000000000100101",
};

// Multiply data (k bits, first bit in bit k-1) by a generator matrix given as rows of '0' / '1'
static uint32_t gen_mul(const char *const *rows, int k, int n, uint32_t data)
{
	uint32_t cw = 0;

	for (int r = 0; r < k; r++) {
		if ((data >> (k - 1 - r)) & 1u) {
			uint32_t row = 0;

			for (int c = 0; c < n; c++) {
				row = (row << 1) | (uint32_t)(rows[r][c] == '1');
			}
			cw ^= row;
		}
	}
	return cw;
}

// Figure B.9: TX(23..0) of the CACH burst, by name
static const char *B9[24] = {
	"AT", "P16", "P15", "P14", "TC", "P13", "P12", "P11", "LS1", "P10", "P9", "P8",
	"LS0", "P7", "H2", "P6", "P5", "P4", "H1", "P3", "P2", "P1", "H0", "P0",
};

// Figure B.6: the transmit matrix, four CACH payloads P(16..0)
static const char *B6[4][17] = {
	{ "L27", "L15", "L3", "P16", "L26", "L14", "L2", "P15", "L25", "L13", "L1", "P14", "L24", "L12", "L0", "P13", "L23" },
	{ "L11", "C7", "P12", "L22", "L10", "C6", "P11", "L21", "L9", "C5", "P10", "L20", "L8", "C4", "P9", "L19", "L7" },
	{ "C3", "P8", "L18", "L6", "C2", "P7", "L17", "L5", "C1", "P6", "L16", "L4", "C0", "P5", "A4", "B4", "D4" },
	{ "P4", "A3", "B3", "D3", "P3", "A2", "B2", "D2", "P2", "A1", "B1", "D1", "P1", "A0", "B0", "D0", "P0" },
};	// A, B, D: H1, H2, H3 (Hamming parity of encode matrix rows 1, 2, 3)

// Table D.2, I(95) first
static const char *D2 =
	"111111111000001111011111"	// I(95..72)
	"000101110011001000001001"	// I(71..48)
	"010011101101000111100111"	// I(47..24)
	"110011011000101010010001";	// I(23..0)

// B.3.7 by long division: x^8 M(x) mod x^8 + x^2 + x + 1, M of the given degree + 1 bits
static uint8_t crc8_div(uint64_t m, int bits)
{
	uint64_t r = m << 8;

	for (int i = bits + 7; i >= 8; i--) {
		if ((r >> i) & 1u) {
			r ^= (uint64_t)0x107 << (i - 8);
		}
	}
	return (uint8_t)r;
}

// ---- CACH and short LC coding ----

static void test_tact(void)
{
	for (uint32_t d = 0; d < 16; d++) {
		uint8_t want = (uint8_t)gen_mul(B17, 4, 7, d);
		uint8_t got = dmrCachTactEncode((d >> 3) & 1u, (d >> 2) & 1u, d & 3u);

		CHECK(got == want);
		for (int e = -1; e < 7; e++) {
			uint8_t at, tc, lcss;
			uint8_t c = (e < 0) ? got : (uint8_t)(got ^ (1u << e));
			int fixed = dmrCachTactDecode(c, &at, &tc, &lcss);

			CHECK(fixed == (e >= 0));
			CHECK((uint32_t)((at << 3) | (tc << 2) | lcss) == d);
		}
	}
}

static void test_hamming1712(void)
{
	int bad = 0, unfixed = 0;

	for (uint32_t d = 0; d < 4096; d++) {
		uint32_t want = gen_mul(B13, 12, 17, d);

		if (dmrCachHamming1712Encode(d) != want) {
			bad++;
		}
		for (int e = 0; e < 17; e += 4) {
			uint32_t c = want ^ (1u << e);

			if (!dmrCachHamming1712Correct(&c) || (c != want)) {
				unfixed++;
			}
		}
	}
	CHECK(bad == 0);
	CHECK(unfixed == 0);
}

static int name_bit(const char *name, uint8_t at, uint8_t tc, uint8_t lcss, uint8_t h, uint32_t payload)
{
	if (strcmp(name, "AT") == 0) {
		return at;
	}
	if (strcmp(name, "TC") == 0) {
		return tc;
	}
	if (strcmp(name, "LS1") == 0) {
		return (lcss >> 1) & 1;
	}
	if (strcmp(name, "LS0") == 0) {
		return lcss & 1;
	}
	if (name[0] == 'H') {
		return (h >> atoi(name + 1)) & 1;
	}
	return (int)((payload >> atoi(name + 1)) & 1u);
}

static void test_interleave(void)
{
	srand(7);
	for (int n = 0; n < 200; n++) {
		uint8_t at = (uint8_t)(rand() & 1), tc = (uint8_t)(rand() & 1), lcss = (uint8_t)(rand() & 3);
		uint32_t payload = (uint32_t)rand() & 0x1FFFFu;
		uint8_t h = (uint8_t)(gen_mul(B17, 4, 7, (uint32_t)((at << 3) | (tc << 2) | lcss)) & 7u);
		uint32_t bits = dmrCachBuild(at, tc, lcss, payload);
		int wrong = 0;

		for (int i = 0; i < 24; i++) {
			if ((int)((bits >> (23 - i)) & 1u) != name_bit(B9[i], at, tc, lcss, h, payload)) {
				wrong++;
			}
		}
		CHECK(wrong == 0);
		uint8_t tact;
		uint32_t p;

		dmrCachDeinterleave(bits, &tact, &p);
		CHECK(p == payload);
		CHECK(tact == dmrCachTactEncode(at, tc, lcss));
	}
}

static int b6_bit(const char *name, uint32_t slc, uint8_t crc, const uint32_t enc[3])
{
	int idx = atoi(name + 1);

	switch (name[0]) {
	case 'L':
		return (int)((slc >> idx) & 1u);
	case 'C':
		return (crc >> idx) & 1;
	case 'A':
		return (int)((enc[0] >> idx) & 1u);
	case 'B':
		return (int)((enc[1] >> idx) & 1u);
	case 'D':
		return (int)((enc[2] >> idx) & 1u);
	default:	// P: column parity of column 16 - idx ... i.e. bit idx of the three rows
		return (int)(((enc[0] ^ enc[1] ^ enc[2]) >> idx) & 1u);
	}
}

static void test_short_lc(void)
{
	// CRC-8 against long division, including the Act_Updt hash
	srand(11);
	for (int n = 0; n < 500; n++) {
		uint32_t slc = (uint32_t)rand() & 0x0FFFFFFFu;
		uint32_t addr = (uint32_t)rand() & 0xFFFFFFu;

		CHECK(dmrShortLcCrc8(slc) == crc8_div(slc, 28));
		CHECK(dmrShortLcHash(addr) == crc8_div(addr, 24));
		CHECK(crc8_div(((uint64_t)slc << 8) | dmrShortLcCrc8(slc), 36) == 0);	// a codeword divides
	}
	CHECK(dmrShortLcCrc8(0) == 0);

	// Figure B.6 placement, bit by bit, for a spread of messages
	for (int n = 0; n < 300; n++) {
		uint32_t slc = (n == 0) ? dmrShortLcNull() : ((n == 1) ? dmrShortLcActivity(8, 0, 0x5A, 0) :
							     ((uint32_t)rand() & 0x0FFFFFFFu));
		uint8_t crc = crc8_div(slc, 28);
		uint64_t all = ((uint64_t)slc << 8) | crc;
		uint32_t enc[3], rows[4];
		int wrong = 0;

		enc[0] = gen_mul(B13, 12, 17, (uint32_t)(all >> 24) & 0xFFFu) & 0x1Fu;	// parity parts only for A/B/D
		enc[1] = gen_mul(B13, 12, 17, (uint32_t)(all >> 12) & 0xFFFu) & 0x1Fu;
		enc[2] = gen_mul(B13, 12, 17, (uint32_t)all & 0xFFFu) & 0x1Fu;
		uint32_t full[3] = { gen_mul(B13, 12, 17, (uint32_t)(all >> 24) & 0xFFFu),
				     gen_mul(B13, 12, 17, (uint32_t)(all >> 12) & 0xFFFu),
				     gen_mul(B13, 12, 17, (uint32_t)all & 0xFFFu) };

		dmrShortLcEncode(slc, rows);
		for (int r = 0; r < 4; r++) {
			for (int i = 0; i < 17; i++) {
				const char *name = B6[r][i];
				int want = (name[0] == 'P') ? b6_bit(name, slc, crc, full) : b6_bit(name, slc, crc, enc);

				if ((int)((rows[r] >> (16 - i)) & 1u) != want) {
					wrong++;
				}
			}
		}
		CHECK(wrong == 0);

		// Back, also with one error per row, and the packed 9-byte form
		uint32_t got;
		uint8_t packed[DMR_CACH_SLC_BYTES], again[4];

		CHECK(dmrShortLcDecode(rows, &got) && (got == slc));
		uint32_t hit[4];

		memcpy(hit, rows, sizeof(hit));
		hit[n % 4] ^= 1u << (n % 17);
		if ((n % 4) < 3) {
			// The Hamming row decodes it; a hit in the parity row only breaks its column's parity
		}
		dmrShortLcPack(rows, packed);
		CHECK((packed[8] & 0x0F) == 0);
		uint32_t back[4];

		dmrShortLcUnpack(packed, back);
		CHECK(memcmp(back, rows, sizeof(back)) == 0);
		(void)again;
	}
	// One flipped bit in a payload row: a column of the BPTC matrix holds bits of several CACH rows, so the Hamming
	// rows correct it (when it lands in rows 1-3 of the encode matrix) or the column parity catches it
	uint32_t rows[4], slc;

	dmrShortLcEncode(dmrShortLcActivity(8, 9, 0x12, 0x34), rows);
	for (int r = 0; r < 4; r++) {
		for (int i = 0; i < 17; i++) {
			uint32_t hit[4];

			memcpy(hit, rows, sizeof(hit));
			hit[r] ^= 1u << i;
			bool ok = dmrShortLcDecode(hit, &slc);

			// transmit bit t = 17 r + 16 - i sits in encode row t % 4: rows 0-2 correct it, row 3 (parity) fails
			int t = 17 * r + 16 - i;

			CHECK(ok == ((t % 4) != 3));
			if (ok) {
				CHECK(slc == dmrShortLcActivity(8, 9, 0x12, 0x34));
			}
		}
	}
}

static void test_idle_info(void)
{
	uint8_t want[12] = { 0 };

	for (int i = 0; i < 96; i++) {
		if (D2[i] == '1') {
			want[i / 8] |= (uint8_t)(0x80u >> (i % 8));
		}
	}
	CHECK(memcmp(want, BS_IDLE_INFO, 12) == 0);
}

// ---- bsOutbound ----

static dmr_tx_burst_t voice(uint8_t index, uint8_t seed)
{
	dmr_tx_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.len = DMR_VOICE_BYTES;
	memset(b.payload, seed, DMR_VOICE_BYTES);
	return b;
}

static dmr_tx_burst_t lcb(uint8_t kind, uint8_t tag)
{
	dmr_tx_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.host_crc = true;
	b.len = 12;
	memset(b.payload, tag, 9);
	return b;
}

static void test_outbound(void)
{
	bsOutbound_t o;
	dmr_tx_burst_t b;
	dmr_cach_t c;
	const uint8_t *lc;
	uint32_t t = 1000;

	bsOutboundInit(&o, 3);
	// Nothing buffered: idle with the idle message, Null short LC, TC alternating, AT 0
	uint32_t rows[4];
	for (int i = 0; i < 8; i++) {
		int8_t ts = (int8_t)(1 + (i & 1));

		bsOutboundNext(&o, ts, t, &b, &c, &lc);
		CHECK(b.kind == DMR_BURST_IDLE && b.sync_override == DMR_SYNC_BS && !b.host_crc);
		CHECK(memcmp(b.payload, BS_IDLE_INFO, 12) == 0);
		CHECK(c.tc == (uint8_t)(ts - 1) && c.at == 0);
		rows[i % 4] = c.payload;
		CHECK(c.lcss == ((i % 4 == 0) ? DMR_LCSS_FIRST : ((i % 4 == 3) ? DMR_LCSS_LAST : DMR_LCSS_CONT)));
		uint8_t at, tc, lcss;
		uint8_t tact;
		uint32_t p;

		dmrCachDeinterleave(c.bits, &tact, &p);
		CHECK(dmrCachTactDecode(tact, &at, &tc, &lcss) == 0 && at == c.at && tc == c.tc && lcss == c.lcss);
		CHECK(p == c.payload);
		// The first fragment carries the whole short LC for a chip that encodes it (the HR_C6000's TX RAM)
		CHECK(i % 4 != 0 || (c.slc == dmrShortLcNull() && c.slc_crc == dmrShortLcCrc8(c.slc)));
	}
	uint32_t slc;
	CHECK(dmrShortLcDecode(rows, &slc) && slc == dmrShortLcNull());

	// A stream on TS1: idle until 3 are in (prebuffer), then in order; AT set by the automatic rule
	CHECK(bsOutboundPush(&o, 1, &(dmr_tx_burst_t){ 0 }, NULL, t) == 0);	// placeholder replaced below
	bsOutboundReset(&o);
	dmr_tx_burst_t hdr = lcb(DMR_BURST_VOICE_LC_HEADER, 0x11);
	CHECK(bsOutboundPush(&o, 1, &hdr, NULL, t) == 0);
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_IDLE && c.at == 0);
	dmr_tx_burst_t a0 = voice(0, 0xA0);
	uint8_t elc[9] = { 0, 0, 0, 0, 0, 9, 0, 0, 1 };
	CHECK(bsOutboundPush(&o, 1, &a0, elc, t) == 0);
	dmr_tx_burst_t v1 = voice(1, 0xA1);
	CHECK(bsOutboundPush(&o, 1, &v1, NULL, t) == 0);
	CHECK(bsOutboundCount(&o, 1) == 3 && bsOutboundSpace(&o, 1) == BS_OUT_DEPTH - 3 && bsOutboundSpace(&o, 2) == 16);
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_VOICE_LC_HEADER && b.sync_override == DMR_SYNC_BS && c.at == 1 && c.tc == 0);
	bsOutboundNext(&o, 2, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_IDLE && c.at == 0 && c.tc == 1);	// TS2 untouched
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_VOICE && b.voice_index == 0 && lc != NULL && memcmp(lc, elc, 9) == 0);
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_VOICE && b.voice_index == 1 && lc == NULL && b.payload[0] == 0xA1);
	// Underrun: silence at the next positions, counted once, A gets the header's LC
	t += 60;
	for (int i = 2; i <= 6; i++) {
		bsOutboundNext(&o, 1, t, &b, &c, &lc);
		CHECK(b.kind == DMR_BURST_VOICE && b.voice_index == (i % 6) && c.at == 1);
		if ((i % 6) == 0) {
			CHECK(lc != NULL && memcmp(lc, hdr.payload, 9) == 0);
		}
	}
	CHECK(o.slot[0].underruns == 1 && o.st.silence == 5);
	// The host gone for 500 ms: our terminator, then idle
	t += BS_OUT_STREAM_LOST_MS;
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_TERMINATOR_LC && memcmp(b.payload, hdr.payload, 9) == 0 && o.st.closed == 1);
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_IDLE && !bsOutboundBusy(&o));

	// A stream that ends before the prebuffer: plays at once on its terminator; the 240 ms rule
	dmr_tx_burst_t term = lcb(DMR_BURST_TERMINATOR_LC, 0x11);
	bsOutboundPush(&o, 2, &hdr, NULL, t);
	bsOutboundPush(&o, 2, &term, NULL, t);
	bsOutboundNext(&o, 2, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_VOICE_LC_HEADER);
	bsOutboundNext(&o, 2, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_TERMINATOR_LC);
	dmr_tx_burst_t csbk = lcb(DMR_BURST_CSBK, 0x22);
	bsOutboundPush(&o, 2, &csbk, NULL, t);
	bsOutboundNext(&o, 2, t + BS_OUT_WAIT_MS - 1, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_IDLE);
	bsOutboundNext(&o, 2, t + BS_OUT_WAIT_MS, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_CSBK && b.host_crc);

	// Full: NAK 5 at 16
	bsOutboundReset(&o);
	for (int i = 0; i < BS_OUT_DEPTH; i++) {
		dmr_tx_burst_t v = voice((uint8_t)(i % 6), 1);
		CHECK(bsOutboundPush(&o, 1, &v, NULL, t) == 0);
	}
	CHECK(bsOutboundPush(&o, 1, &a0, NULL, t) == 5 && bsOutboundSpace(&o, 1) == 0);
	bsOutboundDrop(&o, 1);
	CHECK(bsOutboundCount(&o, 1) == 0);

	// AT: the host's mask, its 2 s expiry, mode 1 (mask alone)
	bsOutboundReset(&o);
	bsOutboundSetAt(&o, 2, BS_AT_MODE_OR, t);
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(c.at == 0);
	bsOutboundNext(&o, 2, t, &b, &c, &lc);
	CHECK(c.at == 1 && o.at_on_air == 2);
	bsOutboundNext(&o, 2, t + BS_OUT_AT_HOLD_MS, &b, &c, &lc);
	CHECK(c.at == 0 && bsOutboundAtMask(&o, t + BS_OUT_AT_HOLD_MS) == 0);
	bsOutboundSetAt(&o, 0, BS_AT_MODE_MASK, t);
	bsOutboundPush(&o, 1, &hdr, NULL, t);
	bsOutboundPush(&o, 1, &term, NULL, t);
	bsOutboundNext(&o, 1, t, &b, &c, &lc);
	CHECK(b.kind == DMR_BURST_VOICE_LC_HEADER && c.at == 0);	// mode 1: not the automatic rule

	// Short LC switch: from the next multiple of 4, for both slots alike
	bsOutboundInit(&o, 3);
	uint32_t act[4];
	dmrShortLcEncode(dmrShortLcActivity(8, 0, 0x5A, 0), act);
	bsOutboundNext(&o, 1, t, &b, &c, &lc);		// s = 0
	bsOutboundNext(&o, 2, t, &b, &c, &lc);		// s = 1
	bsOutboundNext(&o, 1, t, &b, &c, &lc);		// s = 2
	bsOutboundSetShortLc(&o, act);			// next s: TS2 3, TS1 4 -> switch at 4
	bsOutboundNext(&o, 2, t, &b, &c, &lc);		// s = 3: old
	uint32_t nul[4];
	dmrShortLcEncode(dmrShortLcNull(), nul);
	CHECK(c.payload == nul[3] && c.lcss == DMR_LCSS_LAST);
	uint32_t got[4];
	for (int i = 0; i < 4; i++) {
		bsOutboundNext(&o, (int8_t)(1 + (i & 1)), t, &b, &c, &lc);
		got[i] = c.payload;
	}
	CHECK(memcmp(got, act, sizeof(got)) == 0);
	CHECK(dmrShortLcDecode(got, &slc) && slc == dmrShortLcActivity(8, 0, 0x5A, 0));
}

// ---- slotEpoch ----

static void test_epoch(void)
{
	slotEpoch_t e;
	int32_t res;

	slotEpochInit(&e);
	CHECK(slotEpochLabel(&e, 123, 0, &res) == 0);		// no epoch
	CHECK(!slotEpochSet(&e, 0, 29989999u, 0) && !slotEpochSet(&e, 0, 30010001u, 0));
	CHECK(slotEpochSet(&e, 1000000u, 0, 100));
	// Outbound TS1 at E: the inbound slot then is TS2; one slot later TS1
	CHECK(slotEpochLabel(&e, 1000000u, 100, &res) == 2 && res == 0);
	CHECK(slotEpochLabel(&e, 1030000u, 100, &res) == 1 && res == 0);
	CHECK(slotEpochLabel(&e, 1060000u + 14000u, 100, &res) == 2 && res == 14000);
	CHECK(slotEpochLabel(&e, 1060000u - 14000u, 100, &res) == 2 && res == -14000);
	CHECK(slotEpochLabel(&e, 1000000u - 30000u, 100, &res) == 1);	// before E
	// Wrap-around of the 32-bit clock
	CHECK(slotEpochSet(&e, 0xFFFFFFFFu - 10000u, 0, 200));
	CHECK(slotEpochLabel(&e, 0xFFFFFFFFu - 10000u + 60000u, 200, &res) == 2 && res == 0);
	CHECK(slotEpochLabel(&e, 0xFFFFFFFFu - 10000u + 90000u, 200, &res) == 1 && res == 0);
	// Too old
	CHECK(slotEpochLabel(&e, 0, 200 + SLOT_EPOCH_MAX_AGE_MS, &res) == 0);

	// A rate error: the period in this radio's clock 100 ppm long, 50 s on (the anchor moves forward itself)
	CHECK(slotEpochSet(&e, 5000000u, 30003000u, 0));
	uint32_t n = 3001u;	// slots in about 90 s
	uint64_t t = 5000000ull + ((uint64_t)n * 30003000ull) / 1000ull;
	CHECK(slotEpochLabel(&e, (uint32_t)t, 1000, &res) == ((n & 1) ? 1 : 2) && res > -2 && res < 2);
	uint64_t t2 = 5000000ull + ((uint64_t)(n + 1) * 30003000ull) / 1000ull;
	CHECK(slotEpochLabel(&e, (uint32_t)t2, 1000, &res) == ((n & 1) ? 2 : 1) && res > -2 && res < 2);

	// Tracking: a constant 4 ms chip delay is followed; never more than 5 ms in all
	CHECK(slotEpochSet(&e, 0, 0, 0));
	for (int i = 0; i < 200; i++) {
		uint32_t s = (uint32_t)(i * 2 + 1) * 30000u + 4000u;
		CHECK(slotEpochLabel(&e, s, 0, &res) == 1);
		slotEpochTrack(&e, res);
	}
	CHECK(e.track_us > 3800 && e.track_us <= 4100);
	CHECK(slotEpochLabel(&e, 30000u * 501u + 4000u, 0, &res) == 1 && res > -300 && res < 300);
	for (int i = 0; i < 400; i++) {
		CHECK(slotEpochLabel(&e, (uint32_t)(i * 2 + 1) * 30000u + 12000u, 0, &res) == 1);
		slotEpochTrack(&e, res);
	}
	CHECK(e.track_us == SLOT_EPOCH_TRACK_MAX_US);
}

// ---- The extension through mmdvm.c and hotspot.c ----

#define MAX_OUT 4096
static struct { uint8_t b[MMDVM_FRAME_MAX]; size_t len; } out[MAX_OUT];
static int out_count;
static uint32_t now_ms;

static void io_write(const uint8_t *buf, size_t len)
{
	if (out_count < MAX_OUT) {
		memcpy(out[out_count].b, buf, len);
		out[out_count].len = len;
		out_count++;
	}
}

static const mmdvm_io_t io = { .write = io_write, .description = "OpenDigitalRadio Test 0123abcd" };

static uint8_t power_set;
static bool r_enter(uint32_t rx) { (void)rx; return true; }
static bool r_set_freq(uint32_t rx, uint32_t tx) { (void)rx; (void)tx; return true; }
static void r_set_power(uint8_t level) { power_set = level; }
static void r_leave(void) {}
static const hotspot_radio_ops_t radio_ops = { .enter = r_enter, .set_freq = r_set_freq, .set_power = r_set_power,
	.leave = r_leave };

static uint32_t clock_base_us = 7000000u;	// modem clock = now_ms x 1000 + base

static void host(uint8_t cmd, const uint8_t *p, uint8_t len)
{
	uint8_t f[MMDVM_FRAME_MAX];

	f[0] = MMDVM_FRAME_START;
	f[1] = (uint8_t)(len + 3);
	f[2] = cmd;
	if (len > 0) {
		memcpy(f + 3, p, len);
	}
	ssim_advance_to(now_ms * 1000u + clock_base_us);
	mmdvm_frame(f, (uint16_t)(len + 3), now_ms);
}

static void run_ms(uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i += 5) {
		now_ms += 5;
		ssim_advance_to(now_ms * 1000u + clock_base_us);
		mmdvm_poll(now_ms);
	}
}

static const uint8_t *last_out(uint8_t cmd)
{
	for (int i = out_count - 1; i >= 0; i--) {
		if (out[i].b[2] == cmd) {
			return out[i].b;
		}
	}
	return NULL;
}

static const uint8_t *split_reply(uint8_t sub)
{
	for (int i = out_count - 1; i >= 0; i--) {
		if ((out[i].b[2] == MMDVM_ODR_SPLIT) && (out[i].b[3] == sub)) {
			return out[i].b;
		}
	}
	return NULL;
}

static int nak_of(uint8_t cmd, int from)
{
	for (int i = from; i < out_count; i++) {
		if ((out[i].b[2] == MMDVM_NAK) && (out[i].b[3] == cmd)) {
			return out[i].b[4];
		}
	}
	return -1;
}

static bool acked(uint8_t cmd, int from)
{
	for (int i = from; i < out_count; i++) {
		if ((out[i].b[2] == MMDVM_ACK) && (out[i].b[3] == cmd)) {
			return true;
		}
	}
	return false;
}

static int count_out(uint8_t cmd, int from)
{
	int n = 0;

	for (int i = from; i < out_count; i++) {
		n += out[i].b[2] == cmd;
	}
	return n;
}

static uint8_t split_cmd(uint8_t sub, const uint8_t *data, uint8_t len)
{
	uint8_t p[40];

	p[0] = sub;
	if (len > 0) {
		memcpy(p + 1, data, len);
	}
	host(MMDVM_ODR_SPLIT, p, (uint8_t)(len + 1));
	const uint8_t *r = split_reply(sub);

	return (r != NULL) ? r[4] : 0xEE;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void configure(bool simplex, uint8_t cc)
{
	uint8_t p[23] = { simplex ? 0x80 : 0x00, 0x02, 10, MMDVM_MODE_DMR, 128, 128, cc, 0, 128, 0, 128 };

	host(MMDVM_SET_CONFIG, p, sizeof(p));
}

static void set_freq(uint32_t rx, uint32_t tx, uint8_t level)
{
	uint8_t p[17] = { 0 };

	for (int i = 0; i < 4; i++) {
		p[1 + i] = (uint8_t)(rx >> (8 * i));
		p[5 + i] = (uint8_t)(tx >> (8 * i));
	}
	p[9] = level;
	host(MMDVM_SET_FREQ, p, sizeof(p));
}

static void reset_world(const hal_dmr_modem_t *m)
{
	ssim_reset();
	out_count = 0;
	now_ms = 1000;
	power_set = 0;
	hotspot_init(m, mmdvm_send);
	hotspot_set_radio(&radio_ops);
	mmdvm_init(&io);
	hotspot_split_init();
}

static void session(const hal_dmr_modem_t *m)
{
	reset_world(m);
	host(MMDVM_GET_VERSION, NULL, 0);
	configure(false, 1);
	set_freq(433000000, 438000000, 255);
}

// A 33-byte frame for a data burst (BPTC of the 12 info bytes with slot type)
static void data_frame(uint8_t dt, const uint8_t *info, uint8_t len, uint8_t *frame)
{
	dmrBurst_t d;

	memset(frame, 0, 33);
	d.dataType = dt;
	d.length = len;
	d.flags = 0;
	memcpy(d.payload, info, len);
	dmrDataBurstToFrame(&d, 1, frame);
}

static void lc_frame(uint8_t dt, const uint8_t *lc, uint8_t *frame)
{
	uint8_t full[12];

	dmr_lc_encode_full(lc, (dt == DT_VOICE_LC_HEADER) ? DMR_RS_MASK_VOICE_HEADER : DMR_RS_MASK_TERMINATOR, full);
	data_frame(dt, full, 12, frame);
}

static const uint8_t LC1[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x2F, 0x5A, 0x01 };	// TG 9
static const uint8_t LC2[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B, 0x2F, 0x5A, 0x02 };	// TG 91

// A voice frame as MMDVMHost builds it: AMBE, then the EMB and embedded LC fragment for B-E
static void voice_frame(uint8_t index, uint8_t seed, const uint8_t *lc, uint8_t *frame)
{
	uint8_t frag[4][4];
	uint8_t mid[6] = { 0 };

	memset(frame, seed, 33);
	dmr_emb_lc_encode(lc, frag);
	if (index == 0) {
		static const uint8_t BS_VOICE[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };
		memcpy(mid, BS_VOICE, 6);
	} else {
		uint16_t emb = dmrFecEmbEncode(1, false, dmr_emb_lcss(index));

		mid[0] = (uint8_t)(emb >> 8);
		if (index <= 4) {
			memcpy(mid + 1, frag[index - 1], 4);
		}
		mid[5] = (uint8_t)emb;
	}
	frame[13] = (uint8_t)((frame[13] & 0xF0) | (mid[0] >> 4));
	for (int i = 0; i < 5; i++) {
		frame[14 + i] = (uint8_t)((mid[i] << 4) | (mid[i + 1] >> 4));
	}
	frame[19] = (uint8_t)((mid[5] << 4) | (frame[19] & 0x0F));
}

static void net(uint8_t cmd, uint8_t control, const uint8_t *frame)
{
	uint8_t p[34];

	p[0] = control;
	memcpy(p + 1, frame, 33);
	host(cmd, p, 34);
}

static void test_normal(void)
{
	// Built in, no role: GET_VERSION gains " SPLIT1"; 0xD6 is answered; nothing else changes
	reset_world(&ssim_modem);
	host(MMDVM_GET_VERSION, NULL, 0);
	const uint8_t *v = last_out(MMDVM_GET_VERSION);
	CHECK(v != NULL && v[1] == 4 + strlen(io.description) + 7);
	CHECK(memcmp(v + 4 + strlen(io.description), " SPLIT1", 7) == 0);
	CHECK(split_cmd(SPLIT_SUB_INFO, NULL, 0) == 0);
	const uint8_t *r = split_reply(SPLIT_SUB_INFO);
	CHECK(r[1] == 5 + SPLIT_INFO_LEN && r[5] == 1 && r[6] == 3 && r[7] == 0 && r[8] == 16 && r[9] == DMR_BS_CACH_OURS);
	CHECK(rd32(r + 10) == now_ms * 1000u + clock_base_us);
	CHECK(split_cmd(0x07, NULL, 0) == SPLIT_RESULT_UNKNOWN);		// unknown sub-command
	CHECK(split_cmd(SPLIT_SUB_GET_EPOCH, NULL, 0) == SPLIT_RESULT_STATE);
	CHECK(split_cmd(SPLIT_SUB_SET_EPOCH, (const uint8_t[]){ 0, 0, 0, 0 }, 4) == SPLIT_RESULT_STATE);
	CHECK(split_cmd(SPLIT_SUB_SET_AT, (const uint8_t[]){ 1 }, 1) == SPLIT_RESULT_STATE);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 3 }, 1) == SPLIT_RESULT_INVALID);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, NULL, 0) == SPLIT_RESULT_INVALID);
	host(MMDVM_ODR_SPLIT, NULL, 0);
	CHECK(out[out_count - 1].b[2] == MMDVM_ODR_SPLIT && out[out_count - 1].b[4] == SPLIT_RESULT_INVALID);
	// Simplex DMR_DATA1 still NAK 2, DMR_START / SHORTLC still silent, unknown commands still NAK 1
	int from = out_count;
	uint8_t f[33];
	lc_frame(DT_VOICE_LC_HEADER, LC1, f);
	configure(true, 1);
	set_freq(438000000, 438000000, 255);
	net(MMDVM_DMR_DATA1, 0x41, f);
	CHECK(nak_of(MMDVM_DMR_DATA1, from) == MMDVM_NAK_WRONG_MODE);
	from = out_count;
	host(MMDVM_DMR_START, (const uint8_t[]){ 1 }, 1);
	host(MMDVM_DMR_SHORTLC, (const uint8_t[]){ 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 9);
	CHECK(out_count == from);
	host(0xD7, NULL, 0);
	CHECK(nak_of(0xD7, from) == MMDVM_NAK_UNKNOWN);
	// TIME: token echoed, receive and transmit stamps
	uint8_t tok[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	CHECK(split_cmd(SPLIT_SUB_TIME, tok, 8) == 0);
	r = split_reply(SPLIT_SUB_TIME);
	CHECK(r[1] == 5 + 16 && memcmp(r + 5, tok, 8) == 0);
	CHECK(rd32(r + 13) == now_ms * 1000u + clock_base_us && rd32(r + 17) == rd32(r + 13));
	CHECK(split_cmd(SPLIT_SUB_TIME, tok, 7) == SPLIT_RESULT_INVALID);

	// A modem without the split operations: no roles offered, SET_ROLE refused, the version suffix still there
	reset_world(&ssim_modem_plain);
	host(MMDVM_GET_VERSION, NULL, 0);
	CHECK(split_cmd(SPLIT_SUB_INFO, NULL, 0) == 0 && split_reply(0)[6] == 0);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1 }, 1) == SPLIT_RESULT_STATE);
	CHECK(split_cmd(SPLIT_SUB_TIME, tok, 8) == SPLIT_RESULT_STATE);

	// Before the hotspot has the modem: SET_ROLE 2 (wrong state); role 0 is always fine
	ssim_reset();
	out_count = 0;
	hotspot_init(&ssim_modem, mmdvm_send);
	hotspot_set_radio(NULL);
	mmdvm_init(&io);
	hotspot_split_init();
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1 }, 1) == SPLIT_RESULT_STATE);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 0 }, 1) == 0);
}

// The air log from index from: bursts of slot ts
static int air_find(unsigned from, int8_t ts, uint8_t kind, int index)
{
	for (unsigned i = from; i < ssim.air_n; i++) {
		if ((ssim.air[i].ts == ts) && (ssim.air[i].b.kind == kind) &&
		    ((index < 0) || (ssim.air[i].b.voice_index == index))) {
			return (int)i;
		}
	}
	return -1;
}

static void test_tx_half(void)
{
	session(&ssim_modem);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1, 10, 50, 3 }, 4) == 0);
	CHECK(split_reply(SPLIT_SUB_SET_ROLE)[7] == SPLIT_ROLE_TX);
	CHECK(ssim.bs && ssim.bs_starts == 1 && ssim.bs_cfg.cc == 1 && !ssim.keyed);
	CHECK(power_set == 50);			// SET_FREQ's 255 capped
	run_ms(100);
	// The epoch: the slot clock runs before keying
	CHECK(split_cmd(SPLIT_SUB_GET_EPOCH, NULL, 0) == 0);
	const uint8_t *r = split_reply(SPLIT_SUB_GET_EPOCH);
	CHECK(r[1] == 5 + 10 && rd32(r + 5) == ssim.counter && rd32(r + 9) == ssim.t_tick && r[13] == ssim.ts_tick &&
	      r[14] == 2);
	CHECK(split_cmd(SPLIT_SUB_SET_EPOCH, (const uint8_t[]){ 0, 0, 0, 0 }, 4) == SPLIT_RESULT_STATE);

	// Status: no TX yet, the space of both slots
	host(MMDVM_GET_STATUS, NULL, 0);
	const uint8_t *s = last_out(MMDVM_GET_STATUS);
	CHECK(s[1] == 13 && (s[5] & MMDVM_STATUS_TX) == 0 && s[7] == 14 && s[8] == 14);

	// DMR_SHORTLC (the 68 payload bits) and DMR_START 01: keyed, idle bursts with the CACH
	uint32_t rows[4];
	uint8_t slc9[9];
	dmrShortLcEncode(dmrShortLcActivity(8, 0, dmrShortLcHash(9), 0), rows);
	dmrShortLcPack(rows, slc9);
	int from = out_count;
	host(MMDVM_DMR_SHORTLC, slc9, 9);
	host(MMDVM_DMR_START, (const uint8_t[]){ 1 }, 1);
	CHECK(acked(MMDVM_DMR_SHORTLC, from) && acked(MMDVM_DMR_START, from));
	unsigned air0 = ssim.air_n;
	run_ms(300);
	CHECK(ssim.keyed && ssim.keyups == 1);
	CHECK(ssim.air_n >= air0 + 9 && ssim.underruns == 0);
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK(last_out(MMDVM_GET_STATUS)[5] & MMDVM_STATUS_TX);
	// Each slot alternates, TC follows it, the short LC decodes from four CACH in a row starting at LCSS first
	int first = -1;
	for (unsigned i = air0; i + 4 <= ssim.air_n; i++) {
		CHECK(ssim.air[i].cach.tc == (uint8_t)(ssim.air[i].ts - 1));
		CHECK(ssim.air[i].b.kind == DMR_BURST_IDLE && ssim.air[i].cach.at == 0);
		if ((first < 0) && (ssim.air[i].cach.lcss == DMR_LCSS_FIRST)) {
			first = (int)i;
		}
	}
	if (getenv("SPLIT_DEBUG") != NULL) {
		for (unsigned i = air0; i < ssim.air_n; i++) {
			printf("air %u ts %d lcss %d kind %d\n", i, ssim.air[i].ts, ssim.air[i].cach.lcss, ssim.air[i].b.kind);
		}
	}
	CHECK(first >= 0);
	uint32_t got[4], slc;
	for (int i = 0; i < 4; i++) {
		got[i] = ssim.air[first + i].cach.payload;
	}
	CHECK(dmrShortLcDecode(got, &slc) && slc == dmrShortLcActivity(8, 0, dmrShortLcHash(9), 0));

	// Two network calls, one per slot, MMDVMHost style (headers, voice with control bytes, terminators)
	uint8_t f[33];
	air0 = ssim.air_n;
	lc_frame(DT_VOICE_LC_HEADER, LC1, f);
	net(MMDVM_DMR_DATA1, 0x41, f);
	net(MMDVM_DMR_DATA1, 0x41, f);
	lc_frame(DT_VOICE_LC_HEADER, LC2, f);
	net(MMDVM_DMR_DATA2, 0x41, f);
	uint8_t idle[33];
	data_frame(DT_IDLE, BS_IDLE_INFO, 12, idle);
	net(MMDVM_DMR_DATA2, 0x49, idle);		// taken, dropped
	for (int sf = 0; sf < 2; sf++) {
		for (uint8_t i = 0; i < 6; i++) {
			voice_frame(i, (uint8_t)(0x10 + i), LC1, f);
			net(MMDVM_DMR_DATA1, (i == 0) ? 0x20 : i, f);
			voice_frame(i, (uint8_t)(0x30 + i), LC2, f);
			net(MMDVM_DMR_DATA2, 0x00, f);	// control byte 0: positions follow on (A from the sync)
			run_ms(60);
		}
	}
	lc_frame(DT_TERMINATOR_WITH_LC, LC1, f);
	for (int i = 0; i < 3; i++) {
		net(MMDVM_DMR_DATA1, 0x42, f);
	}
	lc_frame(DT_TERMINATOR_WITH_LC, LC2, f);
	net(MMDVM_DMR_DATA2, 0x42, f);
	CHECK(nak_of(MMDVM_DMR_DATA1, from) < 0 && nak_of(MMDVM_DMR_DATA2, from) < 0);
	run_ms(600);
	// On air: TS1 header, A-F twice with our payload, one terminator; TS2 likewise; AT busy on the call's slot
	for (int8_t ts = 1; ts <= 2; ts++) {
		int h = air_find(air0, ts, DMR_BURST_VOICE_LC_HEADER, -1);
		CHECK(h >= 0 && memcmp(ssim.air[h].b.payload, (ts == 1) ? LC1 : LC2, 9) == 0 && ssim.air[h].b.host_crc);
		CHECK(h >= 0 && ssim.air[h].cach.at == 1 && ssim.air[h].b.sync_override == DMR_SYNC_BS);
		int a = air_find(air0, ts, DMR_BURST_VOICE, 0);
		CHECK(a > h && ssim.air[a].emb_lc_valid && memcmp(ssim.air[a].emb_lc, (ts == 1) ? LC1 : LC2, 9) == 0);
		int pos = a, voices = 0;
		for (unsigned i = (unsigned)a; i < ssim.air_n; i++) {
			if ((ssim.air[i].ts == ts) && (ssim.air[i].b.kind == DMR_BURST_VOICE)) {
				CHECK(ssim.air[i].b.voice_index == voices % 6);
				CHECK(ssim.air[i].b.payload[0] == (uint8_t)(((ts == 1) ? 0x10 : 0x30) + voices % 6));
				voices++;
				pos = (int)i;
			}
		}
		CHECK(voices == 12);
		int t = air_find((unsigned)pos, ts, DMR_BURST_TERMINATOR_LC, -1);
		CHECK(t > pos && air_find((unsigned)t + 1, ts, DMR_BURST_TERMINATOR_LC, -1) < 0);	// one terminator
	}
	CHECK(ssim.underruns == 0 && split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0);
	r = split_reply(SPLIT_SUB_STATUS);
	CHECK(r[1] == 5 + SPLIT_STATUS_LEN && r[5] == 1 && (r[6] & SPLIT_FLAG_KEYED) && (r[6] & SPLIT_FLAG_CLOCK));
	CHECK(r[9] == 0 && r[10] == 0 && rd16(r + 11) == 0 && rd16(r + 13) == 0);
	CHECK(rd16(r + 19) == 1 && rd16(r + 33) == 0);	// one short LC taken, none bad

	// SET_AT from the RX half: AT busy on TS2 in the CACH before outbound TS2 only
	CHECK(split_cmd(SPLIT_SUB_SET_AT, (const uint8_t[]){ 2 }, 1) == 0 && split_reply(5)[5] == 2);
	air0 = ssim.air_n + 4;			// what the driver held already went out with the old bits
	run_ms(200);
	for (unsigned i = air0; i < ssim.air_n; i++) {
		CHECK(ssim.air[i].cach.at == (ssim.air[i].ts == 2));
	}
	for (int i = 0; i < 9; i++) {		// not refreshed: back to idle after 2 s (the host still polls)
		host(MMDVM_GET_STATUS, NULL, 0);
		run_ms(250);
	}
	CHECK(ssim.air[ssim.air_n - 1].cach.at == 0 && ssim.air[ssim.air_n - 2].cach.at == 0);
	CHECK(split_cmd(SPLIT_SUB_SET_AT, (const uint8_t[]){ 1, 2 }, 2) == SPLIT_RESULT_INVALID);

	// Full: the 17th burst on a slot gets NAK 5 and the overflow flag once
	from = out_count;
	voice_frame(0, 0x55, LC1, f);
	lc_frame(DT_VOICE_LC_HEADER, LC1, idle);
	net(MMDVM_DMR_DATA1, 0x41, idle);
	for (int i = 0; i < 16; i++) {
		net(MMDVM_DMR_DATA1, (i % 6 == 0) ? 0x20 : (uint8_t)(i % 6), f);
	}
	CHECK(nak_of(MMDVM_DMR_DATA1, from) == MMDVM_NAK_FULL);
	host(MMDVM_GET_STATUS, NULL, 0);
	s = last_out(MMDVM_GET_STATUS);
	CHECK((s[5] & MMDVM_STATUS_TX_OVERFLOW) && s[7] == 0);
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK((last_out(MMDVM_GET_STATUS)[5] & MMDVM_STATUS_TX_OVERFLOW) == 0);
	// DMR_ABORT 1: slot 1 dropped
	from = out_count;
	host(MMDVM_DMR_ABORT, (const uint8_t[]){ 1 }, 1);
	CHECK(acked(MMDVM_DMR_ABORT, from));
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK(last_out(MMDVM_GET_STATUS)[7] == 14);
	// Wrong length: NAK 4
	from = out_count;
	host(MMDVM_DMR_DATA2, f, 10);
	CHECK(nak_of(MMDVM_DMR_DATA2, from) == MMDVM_NAK_INVALID);

	// DMR_START 00: unkeyed once drained (at most 1 s)
	host(MMDVM_DMR_START, (const uint8_t[]){ 0 }, 1);
	run_ms(1100);
	CHECK(!ssim.keyed);
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK((last_out(MMDVM_GET_STATUS)[5] & MMDVM_STATUS_TX) == 0);

	// A retune restarts the slot clock (a new epoch), the role stays
	unsigned starts = ssim.bs_starts;
	set_freq(433000000, 438100000, 255);
	run_ms(20);
	CHECK(ssim.bs_starts == starts + 1 && ssim.bs);
	CHECK(split_cmd(SPLIT_SUB_INFO, NULL, 0) == 0 && split_reply(0)[7] == SPLIT_ROLE_TX);

	// The host silent for 2 s while keyed: unkeyed (the lease of 10 s still holds)
	host(MMDVM_DMR_START, (const uint8_t[]){ 1 }, 1);
	run_ms(100);
	CHECK(ssim.keyed);
	run_ms(SPLIT_HOST_SILENT_MS);
	CHECK(!ssim.keyed);

	// SET_MODE idle unkeys too
	host(MMDVM_DMR_START, (const uint8_t[]){ 1 }, 1);
	run_ms(100);
	CHECK(ssim.keyed);
	host(MMDVM_SET_MODE, (const uint8_t[]){ MMDVM_MODE_IDLE }, 1);
	run_ms(20);
	CHECK(!ssim.keyed);

	// The lease: no 0xD6 for 10 s, back to the standard hotspot (GET_STATUS polls don't keep it)
	for (int i = 0; i < 45; i++) {
		host(MMDVM_GET_STATUS, NULL, 0);
		run_ms(250);
	}
	CHECK(!ssim.bs && !ssim.keyed && (power_set == 255));
	host(MMDVM_GET_STATUS, NULL, 0);
	s = last_out(MMDVM_GET_STATUS);
	CHECK(s[7] == 0);			// simplex again: no slot 1 space
	hotspot_split_view_t v;
	hotspot_split_get_view(&v);
	CHECK(v.role == 0 && v.lease_expiries == 1);
	from = out_count;
	net(MMDVM_DMR_DATA1, 0x41, idle);
	CHECK(nak_of(MMDVM_DMR_DATA1, from) == MMDVM_NAK_WRONG_MODE);
}

static void test_tx_guard(void)
{
	session(&ssim_modem);
	// Longest key-up 10 s, cool-down 3 s
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1, 30, 0, 0, 1, 3 }, 6) == 0);
	host(MMDVM_DMR_START, (const uint8_t[]){ 1 }, 1);
	for (int i = 0; i < 42; i++) {
		split_cmd(SPLIT_SUB_TIME, (const uint8_t[]){ 0, 0, 0, 0, 0, 0, 0, 0 }, 8);	// keeps lease and host alive
		run_ms(250);
	}
	CHECK(!ssim.keyed);
	CHECK(split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0 && (split_reply(6)[6] & SPLIT_FLAG_COOLDOWN));
	int from = out_count;
	uint8_t f[33];
	lc_frame(DT_VOICE_LC_HEADER, LC1, f);
	net(MMDVM_DMR_DATA2, 0x41, f);
	CHECK(nak_of(MMDVM_DMR_DATA2, from) == MMDVM_NAK_WRONG_MODE);
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK(last_out(MMDVM_GET_STATUS)[8] == 0);
	for (int i = 0; i < 14; i++) {
		split_cmd(SPLIT_SUB_TIME, (const uint8_t[]){ 0, 0, 0, 0, 0, 0, 0, 0 }, 8);
		run_ms(250);
	}
	CHECK(split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0 && !(split_reply(6)[6] & SPLIT_FLAG_COOLDOWN));
	net(MMDVM_DMR_DATA2, 0x41, f);
	run_ms(50);
	CHECK(ssim.keyed);
	// The end of the session ends the role
	mmdvm_end();
	CHECK(!ssim.keyed && !ssim.bs);
	hotspot_split_view_t v;
	hotspot_split_get_view(&v);
	CHECK(v.role == SPLIT_ROLE_NORMAL);
	// Bad SET_ROLE values
	session(&ssim_modem);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1, 0, 0, 13 }, 4) == SPLIT_RESULT_INVALID);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1, 0, 0, 0, 61 }, 5) == SPLIT_RESULT_INVALID);
}

static dmr_burst_t rx_voice(uint8_t index, uint8_t seed, const uint8_t *lc)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.crc_ok = true;
	b.in_step = true;
	b.len = DMR_VOICE_BYTES;
	b.rssi_dbm = -80;
	memset(b.payload, seed, DMR_VOICE_BYTES);
	if (index == 5) {
		memcpy(b.lc, lc, 9);
		b.lc_valid = true;
	}
	return b;
}

static dmr_burst_t rx_lc(uint8_t kind, const uint8_t *lc)
{
	dmr_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.crc_ok = true;
	b.in_step = true;
	b.lc_valid = true;
	b.len = 12;
	b.rssi_dbm = -80;
	memcpy(b.lc, lc, 9);
	return b;
}

static uint32_t clock_us(void)
{
	return now_ms * 1000u + clock_base_us;
}

// Poll on until the modem clock reaches t_us (5 ms steps)
static void run_to_us(uint32_t t_us)
{
	while ((int32_t)(clock_us() - t_us) < 0) {
		run_ms(5);
	}
}

// A burst whose slot started at t_start: reported 31 ms later
static void rx_at(const dmr_burst_t *b, uint32_t t_start)
{
	run_to_us(t_start + 31000u);
	ssim_rx_burst(b, t_start);
}

static void test_rx_half(void)
{
	session(&ssim_modem);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 2 }, 1) == 0 && split_reply(1)[7] == SPLIT_ROLE_RX);
	CHECK(ssim.locked_ts == 0);
	int from = out_count;
	uint8_t f[33];
	lc_frame(DT_VOICE_LC_HEADER, LC1, f);
	net(MMDVM_DMR_DATA1, 0x41, f);
	CHECK(nak_of(MMDVM_DMR_DATA1, from) == MMDVM_NAK_WRONG_MODE);
	host(MMDVM_DMR_START, (const uint8_t[]){ 1 }, 1);
	CHECK(acked(MMDVM_DMR_START, from));
	CHECK(split_cmd(SPLIT_SUB_GET_EPOCH, NULL, 0) == SPLIT_RESULT_STATE);
	CHECK(split_cmd(SPLIT_SUB_SET_EPOCH, (const uint8_t[]){ 0, 0, 0, 0, 0x01, 0, 0, 0 }, 8) == SPLIT_RESULT_INVALID);

	// No epoch yet: a burst goes up as DMR_DATA2, counted
	from = out_count;
	dmr_burst_t h = rx_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
	rx_at(&h, clock_us());
	run_ms(10);
	CHECK(count_out(MMDVM_DMR_DATA2, from) == 1 && count_out(MMDVM_DMR_DATA1, from) == 0);
	CHECK(split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0 && rd16(split_reply(6) + 25) == 1);
	run_ms(600);
	CHECK(count_out(MMDVM_DMR_LOST2, from) == 1);

	// The epoch: outbound TS1 starts at E (RX clock). Inbound TS1 runs during outbound TS2: E + 30 ms + 60 k ms
	uint32_t E = now_ms * 1000u + clock_base_us + 100000u;
	uint8_t ep[8];
	for (int i = 0; i < 4; i++) {
		ep[i] = (uint8_t)(E >> (8 * i));
		ep[4 + i] = (uint8_t)(30000000u >> (8 * i));
	}
	CHECK(split_cmd(SPLIT_SUB_SET_EPOCH, ep, 8) == 0);
	CHECK(split_cmd(SPLIT_SUB_INFO, NULL, 0) == 0 && (split_reply(0)[14] & SPLIT_FLAG_EPOCH));

	// Two calls at once, interleaved: TS1 (LC1) and TS2 (LC2), 3 ms chip delay on each burst
	from = out_count;
	uint32_t base = E + 600000u;
	dmr_burst_t b;
	for (int k = 0; k < 14; k++) {
		uint32_t s1 = base + (uint32_t)k * 60000u + 30000u + 3000u;	// inbound TS1
		uint32_t s2 = base + (uint32_t)k * 60000u + 3000u;		// inbound TS2
		if (k == 0) {
			b = rx_lc(DMR_BURST_VOICE_LC_HEADER, LC2);
		} else if (k < 13) {
			b = rx_voice((uint8_t)((k - 1) % 6), (uint8_t)(0x60 + k), LC2);
		} else {
			b = rx_lc(DMR_BURST_TERMINATOR_LC, LC2);
		}
		rx_at(&b, s2);
		if (k == 0) {
			b = rx_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
		} else if (k < 13) {
			b = rx_voice((uint8_t)((k - 1) % 6), (uint8_t)(0x40 + k), LC1);
		} else {
			b = rx_lc(DMR_BURST_TERMINATOR_LC, LC1);
		}
		rx_at(&b, s1);
	}
	run_ms(20);
	// Each slot's call went up whole on its own command: header, 12 voice in order, terminator
	if (getenv("SPLIT_DEBUG") != NULL) {
		for (int i = from; i < out_count; i++) {
			printf("%02X %02X %02X %02X\n", out[i].b[1], out[i].b[2], out[i].b[3], out[i].b[4]);
		}
	}
	for (int k = 0; k < 2; k++) {
		uint8_t cmd = (k == 0) ? MMDVM_DMR_DATA1 : MMDVM_DMR_DATA2;
		const uint8_t *want_lc = (k == 0) ? LC1 : LC2;
		int hdr = 0, voices = 0, terms = 0;
		for (int i = from; i < out_count; i++) {
			const uint8_t *o = out[i].b;
			if (o[2] != cmd) {
				continue;
			}
			if (o[3] == (MMDVM_DMR_SYNC_DATA | DT_VOICE_LC_HEADER)) {
				uint8_t info[12], lc[9];
				dmrFecBptcDecode(o + 4, info);
				CHECK(dmr_lc_decode_full(info, DMR_RS_MASK_VOICE_HEADER, lc) >= 0 && memcmp(lc, want_lc, 9) == 0);
				hdr++;
			} else if (o[3] == (MMDVM_DMR_SYNC_DATA | DT_TERMINATOR_WITH_LC)) {
				terms++;
			} else if ((o[3] & MMDVM_DMR_SYNC_DATA) == 0) {
				uint8_t idx = (o[3] & MMDVM_DMR_SYNC_AUDIO) ? 0 : (o[3] & 0x0F);
				CHECK(idx == voices % 6);
				CHECK(o[4] == (uint8_t)(((k == 0) ? 0x40 : 0x60) + voices + 1));
				CHECK(out[i].len == 3 + 34 + 2);	// RSSI bytes
				voices++;
			}
		}
		CHECK(hdr == 1 && voices == 12 && terms == 1);
	}
	CHECK(count_out(MMDVM_DMR_LOST1, from) == 0 && count_out(MMDVM_DMR_LOST2, from) == 0);
	hotspot_split_view_t v;
	hotspot_split_get_view(&v);
	CHECK(v.labeled[0] == 14 && v.labeled[1] == 14);
	CHECK(v.track_us > 1000 && v.track_us <= 3100);	// the 3 ms chip delay being followed
	CHECK(split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0);
	const uint8_t *r = split_reply(6);
	CHECK(rd16(r + 21) == 14 && rd16(r + 23) == 14 && rd16(r + 25) == 1 && rd16(r + 29) <= 2);

	// A call that loses its end: DMR_LOST on its own slot only; inbound busy and carrier detect meanwhile
	from = out_count;
	base = E + 3000000u;
	b = rx_lc(DMR_BURST_VOICE_LC_HEADER, LC1);
	rx_at(&b, base + 30000u + 3000u);
	run_ms(10);
	CHECK(split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0 && split_reply(6)[7] == 1);
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK(last_out(MMDVM_GET_STATUS)[5] & MMDVM_STATUS_CD);
	run_ms(600);
	CHECK(count_out(MMDVM_DMR_LOST1, from) == 1 && count_out(MMDVM_DMR_LOST2, from) == 0);
	CHECK(split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0 && split_reply(6)[7] == 0);

	// Data on TS2: up as DMR_DATA2, busy 360 ms
	from = out_count;
	b = rx_lc(DMR_BURST_CSBK, LC1);
	b.lc_valid = false;
	memset(b.payload, 0x77, 12);
	rx_at(&b, base + 1200000u + 3000u);
	run_ms(10);
	CHECK(count_out(MMDVM_DMR_DATA2, from) == 1);
	CHECK(split_cmd(SPLIT_SUB_STATUS, NULL, 0) == 0 && split_reply(6)[7] == 2);

	// SET_ROLE 0: back to normal, the epoch forgotten
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 0 }, 1) == 0);
	hotspot_split_get_view(&v);
	CHECK(v.role == 0 && !(v.flags & SPLIT_FLAG_EPOCH));
}

// The extension and the SFR: a role keeps the SFR off and the SFR trigger out; the SFR comes back after the role
static void test_space_when_role_ends(void)
{
	session(&ssim_modem);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1 }, 1) == 0);
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK(last_out(MMDVM_GET_STATUS)[7] == 14);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 0 }, 1) == 0);
	host(MMDVM_GET_STATUS, NULL, 0);
	CHECK(last_out(MMDVM_GET_STATUS)[7] == 0 && !ssim.bs);
}

// The debug link's SPLIT command (debuglink_split.c): the experiment's variant reaches bs_start, the view text
static void test_debuglink(void)
{
	uint8_t dl[DL_REPLY_MAX];
	size_t n;

	session(&ssim_modem);
	CHECK(debuglink_cmd_split((const uint8_t[]){ 1, 255, 1, 0x0D, 0x5A }, 5, dl, sizeof(dl)) == 1 &&
	      dl[0] == DL_ST_OK);
	CHECK(debuglink_cmd_split((const uint8_t[]){ 1, 255, 5 }, 3, dl, sizeof(dl)) == 1 && dl[0] == DL_ST_BAD_ARG);
	CHECK(debuglink_cmd_split((const uint8_t[]){ 1, 2, 1, 0x0D }, 4, dl, sizeof(dl)) == 1 && dl[0] == DL_ST_BAD_ARG);
	CHECK(debuglink_cmd_split((const uint8_t[]){ 9 }, 1, dl, sizeof(dl)) == 1 && dl[0] == DL_ST_BAD_ARG);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 1 }, 1) == 0);
	CHECK(ssim.bs_cfg.variant == 255 && ssim.bs_cfg.nregs == 1 && ssim.bs_cfg.regs[0][0] == 0x0D &&
	      ssim.bs_cfg.regs[0][1] == 0x5A);
	n = debuglink_cmd_split((const uint8_t[]){ 2 }, 1, dl, sizeof(dl));
	CHECK(n > 100 && dl[0] == DL_ST_OK);
	dl[n < sizeof(dl) ? n : sizeof(dl) - 1] = 0;
	const char *text = (const char *)dl + 1;
	CHECK(strstr(text, "split.built=1\n") != NULL && strstr(text, "split.role=tx\n") != NULL);
	CHECK(strstr(text, "split.variant=255\n") != NULL && strstr(text, "split.cach_method=ours\n") != NULL);
	CHECK(strstr(text, "split.underruns1=") != NULL && strstr(text, "split.offset_us=") != NULL &&
	      strstr(text, "split.epoch_us=") != NULL && strstr(text, "split.buf2=") != NULL);
	CHECK(split_cmd(SPLIT_SUB_SET_ROLE, (const uint8_t[]){ 0 }, 1) == 0);
	CHECK(debuglink_cmd_split((const uint8_t[]){ 1, 0, 0 }, 3, dl, sizeof(dl)) == 1 && dl[0] == DL_ST_OK);
}

int main(void)
{
	test_tact();
	test_hamming1712();
	test_interleave();
	test_short_lc();
	test_idle_info();
	test_outbound();
	test_epoch();
	test_normal();
	test_tx_half();
	test_tx_guard();
	test_rx_half();
	test_space_when_role_ends();
	test_debuglink();
	printf("# splitDuplexTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
