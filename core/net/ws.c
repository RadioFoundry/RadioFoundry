// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// WebSocket handshake and frame headers (RFC 6455; ws.h). SHA-1 from FIPS 180-4 6.1, written for size: one block of
// message schedule kept as a 16 word ring (6.1.3's alternative method).
#include <string.h>
#include "net/ws.h"

#define GUID	"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"	// RFC 6455 1.3

static uint32_t rol(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static uint32_t get32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void sha1_block(uint32_t hs[5], const uint8_t *blk)
{
	uint32_t w[16], a = hs[0], b = hs[1], c = hs[2], d = hs[3], e = hs[4];

	for (int t = 0; t < 16; t++) {
		w[t] = get32be(blk + 4 * t);
	}
	for (int t = 0; t < 80; t++) {
		uint32_t f, k;

		if (t >= 16) {
			w[t & 15] = rol(w[(t + 13) & 15] ^ w[(t + 8) & 15] ^ w[(t + 2) & 15] ^ w[t & 15], 1);
		}
		if (t < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999u;
		} else if (t < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1u;
		} else if (t < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCu;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6u;
		}
		uint32_t tmp = rol(a, 5) + f + e + k + w[t & 15];

		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = tmp;
	}
	hs[0] += a;
	hs[1] += b;
	hs[2] += c;
	hs[3] += d;
	hs[4] += e;
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
	uint32_t hs[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
	uint8_t blk[64];
	size_t i = 0;

	for (; len - i >= 64; i += 64) {
		sha1_block(hs, data + i);
	}
	// The padding (5.1.1): a 1 bit, zeros, the length in bits as 64 bits; a second block when it doesn't fit
	size_t r = len - i;
	uint64_t bits = (uint64_t)len * 8u;

	memset(blk, 0, sizeof(blk));
	memcpy(blk, data + i, r);
	blk[r] = 0x80;
	if (r >= 56) {
		sha1_block(hs, blk);
		memset(blk, 0, sizeof(blk));
	}
	for (int j = 0; j < 8; j++) {
		blk[63 - j] = (uint8_t)(bits >> (8 * j));
	}
	sha1_block(hs, blk);
	for (int j = 0; j < 20; j++) {
		out[j] = (uint8_t)(hs[j / 4] >> (24 - 8 * (j % 4)));
	}
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void ws_base64(const uint8_t *data, size_t len, char *out)
{
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)data[i] << 16;
		size_t n = len - i;

		if (n > 1) {
			v |= (uint32_t)data[i + 1] << 8;
		}
		if (n > 2) {
			v |= data[i + 2];
		}
		*out++ = b64[(v >> 18) & 63];
		*out++ = b64[(v >> 12) & 63];
		*out++ = (n > 1) ? b64[(v >> 6) & 63] : '=';
		*out++ = (n > 2) ? b64[v & 63] : '=';
	}
	*out = '\0';
}

bool ws_accept(const char *key, size_t key_len, char out[WS_ACCEPT_LEN + 1])
{
	uint8_t in[WS_KEY_LEN + sizeof(GUID) - 1], dig[20];

	// 16 bytes in base64: 22 characters and "==" (4.2.1 item 5)
	if ((key_len != WS_KEY_LEN) || (key[22] != '=') || (key[23] != '=')) {
		return false;
	}
	for (int i = 0; i < 22; i++) {
		if ((key[i] == '\0') || (strchr(b64, key[i]) == NULL)) {
			return false;
		}
	}
	memcpy(in, key, WS_KEY_LEN);
	memcpy(in + WS_KEY_LEN, GUID, sizeof(GUID) - 1);
	ws_sha1(in, sizeof(in), dig);
	ws_base64(dig, sizeof(dig), out);
	return true;
}

int ws_parse_header(const uint8_t *p, size_t n, uint32_t max, ws_hdr_t *h)
{
	if (n < 2) {
		return WS_NEED_MORE;
	}
	uint8_t op = p[0] & 0x0F, ln = p[1] & 0x7F;
	bool fin = (p[0] & 0x80) != 0;
	size_t need = 2 + ((ln == 126) ? 2u : (ln == 127) ? 8u : 0u) + 4u;

	if (((p[0] & 0x70) != 0) || ((p[1] & 0x80) == 0)) {
		return WS_ERR_PROTOCOL;		// an extension's bit, or a client frame without a mask (5.1)
	}
	if (!fin || (op == WS_OP_CONT)) {
		return WS_ERR_PROTOCOL;		// fragmented: not taken
	}
	if ((op != WS_OP_TEXT) && (op != WS_OP_BINARY) && (op != WS_OP_CLOSE) && (op != WS_OP_PING) &&
	    (op != WS_OP_PONG)) {
		return WS_ERR_PROTOCOL;
	}
	if ((op >= WS_OP_CLOSE) && (ln > WS_CTRL_MAX)) {
		return WS_ERR_PROTOCOL;
	}
	if (n < need) {
		return WS_NEED_MORE;
	}
	uint32_t len = ln;
	const uint8_t *m = p + 2;

	if (ln == 126) {
		len = ((uint32_t)p[2] << 8) | p[3];
		m = p + 4;
	} else if (ln == 127) {
		if ((p[2] & 0x80) != 0) {
			return WS_ERR_PROTOCOL;	// the most significant bit must be 0 (5.2)
		}
		if ((p[2] | p[3] | p[4] | p[5]) != 0) {
			return WS_ERR_TOO_BIG;
		}
		len = get32be(p + 6);
		m = p + 10;
	}
	if (len > max) {
		return WS_ERR_TOO_BIG;
	}
	h->opcode = op;
	h->hlen = (uint8_t)need;
	h->len = len;
	memcpy(h->mask, m, 4);
	return (int)need;
}

void ws_unmask(uint8_t *p, size_t n, const uint8_t mask[4], uint32_t at)
{
	for (size_t i = 0; i < n; i++) {
		p[i] ^= mask[(at + i) & 3];
	}
}

int ws_frame_header(uint8_t *out, uint8_t opcode, uint32_t len)
{
	out[0] = (uint8_t)(0x80 | (opcode & 0x0F));
	if (len < 126) {
		out[1] = (uint8_t)len;
		return 2;
	}
	if (len <= 0xFFFFu) {
		out[1] = 126;
		out[2] = (uint8_t)(len >> 8);
		out[3] = (uint8_t)len;
		return 4;
	}
	out[1] = 127;
	memset(out + 2, 0, 4);
	out[6] = (uint8_t)(len >> 24);
	out[7] = (uint8_t)(len >> 16);
	out[8] = (uint8_t)(len >> 8);
	out[9] = (uint8_t)len;
	return 10;
}
