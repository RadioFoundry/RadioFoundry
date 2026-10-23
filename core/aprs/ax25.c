// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AX.25 UI frames (ax25.h). Written from AX.25 2.2 (TAPR, 1998): 3.12 addresses, 4.3.3.6 UI frames, 3.8 FCS
#include <string.h>
#include "aprs/ax25.h"

uint16_t ax25_fcs(const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFFu;
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++) {
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
		}
	}
	return (uint16_t)~crc;
}

static bool call_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool ax25_addr_parse(const char *s, size_t n, ax25_addr_t *a)
{
	size_t i = 0;
	memset(a, 0, sizeof(*a));
	while (i < n && s[i] != '-') {
		char c = s[i];
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		if (i >= AX25_CALL_LEN || !call_char(c)) {
			return false;
		}
		a->call[i++] = c;
	}
	if (i == 0) {
		return false;
	}
	if (i < n) {		// "-SSID"
		size_t k = i + 1;
		unsigned v = 0;
		if (k >= n || n - k > 2) {
			return false;
		}
		for (; k < n; k++) {
			if (s[k] < '0' || s[k] > '9') {
				return false;
			}
			v = v * 10u + (unsigned)(s[k] - '0');
		}
		if (v > 15) {
			return false;
		}
		a->ssid = (uint8_t)v;
	}
	return true;
}

size_t ax25_addr_format(const ax25_addr_t *a, bool star, char *out)
{
	size_t n = 0;
	for (const char *c = a->call; *c; c++) {
		out[n++] = *c;
	}
	if (a->ssid != 0) {
		out[n++] = '-';
		if (a->ssid >= 10) {
			out[n++] = '1';
		}
		out[n++] = (char)('0' + a->ssid % 10u);
	}
	if (star && a->h) {
		out[n++] = '*';
	}
	out[n] = '\0';
	return n;
}

bool ax25_addr_equal(const ax25_addr_t *a, const ax25_addr_t *b)
{
	return a->ssid == b->ssid && strcmp(a->call, b->call) == 0;
}

// One address field: the call shifted left one bit, space padded; then 0b0RR SSID E with the C / H bit on top. The
// reserved bits are 1 (AX.25 2.2 3.12.2)
static void put_addr(uint8_t *p, const ax25_addr_t *a, bool last)
{
	size_t i = 0;
	for (; i < AX25_CALL_LEN && a->call[i]; i++) {
		p[i] = (uint8_t)(a->call[i] << 1);
	}
	for (; i < AX25_CALL_LEN; i++) {
		p[i] = (uint8_t)(' ' << 1);
	}
	p[6] = (uint8_t)(0x60u | (a->h ? 0x80u : 0u) | ((a->ssid & 0x0Fu) << 1) | (last ? 1u : 0u));
}

size_t ax25_encode(const ax25_frame_t *f, uint8_t *out, size_t max)
{
	size_t naddr = 2u + f->ndigi;
	size_t n = naddr * AX25_ADDR_BYTES + 2u + f->info_len;
	if (f->ndigi > AX25_MAX_DIGIS || f->info_len > AX25_INFO_MAX || n > max) {
		return 0;
	}
	put_addr(out, &f->dst, false);
	put_addr(out + 7, &f->src, f->ndigi == 0);
	for (uint8_t i = 0; i < f->ndigi; i++) {
		put_addr(out + 14 + 7u * i, &f->digi[i], i + 1u == f->ndigi);
	}
	size_t k = naddr * AX25_ADDR_BYTES;
	out[k++] = f->control;
	out[k++] = f->pid;
	if (f->info_len != 0) {
		memcpy(out + k, f->info, f->info_len);
	}
	return n;
}

static bool get_addr(const uint8_t *p, ax25_addr_t *a)
{
	size_t i;
	memset(a, 0, sizeof(*a));
	for (i = 0; i < AX25_CALL_LEN; i++) {
		if (p[i] & 1u) {
			return false;	// the extension bit belongs in the SSID byte only
		}
		char c = (char)(p[i] >> 1);
		if (c == ' ') {
			break;
		}
		if (!call_char(c)) {
			return false;
		}
		a->call[i] = c;
	}
	for (size_t k = i; k < AX25_CALL_LEN; k++) {
		if ((p[k] >> 1) != ' ') {
			return false;	// characters after the padding
		}
	}
	if (i == 0) {
		return false;
	}
	a->ssid = (uint8_t)((p[6] >> 1) & 0x0Fu);
	a->h = (p[6] & 0x80u) != 0;
	return true;
}

bool ax25_decode(const uint8_t *buf, size_t n, ax25_frame_t *f)
{
	memset(f, 0, sizeof(*f));
	size_t naddr = 0;
	for (;;) {
		if ((naddr + 1u) * AX25_ADDR_BYTES > n || naddr >= 2u + AX25_MAX_DIGIS) {
			return false;
		}
		const uint8_t *p = buf + naddr * AX25_ADDR_BYTES;
		ax25_addr_t *a = naddr == 0 ? &f->dst : naddr == 1 ? &f->src : &f->digi[naddr - 2u];
		if (!get_addr(p, a)) {
			return false;
		}
		naddr++;
		if (p[6] & 1u) {
			break;
		}
	}
	if (naddr < 2) {
		return false;
	}
	f->ndigi = (uint8_t)(naddr - 2u);
	size_t k = naddr * AX25_ADDR_BYTES;
	if (k + 2u > n) {
		return false;
	}
	f->control = buf[k];
	f->pid = buf[k + 1u];
	f->info = buf + k + 2u;
	f->info_len = (uint16_t)(n - k - 2u);
	return f->info_len <= AX25_INFO_MAX;
}

size_t ax25_to_tnc2(const ax25_frame_t *f, char *out, size_t max)
{
	char tmp[12];
	size_t n = 0;
	int last_h = -1;
	for (int i = 0; i < f->ndigi; i++) {
		if (f->digi[i].h) {
			last_h = i;
		}
	}
#define PUT(s, len) do { if (n + (len) >= max) return 0; memcpy(out + n, (s), (len)); n += (len); } while (0)
	size_t k = ax25_addr_format(&f->src, false, tmp);
	PUT(tmp, k);
	PUT(">", 1);
	k = ax25_addr_format(&f->dst, false, tmp);
	PUT(tmp, k);
	for (int i = 0; i < f->ndigi; i++) {
		PUT(",", 1);
		k = ax25_addr_format(&f->digi[i], i == last_h, tmp);
		PUT(tmp, k);
	}
	PUT(":", 1);
	PUT(f->info, f->info_len);
#undef PUT
	out[n] = '\0';
	return n;
}

bool ax25_from_tnc2(const char *line, size_t n, ax25_frame_t *f)
{
	memset(f, 0, sizeof(*f));
	const char *colon = memchr(line, ':', n);
	const char *gt = memchr(line, '>', n);
	if (colon == NULL || gt == NULL || gt > colon) {
		return false;
	}
	if (!ax25_addr_parse(line, (size_t)(gt - line), &f->src)) {
		return false;
	}
	const char *p = gt + 1;
	int last_star = -1;
	bool first = true;
	while (p < colon) {
		const char *e = p;
		while (e < colon && *e != ',') {
			e++;
		}
		size_t len = (size_t)(e - p);
		bool star = len > 0 && p[len - 1] == '*';
		if (star) {
			len--;
		}
		if (first) {
			if (!ax25_addr_parse(p, len, &f->dst)) {
				return false;
			}
			first = false;
		} else if (f->ndigi < AX25_MAX_DIGIS && ax25_addr_parse(p, len, &f->digi[f->ndigi])) {
			if (star) {
				last_star = f->ndigi;
			}
			f->ndigi++;
		}
		p = e + 1;
	}
	if (first) {
		return false;
	}
	for (int i = 0; i <= last_star; i++) {
		f->digi[i].h = true;	// everything up to the starred entry has been repeated
	}
	f->control = AX25_CONTROL_UI;
	f->pid = AX25_PID_NONE;
	f->info = (const uint8_t *)colon + 1;
	size_t info_len = n - (size_t)(colon + 1 - line);
	while (info_len > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
		info_len--;
		n--;
	}
	if (info_len > AX25_INFO_MAX) {
		return false;
	}
	f->info_len = (uint16_t)info_len;
	return true;
}
