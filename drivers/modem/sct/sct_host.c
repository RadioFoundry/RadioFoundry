// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stddef.h>
#include "sct_host.h"

// SCT3288: start the application in the chip's flash (boot ROM command; whether the chip starts it by itself is
// still open, modem-sct.md 9)
static const uint8_t rom_start[6] = { 0x74, 0xA9, 0x01, 0x00, 0x01, 0x00 };

int sct_ack_code(const uint8_t *pkt, uint16_t len)
{
	sct_fields_t it;
	uint8_t id, n;
	const uint8_t *d;
	bool ack;

	if (!sct_fields_begin(&it, pkt, len, false))
		return -1;
	while (sct_fields_next(&it, &id, &d, &n, &ack))
		if (id == SCT_F_ACK)
			return d[0];
	return -1;
}

bool sct_host_send(const sct_host_t *h, const uint8_t *buf, uint16_t len)
{
	return len != 0 && (len & 1) == 0 && h->write(buf, len, h->ctx);
}

uint16_t sct_host_poll(const sct_host_t *h, sct_rx_t *rx, sct_packet_cb_t cb, void *ctx)
{
	uint8_t buf[64];
	uint16_t total = 0;
	uint16_t n;

	// Bounded: a chip that keeps HOBIB asserted can't hold the task here
	for (int rounds = 0; rounds < 8 && (n = h->read(buf, sizeof(buf), h->ctx)) != 0; rounds++) {
		total = (uint16_t)(total + n);
		for (uint16_t i = 0; i < n; i++) {
			uint16_t plen;

			if (sct_rx_feed(rx, buf[i], &plen) && cb != NULL)
				cb(rx->buf, plen, ctx);
		}
	}
	return total;
}

// The status for field id in the chip's next answer within the deadline, -1 if none came. An ACK_MESSAGE with an
// error code answers any field (the chip refused the packet); "processing" only means the answer is still to come
static int wait_status(const sct_host_t *h, sct_rx_t *rx, uint8_t id, uint32_t deadline)
{
	uint8_t buf[64];

	while ((int32_t)(h->now_ms() - deadline) < 0) {
		uint16_t n = h->read(buf, sizeof(buf), h->ctx);

		if (n == 0) {
			h->delay_ms(1);
			continue;
		}
		for (uint16_t i = 0; i < n; i++) {
			uint16_t plen;
			sct_fields_t it;
			uint8_t fid, flen;
			const uint8_t *d;
			bool ack;

			if (!sct_rx_feed(rx, buf[i], &plen) || !sct_fields_begin(&it, rx->buf, plen, false))
				continue;
			while (sct_fields_next(&it, &fid, &d, &flen, &ack)) {
				if (fid == SCT_F_ACK && d[0] == SCT_ACK_PROCESSING)
					continue;
				if (fid == SCT_F_ACK && (id == SCT_F_ACK || d[0] != SCT_ACK_OK))
					return d[0];
				if (fid == id && ack)
					return d[0];
			}
		}
	}
	return -1;
}

static int wait_ack(const sct_host_t *h, sct_rx_t *rx, uint32_t deadline)
{
	return wait_status(h, rx, SCT_F_ACK, deadline);
}

// Answer times: the Packet Interface's field timeouts (CODEC_SELECT 1 s, VOCODER_SEL up to 3 s); a codec write is
// one I2C transfer; other control fields get what the stock firmware allows them
#define CODEC_SELECT_MS		1000u
#define VOCODER_SEL_MS		3000u
#define CODEC_WRITE_MS		200u
#define FIELD_MS		200u

// One TYPE 0 write of a single field, answered by the field's status
static bool control1(const sct_host_t *h, sct_rx_t *rx, uint8_t id, const uint8_t *data, uint8_t len, uint32_t wait_ms)
{
	sct_pkt_t p;

	sct_pkt_begin(&p, SCT_TYPE_CONTROL);
	sct_pkt_add(&p, id, data, len);
	uint16_t n = sct_pkt_end(&p, false);

	return sct_host_send(h, p.buf, n) && wait_status(h, rx, id, h->now_ms() + wait_ms) == SCT_ACK_OK;
}

static sct_boot_result_t voice_setup(const sct_host_t *h, sct_rx_t *rx, const sct_voice_t *v)
{
	if (v->codec != 0 && !control1(h, rx, SCT_F_CODEC_SELECT, &v->codec, 1, CODEC_SELECT_MS))
		return SCT_BOOT_NO_VOICE;
	if (v->vocoder != 0 && !control1(h, rx, SCT_F_VOCODER_SEL, &v->vocoder, 1, VOCODER_SEL_MS))
		return SCT_BOOT_NO_VOICE;
	for (uint8_t i = 0; i < v->field_count; i++) {
		const sct_field_t *f = &v->fields[i];

		// A field of the wrong length makes no packet (sct_host_send refuses 0 bytes)
		if (!control1(h, rx, f->id, f->data, f->len, FIELD_MS))
			return SCT_BOOT_NO_VOICE;
	}
	for (uint8_t i = 0; i < v->codec_count; i++) {
		uint8_t p[SCT_CODEC_PKT_BYTES];

		sct_pkt_codec(p, &v->codec_writes[i]);
		// The answer is an ACK_MESSAGE (`40 17 00`)
		if (!sct_host_send(h, p, sizeof(p)) || wait_ack(h, rx, h->now_ms() + CODEC_WRITE_MS) != SCT_ACK_OK)
			return SCT_BOOT_NO_VOICE;
	}
	return SCT_BOOT_OK;
}

static sct_boot_result_t boot_app(const sct_host_t *h, sct_rx_t *rx, uint32_t timeout_ms)
{
	if (h->boot == SCT_BOOT_BLOCK && h->boot_block == NULL)
		return SCT_BOOT_NO_BLOCK;
	sct_rx_reset(rx);
	h->reset(h->ctx);
	uint32_t deadline = h->now_ms() + timeout_ms;
	int code;

	if (h->boot == SCT_BOOT_BLOCK) {
		if (!h->write(h->boot_block, SCT_BOOT_BLOCK_BYTES, h->ctx))
			return SCT_BOOT_BUS;
		code = wait_ack(h, rx, deadline);
		if (code == SCT_ACK_RESET_READY)
			return SCT_BOOT_OK;
		return code < 0 ? SCT_BOOT_SILENT : SCT_BOOT_NO_APP;
	}
	// Boot ROM: a greeting first. NOT_LOADED asks for the start command; RESET_READY means the chip started itself
	code = wait_ack(h, rx, deadline);
	if (code == SCT_ACK_RESET_READY)
		return SCT_BOOT_OK;
	if (code < 0)
		return SCT_BOOT_SILENT;
	if (!h->write(rom_start, sizeof(rom_start), h->ctx))
		return SCT_BOOT_BUS;
	code = wait_ack(h, rx, deadline);
	if (code == SCT_ACK_RESET_READY)
		return SCT_BOOT_OK;
	return code < 0 ? SCT_BOOT_SILENT : SCT_BOOT_NO_APP;
}

sct_boot_result_t sct_host_boot(const sct_host_t *h, sct_rx_t *rx, uint32_t timeout_ms)
{
	sct_boot_result_t r = boot_app(h, rx, timeout_ms);

	if (r != SCT_BOOT_OK || h->voice == NULL)
		return r;
	return voice_setup(h, rx, h->voice);
}
