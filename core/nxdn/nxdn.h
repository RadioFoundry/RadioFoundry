// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN conventional voice (docs/spec/nxdn.md): the frame and its channels, the framer's table for the mode, and a
// voice call on top of them. Section numbers are NXDN TS 1-A's (Common Air Interface) unless another part is named.
//
// A frame is 192 symbols (384 bits, 80 ms at 4800 bit/s, 40 ms at 9600): FSW, LICH, SACCH, then four 72-bit voice
// channels (VCH) or one or two FACCH1 in their place (4.4, figure 4.4-3). Everything behind the FSW is scrambled on
// air (4.6). Buffers here hold a frame as on air: packed dibits (hal_air.h), scrambled; the functions below scramble
// as they write and descramble as they read. No RTOS or board call: the host tests run all of it.
#ifndef RFOUNDRY_NXDN_H
#define RFOUNDRY_NXDN_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "../air/air_framer.h"
#include "../fec/conv.h"

// ── The frame (4.4) ──
#define NXDN_FRAME_SYMS		192
#define NXDN_FRAME_BYTES	(NXDN_FRAME_SYMS / 4)
#define NXDN_FSW_SYMS		10
#define NXDN_LICH_SYM		10		// 8 symbols
#define NXDN_SACCH_SYM		18		// 30 symbols
#define NXDN_VCH_SYM(n)		(48 + 36 * (n))	// VCH 1..4 as n = 0..3, 36 symbols each
#define NXDN_HEAD_SYMS		48		// FSW, LICH, SACCH: what tells a frame and its kind
#define NXDN_PREAMBLE_SYMS	24		// what this side sends ahead of the first frame (4.4.3)

// ── LICH (4.5.3, 5.2.1): seven bits, sent with an even parity bit ──
#define NXDN_LICH(rf, usc, opt, dir)	((uint8_t)((rf) << 5 | (usc) << 3 | (opt) << 1 | (dir)))
#define NXDN_LICH_RF(l)		(((l) >> 5) & 3)
#define NXDN_LICH_USC(l)	(((l) >> 3) & 3)
#define NXDN_LICH_OPT(l)	(((l) >> 1) & 3)
#define NXDN_LICH_DIR(l)	((l) & 1)
// RF channel type
#define NXDN_RF_RCCH		0
#define NXDN_RF_RTCH		1
#define NXDN_RF_RDCH		2
#define NXDN_RF_RTCH_C		3
// Functional channel type (USC) on an RDCH or RTCH
#define NXDN_USC_SACCH_NS	0		// SACCH, non-superframe (a call's first and last frames)
#define NXDN_USC_UDCH		1
#define NXDN_USC_SACCH_SF	2		// SACCH, superframe
#define NXDN_USC_SACCH_SF_IDLE	3		// SACCH, superframe; the FACCH1 frame idle (9600 bit/s, TS 1-B 4.1.2.1)
// Option: which halves of the voice part are VCH and which FACCH1
#define NXDN_STEAL_BOTH		0		// two FACCH1
#define NXDN_STEAL_FIRST	1		// FACCH1 in VCH 1-2, voice in VCH 3-4
#define NXDN_STEAL_SECOND	2		// voice in VCH 1-2, FACCH1 in VCH 3-4
#define NXDN_STEAL_NONE		3		// four VCH
// Direction (table 5.2-2: a subscriber unit sends inbound, a repeater outbound)
#define NXDN_DIR_INBOUND	0
#define NXDN_DIR_OUTBOUND	1

// ── Layer 3 (6) ──
#define NXDN_SACCH_BITS		26		// per frame: SR (8) and 18 bits of a message
#define NXDN_SACCH_BYTES	4		// ... MSB first
#define NXDN_SACCH_MSG_BYTES	9		// a message over a superframe's four SACCH (72 bits)
#define NXDN_FACCH1_BYTES	10		// 80 bits
#define NXDN_EHR_BYTES		9		// a 72-bit VCH: one 20 ms voice frame with its FEC (7.2.1)
#define NXDN_VOICE_FRAMES	4		// VCH per frame

// SR (6.3): structure in bits 7-6, RAN in 5-0. The structure counts down over a superframe
#define NXDN_SR(part, ran)	((uint8_t)((3 - (part)) << 6 | ((ran) & 0x3F)))
#define NXDN_SR_PART(sr)	(3 - ((sr) >> 6))	// 0..3 = 1/4..4/4 (a single SACCH is 3: 4/4)
#define NXDN_SR_RAN(sr)		((sr) & 0x3F)
#define NXDN_RAN_ANY		0		// a receiver's RAN 0 takes every RAN (TS 1-B 4.1.7.1)

// Message types (6.4.1)
#define NXDN_MSG_VCALL		0x01
#define NXDN_MSG_VCALL_IV	0x03
#define NXDN_MSG_TX_REL_EX	0x07
#define NXDN_MSG_TX_REL		0x08
#define NXDN_MSG_PROP_FORM	0x3F
#define NXDN_ALIAS_LEN		14
#define NXDN_MSG_IDLE		0x10
// Call type (6.5.x)
#define NXDN_CALL_BROADCAST	0
#define NXDN_CALL_GROUP		1		// conference group call
#define NXDN_CALL_UNSPECIFIED	2		// TX_REL only
#define NXDN_CALL_INDIVIDUAL	4
#define NXDN_CALL_INTERCONNECT	6
#define NXDN_CALL_SPEED_DIAL	7
// Voice call option: the mode in bits 2-0
#define NXDN_VOICE_4800_EHR	0
#define NXDN_VOICE_9600_EHR	2
#define NXDN_VOICE_9600_EFR	3
// Cipher type
#define NXDN_CIPHER_NONE	0
#define NXDN_CIPHER_SCRAMBLE	1
#define NXDN_KEY_IDS		64u
#define NXDN_SCRAMBLE_KEY_MAX	32767u
#define NXDN_CIPHER_DES		2
#define NXDN_CIPHER_AES		3
// IDs (6.5): 16 bits
#define NXDN_ID_NULL		0
#define NXDN_ID_ALL		0xFFFF		// all units / all groups
#define NXDN_UNIT_MAX		0xFFEF		// the last standard unit ID
// CC option bits
#define NXDN_CC_EMERGENCY	0x80

// VCALL and TX_REL (6.4.1.1, 6.4.1.3): the fields a conventional voice call uses
typedef struct {
	uint8_t msg;		// NXDN_MSG_VCALL or NXDN_MSG_TX_REL
	uint8_t cc_opt;		// NXDN_CC_xxx
	uint8_t call_type;	// NXDN_CALL_xxx
	uint8_t voice_opt;	// 5 bits: NXDN_VOICE_xxx in 2-0, duplex in 4
	uint16_t src, dst;	// unit ID; group or unit ID
	uint8_t cipher;		// NXDN_CIPHER_xxx (VCALL only)
	uint8_t key_id;		// 6 bits
} nxdn_vcall_t;

// Build into a FACCH1 (10 bytes) or a superframe's SACCH message (9 bytes); the octets past the message are 0
void nxdn_vcall_build(const nxdn_vcall_t *v, uint8_t *out, uint8_t len);
// False: not a VCALL or TX_REL
bool nxdn_vcall_parse(const uint8_t *msg, nxdn_vcall_t *v);
static inline uint8_t nxdn_msg_type(const uint8_t *msg) { return msg[0] & 0x3F; }

// ── Frame fields. hard: the frame (NXDN_FRAME_BYTES), soft: one value per symbol (AIR_SOFT_OUTER at +3) or NULL ──
// The decoders' memory, the caller's: the Viterbi decisions and the depunctured soft bits of a FACCH1, the longest
typedef struct {
	uint16_t dec[96];
	int8_t soft[192];
} nxdn_fec_work_t;

// FSW and LICH; the rest of the frame is left as it was
void nxdn_frame_begin(uint8_t *frame, uint8_t lich);
// The LICH, or -1 when its parity fails. Each symbol is read by its sign (figure 4.5-7)
int nxdn_lich_get(const uint8_t *hard, const int8_t *soft);
void nxdn_sacch_put(uint8_t *frame, const uint8_t in[NXDN_SACCH_BYTES]);
// Corrected bits (0..), or -1 when the CRC fails
int nxdn_sacch_get(const uint8_t *hard, const int8_t *soft, nxdn_fec_work_t *w, uint8_t out[NXDN_SACCH_BYTES]);
// half: 0 = VCH 1-2, 1 = VCH 3-4
void nxdn_facch1_put(uint8_t *frame, uint8_t half, const uint8_t in[NXDN_FACCH1_BYTES]);
int nxdn_facch1_get(const uint8_t *hard, const int8_t *soft, uint8_t half, nxdn_fec_work_t *w,
		    uint8_t out[NXDN_FACCH1_BYTES]);
// n = 0..3. The 72 bits as on air, descrambled: their FEC is the voice path's (docs/spec/nxdn.md 5)
void nxdn_vch_put(uint8_t *frame, uint8_t n, const uint8_t in[NXDN_EHR_BYTES]);
void nxdn_vch_get(const uint8_t *hard, uint8_t n, uint8_t out[NXDN_EHR_BYTES]);
// What a transmission opens with (24 symbols: the deviation pattern, then 4.4.3's)
extern const uint8_t nxdn_preamble[NXDN_PREAMBLE_SYMS / 4];

// The parts of the codec, for tests and tools
uint8_t nxdn_pn(uint16_t sym);		// the scrambler's bit for frame symbol sym (0 in the FSW)
uint16_t nxdn_crc(const uint8_t *bits, uint16_t n, uint8_t width, uint16_t poly);

// ── Receive: the framer's table and its hooks ──
// One frame as taken: its LICH and SACCH read by the head hook
typedef struct {
	int16_t lich;		// -1 lost: placed by the call's order (coasting)
	int8_t sacch_errs;	// -1: the CRC failed
	uint8_t sacch[NXDN_SACCH_BYTES];
	uint8_t flags;		// AIR_FRAME_xxx
	uint32_t t_us;
	int16_t rssi_dbm;
	const uint8_t *hard;	// the frame as on air
	const int8_t *soft;	// or NULL
} nxdn_rx_frame_t;

typedef struct {
	uint8_t ran;		// NXDN_RAN_ANY: every system's frames
	void (*frame)(const nxdn_rx_frame_t *f, void *ctx);
	void (*lost)(void *ctx);
	void *ctx;
	// The frame in hand, from its head
	int16_t cur_lich;
	int8_t cur_errs;
	uint8_t cur_sacch[NXDN_SACCH_BYTES];
	int16_t last_lich;	// the frame before (-1 none)
	uint8_t coasted;	// frames in a row placed without a LICH
	nxdn_fec_work_t work;	// the decoders' memory (the call layer's too)
	uint32_t ran_rejects;	// frames of another RAN dropped
} nxdn_rx_t;

extern const air_mode_t nxdn_air_mode;		// 4800 bit/s (2400 symbols/s, 6.25 kHz channel)
extern const air_mode_t nxdn_air_mode_9600;	// 9600 bit/s (4800 symbols/s, 12.5 kHz channel)

void nxdn_rx_init(nxdn_rx_t *rx, uint8_t ran, void (*frame)(const nxdn_rx_frame_t *f, void *ctx), void (*lost)(void *ctx),
		  void *ctx);

// ── A call ──
typedef struct {
	uint8_t ran;		// of the frame that opened it
	bool late;		// joined after its first frame: the fields came from the SACCH superframe
	bool have_vcall;	// the fields below are in
	bool iv;		// a VCALL_IV was seen (DES or AES)
	nxdn_vcall_t vcall;
	int16_t rssi_dbm;
#if CONFIG_NXDN_OTA_ALIAS
	char alias[NXDN_ALIAS_LEN + 1];
#endif
} nxdn_call_t;

typedef struct {
	void (*start)(const nxdn_call_t *c, void *ctx);
	void (*update)(const nxdn_call_t *c, void *ctx);	// a field changed
	void (*voice)(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx);
	void (*end)(void *ctx);
} nxdn_call_sink_t;

typedef struct {
	uint32_t calls, frames, lich_fail, sacch_fail, facch_fail;
	uint32_t voice;		// VCH handed on
	uint32_t voice_held;	// VCH of a call whose VCALL was not in yet (late entry), or encrypted: not handed on
	uint32_t fec_bits;	// bits the SACCH and FACCH1 decoders corrected
} nxdn_call_stats_t;

#if CONFIG_NXDN_OTA_ALIAS
typedef struct {
	uint8_t data[NXDN_ALIAS_LEN];
	uint8_t next;
	bool ready;
} nxdn_alias_rx_t;
#endif

typedef struct {
	nxdn_rx_t rx;			// first: the framer's context is the call's
	const nxdn_call_sink_t *sink;
	void *ctx;
	bool active;
	nxdn_call_t call;
#if CONFIG_NXDN_OTA_ALIAS
	nxdn_alias_rx_t facch_alias, sacch_alias;
	bool seen_superframe;
#endif
	// The superframe's SACCH message as it comes in, one part a frame
	uint8_t sf[NXDN_SACCH_MSG_BYTES];
	uint8_t sf_parts;		// bit k: part k is in
	int8_t sf_next;			// the part expected next, -1 none
	uint16_t scramble_key;		// zero disables NXDN scramble decryption
	nxdn_call_stats_t stats;
} nxdn_call_rx_t;

void nxdn_call_rx_init(nxdn_call_rx_t *c, uint8_t ran, const nxdn_call_sink_t *sink, void *ctx);

// Transmit: a call's frames, built one at a time and released as they fill. ready: symbols from `first` that can
// go now; sent: n of them went
typedef struct {
	uint8_t state;
	bool ending;
	bool rate9600;		// 9600 bit/s, EHR: every second frame is FACCH1 (TS 1-B 4.1.2.1)
	uint8_t ran, dir;
	uint8_t sf_frame;	// frame of the superframe, 0..3
	uint8_t nvoice;		// VCH filled in the frame in hand
	uint16_t scramble_key;	// zero disables scrambling
	uint16_t nsyms, ready, sent;
	uint8_t msg[NXDN_FACCH1_BYTES];	// VCALL, as FACCH1 and SACCH carry it
	uint8_t rel[NXDN_FACCH1_BYTES];	// TX_REL
#if CONFIG_NXDN_OTA_ALIAS
	char alias[NXDN_ALIAS_LEN];
	uint8_t alias_frames;	// the two FACCH1 frames immediately after the header
	uint8_t alias_sf;	// 0 off; 1..8 alternate VCALL and the four SACCH alias messages
#endif
	uint8_t frame[NXDN_FRAME_BYTES];
} nxdn_call_tx_t;

void nxdn_call_tx_begin(nxdn_call_tx_t *t, const nxdn_vcall_t *v, uint8_t ran, uint8_t dir, bool rate9600);
#if CONFIG_NXDN_OTA_ALIAS
// Set after begin, before the preamble is queued. An empty alias leaves all voice slots available as usual.
void nxdn_call_tx_alias(nxdn_call_tx_t *t, const char *alias);
#endif
uint16_t nxdn_call_tx_ready(const nxdn_call_tx_t *t, uint16_t *first);
const uint8_t *nxdn_call_tx_syms(const nxdn_call_tx_t *t);
void nxdn_call_tx_sent(nxdn_call_tx_t *t, uint16_t n);
bool nxdn_call_tx_wants_voice(const nxdn_call_tx_t *t);
// False if the call cannot accept a voice frame or scrambling rejects its FEC.
bool nxdn_call_tx_voice(nxdn_call_tx_t *t, const uint8_t ehr[NXDN_EHR_BYTES]);
// The operator let go: the frame in hand is filled with silence (one without voice yet is not sent), then the
// release frame
void nxdn_call_tx_end(nxdn_call_tx_t *t, const uint8_t silence[NXDN_EHR_BYTES]);
bool nxdn_call_tx_done(const nxdn_call_tx_t *t);

#endif
