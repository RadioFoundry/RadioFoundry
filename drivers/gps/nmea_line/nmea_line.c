// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// NMEA 0183 line framing (nmea_line.h)
#include "nmea_line.h"

static int hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

bool nmea_line_checksum_ok(const char *s, uint32_t n)
{
	// "$" body "*" h h: XOR of the body's characters
	if (n < 4 || s[0] != '$' || s[n - 3] != '*') {
		return false;
	}
	uint8_t x = 0;
	for (uint32_t i = 1; i < n - 3; i++) {
		if (s[i] == '*' || s[i] == '$') {
			return false;
		}
		x ^= (uint8_t)s[i];
	}
	int hi = hexval(s[n - 2]), lo = hexval(s[n - 1]);
	return hi >= 0 && lo >= 0 && (uint8_t)((hi << 4) | lo) == x;
}

void nmea_line_init(nmea_line_t *l)
{
	l->len = 0;
	l->in = false;
	l->buf[0] = '\0';
	l->good = 0;
	l->bad = 0;
}

bool nmea_line_feed(nmea_line_t *l, uint8_t c)
{
	if (c == '$') {
		if (l->in && l->len > 1) {
			l->bad++;	// a sentence cut short by the next one
		}
		l->in = true;
		l->len = 0;
		l->buf[l->len++] = '$';
		return false;
	}
	if (!l->in) {
		return false;
	}
	if (c == '\r' || c == '\n') {
		l->in = false;
		l->buf[l->len] = '\0';
		if (nmea_line_checksum_ok(l->buf, l->len)) {
			l->good++;
			return true;
		}
		l->bad++;
		return false;
	}
	if (c < 0x20 || c > 0x7E || l->len >= NMEA_LINE_LEN) {
		l->in = false;	// noise (a framing error) or an overlong line
		l->bad++;
		return false;
	}
	l->buf[l->len++] = (char)c;
	return false;
}
