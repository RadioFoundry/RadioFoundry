// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// YSF (C4FM) voice, DN mode (docs/spec/ysf.md): the frame and its channels, the framer's table for the
// mode, and a voice call on top of them.
//
// A frame is 480 symbols (960 bits, 100 ms at 4800 symbols/s): a 20-symbol sync, the 100-symbol FICH, then five
// 72-symbol payload blocks. In DN mode each block is 20 symbols of the data channel (DCH) and 52 of one voice
// channel (VCH); in a header or terminator each block is 36 symbols of each of two long channels (CSD1, CSD2).
// Buffers hold a frame as on air, packed dibits (hal_air.h): the frame's bits are its dibits' bits, the first bit of
// a dibit the higher one. No RTOS or board call: the host tests run all of it.
#ifndef RFOUNDRY_YSF_H
#define RFOUNDRY_YSF_H

#include <stdbool.h>
#include <stdint.h>

#include "../air/air_framer.h"
#include "../dmr/ambe/ambeFec.h"
#include "ysf_wx.h"

// ── The frame ──
#define YSF_FRAME_SYMS		480
#define YSF_FRAME_BYTES		(YSF_FRAME_SYMS / 4)
#define YSF_SYNC_SYMS		20
#define YSF_FICH_SYM		20		// 100 symbols
#define YSF_FICH_SYMS		100
#define YSF_BLOCK_SYM(b)	(120 + 72 * (b))	// payload block 0..4
#define YSF_BLOCK_SYMS		72
#define YSF_BLOCKS		5
#define YSF_HEAD_SYMS		120		// sync and FICH: what tells a frame and its kind
#define YSF_PREAMBLE_SYMS	480		// what this side sends ahead of the header (100 ms)

// ── FICH ──
// FI: what the frame is
#define YSF_FI_HEADER		0
#define YSF_FI_COMMS		1
#define YSF_FI_TERM		2
#define YSF_FI_TEST		3
// DT: what a communications frame carries
#define YSF_DT_VD1		0		// voice and data, the voice at full FEC
#define YSF_DT_DATA		1		// data at full rate (room commands)
#define YSF_DT_VD2		2		// DN: voice (AMBE+2 2450 bit/s) and data
#define YSF_DT_VW		3		// voice at full rate
// MR: message route
#define YSF_MR_DIRECT		0
#define YSF_MR_UP_FREE		1		// to a repeater, which is idle
#define YSF_MR_UP_BUSY		2
#define YSF_FT_DN		6		// the DCH cycle of a DN call: frames FN 0..6
#define YSF_DGID_MAX		99
#define YSF_DGID_ALL		0		// a receiver's DG-ID 0 takes every one

typedef struct {
	uint8_t fi, cs, cm;	// frame information; callsign information; call mode
	uint8_t bn, bt;		// block number and total (data mode)
	uint8_t fn, ft;		// frame number in the DCH cycle, and the cycle's last
	bool dev;		// narrow deviation
	uint8_t mr;		// message route
	bool voip;
	uint8_t dt;		// data type
	bool sql;		// squelch code (DG-ID) in use
	uint8_t dgid;		// 0..127
} ysf_fich_t;

// ── Channels ──
#define YSF_CS_LEN		10		// a callsign field: ASCII, space padded
#define YSF_CSD_BYTES		20		// a long channel: two callsign fields
#define YSF_DCH_BYTES		10
#define YSF_VCH_PER_FRAME	5
#define YSF_ID_LEN		5		// a radio ID: five characters 0-9, A-Z
#define YSF_VOICE_BYTES		AMBE_FRAME_BYTES	// a voice frame as the DMR side's voice path takes it

// The decoders' memory, the caller's: Viterbi decisions and soft bits of the longest channel (a CSD: 180 inputs)
typedef struct {
	uint16_t dec[180];
	int8_t soft[360];
} ysf_fec_work_t;

// hard: the frame (YSF_FRAME_BYTES); soft: one value per symbol (AIR_SOFT_OUTER at +3), or NULL
void ysf_frame_begin(uint8_t *frame);	// the sync; the rest is left as it was
unsigned ysf_sync_errs(const uint8_t *hard);
void ysf_fich_put(uint8_t *frame, const ysf_fich_t *f);
// Bits the decoders corrected (0..), or -1: a Golay word or the CRC failed
int ysf_fich_get(const uint8_t *hard, const int8_t *soft, ysf_fec_work_t *w, ysf_fich_t *f);
// Header and terminator: CSD1 (destination, source) and CSD2 (downlink, uplink). Data mode carries its 20-byte
// pieces in the same two channels
void ysf_csd_put(uint8_t *frame, const uint8_t csd1[YSF_CSD_BYTES], const uint8_t csd2[YSF_CSD_BYTES]);
int ysf_csd_get(const uint8_t *hard, const int8_t *soft, uint8_t which, ysf_fec_work_t *w, uint8_t out[YSF_CSD_BYTES]);
// DN mode: the DCH (its content by the FICH's FN) and VCH n = 0..4
void ysf_dch_put(uint8_t *frame, const uint8_t in[YSF_DCH_BYTES]);
int ysf_dch_get(const uint8_t *hard, const int8_t *soft, ysf_fec_work_t *w, uint8_t out[YSF_DCH_BYTES]);
// A VCH carries the vocoder's 49 parameter bits (one per byte, the DMR side's order, ambeFec.h). get returns the
// repeated bits whose copies disagreed (0..27)
void ysf_vch_put(uint8_t *frame, uint8_t n, const uint8_t bits[AMBE_PARAM_BITS]);
int ysf_vch_get(const uint8_t *hard, const int8_t *soft, uint8_t n, uint8_t bits[AMBE_PARAM_BITS]);

// The parts of the codec, for tests and tools
uint8_t ysf_pn(uint16_t i);				// the whitening sequence's bit i
extern const uint8_t ysf_sync[5];

// ── Receive: the framer's table and its hooks ──
typedef struct {
	int8_t fich_errs;	// -1: the FICH was lost; the frame is placed by the call's order
	ysf_fich_t fich;	// valid with fich_errs >= 0
	uint8_t flags;		// AIR_FRAME_xxx
	uint32_t t_us;
	int16_t rssi_dbm;
	const uint8_t *hard;
	const int8_t *soft;
} ysf_rx_frame_t;

typedef struct {
	void (*frame)(const ysf_rx_frame_t *f, void *ctx);
	void (*lost)(void *ctx);
	void *ctx;
	int8_t cur_errs;
	ysf_fich_t cur;
	bool in_call;		// the frame before was a header or communications frame
	uint8_t coasted;	// frames in a row placed without a FICH
	ysf_fec_work_t work;	// the decoders' memory (the call layer's too)
} ysf_rx_t;

extern const air_mode_t ysf_air_mode;

void ysf_rx_init(ysf_rx_t *rx, void (*frame)(const ysf_rx_frame_t *f, void *ctx), void (*lost)(void *ctx), void *ctx);

// ── A call ──
typedef struct {
	bool late;			// joined after its header: the fields come from the DCH
	uint8_t dgid, dt, mr;		// of the last good FICH
	char src[YSF_CS_LEN + 1];	// trimmed, "" until heard
	char dst[YSF_CS_LEN + 1];
	char src_id[YSF_ID_LEN + 1];	// the caller's radio ID, "" until heard
	int16_t rssi_dbm;
} ysf_call_t;

typedef struct {
	void (*start)(const ysf_call_t *c, void *ctx);
	void (*update)(const ysf_call_t *c, void *ctx);		// a field changed
	void (*voice)(const uint8_t v[YSF_VOICE_BYTES], void *ctx);
	void (*end)(void *ctx);
	// A room reply heard (a data transmission, ysf.md 9): valid during the call. NULL: data is ignored
	void (*wx)(const ysf_wx_t *w, void *ctx);
} ysf_call_sink_t;

typedef struct {
	uint32_t calls, frames, fich_fail, csd_fail, dch_fail;
	uint32_t voice;		// VCH handed on
	uint32_t voice_held;	// VCH of another data type
	uint32_t fec_bits;	// bits the FICH, CSD and DCH decoders corrected
	uint32_t vch_fixed;	// VCH bits the vote corrected
	uint32_t data_frames, wx_replies;	// frames of data transmissions; room replies parsed from them
} ysf_call_stats_t;

typedef struct {
	ysf_rx_t rx;			// first: the framer's context is the call's
	const ysf_call_sink_t *sink;
	void *ctx;
	bool active;
	ysf_call_t call;
	uint8_t next_fn;		// the FN a frame placed without its FICH takes
	ysf_call_stats_t stats;
	ysf_wx_rx_t wx;			// a data transmission being received
} ysf_call_rx_t;

void ysf_call_rx_init(ysf_call_rx_t *c, const ysf_call_sink_t *sink, void *ctx);

// What a transmission says about us
typedef struct {
	char callsign[YSF_CS_LEN + 1];
	char radio_id[YSF_ID_LEN + 1];
	uint8_t dgid;
	uint8_t mr;			// YSF_MR_xxx
} ysf_tx_id_t;

// Transmit: a call's frames, built one at a time and released as they fill (nxdn.h has the same shape). ready:
// symbols from `first` that can go now; sent: n of them went
typedef struct {
	uint8_t state;
	bool ending;
	uint8_t fn;			// of the communications frame in hand
	uint8_t nvoice;			// VCH filled in it
	uint16_t nsyms, ready, sent, pre_left;
	ysf_tx_id_t id;
	// A data transmission (a room command) in place of voice: its bytes, their count, the last FN
	bool data;
	uint8_t data_len, data_ft;
	uint8_t data_buf[YSF_WX_CMD_MAX];
	uint8_t frame[YSF_FRAME_BYTES];
} ysf_call_tx_t;

void ysf_call_tx_begin(ysf_call_tx_t *t, const ysf_tx_id_t *id);
// A data transmission instead (ysf.md 9.1): preamble, header, data frames FN 0 to the last, terminator; it ends by
// itself, no voice asked for
void ysf_call_tx_begin_data(ysf_call_tx_t *t, const ysf_tx_id_t *id, const uint8_t *data, uint8_t len);
uint16_t ysf_call_tx_ready(const ysf_call_tx_t *t, uint16_t *first);
const uint8_t *ysf_call_tx_syms(const ysf_call_tx_t *t);
void ysf_call_tx_sent(ysf_call_tx_t *t, uint16_t n);
bool ysf_call_tx_wants_voice(const ysf_call_tx_t *t);
// v: a voice frame of the DMR side's voice path. False when the call takes none now
bool ysf_call_tx_voice(ysf_call_tx_t *t, const uint8_t v[YSF_VOICE_BYTES]);
// The operator let go: the frame in hand is filled with silence (one without voice yet is dropped), then the
// terminator
void ysf_call_tx_end(ysf_call_tx_t *t, const uint8_t silence[YSF_VOICE_BYTES]);
bool ysf_call_tx_done(const ysf_call_tx_t *t);

// The DCH a DN transmission sends at FN 0..6 (docs/spec/ysf.md 4), and the header's two long channels
void ysf_dch_content(const ysf_tx_id_t *id, uint8_t fn, uint8_t out[YSF_DCH_BYTES]);
void ysf_head_csd(const ysf_tx_id_t *id, uint8_t csd1[YSF_CSD_BYTES], uint8_t csd2[YSF_CSD_BYTES]);

#endif
