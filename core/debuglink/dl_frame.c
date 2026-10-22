// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The host link's framing and dispatch (see dl_frame.h)
#include "debuglink/dl_frame.h"

enum { S_SOF1, S_SOF2, S_CMD, S_SEQ, S_LEN0, S_LEN1, S_DATA, S_CRC0, S_CRC1 };

uint16_t dl_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
	while (n-- > 0) {
		crc ^= (uint16_t)(*p++ << 8);
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

void dl_parser_init(dl_parser_t *p)
{
	p->state = S_SOF1;
	p->len = p->got = 0;
	p->frames = p->bad_crc = p->too_long = p->timeouts = 0;
	p->reply_cap = DL_REPLY_MAX;
}

static void reset(dl_parser_t *p)
{
	p->state = S_SOF1;
	p->len = p->got = 0;
}

bool dl_parser_in_frame(dl_parser_t *p, uint32_t now_ms)
{
	if (p->state == S_SOF1) {
		return false;
	}
	if (now_ms - p->last_ms > DL_FRAME_TIMEOUT_MS) {
		p->timeouts++;
		reset(p);
		return false;
	}
	return true;
}

size_t dl_frame(uint8_t *buf, uint8_t cmd, uint8_t seq, uint16_t len)
{
	buf[0] = DL_SOF1;
	buf[1] = DL_SOF2;
	buf[2] = cmd;
	buf[3] = seq;
	buf[4] = (uint8_t)len;
	buf[5] = (uint8_t)(len >> 8);
	uint16_t crc = dl_crc16(DL_CRC_INIT, buf + 2, (size_t)len + 4u);
	buf[DL_HDR + len] = (uint8_t)crc;
	buf[DL_HDR + len + 1u] = (uint8_t)(crc >> 8);
	return DL_HDR + len + 2u;
}

static void send_error(dl_parser_t *p, uint8_t status, dl_write_fn write, uint8_t *reply)
{
	reply[DL_HDR] = status;
	reply[DL_HDR + 1] = p->cmd;	// which request it was, as far as it got
	write(reply, dl_frame(reply, DL_CMD_ERROR, p->seq, 2));
}

size_t dl_parser_feed(dl_parser_t *p, const uint8_t *buf, size_t len, uint32_t now_ms, dl_handler_fn handler,
		      dl_write_fn write, uint8_t *reply_buf)
{
	(void)dl_parser_in_frame(p, now_ms);	// drop a stale partial frame first
	for (size_t i = 0; i < len; i++) {
		uint8_t b = buf[i];

		p->last_ms = now_ms;
		switch (p->state) {
		case S_SOF1:
			if (b != DL_SOF1) {
				return i;
			}
			p->state = S_SOF2;
			break;
		case S_SOF2:
			if (b != DL_SOF2) {
				reset(p);
				return i;	// not ours after all; this byte is looked at again by the caller
			}
			p->state = S_CMD;
			break;
		case S_CMD:
			p->cmd = b;
			p->crc = dl_crc16(DL_CRC_INIT, &b, 1);
			p->state = S_SEQ;
			break;
		case S_SEQ:
			p->seq = b;
			p->crc = dl_crc16(p->crc, &b, 1);
			p->state = S_LEN0;
			break;
		case S_LEN0:
			p->len = b;
			p->crc = dl_crc16(p->crc, &b, 1);
			p->state = S_LEN1;
			break;
		case S_LEN1:
			p->len |= (uint16_t)(b << 8);
			p->crc = dl_crc16(p->crc, &b, 1);
			if (p->len > DL_REQ_MAX) {
				// Answer and resynchronize on what follows (not skipped: it may be another protocol's bytes)
				p->too_long++;
				send_error(p, DL_ST_TOO_LONG, write, reply_buf);
				reset(p);
				return i + 1;
			}
			p->got = 0;
			p->state = p->len > 0 ? S_DATA : S_CRC0;
			break;
		case S_DATA:
			p->req[p->got++] = b;
			p->crc = dl_crc16(p->crc, &b, 1);
			if (p->got == p->len) {
				p->state = S_CRC0;
			}
			break;
		case S_CRC0:
			p->got = b;	// low byte kept here
			p->state = S_CRC1;
			break;
		case S_CRC1: {
			uint16_t rx = (uint16_t)(p->got | (b << 8));
			if (rx != p->crc) {
				p->bad_crc++;
				send_error(p, DL_ST_BAD_CRC, write, reply_buf);
			} else {
				p->frames++;
				size_t n = handler(p->cmd, p->req, p->len, reply_buf + DL_HDR, p->reply_cap);
				if (n == 0) {
					reply_buf[DL_HDR] = DL_ST_UNKNOWN_CMD;
					n = 1;
				}
				if (n > p->reply_cap) {
					n = p->reply_cap;
				}
				write(reply_buf, dl_frame(reply_buf, (uint8_t)(p->cmd | DL_REPLY_FLAG), p->seq, (uint16_t)n));
			}
			reset(p);
			return i + 1;	// one frame per call: the caller looks at what follows
		}
		default:
			reset(p);
			return i;
		}
	}
	return len;
}
