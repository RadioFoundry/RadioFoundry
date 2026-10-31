// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <string.h>
#include "sct_pkt.h"

// Data bytes after the ID in a packet to the chip (the datasheet's control field length less the ID). 0 is a valid
// length (a bare ID: a query, CALL_STOP, DMR_CALL_START), so unknown IDs are marked with UNK
#define UNK	((int8_t)SCT_LEN_UNKNOWN)
#define VAR	((int8_t)SCT_LEN_BYTE)
static const int8_t field_len[128] = {
	// 0x00: CHAN_D has a length byte; 0x08 VOCODE_TONE is variable in the datasheet, 3 in the firmware seen
	[0x00] = 1, [0x01] = VAR, [0x02] = 1, [0x03] = UNK, [0x04] = UNK, [0x05] = 2, [0x06] = 7, [0x07] = 9,
	[0x08] = 3, [0x09] = UNK, [0x0A] = 2, [0x0B] = UNK, [0x0C] = UNK, [0x0D] = UNK, [0x0E] = UNK, [0x0F] = UNK,
	[0x10] = 1, [0x11] = UNK, [0x12] = 1, [0x13] = UNK, [0x14] = 0, [0x15] = 1, [0x16] = 1, [0x17] = 1,
	[0x18] = 3, [0x19] = 1, [0x1A] = 1, [0x1B] = 1, [0x1C] = 2, [0x1D] = 1, [0x1E] = 1, [0x1F] = 17,
	[0x20] = 1, [0x21] = 0, [0x22] = 1, [0x23] = 1, [0x24] = 1, [0x25] = 1, [0x26] = 2, [0x27] = 1,
	[0x28] = 1, [0x29] = 4, [0x2A] = 1, [0x2B] = 1, [0x2C] = 2, [0x2D] = 1, [0x2E] = 1, [0x2F] = 1,
	[0x30] = 0, [0x31] = 0, [0x32] = 0, [0x33] = 0, [0x34] = UNK, [0x35] = UNK, [0x36] = 5, [0x37] = 1,
	[0x38] = UNK, [0x39] = 12, [0x3A] = UNK, [0x3B] = 4, [0x3C] = 11, [0x3D] = 22, [0x3E] = 1, [0x3F] = UNK,
	[0x40] = 10, [0x41] = 5, [0x42] = 9, [0x43] = (int8_t)SCT_LEN_FRAME, [0x44] = 7, [0x45] = UNK, [0x46] = 2,
	[0x47] = 5,
	[0x48] = 15, [0x49] = 9, [0x4A] = 5, [0x4B] = 3, [0x4C] = 6, [0x4D] = 3, [0x4E] = 3, [0x4F] = 6,
	[0x50] = 3, [0x51] = 3, [0x52] = 4, [0x53] = 4, [0x54] = 1, [0x55] = 1, [0x56] = 3, [0x57] = 2,
	[0x58] = UNK, [0x59] = UNK, [0x5A] = 2, [0x5B] = UNK, [0x5C] = 3, [0x5D] = UNK, [0x5E] = UNK, [0x5F] = 0,
	[0x60] = 0, [0x61] = 0, [0x62] = 0, [0x63] = UNK, [0x64] = UNK, [0x65] = UNK, [0x66] = UNK, [0x67] = UNK,
	// 0x6A I2C_OPERATION: 3 for the register writes the firmware sends (variable in the datasheet)
	[0x68] = 2, [0x69] = 2, [0x6A] = 3, [0x6B] = 12, [0x6C] = 12, [0x6D] = 2, [0x6E] = 4, [0x6F] = 1,
	[0x70] = 11, [0x71] = UNK, [0x72] = UNK, [0x73] = 2, [0x74] = 5, [0x75] = 4, [0x76] = 3, [0x77] = 1,
	// 0x78 DMR_CALL_START: the datasheet says 4 bytes, the firmware and the vendor scripts send the bare ID
	[0x78] = 0, [0x79] = 5, [0x7A] = 8, [0x7B] = 1, [0x7C] = 1, [0x7D] = 4, [0x7E] = 4, [0x7F] = 1,
};

int sct_field_len(uint8_t id)
{
	return id < 128 ? field_len[id] : SCT_LEN_UNKNOWN;
}

int sct_report_len(uint8_t id)
{
	switch (id) {
	case SCT_F_Q_PRO_STR:
	case SCT_F_Q_HW_VER:
	case SCT_F_Q_SW_VER:
		return SCT_LEN_BYTE;
	case SCT_F_Q_CID_SN:
		return 8;
	case SCT_F_BER_REPORT:
		return 9;
	default:
		return sct_field_len(id);
	}
}

static void put(sct_pkt_t *p, const uint8_t *data, uint16_t len)
{
	if (p->len + len > SCT_PKT_MAX - 3) {	// room left for the parity pair and the pad
		p->bad = true;
		return;
	}
	if (len != 0)
		memcpy(&p->buf[p->len], data, len);
	p->len = (uint16_t)(p->len + len);
}

void sct_pkt_begin(sct_pkt_t *p, uint8_t type)
{
	p->buf[0] = SCT_SYNC0;
	p->buf[1] = SCT_SYNC1;
	p->buf[2] = SCT_SYNC2;
	p->buf[3] = 0;
	p->buf[4] = 0;
	p->buf[5] = type;
	p->len = SCT_HDR_BYTES;
	p->bad = false;
}

void sct_pkt_add(sct_pkt_t *p, uint8_t id, const uint8_t *data, uint8_t len)
{
	int want = sct_field_len(id);

	if (want == SCT_LEN_BYTE) {
		uint8_t hdr[2] = { id, len };

		put(p, hdr, 2);
	} else if (want == len) {
		put(p, &id, 1);
	} else {
		p->bad = true;
		return;
	}
	put(p, data, len);
}

void sct_pkt_add1(sct_pkt_t *p, uint8_t id, uint8_t value)
{
	sct_pkt_add(p, id, &value, 1);
}

void sct_pkt_add_frame(sct_pkt_t *p, uint8_t prop, const uint8_t *data, uint8_t len)
{
	uint8_t hdr[3] = { SCT_F_DATA_FRAME, prop, len };

	if (len > SCT_FRAME_MAX) {
		p->bad = true;
		return;
	}
	put(p, hdr, 3);
	put(p, data, len);
}

void sct_pkt_add_raw(sct_pkt_t *p, const uint8_t *data, uint8_t len)
{
	put(p, data, len);
}

// Built in place: callers keep a 10-byte buffer, not an sct_pkt_t, on their stack
void sct_pkt_codec(uint8_t out[SCT_CODEC_PKT_BYTES], const sct_codec_write_t *w)
{
	out[0] = SCT_SYNC0;
	out[1] = SCT_SYNC1;
	out[2] = SCT_SYNC2;
	out[3] = 0;
	out[4] = 3;		// LEN: page, register, value
	out[5] = SCT_TYPE_CODEC;
	out[6] = w->page;
	out[7] = w->reg;
	out[8] = w->value;
	out[9] = 0;		// the pad of an odd length
}

uint16_t sct_pkt_end(sct_pkt_t *p, bool parity)
{
	if (p->bad || p->len <= SCT_HDR_BYTES)
		return 0;
	uint16_t body = (uint16_t)(p->len - SCT_HDR_BYTES + (parity ? 2 : 0));

	p->buf[3] = (uint8_t)(body >> 8);
	p->buf[4] = (uint8_t)body;
	if (parity) {
		// XOR of every byte from LEN_hi through the marker
		uint8_t x = SCT_PARITY_MARK;

		for (uint16_t i = 3; i < p->len; i++)
			x ^= p->buf[i];
		p->buf[p->len++] = SCT_PARITY_MARK;
		p->buf[p->len++] = x;
	}
	if (p->len & 1)
		p->buf[p->len++] = 0;
	return p->len;
}

void sct_rx_reset(sct_rx_t *rx)
{
	rx->pos = 0;
	rx->need = 0;
}

bool sct_rx_feed(sct_rx_t *rx, uint8_t b, uint16_t *len)
{
	static const uint8_t sync[3] = { SCT_SYNC0, SCT_SYNC1, SCT_SYNC2 };

	if (rx->pos < 3) {
		if (b == sync[rx->pos]) {
			rx->buf[rx->pos++] = b;
		} else if (b == sync[0]) {
			rx->skipped += rx->pos;	// a stray 84 (or a broken sync) right before the real one
			rx->buf[0] = b;
			rx->pos = 1;
		} else {
			rx->skipped += rx->pos + 1u;
			rx->pos = 0;
		}
		return false;
	}
	rx->buf[rx->pos++] = b;
	if (rx->pos == SCT_HDR_BYTES) {
		uint16_t body = (uint16_t)((rx->buf[3] << 8) | rx->buf[4]);

		if (body > SCT_PKT_MAX - SCT_HDR_BYTES) {
			rx->overruns++;
			sct_rx_reset(rx);
			return false;
		}
		rx->need = (uint16_t)(SCT_HDR_BYTES + body);
	}
	if (rx->pos >= SCT_HDR_BYTES && rx->pos == rx->need) {
		*len = rx->pos;
		sct_rx_reset(rx);
		return true;
	}
	return false;
}

bool sct_fields_begin(sct_fields_t *it, const uint8_t *pkt, uint16_t len, bool parity)
{
	if (len < SCT_HDR_BYTES || pkt[0] != SCT_SYNC0 || pkt[1] != SCT_SYNC1 || pkt[2] != SCT_SYNC2)
		return false;
	uint16_t body = (uint16_t)((pkt[3] << 8) | pkt[4]);

	if (body + SCT_HDR_BYTES > len)
		return false;
	it->type = pkt[5];
	it->p = &pkt[SCT_HDR_BYTES];
	it->left = body;
	if (parity) {
		if (body < 2 || it->p[body - 2] != SCT_PARITY_MARK)
			return false;
		uint8_t x = 0;

		for (uint16_t i = 3; i < SCT_HDR_BYTES + body - 1; i++)
			x ^= pkt[i];
		if (x != pkt[SCT_HDR_BYTES + body - 1])
			return false;
		it->left = (uint16_t)(body - 2);
	}
	return true;
}

bool sct_fields_next(sct_fields_t *it, uint8_t *id, const uint8_t **data, uint8_t *len, bool *ack)
{
	if (it->left == 0)
		return false;
	*id = it->p[0];
	*ack = false;
	int want = sct_report_len(*id);
	uint16_t take;

	// Answers to writes come in packets without the read bit, each field its ID and a status byte (T00 `19 00`). A
	// report packet has the read bit; one there with a single byte left after its ID is an answer too (`20 43 00`)
	if (((it->type & SCT_TYPE_READ) == 0 || (it->left == 2 && want != 1)) && it->left >= 2) {
		*ack = true;
		*data = &it->p[1];
		*len = 1;
		take = 2;
	} else if (want == SCT_LEN_BYTE) {
		if (it->left < 2 || it->p[1] > it->left - 2)
			goto bad;
		*data = &it->p[2];
		*len = it->p[1];
		take = (uint16_t)(2 + it->p[1]);
	} else if (want == SCT_LEN_FRAME) {
		if (it->left < 3 || it->p[2] > it->left - 3)
			goto bad;
		*data = &it->p[1];
		*len = (uint8_t)(2 + it->p[2]);
		take = (uint16_t)(3 + it->p[2]);
	} else if (want >= 0 && (uint16_t)want < it->left) {
		*data = &it->p[1];
		*len = (uint8_t)want;
		take = (uint16_t)(1 + want);
	} else {
		goto bad;
	}
	it->p += take;
	it->left = (uint16_t)(it->left - take);
	return true;
bad:
	return false;	// left stays nonzero: the caller can tell a bad field from the end
}

void sct_id_bcd(uint32_t id, uint8_t out[4])
{
	for (int i = 3; i >= 0; i--) {
		uint8_t lo = (uint8_t)(id % 10);

		id /= 10;
		out[i] = (uint8_t)(((id % 10) << 4) | lo);
		id /= 10;
	}
}
