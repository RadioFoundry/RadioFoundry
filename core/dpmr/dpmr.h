// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR Mode 1 voice (docs/spec/dpmr.md): the frames and their fields, the framer's table for the mode, and a voice
// call on top of them. Section numbers are ETSI TS 102 490 V1.7.1's (the licence-exempt dPMR446 air interface).
//
// The physical layer is NXDN 4800's: 4FSK at 2400 symbols/s, +-1050 / +-350 Hz, root raised cosine 0.2 (clause 4.1,
// 12.2), so dPMR rides the shared air interface (core/air) as a profile and nothing below the framer knows it. A
// voice transmission (4.2.3):
//
//   Header frame, 192 symbols: preamble (36 or more) | FS1 (24) | HI0 (60) | CC (12) | HI1 (60). HI0 and HI1 carry
//     the same coded header information. A radio may send several (extended wake-up headers count down in CI, 5.10.1).
//   Superframes of four payload frames, 192 symbols each: FS2 or CC (12) | CCH (36) | four TCH (36 each). Frames 1 and
//     3 open with FS2, frames 2 and 4 with the channel code. A call is a whole number of superframes.
//   End frame, 48 symbols: FS3 (12) | END (36).
//
// The framer cuts a header (from FS1), a pair of payload frames (from FS2: 1-2 or 3-4) or an end frame as one unit,
// so every unit opens with a sync pattern the profile names; the channel code (which only the channel knows) is read
// by the mode inside the pair. A pair is handed over in two pieces, one payload frame each. The preamble of a header
// that follows another is a unit of its own (dpmr_rx.c).
//
// Buffers here hold symbols as on air: packed dibits (hal_air.h), which are also the frame's bits in air order,
// most significant first. Nothing in a frame is scrambled outside its coded blocks (dpmr_fec.h). No RTOS or board
// call: the host tests run all of it.
#ifndef RFOUNDRY_DPMR_H
#define RFOUNDRY_DPMR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../air/air_framer.h"
#include "dpmr_fec.h"

// ── Frames (4.2.3, 6.1) ──
#define DPMR_FRAME_SYMS		192u
#define DPMR_FRAME_BYTES	(DPMR_FRAME_SYMS / 4u)
#define DPMR_PREAMBLE_SYMS	36u		// the least a header carries (6.1.6), and what this side sends
#define DPMR_FS1_SYMS		24u
#define DPMR_SYNC_SYMS		12u		// FS2, FS3 and the channel code field
#define DPMR_HI_SYMS		60u
// A header from its FS1: FS1, HI0, CC, HI1
#define DPMR_HDR_HI0_SYM	24u
#define DPMR_HDR_CC_SYM		84u
#define DPMR_HDR_HI1_SYM	96u
#define DPMR_HDR_SYMS		156u
// A payload frame: sync or CC, CCH, four TCH
#define DPMR_CCH_SYM		12u
#define DPMR_TCH_SYM(n)		(48u + 36u * (n))
#define DPMR_PAIR_SYMS		(2u * DPMR_FRAME_SYMS)
#define DPMR_END_SYMS		48u
#define DPMR_HEAD_SYMS		48u		// a pair's sync and CCH: what tells a pair from a channel code

// Frame syncs (6.1), most significant bit first
#define DPMR_FS1		0x57FF5F75D577ULL	// a header frame (48 bits)
#define DPMR_FS4		0xFD55F5DF7FDDULL	// a packet data header: FS1 with every symbol's sign turned
#define DPMR_FS2		0x5FF77Du		// payload frames 1 and 3 (24 bits)
#define DPMR_FS3		0x7DDFF5u		// an end frame (24 bits)
#define DPMR_PREAMBLE_BYTE	0x5Fu			// 6.1.6: +3 +3 -3 -3 repeated
// The profile's sync ids (air_chunk_t.sync_id, air_frame_t.sync_id)
enum { DPMR_SYNC_FS2, DPMR_SYNC_FS3, DPMR_SYNC_FS1 };

// The voice frame: an AMBE+2 air frame with its FEC, the very 72 bits DMR carries (core/dmr/ambe/ambeFec.h)
#define DPMR_AMBE_BYTES		9u
#define DPMR_TCH_FRAMES		4u	// per payload frame

// Header type (5.11), communications mode (5.7) and format (5.8): the values a voice call uses
#define DPMR_HT_COMM_START	0u	// a superframe follows
#define DPMR_HT_CONNECT		1u	// a connection request: an end frame follows
#define DPMR_M_VOICE		0u
#define DPMR_M_VOICE_SLD	1u	// voice and slow data
#define DPMR_F_ALL		0u	// a call to all
#define DPMR_F_P2P		1u	// peer to peer, what a voice call normally is
#define DPMR_CI_EXT_WAKEUP	0x700u	// CI type 111: an extended wake-up header, the headers still to come below it

// Identities (8.1.1): an initial-services radio addresses with an 8-bit common ID followed by sixteen ones. 255 calls
// everyone, 0 is reserved
#define DPMR_ID_COMMON(id)	(((uint32_t)(id) << 16) | 0xFFFFu)
#define DPMR_COMMON_ALL		255u
#define DPMR_ID_ALL		DPMR_ID_COMMON(DPMR_COMMON_ALL)
static inline bool dpmr_id_is_common(uint32_t id)
{
	return (id & 0xFFFFu) == 0xFFFFu;
}

// ── Channel codes (6.1.5, 7.1): twelve bits sent one per symbol, 0 as +3 (01) and 1 as -3 (11) ──
// The 24 bits on air for a code, and the code read back from them with the number of symbols that were neither
uint32_t dpmr_cc_air(uint16_t cc);
uint16_t dpmr_cc_from_air(uint32_t air, uint8_t *bad);
// Table 6.1's codes: group A (initial services) and B (configured services), 16 each, for the RF channels
// 446.103125 + 6.25 kHz n, n = 0-15
uint16_t dpmr_cc_group(bool group_b, uint8_t n);
// Where cc stands in table 6.1: 1-16 group A, 17-32 group B, 0 any other code
uint8_t dpmr_cc_index(uint16_t cc);
// The group A code the standard ties to the dPMR446 channel nearest hz, else group A's first: a channel outside
// 446.1-446.2 MHz (an amateur band, the newer PMR446 channels) has no code of its own, and both ends only need to agree
uint16_t dpmr_cc_for_hz(uint32_t hz);

// An ID as the screens show it: "All", "CID n" (a common ID), else the 24-bit number in decimal; a channel code as
// table 6.1's group and place ("A1"-"A16", "B1"-"B16"), else its 12 bits in hex
void dpmr_id_text(uint32_t id, char *out, size_t n);
void dpmr_cc_text(uint16_t cc, char *out, size_t n);

// ── Information fields ──
// The header information (5.2, 9.5)
typedef struct {
	uint8_t ht;		// header type
	uint32_t called;	// 24 bits
	uint32_t own;		// 24 bits
	uint8_t mode;		// M, 3 bits
	uint8_t version;	// V, 2 bits (0: standard content)
	uint8_t format;		// F, 2 bits
	uint16_t ci;		// call information, 11 bits (CI type << 8 | CI information)
} dpmr_hi_t;

void dpmr_hi_pack(const dpmr_hi_t *h, uint8_t data[DPMR_HI_BYTES]);	// the 72 field bits; the CRC is dpmr_hi_encode's
void dpmr_hi_unpack(const uint8_t data[DPMR_HI_BYTES], dpmr_hi_t *h);

// The control channel of one payload frame (5.1, 9.1)
typedef struct {
	uint8_t fn;		// frame number 0-3
	uint16_t id12;		// called ID upper / lower 12 bits (fn 0 / 1), own ID upper / lower (fn 2 / 3)
	uint8_t mode, version, format;
	uint32_t sld;		// 18 bits of slow data
} dpmr_cch_t;

void dpmr_cch_pack(const dpmr_cch_t *c, uint8_t data[DPMR_CCH_BYTES]);
void dpmr_cch_unpack(const uint8_t data[DPMR_CCH_BYTES], dpmr_cch_t *c);
// The ID quarter a CCH of frame fn carries for header h (9.1)
uint16_t dpmr_id_quarter(const dpmr_hi_t *h, uint8_t fn);

// The end frame's information (5.3, 9.6)
typedef struct {
	uint8_t et, arq, wait, status;
} dpmr_end_info_t;

void dpmr_end_pack(const dpmr_end_info_t *e, uint8_t data[DPMR_END_BYTES]);
void dpmr_end_unpack(const uint8_t data[DPMR_END_BYTES], dpmr_end_info_t *e);

// ── Frames as symbols. Each writer fills its part of a frame buffer; each reader decodes it, res as dpmr_fec.h's ──
// A header frame, preamble included (DPMR_FRAME_SYMS)
void dpmr_header_put(uint8_t *frame, const dpmr_hi_t *h, uint16_t cc);
// A header from its FS1 (hdr: DPMR_HDR_SYMS): HI0, else HI1. False when neither copy's CRC holds
bool dpmr_header_get(const uint8_t *hdr, dpmr_hi_t *h, dpmr_block_result_t *res);
// A payload frame's sync or channel code field (by c->fn) and its CCH; its TCH are dpmr_tch_put's
void dpmr_payload_put(uint8_t *frame, const dpmr_cch_t *c, uint16_t cc);
bool dpmr_cch_get(const uint8_t *frame, dpmr_cch_t *c, dpmr_block_result_t *res);
// The 72 bits of TCH n (0-3) as on air: an AMBE+2 frame with its FEC
void dpmr_tch_put(uint8_t *frame, uint8_t n, const uint8_t ambe[DPMR_AMBE_BYTES]);
void dpmr_tch_get(const uint8_t *frame, uint8_t n, uint8_t ambe[DPMR_AMBE_BYTES]);
// An end frame (DPMR_END_SYMS)
void dpmr_end_put(uint8_t *frame, const dpmr_end_info_t *e);
bool dpmr_end_get(const uint8_t *frame, dpmr_end_info_t *e, dpmr_block_result_t *res);
// A payload frame's first 24 bits (its sync field)
uint32_t dpmr_sync_field(const uint8_t *frame);

// ── Receive: the framer's table and its hooks ──
enum { DPMR_UNIT_HEADER, DPMR_UNIT_PAYLOAD, DPMR_UNIT_END, DPMR_UNIT_PREAMBLE };

// One unit (a payload frame of a pair, a header, an end frame) as the head hook and rx_frame read it
typedef struct {
	uint8_t kind;		// DPMR_UNIT_xxx
	uint8_t flags;		// AIR_FRAME_xxx (NO_SYNC: placed by counting)
	bool second;		// a payload frame: the pair's second (frame 2 or 4, its sync field is the channel code)
	bool ok;		// its block's CRC holds (HI, CCH, END)
	dpmr_block_result_t res;
	union {
		dpmr_hi_t hi;
		dpmr_cch_t cch;
		dpmr_end_info_t end;
	} u;
	uint16_t cc;		// the header's or the second frame's channel code, read symbol by symbol ...
	uint8_t cc_bad;		// ... and its symbols that were not outer ones
	uint32_t t_us;
	int16_t rssi_dbm;
	const uint8_t *hard;	// the frame as on air (a header from FS1)
} dpmr_rx_unit_t;

typedef struct {
	void (*unit)(const dpmr_rx_unit_t *u, void *ctx);
	void (*lost)(void *ctx);
	void *ctx;
	bool locked;		// the head hook took a unit since the last lost or rejection
	uint8_t kind;		// the unit in hand
	uint8_t last;		// the unit before
	uint8_t coasted;	// pairs in a row taken without a sync or a readable CCH
	dpmr_rx_unit_t head;	// the first payload frame of the pair in hand, read by the head hook
	uint32_t rejects;	// syncs the head hook turned down
} dpmr_rx_t;

extern const air_mode_t dpmr_air_mode;

void dpmr_rx_init(dpmr_rx_t *rx, void (*unit)(const dpmr_rx_unit_t *u, void *ctx), void (*lost)(void *ctx), void *ctx);

// ── A call ──
#define DPMR_HAVE_CALLED	0x01u	// from the header, or both its quarters from the CCH
#define DPMR_HAVE_OWN		0x02u
#define DPMR_HAVE_CC		0x04u
#define DPMR_HAVE_HEADER	0x08u	// a header opened it (else a late entry)

typedef struct {
	uint32_t called, own;	// 24 bits, valid by have
	uint16_t cc;
	uint8_t have;		// DPMR_HAVE_xxx
	uint8_t quarters;	// the CCH's ID quarters seen: bit fn
	uint8_t mode, format;	// the latest header or CCH's
	bool ended;		// an end frame closed it (else the signal went)
	int16_t rssi_dbm;
} dpmr_call_t;

typedef struct {
	void (*start)(const dpmr_call_t *c, void *ctx);
	void (*update)(const dpmr_call_t *c, void *ctx);	// what is known grew
	void (*voice)(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx);
	void (*end)(const dpmr_call_t *c, void *ctx);
} dpmr_call_sink_t;

typedef struct {
	uint32_t calls, headers, frames, ends;
	uint32_t hi_fail, cch_fail, end_fail;
	uint32_t voice;		// TCH handed on
	uint32_t voice_held;	// TCH of no call (a pair before a readable CCH), or of a data mode: not handed on
	uint32_t ham_fixed;	// Hamming words the HI, CCH and END decoders corrected ...
	uint32_t ham_bad;	// ... and found past correcting
} dpmr_call_stats_t;

typedef struct {
	dpmr_rx_t rx;			// first: the framer's context is the call's
	const dpmr_call_sink_t *sink;
	void *ctx;
	bool active;
	dpmr_call_t call;
	// A header heard ahead of the voice: the call starts at its first payload frame with these fields
	bool pending;
	dpmr_hi_t hdr;
	uint16_t hdr_cc;
	uint32_t hdr_us;
	dpmr_call_stats_t stats;
} dpmr_call_rx_t;

void dpmr_call_rx_init(dpmr_call_rx_t *c, const dpmr_call_sink_t *sink, void *ctx);

// Transmit: a call's frames, built one at a time and released as they fill. ready: symbols from `first` that can go
// now; sent: n of them went
typedef struct {
	uint8_t state;
	bool ending;
	uint8_t headers, headers_sent;
	uint8_t fn;			// the payload frame in hand, 0-3
	uint8_t nvoice;			// TCH filled in it
	uint16_t nsyms, ready, sent;
	dpmr_hi_t hi;
	uint16_t cc;
	const uint8_t *silence;		// from dpmr_call_tx_end: what fills the rest of the superframe
	uint8_t frame[DPMR_FRAME_BYTES];
} dpmr_call_tx_t;

// headers: header frames ahead of the voice (at least one; all but the last are extended wake-up headers)
void dpmr_call_tx_begin(dpmr_call_tx_t *t, const dpmr_hi_t *hi, uint16_t cc, uint8_t headers);
uint16_t dpmr_call_tx_ready(const dpmr_call_tx_t *t, uint16_t *first);
const uint8_t *dpmr_call_tx_syms(const dpmr_call_tx_t *t);
void dpmr_call_tx_sent(dpmr_call_tx_t *t, uint16_t n);
bool dpmr_call_tx_wants_voice(const dpmr_call_tx_t *t);
bool dpmr_call_tx_voice(dpmr_call_tx_t *t, const uint8_t ambe[DPMR_AMBE_BYTES]);
// The operator let go: the superframe in hand is finished with silence (a call is whole superframes), then the end
// frame
void dpmr_call_tx_end(dpmr_call_tx_t *t, const uint8_t silence[DPMR_AMBE_BYTES]);
bool dpmr_call_tx_done(const dpmr_call_tx_t *t);

#endif
