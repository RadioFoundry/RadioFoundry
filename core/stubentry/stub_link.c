// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stub entry's own listener's commands (see stub_link.h)
#include <string.h>
#include "stubentry/stub_link.h"

// Append "key=value\n" to the text at out[*n], stopping short of cap
static void kv(char *out, size_t *n, size_t cap, const char *key, const char *value)
{
	const char *parts[4] = { key, "=", value != NULL ? value : "", "\n" };

	for (int p = 0; p < 4; p++) {
		for (const char *c = parts[p]; *c != '\0' && *n < cap; c++) {
			out[(*n)++] = *c;
		}
	}
}

// VERSION with the keys the flashing tools read (docs/spec/debuglink.md 3.1): which radio, which build, which codec.
// stub=1 says this is the stub entry's listener, so a host knows the debug link's other commands aren't built
static size_t version(const stub_link_t *l, uint8_t *out, size_t cap)
{
	size_t n = 1;

	out[0] = DL_ST_OK;
	kv((char *)out, &n, cap, "proto", "1");
	kv((char *)out, &n, cap, "git", l->git);
	kv((char *)out, &n, cap, "board", l->board);
	kv((char *)out, &n, cap, "codec", l->codec != NULL ? l->codec() : "none");
	kv((char *)out, &n, cap, "stub", "1");
	return n;
}

size_t stub_link_handle(const stub_link_t *l, uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	switch (cmd) {
	case DL_CMD_PING:
		if (1u + len > cap) {
			len = (uint16_t)(cap - 1u);
		}
		out[0] = DL_ST_OK;
		memcpy(out + 1, req, len);
		return 1u + len;
	case DL_CMD_VERSION:
		return version(l, out, cap);
	case DL_CMD_STUB:
		if (l->stub == NULL) {
			out[0] = DL_ST_UNAVAILABLE;
			return 1;
		}
		return l->stub(req, len, out, cap);
	default:
		out[0] = DL_ST_UNKNOWN_CMD;
		return 1;
	}
}
