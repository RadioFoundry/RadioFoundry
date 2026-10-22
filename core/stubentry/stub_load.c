// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The runtime RAM-monitor loader's portable core (see stub_load.h).
#include <string.h>
#include "stubentry/stub_load.h"

// The standard CRC-32 (core/storage/storage.c), always built into the firmware; the host test defines its own
uint32_t storage_crc32(uint32_t crc, const void *buf, uint32_t len);

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static size_t status_only(uint8_t *out, uint8_t st)
{
	out[0] = st;
	return 1;
}

void stub_load_init(stub_load_t *s, uint8_t *region, uint32_t region_size, uint32_t load_addr)
{
	memset(s, 0, sizeof(*s));
	s->region = region;
	s->region_size = region_size;
	s->load_addr = load_addr;
}

static size_t op_info(const stub_load_t *s, uint8_t *out)
{
	out[0] = DL_ST_OK;
	put32(out + 1, STUB_MAGIC);
	put32(out + 5, s->load_addr);
	put32(out + 9, s->region_size);
	put16(out + 13, STUB_CHUNK_MAX);
	out[15] = s->armed ? 1u : 0u;
	put32(out + 16, s->received);
	return 20;
}

static size_t op_begin(stub_load_t *s, const uint8_t *req, uint16_t len, uint8_t *out)
{
	if (len != 1u + 16u) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (get32(req + 1) != STUB_MAGIC) {
		return status_only(out, DL_ST_REFUSED);
	}
	uint32_t total = get32(req + 5);
	uint32_t crc = get32(req + 9);
	uint32_t entry = get32(req + 13);
	if (total == 0 || total > s->region_size) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (entry < s->load_addr || entry >= s->load_addr + total) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	memset(s->region, 0, total);	// a byte never written then reads 0, so a gap fails the CRC-32 at ENTER
	s->armed = true;
	s->total_len = total;
	s->total_crc = crc;
	s->entry = entry;
	s->received = 0;
	out[0] = DL_ST_OK;
	put32(out + 1, s->received);
	return 5;
}

static size_t op_data(stub_load_t *s, const uint8_t *req, uint16_t len, uint8_t *out)
{
	if (!s->armed || len < 1u + 4u) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	uint32_t offset = get32(req + 1);
	uint32_t n = (uint32_t)(len - 5u);
	if (n == 0 || n > STUB_CHUNK_MAX || offset > s->total_len || n > s->total_len - offset) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	memcpy(s->region + offset, req + 5, n);
	if (offset + n > s->received) {
		s->received = offset + n;
	}
	out[0] = DL_ST_OK;
	put32(out + 1, s->received);
	return 5;
}

static size_t op_enter(stub_load_t *s, const uint8_t *req, uint16_t len, uint8_t *out, bool *enter, uint32_t *entry)
{
	if (len != 1u + 4u) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (get32(req + 1) != STUB_MAGIC) {
		return status_only(out, DL_ST_REFUSED);
	}
	if (!s->armed || s->received != s->total_len) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (storage_crc32(0, s->region, s->total_len) != s->total_crc) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (s->transmitting) {
		return status_only(out, DL_ST_BUSY);	// the image stays loaded: ENTER again once the radio is receiving
	}
	if (s->image_ok != NULL && !s->image_ok(s->region, s->total_len)) {
		out[0] = DL_ST_REFUSED;		// a stub that would be reset part way through its work: never start it
		out[1] = STUB_REFUSE_WATCHDOG;
		return 2;
	}
	*enter = true;
	*entry = s->entry;
	return status_only(out, DL_ST_OK);
}

size_t stub_load_request(stub_load_t *s, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap, bool *enter,
			 uint32_t *entry)
{
	if (cap < STUB_REPLY_MAX) {
		return status_only(out, DL_ST_UNAVAILABLE);	// a caller's buffer too short for INFO: never written past
	}
	if (len < 1) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	switch (req[0]) {
	case STUB_OP_INFO:
		return op_info(s, out);
	case STUB_OP_BEGIN:
		return op_begin(s, req, len, out);
	case STUB_OP_DATA:
		return op_data(s, req, len, out);
	case STUB_OP_ENTER:
		return op_enter(s, req, len, out, enter, entry);
	default:
		return status_only(out, DL_ST_BAD_ARG);
	}
}
