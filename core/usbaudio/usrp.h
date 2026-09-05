// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * The "USRP" voice-over-UDP framing that AllStarLink's USRP channel driver, DVSwitch Analog_Bridge and their clients
 * exchange, and the radio side's link state (docs/spec/usb-audio.md 4.2). Hardware independent, host tested
 * (tests/host/usrpTest.c); not built into the firmware yet (phase 2 of the plan).
 *
 * A datagram is a 32-byte header and an optional payload:
 *
 *   0  4  "USRP"
 *   4  4  sequence number, counts up by one per datagram, per sender
 *   8  4  memory (unused, 0)
 *  12  4  keyup: 1 while the sender transmits (PTT / COS), 0 otherwise
 *  16  4  talkgroup (unused by us, 0)
 *  20  4  type (usrp_type_t)
 *  24  4  mpxid (unused, 0)
 *  28  4  reserved (0)
 *  32  .. payload: VOICE = 160 signed 16-bit samples at 8 kHz (20 ms), VOICE_ULAW = 160 G.711 mu-law bytes
 *
 * The header words are big endian; the voice samples are little endian (the byte order every known peer, all on
 * little endian hosts, puts on the wire). Both orders are taken from the public descriptions of the format and must be
 * confirmed against a live peer on the bench (spec 4.2, owner question 6); USRP_SAMPLES_BIG_ENDIAN flips the samples.
 */
#ifndef ODR_USRP_H
#define ODR_USRP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USRP_HEADER_LEN		32
#define USRP_FRAME_SAMPLES	160	// 20 ms at 8 kHz
#define USRP_VOICE_LEN		(USRP_HEADER_LEN + 2 * USRP_FRAME_SAMPLES)	// 352
#define USRP_ULAW_LEN		(USRP_HEADER_LEN + USRP_FRAME_SAMPLES)		// 192
#define USRP_MAX_LEN		USRP_VOICE_LEN

typedef enum {
	USRP_TYPE_VOICE		= 0,
	USRP_TYPE_DTMF		= 1,
	USRP_TYPE_TEXT		= 2,
	USRP_TYPE_PING		= 3,
	USRP_TYPE_TLV		= 4,
	USRP_TYPE_VOICE_ADPCM	= 5,
	USRP_TYPE_VOICE_ULAW	= 6,
} usrp_type_t;

typedef struct {
	uint32_t seq;
	uint32_t memory;
	bool keyup;
	uint32_t talkgroup;
	uint32_t type;
	uint32_t mpxid;
} usrp_header_t;

// G.711 mu-law (ITU-T G.711), 16-bit linear in and out (the 14-bit code range scaled by 4)
uint8_t usrp_ulaw_encode(int16_t pcm);
int16_t usrp_ulaw_decode(uint8_t code);

// One datagram into out. VOICE and VOICE_ULAW take USRP_FRAME_SAMPLES samples of pcm (NULL = silence); other types
// carry no payload. Returns the length, 0 if cap is too small
size_t usrp_pack(uint8_t *out, size_t cap, const usrp_header_t *h, const int16_t *pcm);

typedef enum {
	USRP_PARSE_OK,		// header valid; *samples is USRP_FRAME_SAMPLES for voice, 0 for anything else
	USRP_PARSE_SHORT,	// shorter than a header
	USRP_PARSE_MAGIC,	// not "USRP"
	USRP_PARSE_LENGTH,	// a voice type with the wrong payload length
} usrp_parse_t;

// A datagram from a peer. pcm (USRP_FRAME_SAMPLES) gets the samples of VOICE and VOICE_ULAW; may be NULL
usrp_parse_t usrp_parse(const uint8_t *buf, size_t len, usrp_header_t *h, int16_t *pcm, size_t *samples);

// --- the radio's side of a USRP link: host PTT with its watchdog, sequence accounting ---
//
// Keying follows the peer's keyup flag, with three safety nets (spec 6.3): the peer must keep sending keyup frames
// (keyup_timeout_ms without one unkeys: a crashed or unplugged host can't leave the radio transmitting), a hard
// tx_max_ms per transmission (after which the link stays unkeyed until the peer sends keyup 0 or goes quiet for
// keyup_timeout_ms), and the radio's own TOT, which is the radio core's business and not modeled here.

typedef struct {
	uint32_t keyup_timeout_ms;	// default USRP_KEYUP_TIMEOUT_MS
	uint32_t tx_max_ms;		// default USRP_TX_MAX_MS; 0 = no limit here
} usrp_link_cfg_t;

#define USRP_KEYUP_TIMEOUT_MS	300u	// 15 frames: generous for USB and Wi-Fi jitter, short enough to be safe
#define USRP_TX_MAX_MS		180000u

typedef enum {
	USRP_EV_NONE,
	USRP_EV_KEY,		// start transmitting
	USRP_EV_UNKEY,		// the peer sent keyup 0
	USRP_EV_TIMEOUT,	// no keyup frame for keyup_timeout_ms: unkey
	USRP_EV_TX_LIMIT,	// tx_max_ms reached: unkey, locked out until keyup 0 or a quiet gap
} usrp_event_t;

typedef struct {
	uint32_t frames;	// valid datagrams taken
	uint32_t voice;		// of them, voice frames
	uint32_t lost;		// sequence gaps (frames missing)
	uint32_t out_of_order;	// late or repeated sequence numbers (dropped)
	uint32_t bad;		// datagrams that didn't parse
	uint32_t keys;		// USRP_EV_KEY
	uint32_t timeouts;	// USRP_EV_TIMEOUT
	uint32_t tx_limits;	// USRP_EV_TX_LIMIT
} usrp_link_stats_t;

typedef struct {
	usrp_link_cfg_t cfg;
	bool keyed;
	bool locked_out;	// after USRP_EV_TX_LIMIT
	bool have_seq;
	uint32_t next_seq;
	uint32_t last_keyup_ms;	// last frame with keyup 1
	uint32_t last_rx_ms;	// last valid frame
	uint32_t key_ms;	// when the current transmission started
	uint32_t tx_seq;	// our own sequence number toward the peer
	usrp_link_stats_t stats;
} usrp_link_t;

// cfg NULL = the defaults
void usrp_link_init(usrp_link_t *l, const usrp_link_cfg_t *cfg);
// A datagram from the peer at now_ms. *voice (may be NULL) is set when it carried audio to play / transmit, and
// that audio is only meant for the air while keyed. Returns what to do with the transmitter
usrp_event_t usrp_link_rx(usrp_link_t *l, const uint8_t *buf, size_t len, uint32_t now_ms, int16_t *pcm, bool *voice);
// Call every few ms: the watchdogs
usrp_event_t usrp_link_tick(usrp_link_t *l, uint32_t now_ms);
// Our own frame toward the peer: received audio (pcm, NULL = silence) with keyup = cos (the squelch / call state).
// ulaw picks the 192-byte VOICE_ULAW form. Returns the length
size_t usrp_link_tx(usrp_link_t *l, uint8_t *out, size_t cap, const int16_t *pcm, bool cos, bool ulaw);

#endif
