// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 Phase 1 (TIA-102.BAAA-A, the FDMA common air interface) at the frame level: the data units of a conventional
// voice call as they are on air, their fields and their FEC. docs/spec/p25.md.
//
// A unit here is what the framer hands over and takes back (core/air): packed dibits in air order from the frame
// sync on, status symbols included. No RTOS or board call in this directory.
#ifndef RFOUNDRY_P25_H
#define RFOUNDRY_P25_H

#include <stdbool.h>
#include <stdint.h>

#include "../config.h"
#include "../air/air_framer.h"

// Data unit IDs (table 8-4)
#define P25_DUID_HDU		0x0
#define P25_DUID_TDU		0x3	// terminator without link control
#define P25_DUID_LDU1		0x5
#define P25_DUID_LDU2		0xA
#define P25_DUID_PDU		0xC
#define P25_DUID_TDULC		0xF	// terminator with link control

// Unit lengths in symbols, status symbols included (8.2)
#define P25_HDU_SYMS		396
#define P25_LDU_SYMS		864
#define P25_TDU_SYMS		72
#define P25_TDULC_SYMS		216
#define P25_UNIT_SYMS_MAX	P25_LDU_SYMS
#define P25_UNIT_BYTES_MAX	(P25_UNIT_SYMS_MAX / 4)
#define P25_HEAD_SYMS		57	// frame sync and NID, with the status symbol inside the NID

#define P25_NAC_DEFAULT		0x293
#define P25_NAC_ANY		0xF7E	// a receiver set to it takes every NAC
#define P25_NAC_REPEAT_ANY	0xF7F	// a repeater set to it takes every NAC and sends it on as received

// A system whose WACN is in BEE00..BEEFF, or whose system ID is 00A, gets no transmission from the radio, of any
// kind, however it is heard or programmed
#define P25_WACN_NO_TX		0xBEE00u
#define P25_WACN_NO_TX_MASK	0xFFF00u
#define P25_SYSID_NO_TX		0x00Au

static inline bool p25_wacn_no_tx(uint32_t wacn) { return (wacn & P25_WACN_NO_TX_MASK) == P25_WACN_NO_TX; }
static inline bool p25_sysid_no_tx(uint16_t sysid) { return sysid == P25_SYSID_NO_TX; }

// Status symbols (table 8-2). A subscriber sends only P25_SS_TALKAROUND or P25_SS_UNKNOWN
#define P25_SS_BUSY		1	// 01: the inbound channel is busy (a repeater says)
#define P25_SS_TALKAROUND	0	// 00
#define P25_SS_UNKNOWN		2	// 10
#define P25_SS_IDLE		3	// 11: the inbound channel is idle (a repeater says)

#define P25_VOICE_FRAMES	9	// per LDU
#define P25_IMBE_BYTES		11	// 88 bits: u0..u3 (12 bits each), u4..u6 (11 each), u7 (7), first bit first

#define P25_LC_BYTES		9
#define P25_ES_BYTES		12	// MI 9, ALGID 1, KID 2
#define P25_HDR_BYTES		15	// MI 9, MFID 1, ALGID 1, KID 2, TGID 2
#define P25_ALGID_CLEAR		0x80

// The unit's length in symbols for a data unit ID; 0 for one whose length is not fixed (a packet) or not known
uint16_t p25_unit_syms(uint8_t duid);

// Information bits: bit i of the unit counted without the status symbols (bit 0 is the frame sync's first)
uint32_t p25_bits_get(const uint8_t *unit, uint16_t i, uint8_t n);
void p25_bits_put(uint8_t *unit, uint16_t i, uint8_t n, uint32_t v);

// Start a unit of nsyms symbols: all nulls, the frame sync, the NID, every status symbol set to ss
void p25_unit_begin(uint8_t *unit, uint16_t nsyms, uint16_t nac, uint8_t duid, uint8_t ss);
// The NID of a unit (its first P25_HEAD_SYMS symbols are enough). Returns the bits corrected, -1 = undecodable
int p25_nid_get(const uint8_t *unit, uint16_t *nac, uint8_t *duid);
// Status symbol k of a unit (the symbol after information bits 70 k + 69)
uint8_t p25_status_get(const uint8_t *unit, uint16_t k);

// Link control in an LDU1 (24 Hamming words between the voice frames), the encryption sync in an LDU2, the low
// speed data of either (two octets), the header word of an HDU and the link control of a TDULC. The getters return
// the Reed-Solomon symbols corrected, -1 = undecodable (out then untouched)
int p25_ldu_lc_get(const uint8_t *unit, uint8_t lc[P25_LC_BYTES]);
void p25_ldu_lc_put(uint8_t *unit, const uint8_t lc[P25_LC_BYTES]);
int p25_ldu_es_get(const uint8_t *unit, uint8_t es[P25_ES_BYTES]);
void p25_ldu_es_put(uint8_t *unit, const uint8_t es[P25_ES_BYTES]);
int p25_ldu_lsd_get(const uint8_t *unit, uint8_t lsd[2]);
void p25_ldu_lsd_put(uint8_t *unit, const uint8_t lsd[2]);
int p25_hdu_get(const uint8_t *unit, uint8_t hdr[P25_HDR_BYTES]);
void p25_hdu_put(uint8_t *unit, const uint8_t hdr[P25_HDR_BYTES]);
int p25_tdulc_get(const uint8_t *unit, uint8_t lc[P25_LC_BYTES]);
void p25_tdulc_put(uint8_t *unit, const uint8_t lc[P25_LC_BYTES]);

// Voice frame n (0..8) of an LDU: the 144-bit voice code word to and from the 88 IMBE bits (5.3). The getter
// returns what the codes corrected, packed (P25_VOICE_ERRS_*): they never refuse, and P25_VOICE_ERRS_BAD says when
// the frame is not to be trusted
#define P25_VOICE_ERRS(total, c0, c4)	((total) | (c0) << 4 | (c4) << 6)
#define P25_VOICE_ERRS_TOTAL(e)	((e) & 15)		// bits corrected in all seven code words
#define P25_VOICE_ERRS_C0(e)	(((e) >> 4) & 3)	// in the first (the others are descrambled with its word)
#define P25_VOICE_ERRS_C4(e)	(((e) >> 6) & 1)	// in the fifth
// The first word had two or more and the frame eleven or more: the descrambling was most likely wrong
#define P25_VOICE_ERRS_BAD(e)	(P25_VOICE_ERRS_C0(e) >= 2 && P25_VOICE_ERRS_TOTAL(e) >= 11)
int p25_ldu_voice_get(const uint8_t *unit, uint8_t n, uint8_t imbe[P25_IMBE_BYTES]);
void p25_ldu_voice_put(uint8_t *unit, uint8_t n, const uint8_t imbe[P25_IMBE_BYTES]);

// The symbol of an LDU where voice frame n starts; P25_LDU_SYMS for n = 9. Everything before it is in the unit
// once frames 0..n-1 and the unit's words are: a transmitter can send that far
uint16_t p25_ldu_voice_sym(uint8_t n);

// Link control words of conventional voice (5.5): a group call (LCF 0x00) and a unit to unit call (LCF 0x03)
#define P25_LCF_GROUP		0x00
#define P25_LCF_UNIT		0x03
#define P25_TGID_EVERYONE	0xFFFFu	// the talk group every radio takes
typedef struct {
	uint8_t lcf;
	uint8_t mfid;
	bool emergency;		// group calls
	uint16_t tgid;		// group calls
	uint32_t dst;		// unit to unit calls
	uint32_t src;
} p25_lc_t;
// False: a format this does not know (lc->lcf and mfid are still set)
bool p25_lc_parse(const uint8_t raw[P25_LC_BYTES], p25_lc_t *lc);
void p25_lc_build(const p25_lc_t *lc, uint8_t raw[P25_LC_BYTES]);

// ── Receive: units from the framer ──
typedef struct {
	uint8_t duid;
	uint16_t nac;
	int8_t nid_errs;	// bits corrected in the NID; -1: the NID was lost and the unit placed by the call's order
	uint8_t flags;		// AIR_FRAME_xxx of the frame it came in
	uint32_t t_us;
	int16_t rssi_dbm;
	uint16_t nsyms;
	const uint8_t *air;	// the unit as received, valid during the callback
} p25_rx_unit_t;
// An LDU1's or TDULC's link control names a blocked system (p25_wacn_no_tx, p25_sysid_no_tx): the network status
// broadcast's WACN or system ID, or the RFSS status broadcast's system ID (TIA-102.AABF-D 7.3.16, 7.3.17, 7.3.28:
// LCO 35, 36 or 41, standard format, not protected). False for another unit or link control, or one undecodable
bool p25_unit_sys_no_tx(const p25_rx_unit_t *u);

typedef struct {
	uint16_t nac;		// units of another NAC are not ours; P25_NAC_ANY takes all
	void (*unit)(const p25_rx_unit_t *u, void *ctx);
	void (*lost)(void *ctx);
	void *ctx;
	// State
	uint16_t cur_nac;	// of the unit in hand
	uint8_t cur_duid;
	int8_t cur_errs;
	uint8_t last_duid;	// of the unit before, 0xFF none: an LDU with a lost NID is placed after its twin
	uint8_t coasted;	// units in a row placed that way
#if CONFIG_P25_TRUNK
	bool trunk;		// trunking signaling (TSDU) and multi-block trunking packets are taken too (p25_tsbk.h)
#endif
} p25_rx_t;

// The mode's table for the framer. Its context is a p25_rx_t; its frame buffer holds P25_UNIT_SYMS_MAX symbols
extern const air_mode_t p25_air_mode;
void p25_rx_init(p25_rx_t *rx, uint16_t nac, void (*unit)(const p25_rx_unit_t *u, void *ctx), void (*lost)(void *ctx),
		 void *ctx);

// ── A conventional voice call (5.1, 8.2): what a receiver learns from the units, and the units a transmitter sends ──
typedef struct {
	uint16_t nac;
	bool late;		// entered without the header: fields fill in as the LDUs bring them
	bool have_lc;		// lc is from the air (the header alone gives only a talk group, in lc.tgid)
	p25_lc_t lc;
	bool have_alg;		// algid and kid are from the air
	uint8_t algid;		// P25_ALGID_CLEAR, or the call is encrypted: its voice is not handed on
	uint16_t kid;
	int16_t rssi_dbm;
} p25_call_t;

typedef struct {
	void (*start)(const p25_call_t *c, void *ctx);
	void (*update)(const p25_call_t *c, void *ctx);	// a field came in or changed
	// A voice frame, in air order: nine per LDU, handed over together when the LDU is in. errs: the bits its codes
	// corrected, packed (P25_VOICE_ERRS_*)
	void (*voice)(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx);
	void (*end)(void *ctx);				// a terminator, or the signal is gone
#if CONFIG_P25_TRUNK
	// Every unit taken, before the call layer reads it (NULL: none). Trunking signaling goes only here
	void (*unit)(const p25_rx_unit_t *u, void *ctx);
#endif
} p25_call_sink_t;

typedef struct {
	uint32_t units, voice, voice_bad, voice_bits, lc_fail, es_fail, calls;
	uint32_t sys_no_tx;	// LDU1s and TDULCs whose link control names a blocked system (p25_unit_sys_no_tx)
} p25_call_stats_t;

typedef struct {
	p25_rx_t rx;		// first: the framer's context for p25_air_mode
	const p25_call_sink_t *sink;
	void *ctx;
	bool active;
	p25_call_t call;
	p25_call_stats_t stats;
} p25_call_rx_t;

// nac as p25_rx_init. Hand &c->rx to air_framer_init as the mode's context
void p25_call_rx_init(p25_call_rx_t *c, uint16_t nac, const p25_call_sink_t *sink, void *ctx);

// Transmit: header, LDU1 and LDU2 in turn, a terminator. The units go out symbol by symbol as their voice frames
// come in, so the voice is on air a frame or two after the encoder made it
typedef struct {
	uint16_t nac;
	uint8_t ss;		// the status symbols: P25_SS_UNKNOWN direct or to a repeater, P25_SS_TALKAROUND
	uint8_t lc[P25_LC_BYTES];
	uint16_t tgid;
	uint8_t state;
	uint8_t nvoice;		// voice frames in the LDU in hand
	bool ending;
	uint16_t nsyms, ready, sent;	// of the unit in hand: its length, how far it is final, how far it went out
	uint8_t unit[P25_UNIT_BYTES_MAX];
} p25_call_tx_t;

void p25_call_tx_begin(p25_call_tx_t *t, uint16_t nac, const p25_lc_t *lc, uint8_t ss);
// The symbols that can go out now: n of t->unit from symbol *first. 0: waiting for a voice frame, or done
uint16_t p25_call_tx_ready(const p25_call_tx_t *t, uint16_t *first);
// n of them went out
void p25_call_tx_sent(p25_call_tx_t *t, uint16_t n);
// True while the LDU in hand takes another voice frame (one every 20 ms keeps the air fed)
bool p25_call_tx_wants_voice(const p25_call_tx_t *t);
void p25_call_tx_voice(p25_call_tx_t *t, const uint8_t imbe[P25_IMBE_BYTES]);
// No more voice: the LDU in hand is filled up with `silence` frames, then the terminator goes out
void p25_call_tx_end(p25_call_tx_t *t, const uint8_t silence[P25_IMBE_BYTES]);
// True once the terminator's last symbol went out
bool p25_call_tx_done(const p25_call_tx_t *t);
#if CONFIG_P25_TRUNK
// A unit the caller made in t->unit (nsyms symbols, an inbound trunking block): sent alone, then done
void p25_call_tx_single(p25_call_tx_t *t, uint16_t nsyms);
#endif

#endif
