// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 trunking signaling, outbound, receive side (docs/spec/p25-trunking.md): the trunking signaling data unit
// (TSDU) and its blocks (TSBK), the header block of a multi-block trunking packet, the messages a listener needs, and
// channel numbers to frequencies. Clause numbers are TIA-102.BAAA-A's for the units and their codes; the messages are
// TIA-102.AABC's. Build switch CONFIG_P25_TRUNK. No RTOS or board call.
#ifndef RFOUNDRY_P25_TSBK_H
#define RFOUNDRY_P25_TSBK_H

#include <stdbool.h>
#include <stdint.h>

#include "p25.h"

#define P25_DUID_TSDU		0x7

// A block: 12 octets on air, the 80 bits of a message and a CRC-CCITT, sent as 196 bits of the rate 1/2 trellis
#define P25_TSBK_BYTES		12
#define P25_TSDU_BLOCKS_MAX	3
#define P25_PDU_BLOCKS_MAX	8	// a packet the unit buffer holds: the header and up to 7 more
// A unit of n blocks in symbols, status symbols included: the frame sync, NID and blocks, filled up with null bits to
// the next status symbol (one block 180, two 288, three 360). The symbols it takes to hold block k (from 0)
uint16_t p25_blocks_syms(uint8_t n);
uint16_t p25_block_end_sym(uint8_t k);

// Block k of a TSDU or a packet: trellis decoded into blk. Returns the code bits corrected (0 clean) when the CRC
// holds, -1 when it fails (blk is written either way: its last block flag is the best guess there is)
int p25_block_get(const uint8_t *unit, uint8_t k, uint8_t blk[P25_TSBK_BYTES]);
// Block k trellis decoded into blk without the block CRC check (a packet's data block): the code bits corrected
int p25_block_decode(const uint8_t *unit, uint8_t k, uint8_t blk[P25_TSBK_BYTES]);
// blk (its CRC set) to block k of a unit begun with p25_unit_begin
void p25_block_put(uint8_t *unit, uint8_t k, const uint8_t blk[P25_TSBK_BYTES]);
// The CRC of a block's first ten octets; p25_tsbk_seal sets it in octets 10 and 11
uint16_t p25_tsbk_crc(const uint8_t blk[P25_TSBK_BYTES]);
void p25_tsbk_seal(uint8_t blk[P25_TSBK_BYTES]);

// ── Messages (outbound trunking signaling blocks) ──
// Octet 0: last block (bit 7), protected (bit 6), opcode (bits 5-0); octet 1: manufacturer's ID; octets 2-9 the
// arguments
#define P25_TSBK_LB		0x80
#define P25_TSBK_P		0x40
#define P25_OP_GRP_V_GRANT	0x00
#define P25_OP_GRP_V_UPDT	0x02
#define P25_OP_GRP_V_UPDT_EXP	0x03
#define P25_OP_UU_V_GRANT	0x04
#define P25_OP_UU_V_UPDT	0x06
#define P25_OP_QUE_RSP		0x21
#define P25_OP_DENY_RSP		0x27
#define P25_OP_GRP_AFF_RSP	0x28
#define P25_OP_U_REG_RSP	0x2C
#define P25_OP_SCCB_EXP		0x29
#define P25_OP_IDEN_UP_TDMA	0x33
#define P25_OP_IDEN_UP_VU	0x34
#define P25_OP_SYS_SRV_BCST	0x38
#define P25_OP_SCCB		0x39
#define P25_OP_RFSS_STS_BCST	0x3A
#define P25_OP_NET_STS_BCST	0x3B
#define P25_OP_ADJ_STS_BCST	0x3C
#define P25_OP_IDEN_UP		0x3D
#define P25_OPCODES		64

// Service options of a grant
#define P25_SVC_EMERGENCY	0x80
#define P25_SVC_PROTECTED	0x40	// encrypted

// What a parse made of a block
enum {
	P25_TSBK_OTHER,		// a standard opcode not decoded here: counted and skipped
	P25_TSBK_MFR,		// a manufacturer's own block: skipped
	P25_TSBK_GRANT,		// a voice channel grant or update: grant
	P25_TSBK_IDEN,		// a channel identifier update: iden
	P25_TSBK_NET,		// network status: net (wacn, sysid, ch, ssc)
	P25_TSBK_RFSS,		// RFSS status: site (sysid, rfss, site, ch, ssc)
	P25_TSBK_ADJ,		// an adjacent site: site
	P25_TSBK_SCCB,		// secondary control channels: sccb
	P25_TSBK_SYS_SRV,	// system services: svc
	P25_TSBK_REG_RSP,	// unit registration response: reg
	P25_TSBK_AFF_RSP,	// group affiliation response: aff
	P25_TSBK_DENY,		// deny response: rsp
	P25_TSBK_QUEUED,	// queued response: rsp
};

// The answer values of a registration and an affiliation response
enum { P25_RV_ACCEPT, P25_RV_FAIL, P25_RV_DENY, P25_RV_REFUSED };

// A voice grant, any of its forms. Group: tg; unit to unit: dst. src 0 where the form has none. The update form names
// two grants: ch2 / tg2 (ch2 0: none)
typedef struct {
	bool unit;
	bool has_svc;		// svc is from the air (the update forms carry none)
	uint8_t svc;
	uint16_t ch, ch2;	// the channel the listener tunes to (the explicit form's transmit channel of the site)
	uint16_t tg, tg2;
	uint32_t dst, src;
} p25_grant_t;

// A channel identifier, as the air gives it
typedef struct {
	uint8_t id;		// 0-15
	uint8_t form;		// the opcode it came in
	bool tdma;		// the TDMA form with a channel type that is not single-slot full rate
	uint8_t slots;		// channels per carrier: 1, or the TDMA type's
	uint8_t type;		// the TDMA form's channel type (0-15), else 0
	uint32_t base;		// x 5 Hz
	uint16_t spacing;	// x 125 Hz
	int16_t tx_off;		// x 250 kHz, signed (the VHF/UHF form's unit is unverified: kept as sent)
	uint16_t bw_khz8;	// bandwidth x 125 Hz, 0 unknown
} p25_iden_air_t;

typedef struct {
	uint8_t opcode, mfid, kind;	// kind: P25_TSBK_*
	bool last, prot;
	union {
		p25_grant_t grant;
		p25_iden_air_t iden;
		struct {
			uint8_t lra;
			uint32_t wacn;
			uint16_t sysid;
			uint16_t ch;		// the primary control channel (the extended form's channel (T))
			uint8_t ssc;
		} net;
		struct {
			uint8_t lra, flags;	// the 4 bits ahead of the system ID
			uint16_t sysid;
			uint8_t rfss, site;
			uint16_t ch;
			uint8_t ssc;
		} site;
		struct {
			uint8_t rfss, site;
			uint16_t ch[2];		// 0: none
			uint8_t ssc[2];
		} sccb;
		struct {
			uint32_t avail, supp;
			uint8_t prio;
		} svc;
		struct {
			uint8_t rv;		// P25_RV_*
			uint16_t sysid;
			uint32_t id, addr;	// the unit that asked, and the address the system gives it
		} reg;
		struct {
			bool global;
			uint8_t gav;		// P25_RV_*
			uint16_t ann, tg;	// the announcement group, the group affiliated
			uint32_t dst;
		} aff;
		struct {
			bool aiv, ex;		// additional information valid, extended addressing
			uint8_t svc;		// the opcode of the request answered
			uint8_t reason;
			uint32_t info, dst;
		} rsp;
	} u;
} p25_tsbk_t;

// A block's 10 message octets into m; false for a message not decoded here (m->kind says what it is)
bool p25_tsbk_parse(const uint8_t blk[P25_TSBK_BYTES], p25_tsbk_t *m);
// m to a sealed block (the bench tool and the tests: the radio sends only inbound blocks). kind picks the form: a grant
// by m->opcode (one of the four grant opcodes), an identifier by m->u.iden.form
void p25_tsbk_make(const p25_tsbk_t *m, uint8_t blk[P25_TSBK_BYTES]);

// ── Inbound signaling: the radio's blocks on the control channel's inbound frequency ──
// Inbound opcodes share numbers with outbound ones of another meaning: they have their own parse
#define P25_ISP_GRP_V_REQ	0x00	// group voice service request
#define P25_ISP_GRP_AFF_REQ	0x28	// group affiliation request
#define P25_ISP_U_REG_REQ	0x2C	// unit registration request
typedef struct {
	uint8_t opcode, mfid;
	bool known;		// a layout decoded here (the three above, MFID 0)
	uint8_t svc;		// voice request: service options
	uint32_t wacn;		// registration request
	uint16_t sysid;		// registration and affiliation requests
	uint16_t tg;		// voice and affiliation requests
	uint32_t src;
} p25_isp_t;
// m to a sealed last block; a block back to m (false: not one of the three, or another MFID)
void p25_isp_make(const p25_isp_t *m, uint8_t blk[P25_TSBK_BYTES]);
bool p25_isp_parse(const uint8_t blk[P25_TSBK_BYTES], p25_isp_t *m);
// The one-block inbound TSDU of blk in unit (status symbols "unknown", as a subscriber sends): its length in symbols
uint16_t p25_isp_unit(uint8_t *unit, uint16_t nac, const uint8_t blk[P25_TSBK_BYTES]);

// The header block of a multi-block trunking packet (a PDU of the alternate or unconfirmed trunking format). False:
// another packet. The data blocks are not decoded
#define P25_PDU_FMT_UMBTC	0x15
#define P25_PDU_FMT_AMBTC	0x17
typedef struct {
	uint8_t format, sap, mfid, blocks, opcode;
} p25_mbt_t;
bool p25_mbt_parse(const uint8_t hdr[P25_TSBK_BYTES], p25_mbt_t *h);
void p25_mbt_make(const p25_mbt_t *h, uint8_t hdr[P25_TSBK_BYTES]);
// The packet CRC of an unconfirmed packet's n data octets (TIA-102.BAAA-A 6.3.3), sent in its last four octets
uint32_t p25_packet_crc(const uint8_t *b, unsigned n);
// The network status broadcast, extended format (TIA-102.AABC-D 6.2.11.2): a header (its CRC already checked) and
// its one data block to m (kind P25_TSBK_NET). False: another packet, or the packet CRC failed
bool p25_mbt_net_parse(const uint8_t hdr[P25_TSBK_BYTES], const uint8_t data[P25_TSBK_BYTES], p25_tsbk_t *m);
void p25_mbt_net_make(const p25_tsbk_t *m, uint8_t hdr[P25_TSBK_BYTES], uint8_t data[P25_TSBK_BYTES]);

// A channel number (identifier in bits 15-12, channel in 11-0) to a receive frequency in Hz, from an identifier's
// base (Hz) and spacing (x 5 Hz) and its channels per carrier. 0 when it is past 32 bits
uint32_t p25_chan_hz(uint16_t ch, uint32_t base_hz, uint16_t spacing5, uint8_t slots);

#endif
