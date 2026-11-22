// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV framing (docs/spec/dstar-modem.md 3, JARL D-STAR standard 4.1 and chapter 6): the radio header's fields,
// the transmit bit stream (preamble, frame sync, coded header, 20 ms voice frames of 72 voice and 24 data bits with
// the resync pattern every 21st frame, the end pattern), the slow data in the data frames (the 20-character message
// and the header repeated, both ways) and the receive deframer that takes the demodulator's soft bits back to
// headers, voice frames and messages. Bits are in air order (dstar_fec.h). No RTOS, no board: the transmitter is
// pulled one bit at a time by the modulator, the receiver pushed one soft bit at a time by the demodulator.
#ifndef RFOUNDRY_DSTAR_FRAME_H
#define RFOUNDRY_DSTAR_FRAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dstar/dstar_fec.h"
#include "dstar/dstar_voice.h"

#define DSTAR_CALL_LEN		8u
#define DSTAR_SUFFIX_LEN	4u
#define DSTAR_MSG_LEN		20u	// the slow data message (standard 6.3): four blocks of five characters
#define DSTAR_FRAME_BITS	(DSTAR_VOICE_BITS + DSTAR_DATA_BITS)	// 96 bits, 20 ms
#define DSTAR_SUPERFRAME	21u	// frames from one resync pattern to the next (420 ms)
#define DSTAR_FRAME_SYNC_BITS	15u
#define DSTAR_END_BITS		48u	// 32 bits of 1010 and the 15-bit end pattern and a 0 (standard 4.1.2 h)
#define DSTAR_PREAMBLE_MIN	64u	// the standard's bit sync (4.1.1 a)

// Flag 1 (standard 4.1.1 c)
enum {
	DSTAR_FLAG1_DATA	= 0x80u,	// 0: voice
	DSTAR_FLAG1_REPEATER	= 0x40u,	// 0: direct between terminals
	DSTAR_FLAG1_BREAK_IN	= 0x20u,
	DSTAR_FLAG1_CONTROL	= 0x10u,
	DSTAR_FLAG1_EMERGENCY	= 0x08u,
	DSTAR_FLAG1_FUNC_MASK	= 0x07u,	// repeater control, auto reply, resend request, ACK, no reply, unrelayable, null
};

// The header's fields as on air: callsigns are ASCII padded with spaces, not terminated
typedef struct {
	uint8_t flag[3];
	char rpt2[DSTAR_CALL_LEN];	// destination repeater (sent first)
	char rpt1[DSTAR_CALL_LEN];	// departure repeater
	char ur[DSTAR_CALL_LEN];	// the station called ("CQCQCQ  ")
	char my[DSTAR_CALL_LEN];
	char suffix[DSTAR_SUFFIX_LEN];
} dstar_header_t;

// Fill h from C strings (cut to length, padded with spaces). NULL strings give spaces
void dstar_header_set(dstar_header_t *h, uint8_t flag1, const char *rpt2, const char *rpt1, const char *ur,
		      const char *my, const char *suffix);
// The 41 bytes with their CRC, and back (false on a bad CRC; h is filled either way)
void dstar_header_pack(const dstar_header_t *h, uint8_t out[DSTAR_HEADER_BYTES]);
bool dstar_header_unpack(const uint8_t in[DSTAR_HEADER_BYTES], dstar_header_t *h);
// A header field or the message as received, as text to show: n bytes, the trailing spaces and NULs cut, every byte
// outside printable ASCII a '?'. The standard's fields are ASCII; a byte outside it is a bit error or some radio's own
// character set, never a letter we could name, and a control byte would reach the text layer as one of its own codes
// (core/ui/font.h: glyph bytes, text alternates). out holds n + 1 bytes
void dstar_field_text(char *out, const char *field, size_t n);

// --- slow data (standard chapter 6) ---

// Two data frames, from an even position after the resync frame (frames 1-2, 3-4, ... 19-20 of a superframe whose
// frame 0 carries the resync pattern), join into one six-byte block whose first byte, the mini header, says what the
// other five carry
#define DSTAR_SLOW_BLOCK	6u
#define DSTAR_SLOW_BLOCKS	10u	// per superframe
enum {
	DSTAR_MINI_MESSAGE	= 0x40u,	// low nibble: block 0-3 of the message, five characters each
	DSTAR_MINI_HEADER	= 0x50u,	// low nibble: 1-5 bytes of the 41 header bytes, in order from the resync frame
	DSTAR_MINI_NONE		= 0x66u,	// no data: the whole block is 0x66
};

// What the transmitter puts in the data frames. Even superframes carry the message (blocks 0-3, the rest empty) when
// there is one; the others repeat the header (nine blocks: eight of five bytes and one of one), so a receiver that
// missed the header has it within 840 ms. A real radio's schedule differs; the receiver below doesn't depend on it
typedef struct {
	uint8_t header[DSTAR_HEADER_BYTES];
	char msg[DSTAR_MSG_LEN];
	bool has_msg;
	uint8_t block[DSTAR_SLOW_BLOCK];	// the block being sent
} dstar_slow_tx_t;

void dstar_slow_tx_init(dstar_slow_tx_t *s, const uint8_t header[DSTAR_HEADER_BYTES], const char *msg);
// The three unscrambled bytes of the data frame at frame (1-20) of superframe sf
void dstar_slow_tx_frame(dstar_slow_tx_t *s, uint32_t sf, uint8_t frame, uint8_t out[3]);

typedef struct {
	uint8_t half[3];		// the first data frame of a block
	bool have_half;
	char msg[DSTAR_MSG_LEN];
	uint8_t msg_blocks;		// bit n: message block n received
	uint8_t hdr[DSTAR_HEADER_BYTES];
	uint8_t hdr_len;		// header bytes collected since the resync frame
	bool hdr_bad;			// a block went missing in this superframe: wait for the next
} dstar_slow_rx_t;

typedef enum {
	DSTAR_SLOW_NOTHING = 0,
	DSTAR_SLOW_MESSAGE,		// s->msg holds a whole message (all four blocks seen)
	DSTAR_SLOW_HEADER,		// s->hdr holds 41 header bytes whose CRC checks
} dstar_slow_event_t;

void dstar_slow_rx_init(dstar_slow_rx_t *s);
// The descrambled data frame at frame (0-20; 0 is the resync frame, which starts the block count again); ok false when
// the frame's bits were lost
dstar_slow_event_t dstar_slow_rx_frame(dstar_slow_rx_t *s, uint8_t frame, const uint8_t d[3], bool ok);

// --- transmit ---

typedef enum {
	DSTAR_TX_PREAMBLE = 0,
	DSTAR_TX_SYNC,
	DSTAR_TX_HEADER,
	DSTAR_TX_VOICE,
	DSTAR_TX_END,
	DSTAR_TX_DONE,
} dstar_tx_state_t;

// The transmitter's voice frames: the next 72-bit frame (nine bytes, air order). Return false when the speaker has
// stopped (PTT released): the transmitter finishes with the end pattern
typedef bool (*dstar_voice_pull_t)(void *ctx, uint8_t frame[DSTAR_VOICE_BYTES]);

typedef struct {
	dstar_tx_state_t state;
	uint32_t bit;			// bit within the state
	uint32_t frames;		// voice frames sent
	uint16_t preamble;		// bits of 1010 before the frame sync
	uint8_t air[DSTAR_HEADER_AIR_BYTES];	// the coded header, then the current voice and data frame
	dstar_slow_tx_t slow;
	dstar_voice_pull_t pull;
	void *ctx;
} dstar_tx_t;

// preamble_bits below DSTAR_PREAMBLE_MIN is raised to it. msg may be NULL
void dstar_tx_init(dstar_tx_t *t, const dstar_header_t *h, const char *msg, uint16_t preamble_bits,
		   dstar_voice_pull_t pull, void *ctx);
// The next air bit: 0 or 1, or -1 once the end pattern has gone
int dstar_tx_bit(dstar_tx_t *t);

// --- receive ---

typedef enum {
	DSTAR_END_PATTERN = 0,	// the end pattern was heard
	DSTAR_END_LOST,		// the resync pattern stopped coming
} dstar_end_t;

typedef struct {
	// A header: decoded from the coded header (from_slow false) or rebuilt from the slow data (true), CRC good
	void (*header)(void *ctx, const dstar_header_t *h, bool from_slow);
	// A voice frame, frame = its position 0-20 in the superframe
	void (*voice)(void *ctx, const uint8_t v[DSTAR_VOICE_BYTES], uint8_t frame);
	void (*message)(void *ctx, const char msg[DSTAR_MSG_LEN]);
	void (*end)(void *ctx, dstar_end_t why);
	void *ctx;
} dstar_rx_cb_t;

typedef enum {
	DSTAR_RX_SEARCH = 0,
	DSTAR_RX_HEADER,
	DSTAR_RX_VOICE,
} dstar_rx_state_t;

typedef struct {
	dstar_rx_state_t state;
	int8_t polarity;		// +1, or -1 when the sync was found inverted
	uint64_t win;			// the last 64 hard decisions (before the polarity), newest in bit 0
	uint32_t bit;			// bit within the header or the frame
	uint8_t frame;			// frame within the superframe
	uint8_t sync_miss;		// resync patterns missed in a row
	bool header_seen;		// a header was delivered for this transmission
	bool confirmed;			// locked by a header, or by a second resync pattern after a late entry
	uint8_t cur[DSTAR_FRAME_BITS / 8u];	// the frame being received (voice then data)
	uint8_t soft[DSTAR_HEADER_SOFT_BYTES];	// the coded header being received
	dstar_slow_rx_t slow;
	dstar_rx_cb_t cb;
	// counters: late_entries counts the confirmed ones
	uint32_t headers, headers_bad, slow_headers, frames, syncs, sync_misses, late_entries, ends, losts;
	uint16_t last_corrected;	// the last coded header's corrected bits
} dstar_rx_t;

void dstar_rx_init(dstar_rx_t *r, const dstar_rx_cb_t *cb);
// One soft bit from the demodulator: positive 1, negative 0, magnitude the confidence. soft_unit is the magnitude of
// a clean bit; the header decoder's soft values are soft x DSTAR_SOFT_MAX / soft_unit, clipped
void dstar_rx_bit(dstar_rx_t *r, int16_t soft, int16_t soft_unit);
// True while the deframer is locked to a transmission (the demodulator slows its loops down then)
static inline bool dstar_rx_locked(const dstar_rx_t *r)
{
	return r->state != DSTAR_RX_SEARCH;
}

// The patterns, air order (for tests and for a demodulator that wants to look for them itself)
extern const uint8_t dstar_frame_sync[2];	// 15 bits
extern const uint8_t dstar_resync[3];		// 24 bits
extern const uint8_t dstar_end[6];		// 48 bits

#endif
