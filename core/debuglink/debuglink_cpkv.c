// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_CPKV (docs/spec/debuglink.md 3.27): a key of the codeplug settings area read, written or removed, for tests
// and tools that set what a codeplug tool would (docs/spec/codeplug-settings.md)
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "codeplug/codeplug_kv.h"

#if CONFIG_DMR_DATA_CORE || CONFIG_DMR_TRUNK
#define VALUE_MAX	128u	// the longest value set through here: a request is at most DL_REQ_MAX bytes a piece

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

// noinline: the value buffer stays off the host task's deepest path
size_t __attribute__((noinline)) debuglink_cmd_cpkv(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t v[VALUE_MAX];
	if (cap < 3u || len < 3u) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	uint16_t key = rd16(req + 1);
	out[0] = DL_ST_OK;
	switch (req[0]) {
	case DL_CPKV_GET: {	// key u16, offset u16 -> length u16, the value's bytes from offset
		uint16_t off = len >= 5u ? rd16(req + 3) : 0;
		int n = codeplug_kv_get(key, v, sizeof v);
		if (n < 0) {
			out[0] = DL_ST_UNAVAILABLE;	// no such key
			return 1;
		}
		out[1] = (uint8_t)n;
		out[2] = (uint8_t)(n >> 8);
		size_t have = (size_t)n < sizeof v ? (size_t)n : sizeof v;	// a longer value: its first VALUE_MAX bytes
		size_t k = off < have ? have - off : 0;
		if (k > cap - 3u) {
			k = cap - 3u;
		}
		if (k > 0) {
			memcpy(out + 3, v + off, k);
		}
		return 3u + k;
	}
	case DL_CPKV_SET: {	// key u16, offset u16, bytes: offset 0 replaces the value, else patches it from offset
		if (len < 5u) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		uint16_t off = rd16(req + 3), n = (uint16_t)(len - 5u);
		int had = 0;
		if (off != 0) {
			had = codeplug_kv_get(key, v, sizeof v);
			if (had < (int)off || had > (int)sizeof v) {
				out[0] = DL_ST_BAD_ARG;	// a patch starts inside the value, of one this command holds whole
				return 1;
			}
		}
		if ((uint32_t)off + n > sizeof v) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		memcpy(v + off, req + 5, n);
		uint16_t total = (uint16_t)(off + n) > (uint16_t)had ? (uint16_t)(off + n) : (uint16_t)had;
		if (!codeplug_kv_set(key, v, total)) {
			out[0] = DL_ST_BUSY;
		}
		return 1;
	}
	case DL_CPKV_REMOVE:	// key u16
		if (!codeplug_kv_remove(key)) {
			out[0] = DL_ST_BUSY;
		}
		return 1;
	default:
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
}
#else
size_t debuglink_cmd_cpkv(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}
#endif
