// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stub entry's own listener's commands (see stub_link.h)
#include <string.h>
#include "stubentry/stub_link.h"

// Append "key=value\n" to the text at out[*n], whole or not at all: a line cut short would read as another value
static void kv(char *out, size_t *n, size_t cap, const char *key, const char *value)
{
	const char *parts[4] = { key, "=", value != NULL ? value : "", "\n" };
	size_t len = 0;

	for (int p = 0; p < 4; p++) {
		len += strlen(parts[p]);
	}
	if (*n + len > cap) {
		return;
	}
	for (int p = 0; p < 4; p++) {
		for (const char *c = parts[p]; *c != '\0'; c++) {
			out[(*n)++] = *c;
		}
	}
}

// VERSION with the keys the flashing tools and the web tools read (docs/spec/debuglink.md 3.1): which radio, which
// build, which codec. stub=1 says this is the stub entry's listener, so a host knows the debug link's other commands
// aren't built. preset= and off= come last: a list too long for the reply drops only itself
static size_t version(const stub_link_t *l, uint8_t *out, size_t cap)
{
	size_t n = 1;
	char num[11];

	out[0] = DL_ST_OK;
	kv((char *)out, &n, cap, "proto", "1");
	kv((char *)out, &n, cap, "git", l->git);
	if (l->git_time != 0u) {
		size_t i = sizeof(num) - 1u;
		uint32_t v = l->git_time;

		num[i] = '\0';
		do {
			num[--i] = (char)('0' + v % 10u);
			v /= 10u;
		} while (v != 0u);
		kv((char *)out, &n, cap, "git_time", num + i);
	}
	if (l->release != NULL && l->release[0] != '\0') {
		kv((char *)out, &n, cap, "release", l->release);
	}
	kv((char *)out, &n, cap, "board", l->board);
	if (l->features != NULL) {
		kv((char *)out, &n, cap, "features", l->features);
	}
	kv((char *)out, &n, cap, "codec", l->codec != NULL ? l->codec() : "none");
	kv((char *)out, &n, cap, "stub", "1");
	if (l->preset != NULL && l->off != NULL) {
		kv((char *)out, &n, cap, "preset", l->preset);
		kv((char *)out, &n, cap, "off", l->off);
	}
	return n;
}

// A command the build has a handler for, else unavailable
static size_t either(size_t (*fn)(const uint8_t *, uint16_t, uint8_t *, size_t), const uint8_t *req, uint16_t len,
		     uint8_t *out, size_t cap)
{
	if (fn == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	return fn(req, len, out, cap);
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
		return either(l->stub, req, len, out, cap);
	case DL_CMD_CODEPLUG:
		return either(l->codeplug, req, len, out, cap);
	default:
		out[0] = DL_ST_UNKNOWN_CMD;
		return 1;
	}
}
