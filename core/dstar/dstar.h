// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR digital voice (DV) (docs/spec/dstar.md): the radio header and its FEC, voice frames and their slow data, the
// framer's table for the mode, and a voice call on top of them. Section numbers are the JARL standard's ("Digitalization
// Technology Standard for Amateur Radio", 4.1.2, chapter 6 and appendices 1 and 2).
//
// On air, GMSK at 4800 bit/s, a 1 above the carrier (Ap1.5): the bit synchronization (1010...), the 15-bit frame
// synchronization, the 660 coded bits of the header, then voice frames of 96 bits every 20 ms: 72 bits of voice (the
// half-rate vocoder's frame, core/mbe/ambe.h) and 24 of slow data. Every 21st frame's slow data is the data
// synchronization; the others' are scrambled. The last frame is a pattern of its own. Every field is sent least
// significant bit first (Ap1.4). One bit is one symbol here: 01 (+3) for a 1, 11 (-3) for a 0 (hal_air.h).
//
// No RTOS or board call: the host tests run all of it.
#ifndef RFOUNDRY_DSTAR_H
#define RFOUNDRY_DSTAR_H

#include <stdbool.h>
#include <stdint.h>

#include "../air/air_framer.h"

// ── On air ──
#define DSTAR_PREAMBLE_SYMS	64		// bit synchronization, the standard length (4.1.1 a)
#define DSTAR_FS_SYMS		15		// frame synchronization (4.1.1 b)
#define DSTAR_HDR_BYTES		41		// the header with its P_FCS
#define DSTAR_HDR_FIELDS	39		// ... without
#define DSTAR_HDR_SYMS		660		// coded: (41 x 8 + 2) x 2
#define DSTAR_FRAME_SYMS	96		// a voice frame: 72 bits of voice, 24 of slow data (4.1.2 b)
#define DSTAR_VOICE_BYTES	9
#define DSTAR_DATA_BYTES	3
#define DSTAR_SUPERFRAME	21		// a data synchronization every 21 frames (4.1.2 c)
#define DSTAR_END_SYMS		48		// the last frame's pattern (4.1.2 h)
#define DSTAR_START_SYMS	(DSTAR_PREAMBLE_SYMS + DSTAR_FS_SYMS + DSTAR_HDR_SYMS)

// The sync patterns' ids in the profile
enum { DSTAR_SYNC_HEADER = 1, DSTAR_SYNC_DATA = 2, DSTAR_SYNC_END = 3 };

extern const uint8_t dstar_data_sync[DSTAR_DATA_BYTES];	// 0x55 0x2D 0x16
extern const uint8_t dstar_end_bytes[DSTAR_END_SYMS / 8];	// 32 bits of 1010, 000100110101111, 0

// ── The radio header (4.1.1) ──
#define DSTAR_CALL_LEN		8
#define DSTAR_SUFFIX_LEN	4
// Flag 1
#define DSTAR_F1_DATA		0x80		// data, else voice
#define DSTAR_F1_REPEATER	0x40		// through a repeater, else direct
#define DSTAR_F1_BREAK		0x20		// interruption (break-in)
#define DSTAR_F1_CONTROL	0x10		// a control signal
#define DSTAR_F1_EMERGENCY	0x08		// urgent communication: a receiver opens its squelch
#define DSTAR_F1_FUNC		0x07		// 0 NULL, 1 unrelayable, 2 no response, 3 ACK, 4 resend, 6 auto reply, 7 repeater control

// Callsigns are ASCII, space padded, not terminated
typedef struct {
	uint8_t flag[3];
	char rpt2[DSTAR_CALL_LEN];	// destination repeater ("DIRECT" direct)
	char rpt1[DSTAR_CALL_LEN];	// departure repeater
	char ur[DSTAR_CALL_LEN];	// companion ("CQCQCQ" to all)
	char my[DSTAR_CALL_LEN];	// own callsign 1
	char sfx[DSTAR_SUFFIX_LEN];	// own callsign 2
} dstar_header_t;

// P_FCS: CRC-CCITT, sent reflected (0x8408), from all ones, inverted, low byte first (4.1.1 k)
uint16_t dstar_crc(const uint8_t *b, uint16_t n);
// To and from the 41 bytes as on air (before the FEC); unpack is false when the P_FCS fails
void dstar_header_pack(const dstar_header_t *h, uint8_t raw[DSTAR_HDR_BYTES]);
bool dstar_header_unpack(const uint8_t raw[DSTAR_HDR_BYTES], dstar_header_t *h);
// A callsign field from text: upper case, space padded, at most len characters
void dstar_call_set(char *field, uint8_t len, const char *text);

// ── Symbols ──
// n bits of bytes (each least significant bit first) to symbols at `at` of a packed dibit buffer, and back
void dstar_bits_put(uint8_t *syms, uint16_t at, const uint8_t *bytes, uint16_t nbits);
void dstar_bits_get(const uint8_t *syms, uint16_t at, uint8_t *bytes, uint16_t nbits);

// The header's FEC (Ap2): a rate 1/2 convolutional code of constraint length 3, the 24 x 28 interleave, then the
// scrambler (Ap1). Encode: the 41 bytes to 660 symbols at `at`
void dstar_header_encode(const uint8_t raw[DSTAR_HDR_BYTES], uint8_t *syms, uint16_t at);
// The decoder's memory, the caller's: the Viterbi decisions, a bit per state
typedef struct {
	uint8_t dec[DSTAR_HDR_SYMS / 2];
} dstar_fec_work_t;
// Decode 660 symbols (soft: one per symbol or NULL) to the 41 bytes. Returns the code bits the path disagreed with
// (a measure of the errors), or -1 when no reading of them passes the P_FCS
int dstar_header_decode(const uint8_t *hard, const int8_t *soft, uint16_t at, dstar_fec_work_t *w,
			uint8_t raw[DSTAR_HDR_BYTES]);

// A voice frame: 72 bits of voice as on air (ambe.h) and the 24 bits of the slow data field as on air (scrambled, or
// the data synchronization)
void dstar_frame_put(uint8_t *syms, uint16_t at, const uint8_t voice[DSTAR_VOICE_BYTES], const uint8_t data[DSTAR_DATA_BYTES]);
void dstar_frame_get(const uint8_t *syms, uint16_t at, uint8_t voice[DSTAR_VOICE_BYTES], uint8_t data[DSTAR_DATA_BYTES]);
// The slow data scrambler: the first 24 bits of the scrambler's sequence (Ap1.3), its own inverse
void dstar_data_scramble(uint8_t data[DSTAR_DATA_BYTES]);
// The scrambler's sequence (S(x) = x^7 + x^4 + 1 from all ones), bit n
uint8_t dstar_pn(uint16_t n);

// ── Slow data (chapter 6): two frames' fields make a block, its first byte the mini-header ──
#define DSTAR_MSG_LEN		20
#define DSTAR_TEXT_LEN		48
#define DSTAR_MINI_DATA		0x30		// simple data (D-PRS, GPS): + bytes valid, 1..5
#define DSTAR_MINI_MSG		0x40		// the 20-character message: + block 0..3
#define DSTAR_MINI_HEADER	0x50		// the header again: + bytes valid
#define DSTAR_MINI_SQUELCH	0xC0		// code squelch
#define DSTAR_MINI_NONE		0x66		// no data

typedef struct {
	uint8_t block[6];
	uint8_t have;			// bytes of the block in (0 or 3)
	char msg[DSTAR_MSG_LEN];
	uint8_t msg_parts;		// bit k: block k of the message is in
	uint8_t hdr[DSTAR_HDR_BYTES];
	uint8_t hdr_len;
	char line[DSTAR_TEXT_LEN];	// simple data, the line coming in
	uint8_t line_len;
	char text[DSTAR_TEXT_LEN];	// the last whole line (NUL terminated)
} dstar_slow_rx_t;

// what dstar_slow_rx_frame completed
#define DSTAR_SLOW_MSG		0x01
#define DSTAR_SLOW_HEADER	0x02
#define DSTAR_SLOW_TEXT		0x04

void dstar_slow_rx_reset(dstar_slow_rx_t *s);
// The data field of frame n (1..20) of a superframe, descrambled. Returns DSTAR_SLOW_* of what it completed; the
// header in s->hdr when DSTAR_SLOW_HEADER
uint8_t dstar_slow_rx_frame(dstar_slow_rx_t *s, const uint8_t data[DSTAR_DATA_BYTES], uint8_t n);

typedef struct {
	uint8_t hdr[DSTAR_HDR_BYTES];
	char msg[DSTAR_MSG_LEN];
	bool have_msg;
	uint16_t sf;			// superframes sent
	uint8_t block[6];
} dstar_slow_tx_t;

// What we send: the message (NULL or empty: none) in every other superframe, the header again in the others
void dstar_slow_tx_init(dstar_slow_tx_t *s, const uint8_t hdr[DSTAR_HDR_BYTES], const char *msg);
// The data field of frame n (1..20), before the scrambler; frame 20 ends a superframe
void dstar_slow_tx_frame(dstar_slow_tx_t *s, uint8_t n, uint8_t data[DSTAR_DATA_BYTES]);

// ── Receive: the framer's table and a call ──
extern const air_mode_t dstar_air_mode;

typedef struct {
	bool have_header;	// from the header on air, or sent again in the slow data (late entry)
	bool late;		// joined after the header
	dstar_header_t hdr;
	bool have_msg;
	char msg[DSTAR_MSG_LEN + 1];
	char text[DSTAR_TEXT_LEN];	// the last line of simple data ("" none)
	int16_t rssi_dbm;
} dstar_call_t;

typedef struct {
	void (*start)(const dstar_call_t *c, void *ctx);
	void (*update)(const dstar_call_t *c, void *ctx);	// a field came in
	void (*voice)(const uint8_t voice[DSTAR_VOICE_BYTES], void *ctx);
	void (*end)(void *ctx);
} dstar_call_sink_t;

typedef struct {
	uint32_t calls, headers, header_fail, late, frames, voice, syncs, ends, lost;
	uint32_t slow_msgs, slow_headers, slow_text;
	uint32_t hdr_fec_bits;		// code bits the header's decoder disagreed with
} dstar_call_stats_t;

typedef struct {
	const dstar_call_sink_t *sink;
	void *ctx;
	bool active;
	int8_t n;			// the frame's place in the superframe, -1 not known yet
	dstar_call_t call;
	dstar_slow_rx_t slow;
	dstar_fec_work_t work;
	dstar_call_stats_t stats;
} dstar_call_rx_t;

void dstar_call_rx_init(dstar_call_rx_t *c, const dstar_call_sink_t *sink, void *ctx);

// Transmit: what goes on air, built a piece at a time and released as it fills. ready: symbols from `first` that can
// go now; sent: n of them went
typedef struct {
	uint8_t state;
	bool ending;
	uint8_t n;			// the next voice frame's place in the superframe
	uint16_t nsyms, ready, sent;
	dstar_slow_tx_t slow;
	uint8_t syms[(DSTAR_START_SYMS + 3) / 4];
} dstar_call_tx_t;

void dstar_call_tx_begin(dstar_call_tx_t *t, const dstar_header_t *h, const char *msg);
uint16_t dstar_call_tx_ready(const dstar_call_tx_t *t, uint16_t *first);
const uint8_t *dstar_call_tx_syms(const dstar_call_tx_t *t);
void dstar_call_tx_sent(dstar_call_tx_t *t, uint16_t n);
bool dstar_call_tx_wants_voice(const dstar_call_tx_t *t);
// The next voice frame (false: not wanted now)
bool dstar_call_tx_voice(dstar_call_tx_t *t, const uint8_t voice[DSTAR_VOICE_BYTES]);
// The operator let go: the frame in hand goes, then the last frame's pattern
void dstar_call_tx_end(dstar_call_tx_t *t);
bool dstar_call_tx_done(const dstar_call_tx_t *t);

#endif
