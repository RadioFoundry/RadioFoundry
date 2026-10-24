// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// WebSocket (RFC 6455) pieces for the HTTP server's /ws endpoint (docs/spec/tcp-services.md 5): the opening
// handshake's accept value, and the frame header as a server reads and writes it. No state and no I/O: the caller owns
// the connection. A server's rules: frames from the client are masked, frames to it are not (5.1); no extensions, so
// the RSV bits are 0 (5.2); no fragmented messages (refused here, which 5.4 allows a server to do by closing).
#ifndef ODR_WS_H
#define ODR_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_ACCEPT_LEN	28	// base64 of a 20 byte SHA-1
#define WS_KEY_LEN	24	// base64 of the client's 16 byte nonce (4.1)
#define WS_HDR_MAX	14	// 2 + 8 byte length + 4 byte mask
#define WS_CTRL_MAX	125	// a control frame's payload (5.5)

enum {
	WS_OP_CONT = 0x0, WS_OP_TEXT = 0x1, WS_OP_BINARY = 0x2,
	WS_OP_CLOSE = 0x8, WS_OP_PING = 0x9, WS_OP_PONG = 0xA,
};

// Close status codes (7.4.1)
enum {
	WS_CLOSE_NORMAL = 1000, WS_CLOSE_GOING_AWAY = 1001, WS_CLOSE_PROTOCOL = 1002, WS_CLOSE_UNSUPPORTED = 1003,
	WS_CLOSE_TOO_BIG = 1009,
};

typedef struct {
	uint8_t opcode;
	uint8_t hlen;		// header bytes, mask included
	uint8_t mask[4];
	uint32_t len;		// payload bytes
} ws_hdr_t;

// What ws_parse_header found
enum { WS_NEED_MORE = 0, WS_ERR_PROTOCOL = -1, WS_ERR_TOO_BIG = -2 };

// SHA-1 (FIPS 180-4) of len bytes: what the handshake needs, nothing that relies on its strength
void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
// Base64 (RFC 4648 4) of len bytes into out, padded, NUL terminated: out takes 4 * ((len + 2) / 3) + 1 bytes
void ws_base64(const uint8_t *data, size_t len, char *out);
// The Sec-WebSocket-Accept value for a Sec-WebSocket-Key (4.2.2): false when the key is not 24 base64 characters
bool ws_accept(const char *key, size_t key_len, char out[WS_ACCEPT_LEN + 1]);

// A client frame's header from the first n bytes at p: its length (h filled in), WS_NEED_MORE, or an error: not
// masked, RSV bits set, an unknown opcode, a fragment, a control frame over 125 bytes, a length over max
// (WS_ERR_TOO_BIG, close with 1009)
int ws_parse_header(const uint8_t *p, size_t n, uint32_t max, ws_hdr_t *h);
// Unmask n payload bytes in place; at is their offset in the payload (for a payload taken in pieces)
void ws_unmask(uint8_t *p, size_t n, const uint8_t mask[4], uint32_t at);
// A server frame's header (final, unmasked) for len payload bytes: its length, at most 10
int ws_frame_header(uint8_t *out, uint8_t opcode, uint32_t len);

#endif
