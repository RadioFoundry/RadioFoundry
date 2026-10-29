// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The split duplex TX half's continuous BS outbound, without hardware (docs/spec/split-duplex.md 5): a buffer of host
 * bursts per outbound slot with its prebuffer (the target repeat delay), what goes out in each slot (the next burst, a
 * silence voice burst to keep an underrun superframe whole, a terminator for a stream the host dropped, else the idle
 * message), and the CACH before it: TC from the slot, AT from the host's mask and the automatic rule, the short LC
 * spread over four CACH (core/dmr/fec/dmrCach.h).
 *
 * Slot numbering: the k-th burst produced for outbound slot ts is outbound slot s = 2k + ts - 1 of the stream the
 * driver plays (it plays each slot's bursts in order, TS1 and TS2 alternating). The short LC fragment of slot s is
 * row s % 4, so a new short LC starts at a multiple of 4 for both slots alike.
 */
#ifndef RFOUNDRY_BS_OUTBOUND_H
#define RFOUNDRY_BS_OUTBOUND_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_dmr_modem.h"

#define BS_OUT_DEPTH		16	// bursts per slot (960 ms)
#define BS_OUT_PREBUFFER	3	// default target: this many bursts in before a stream plays (180 ms)
#define BS_OUT_PREBUFFER_MAX	12
#define BS_OUT_WAIT_MS		240	// ... or once its first burst has waited this long
#define BS_OUT_STREAM_LOST_MS	500	// an underrun stream with nothing new for this long is ended with a terminator
#define BS_OUT_AT_HOLD_MS	2000	// the host's AT mask falls back to idle after this long without a new one

#define BS_AT_MODE_OR		0	// AT = host mask OR the automatic rule (an outbound slot not idle)
#define BS_AT_MODE_MASK		1	// AT = host mask alone

// The idle message's 96 information bits (ETSI TS 102 361-1 table D.2, I(95) first)
extern const uint8_t BS_IDLE_INFO[12];

typedef struct {
	dmr_tx_burst_t b;
	uint8_t lc[9];		// voice A: the embedded LC for B-E of its superframe
	bool lc_valid;
} bsOutBurst_t;

typedef struct {
	bsOutBurst_t q[BS_OUT_DEPTH];
	uint8_t head, count;
	uint8_t terms;		// terminators in the buffer
	bool playing;		// the buffer plays out (prebuffer reached)
	bool voice_open;	// a voice stream is on air (header or voice sent, no terminator yet)
	uint8_t last_index;	// last voice position sent
	uint32_t first_ms;	// when the buffer last went from empty to not empty
	uint32_t last_push_ms;
	uint8_t lc[9];		// the stream's LC (from its header): silence A and the closing terminator
	bool lc_valid;
	uint32_t n;		// bursts produced for this slot
	uint16_t underruns;
} bsSlot_t;

typedef struct {
	uint16_t idle;		// idle bursts produced
	uint16_t cach;		// CACH produced
	uint16_t silence;	// silence voice bursts (underrun fill)
	uint16_t closed;	// streams ended by us (terminator after the host stopped)
	uint16_t dropped;	// bursts dropped by abort / reset
} bsOutStats_t;

typedef struct {
	bsSlot_t slot[2];
	uint8_t prebuffer;
	uint8_t at_mask, at_mode;
	uint32_t at_ms;
	bool at_set;
	uint8_t at_on_air;	// AT bits last sent per channel (bit 0 TS1, in the CACH before outbound TS2; bit 1 TS2)
	uint8_t at_auto;	// ... of those, what the automatic rule gave (the slot's last burst not idle)
	uint32_t slc_cur[4];	// short LC rows on air
	uint32_t slc_new[4];	// ... from slot switch_s on
	uint32_t slc_next[4];	// ... after that (a third arrived while a switch was half done)
	bool slc_pending, slc_queued;
	uint32_t switch_s;
	bsOutStats_t st;
} bsOutbound_t;

void bsOutboundInit(bsOutbound_t *o, uint8_t prebuffer);
// Everything buffered dropped, streams forgotten; slot numbering, short LC and AT kept
void bsOutboundReset(bsOutbound_t *o);
void bsOutboundSetPrebuffer(bsOutbound_t *o, uint8_t prebuffer);

// One host burst for outbound slot ts (1 or 2); lc: for voice A, the embedded LC to send in its B-E (NULL: the
// stream's header LC). Returns 0 or MMDVM_NAK_FULL (5)
uint8_t bsOutboundPush(bsOutbound_t *o, int8_t ts, const dmr_tx_burst_t *b, const uint8_t *lc, uint32_t now_ms);
// DMR_ABORT: what is buffered for ts is dropped (a voice stream on air gets its terminator)
void bsOutboundDrop(bsOutbound_t *o, int8_t ts);
uint8_t bsOutboundCount(const bsOutbound_t *o, int8_t ts);
uint8_t bsOutboundSpace(const bsOutbound_t *o, int8_t ts);
// Anything buffered, or a stream still on air
bool bsOutboundBusy(const bsOutbound_t *o);

// SET_AT: host mask (bit 0 inbound TS1, bit 1 TS2) and mode
void bsOutboundSetAt(bsOutbound_t *o, uint8_t mask, uint8_t mode, uint32_t now_ms);
uint8_t bsOutboundAtMask(const bsOutbound_t *o, uint32_t now_ms);	// the host's mask now (0 once expired)

// A new short LC as four CACH payload rows (dmrShortLcEncode), from the next multiple of 4
void bsOutboundSetShortLc(bsOutbound_t *o, const uint32_t rows[4]);

// What goes out in the next outbound slot ts: the burst, its CACH and for voice A the embedded LC (else NULL)
void bsOutboundNext(bsOutbound_t *o, int8_t ts, uint32_t now_ms, dmr_tx_burst_t *out, dmr_cach_t *cach,
		    const uint8_t **emb_lc);

// The slot as sent, for a modem that only modulates (the HR_C6000's physical layer mode, modem-hrc6000.md 9.1): the
// CACH (24 bits) then the 264-bit burst, most significant bit of byte 0 first. Voice with BS voice sync on A, the EMB
// and emb_lc's fragments on B-E (emb_lc: the superframe's embedded LC, NULL: null fragments); data and control bursts
// coded by rate with BS data sync, a voice LC header / terminator without parity (host_crc false) given its RS(12,9).
// cach NULL: TC from ts, AT 0. Returns false when the burst couldn't be coded and the idle message went out instead.
#define BS_RAW_SLOT_BYTES	36
bool bsOutboundRawSlot(const dmr_tx_burst_t *b, const dmr_cach_t *cach, int8_t ts, uint8_t cc, const uint8_t *emb_lc,
		       uint8_t *out);

#endif
