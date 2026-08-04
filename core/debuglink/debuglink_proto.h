// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's wire format (docs/spec/debuglink.md): framing, CRC and command dispatch, without the RTOS or the
// board, so the host tests run it. debuglink.c supplies the command handler and the port writer.
//
//   A5 5A cmd seq len_lo len_hi payload[len] crc_lo crc_hi
//
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR) over cmd .. the end of the payload.
// A reply carries cmd | 0x80 and the request's seq; its payload starts with a status byte (DL_ST_*).
#ifndef DEBUGLINK_PROTO_H
#define DEBUGLINK_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DL_SOF1			0xA5u	// never an MMDVM frame (0xE0) nor a legacy CPS request (ASCII letters)
#define DL_SOF2			0x5Au
#define DL_PROTO_VERSION	1u
#define DL_HDR			6u	// SOF1 SOF2 cmd seq len(2)
#define DL_REQ_MAX		64u	// request payload bytes
#define DL_REPLY_MAX		1024u	// reply payload bytes, status included
#define DL_FRAME_TIMEOUT_MS	200u	// a frame left incomplete this long is dropped
#define DL_REPLY_FLAG		0x80u
#define DL_CMD_ERROR		0xFFu	// reply to a request that could not be decoded (bad CRC, too long)

// Commands
enum {
	DL_CMD_PING		= 0x01,
	DL_CMD_VERSION		= 0x02,
	DL_CMD_GET_DIAG		= 0x03,
	DL_CMD_RESET_COUNTERS	= 0x04,
	DL_CMD_SCREENSHOT	= 0x05,
	DL_CMD_KEY		= 0x06,
	DL_CMD_PTT		= 0x07,
	DL_CMD_GET_LOG		= 0x08,
	DL_CMD_GET_VIEW		= 0x09,
	DL_CMD_RADIO		= 0x0A,
	DL_CMD_INFO_CHIP	= 0x0B,
	DL_CMD_READ_MEM		= 0x0C,
	DL_CMD_REBOOT		= 0x0D,
};

// Reply status (the first payload byte)
enum {
	DL_ST_OK		= 0,
	DL_ST_UNKNOWN_CMD	= 1,
	DL_ST_BAD_ARG		= 2,	// wrong length or a value out of range
	DL_ST_BUSY		= 3,	// try again shortly (an injected key still held, rate limit)
	DL_ST_UNAVAILABLE	= 4,	// not built in, or nothing to act on (no display, no such key)
	DL_ST_BAD_CRC		= 5,	// DL_CMD_ERROR only
	DL_ST_TOO_LONG		= 6,	// DL_CMD_ERROR only: request payload over DL_REQ_MAX
	DL_ST_REFUSED		= 7,	// a safety check said no (READ_MEM outside the whitelist, REBOOT gate)
};

uint16_t dl_crc16(uint16_t crc, const uint8_t *p, size_t n);
#define DL_CRC_INIT		0xFFFFu

// One command: the request payload in, the reply payload out (out[0] = status), at most cap bytes. Returns the reply
// payload length (>= 1)
typedef size_t (*dl_handler_fn)(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
// Bytes to the host
typedef void (*dl_write_fn)(const uint8_t *buf, size_t len);

typedef struct {
	uint8_t state;
	uint8_t cmd, seq;
	uint16_t len, got;
	uint16_t crc;
	uint32_t last_ms;
	uint8_t req[DL_REQ_MAX];
	// counters
	uint32_t frames, bad_crc, too_long, timeouts;
} dl_parser_t;

void dl_parser_init(dl_parser_t *p);
// In the middle of a frame (the next bytes belong to it). A frame idle past DL_FRAME_TIMEOUT_MS is dropped here
bool dl_parser_in_frame(dl_parser_t *p, uint32_t now_ms);
// Feed bytes that start a frame (buf[0] == DL_SOF1) or continue one. Takes bytes up to the end of one frame and
// answers it (handler, then write with the reply framed in reply_buf, which must hold DL_HDR + DL_REPLY_MAX + 2).
// Returns the bytes taken; a byte after SOF1 that isn't SOF2 is not taken (it may start something else), so the
// return is 1 then (0 when SOF1 came in the previous chunk). 0 also when len is 0 or buf[0] is not SOF1 outside
// a frame: the caller gives those bytes to someone else
size_t dl_parser_feed(dl_parser_t *p, const uint8_t *buf, size_t len, uint32_t now_ms, dl_handler_fn handler,
		      dl_write_fn write, uint8_t *reply_buf);

// Frame a payload already placed at buf + DL_HDR: header and CRC around it. Returns the frame length
size_t dl_frame(uint8_t *buf, uint8_t cmd, uint8_t seq, uint16_t len);

#endif
