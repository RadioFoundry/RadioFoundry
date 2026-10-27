// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The link to a second receiver on another SoC (over a UART): RadioFoundry's own protocol, RadioFoundry on both ends
// (docs/spec/rx2-link.md). Portable: the board moves the bytes and calls the poll functions; nothing here waits or
// calls the RTOS, so the host tests run both ends back to back.
//
// Frame: A5 5A len type seq payload[len] crc16, crc16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over len, type,
// seq and the payload, big endian. len <= RX2_PAYLOAD_MAX. A bad frame is dropped; the parser resynchronizes on the
// next A5 5A.
#ifndef RX2_LINK_H
#define RX2_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_rx2.h"

#define RX2_PROTO_VERSION	1u
#define RX2_PAYLOAD_MAX		96u
#define RX2_FRAME_MAX		(RX2_PAYLOAD_MAX + 7u)
#define RX2_SYNC0		0xA5u
#define RX2_SYNC1		0x5Au

// Message types. Main (A) to the receiver (B) below 0x80, the receiver's above
enum {
	RX2_HELLO_REQ	= 0x01,	// no payload: answer with RX2_HELLO
	RX2_TUNE	= 0x02,	// rx2_tune_t, packed (rx2_pack_tune)
	RX2_STOP	= 0x03,	// no payload
	RX2_AUDIO	= 0x04,	// u8 on, u8 volume percent
	RX2_STUB	= 0x05,	// u32 magic RX2_STUB_MAGIC: enter the flash stub on the link UART (stub/README.md)
	RX2_RESET	= 0x06,	// u32 magic RX2_STUB_MAGIC: restart
	RX2_LED		= 0x07,	// u8: the receiver's LEDs that are on (bit 0 LED 1, bit 1 LED 2)
	RX2_DL		= 0x08,	// u8 tag, u8 part, a debug link request: command u8, its payload (rx2-link.md 2.3)
	RX2_HELLO	= 0x81,	// u8 protocol version, u8 flags, text: the firmware version
	RX2_STATUS	= 0x82,	// rx2 status, packed (rx2_pack_status)
	RX2_ACK		= 0x83,	// u8 seq acknowledged, u8 result (0 ok, else refused)
	RX2_LOG		= 0x84,	// text, for the main SoC's log
	RX2_DL_REPLY	= 0x85,	// u8 tag, u8 part, a piece of the debug link reply payload (status first)
};
#define RX2_STUB_MAGIC		0x53545542u	// "STUB"

// HELLO flags
#define RX2_HELLO_DMR		0x01u	// the receiver decodes DMR (codec merged: also its voice)

// The debug link through the main SoC (rx2-link.md 2.3)
#define RX2_DL_LAST		0x80u	// part: the last one; bits 0-6 its index
#define RX2_DL_PIECE		(RX2_PAYLOAD_MAX - 2u)	// reply bytes a part carries
#define RX2_DL_REQ_MAX		RX2_DL_PIECE		// a request: command and payload, one part
#define RX2_DL_REPLY_MAX	1023u	// a reply payload, status included (the main SoC's reply adds its own status)
#define RX2_DL_TIMEOUT_MS	1000u
enum { RX2_DL_IDLE = 0, RX2_DL_WAIT, RX2_DL_DONE, RX2_DL_FAILED };

// --- Frames ---
// Build a frame into out (RX2_FRAME_MAX bytes). Returns its length, 0 when len is too long
uint16_t rx2_frame(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len);
uint16_t rx2_crc16(const uint8_t *p, uint16_t n, uint16_t crc);

// Byte-at-a-time frame parser
typedef struct {
	uint8_t st;		// state
	uint8_t len, type, seq;
	uint16_t at;
	uint16_t crc;
	uint8_t buf[RX2_PAYLOAD_MAX + 2];
	uint32_t bad;		// frames dropped (CRC, length)
} rx2_parser_t;
void rx2_parser_init(rx2_parser_t *p);
// True when b completed a good frame: its type, seq, payload and length are in p (valid until the next byte)
bool rx2_parse(rx2_parser_t *p, uint8_t b);

// --- Payloads (little endian, fixed sizes) ---
#define RX2_TUNE_FIXED		25u
uint8_t rx2_pack_tune(const rx2_tune_t *t, uint8_t *out);	// RX2_TUNE_FIXED + 4 * group_count bytes
bool rx2_unpack_tune(const uint8_t *p, uint8_t len, rx2_tune_t *t);
#define RX2_STATUS_LEN		13u
uint8_t rx2_pack_status(const rx2_status_t *st, uint8_t *out);
bool rx2_unpack_status(const uint8_t *p, uint8_t len, rx2_status_t *st);

// --- Main side (A): a hal_rx2_t over the link ---
typedef void (*rx2_send_fn)(const uint8_t *buf, uint16_t len);
#define RX2_ALIVE_MS		1500u	// no frame from the receiver this long: it's down (it reports at least every 500 ms)
#define RX2_HELLO_EVERY_MS	1000u	// while down, ask
#define RX2_RETUNE_MS		300u	// a TUNE not acknowledged this long goes again

typedef struct {
	rx2_send_fn send;
	rx2_parser_t parser;
	uint8_t seq;
	bool up;
	uint8_t version;	// its RX2_HELLO: protocol version and flags
	uint8_t flags;
	char fw[24];		// its firmware version text
	uint32_t heard_ms;	// last good frame
	uint32_t asked_ms;	// last HELLO_REQ
	bool want_rx;		// the core wants it receiving (tune() since the last stop())
	bool tune_acked;
	uint8_t tune_seq;
	uint32_t tune_sent_ms;
	rx2_tune_t tune;
	bool audio;		// the core gives it the speaker
	uint8_t volume;
	uint8_t leds;		// RX2_LED: the main SoC's status LEDs, which are on the receiver's SoC
	rx2_status_t st;	// its last STATUS
	uint32_t frames, tunes, resent;
	// The debug link request in flight (rx2_main_dl_send): its tag, RX2_DL_* state, the next part expected, the
	// reply so far
	uint8_t dl_tag, dl_state, dl_next;
	uint16_t dl_len;
	uint32_t dl_sent_ms;
	uint32_t dl_requests, dl_failed;
	uint8_t dl_reply[RX2_DL_REPLY_MAX];
} rx2_main_t;

void rx2_main_init(rx2_main_t *m, rx2_send_fn send);
// Each byte the link UART received
void rx2_main_rx(rx2_main_t *m, uint8_t b, uint32_t now_ms);
// Every 10-100 ms: liveness, HELLO while down, TUNE resends, the settings again after the receiver came back
void rx2_main_poll(rx2_main_t *m, uint32_t now_ms);
// What hal_rx2_t's calls do (the board's ops wrap these with its lock and clock)
bool rx2_main_tune(rx2_main_t *m, const rx2_tune_t *t, uint32_t now_ms);
void rx2_main_stop(rx2_main_t *m);
void rx2_main_status(const rx2_main_t *m, rx2_status_t *st);
void rx2_main_audio(rx2_main_t *m, bool on, uint8_t volume);
// The LEDs on the receiver's SoC (RX2_LED bits): sent on a change and again when it comes back
void rx2_main_led(rx2_main_t *m, uint8_t leds);
// Send RX2_STUB or RX2_RESET (the receiver's update route through the main SoC)
void rx2_main_command(rx2_main_t *m, uint8_t type);
// A debug link request to the receiver: command, payload (len <= RX2_DL_REQ_MAX). False when it is down or the request
// too long. A request still in flight is dropped. The reply arrives through rx2_main_rx: dl_state goes to
// RX2_DL_DONE (dl_reply, dl_len) or RX2_DL_FAILED (a part missing, too long, or nothing within RX2_DL_TIMEOUT_MS,
// which rx2_main_poll checks)
bool rx2_main_dl_send(rx2_main_t *m, const uint8_t *req, uint8_t len, uint32_t now_ms);

// --- The update bridge's exit sequence (rx2-link.md 4.1) ---
// The host's bytes, with the time each was read: a burst of exactly RX2_EXIT_SEQ, with RX2_EXIT_GUARD_US of quiet
// before and after it, ends the bridge. Bytes that only start like it are held back until they can't be it, then
// passed on in order, so nothing else is lost or reordered
#define RX2_EXIT_GUARD_US	2000u
#define RX2_EXIT_LEN		4u
extern const uint8_t rx2_exit_seq[RX2_EXIT_LEN];
typedef struct {
	uint8_t held;		// bytes of the sequence matched in this burst, not yet passed on
	bool ok;		// this burst can still be the sequence
	uint32_t last_us;	// the last byte's time
} rx2_exit_t;
void rx2_exit_init(rx2_exit_t *x, uint32_t now_us);
// A byte from the host: the bytes to pass on now go into out (at most RX2_EXIT_LEN + 1); returns how many
uint8_t rx2_exit_byte(rx2_exit_t *x, uint8_t c, uint32_t now_us, uint8_t *out);
// No byte now: true when the whole sequence came and the guard has passed. Otherwise *n held bytes of a burst that
// ended short go into out (at most RX2_EXIT_LEN), to pass on
bool rx2_exit_idle(rx2_exit_t *x, uint32_t now_us, uint8_t *out, uint8_t *n);

// --- Receiver side (B) ---
typedef struct {
	bool (*tune)(const rx2_tune_t *t);	// false: refused
	void (*stop)(void);
	void (*audio)(bool on, uint8_t volume);
	void (*status)(rx2_status_t *st);	// the receiver's state now (up, tuned and heard are filled in here)
	void (*stub)(void);			// RX2_STUB: never returns on the radio
	void (*reset)(void);			// RX2_RESET
	void (*led)(uint8_t leds);		// RX2_LED; NULL: no LEDs
	// RX2_DL: one debug link command (cmd, its payload) into out, status first, at most cap bytes; returns the
	// length (>= 1). NULL: every command is answered DL_ST_UNKNOWN_CMD
	size_t (*dl)(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
} rx2_ops_t;

#define RX2_STATUS_EVERY_MS	500u

typedef struct {
	rx2_send_fn send;
	const rx2_ops_t *ops;
	rx2_parser_t parser;
	uint8_t seq;
	uint8_t flags;		// RX2_HELLO flags
	const char *fw;
	bool tuned, audio;
	rx2_status_t last;	// last STATUS sent
	uint32_t sent_ms;
	uint32_t frames;
	uint32_t dl_requests;
	uint8_t dl_out[RX2_DL_REPLY_MAX];
} rx2_recv_t;

void rx2_recv_init(rx2_recv_t *r, rx2_send_fn send, const rx2_ops_t *ops, uint8_t flags, const char *fw);
void rx2_recv_rx(rx2_recv_t *r, uint8_t b, uint32_t now_ms);
// Every 10-50 ms: a STATUS on any change, and at least every RX2_STATUS_EVERY_MS
void rx2_recv_poll(rx2_recv_t *r, uint32_t now_ms);
// Text to the main SoC's log
void rx2_recv_log(rx2_recv_t *r, const char *text);

#endif
