// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// iGate rules and the digipeater (aprs_igate.h)
#include <string.h>
#include "aprs/aprs_igate.h"

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n--) {
		h = (h ^ *b++) * 16777619u;
	}
	return h;
}

static size_t trimmed(const uint8_t *info, size_t n)
{
	while (n > 0 && (info[n - 1] == '\r' || info[n - 1] == '\n' || info[n - 1] == ' ')) {
		n--;
	}
	return n;
}

uint32_t aprs_frame_hash(const ax25_frame_t *f)
{
	uint32_t h = 2166136261u;
	h = fnv(h, f->src.call, strlen(f->src.call));
	h = fnv(h, &f->src.ssid, 1);
	h = fnv(h, f->dst.call, strlen(f->dst.call));
	h = fnv(h, &f->dst.ssid, 1);
	return fnv(h, f->info, trimmed(f->info, f->info_len));
}

bool aprs_dedup_seen(aprs_dedup_t *d, uint32_t hash, uint32_t now_ms, uint32_t window_ms)
{
	for (int i = 0; i < APRS_DEDUP_SLOTS; i++) {
		if (d->ms[i] != 0 && d->hash[i] == hash && now_ms - d->ms[i] < window_ms) {
			return true;
		}
	}
	d->hash[d->next] = hash;
	d->ms[d->next] = now_ms != 0 ? now_ms : 1;
	d->next = (uint8_t)((d->next + 1u) % APRS_DEDUP_SLOTS);
	return false;
}

static bool is_call(const ax25_addr_t *a, const char *call)
{
	return strcmp(a->call, call) == 0;
}

// TCPIP, TCPXX, NOGATE, RFONLY anywhere in the path (any SSID)
static bool no_gate_path(const ax25_frame_t *f, bool internet_only)
{
	for (uint8_t i = 0; i < f->ndigi; i++) {
		const ax25_addr_t *a = &f->digi[i];
		if (is_call(a, "TCPIP") || is_call(a, "TCPXX")) {
			return true;
		}
		if (!internet_only && (is_call(a, "NOGATE") || is_call(a, "RFONLY"))) {
			return true;
		}
	}
	return false;
}

static size_t info_line_len(const uint8_t *info, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (info[i] == '\r' || info[i] == '\n' || info[i] == 0) {
			return i;
		}
	}
	return n;
}

// "SRC>DST,PATH,qAR,MYCALL:info" for f
static bool gate_line(const ax25_frame_t *f, const ax25_addr_t *mycall, char *out, size_t max)
{
	ax25_frame_t g = *f;
	g.info_len = (uint16_t)info_line_len(f->info, f->info_len);
	static char tmp[AX25_TNC2_MAX];	// one caller at a time (the radio task)
	size_t n = ax25_to_tnc2(&g, tmp, sizeof(tmp));
	if (n == 0) {
		return false;
	}
	const char *colon = strchr(tmp, ':');
	size_t head = (size_t)(colon - tmp);
	char call[12];
	size_t cl = ax25_addr_format(mycall, false, call);
	size_t total = n + 5u + cl;	// ",qAR," + call
	if (total + 1u > max) {
		return false;
	}
	memcpy(out, tmp, head);
	memcpy(out + head, ",qAR,", 5);
	memcpy(out + head + 5, call, cl);
	memcpy(out + head + 5 + cl, colon, n - head);
	out[total] = '\0';
	return true;
}

uint8_t aprs_igate_rf_to_is(aprs_dedup_t *d, const ax25_frame_t *f, const ax25_addr_t *mycall, uint32_t now_ms,
			    char *out, size_t max)
{
	out[0] = '\0';
	if (f->info_len == 0 || f->control != AX25_CONTROL_UI || f->pid != AX25_PID_NONE) {
		return APRS_GATE_BAD;
	}
	if (no_gate_path(f, false)) {
		return APRS_GATE_NO_PATH;
	}
	if (f->info[0] == '?') {
		return APRS_GATE_QUERY;
	}
	ax25_frame_t inner;
	const ax25_frame_t *g = f;
	if (f->info[0] == '}') {
		if (!ax25_from_tnc2((const char *)f->info + 1, f->info_len - 1u, &inner) || inner.info_len == 0) {
			return APRS_GATE_BAD;
		}
		if (no_gate_path(&inner, true)) {
			return APRS_GATE_FROM_IS;
		}
		g = &inner;
	}
	if (aprs_dedup_seen(d, aprs_frame_hash(g), now_ms, APRS_DEDUP_MS)) {
		return APRS_GATE_DUP;
	}
	return gate_line(g, mycall, out, max) ? APRS_GATE_OK : APRS_GATE_BAD;
}

bool aprs_igate_is_to_rf(aprs_dedup_t *d, const char *line, size_t n, const ax25_addr_t *mycall,
			 const aprs_igate_env_t *env, uint32_t now_ms, char *info, size_t max, size_t *len)
{
	ax25_frame_t f;
	*len = 0;
	if (n == 0 || line[0] == '#' || !ax25_from_tnc2(line, n, &f) || f.info_len < 11 || f.info[0] != ':') {
		return false;		// server comments, malformed lines, anything but a message
	}
	if (ax25_addr_equal(&f.src, mycall)) {
		return false;
	}
	for (uint8_t i = 0; i < f.ndigi; i++) {
		if (is_call(&f.digi[i], "NOGATE") || is_call(&f.digi[i], "RFONLY") || is_call(&f.digi[i], "TCPXX")) {
			return false;
		}
	}
	// the addressee: 9 characters, trailing spaces
	char addr[10];
	memcpy(addr, f.info + 1, 9);
	addr[9] = '\0';
	for (int i = 8; i >= 0 && addr[i] == ' '; i--) {
		addr[i] = '\0';
	}
	ax25_addr_t to;
	if (!ax25_addr_parse(addr, strlen(addr), &to) || env == NULL || env->heard_direct == NULL ||
	    !env->heard_direct(to.call, to.ssid, APRS_IGATE_LOCAL_S, env->ctx)) {
		return false;
	}
	if (env->heard_rf != NULL && env->heard_rf(f.src.call, f.src.ssid, APRS_IGATE_SENDER_S, env->ctx)) {
		return false;		// the sender is on RF itself
	}
	if (aprs_dedup_seen(d, aprs_frame_hash(&f), now_ms, APRS_DEDUP_MS)) {
		return false;
	}
	// "}SRC>DST,TCPIP,MYCALL*:info"
	ax25_frame_t t;
	memset(&t, 0, sizeof(t));
	t.src = f.src;
	t.dst = f.dst;
	t.ndigi = 2;
	ax25_addr_parse("TCPIP", 5, &t.digi[0]);
	t.digi[1] = *mycall;
	t.digi[1].h = true;
	t.info = f.info;
	t.info_len = (uint16_t)info_line_len(f.info, f.info_len);
	if (max < 2) {
		return false;
	}
	info[0] = '}';
	size_t k = ax25_to_tnc2(&t, info + 1, max - 1u);
	if (k == 0 || k + 1u > AX25_INFO_MAX) {
		info[0] = '\0';
		return false;
	}
	*len = k + 1u;
	return true;
}

// "WIDEn" with an SSID: n and the hops left
static bool wide(const ax25_addr_t *a, uint8_t *n)
{
	if (strncmp(a->call, "WIDE", 4) != 0 || a->call[4] < '1' || a->call[4] > '7' || a->call[5] != '\0') {
		return false;
	}
	*n = (uint8_t)(a->call[4] - '0');
	return true;
}

bool aprs_digi(aprs_dedup_t *d, const ax25_frame_t *in, ax25_frame_t *out, const ax25_addr_t *mycall, uint8_t mode,
	       uint32_t now_ms)
{
	if (mode == APRS_DIGI_OFF || ax25_addr_equal(&in->src, mycall) || in->info_len == 0) {
		return false;
	}
	int i = 0;
	while (i < in->ndigi && in->digi[i].h) {
		i++;
	}
	if (i >= in->ndigi) {
		return false;		// nothing left to do in the path
	}
	const ax25_addr_t *a = &in->digi[i];
	uint8_t n = 0;
	bool direct = ax25_addr_equal(a, mycall);
	bool ok = direct;
	if (!ok && wide(a, &n) && a->ssid >= 1 && a->ssid <= n) {
		ok = (n == 1) || (mode == APRS_DIGI_WIDE && n == 2);
	}
	if (!ok || aprs_dedup_seen(d, aprs_frame_hash(in), now_ms, APRS_DEDUP_MS)) {
		return false;
	}
	*out = *in;
	if (direct) {
		out->digi[i].h = true;
		return true;
	}
	uint8_t left = (uint8_t)(a->ssid - 1u);
	if (out->ndigi < AX25_MAX_DIGIS) {
		// insert our call, used, before the WIDE entry
		memmove(&out->digi[i + 1], &out->digi[i], (size_t)(out->ndigi - i) * sizeof(ax25_addr_t));
		out->ndigi++;
		out->digi[i] = *mycall;
		out->digi[i].h = true;
		ax25_addr_t *w = &out->digi[i + 1];
		w->ssid = left;
		w->h = left == 0;
	} else if (left == 0) {
		out->digi[i] = *mycall;	// no room to trace: our call replaces the used up entry
		out->digi[i].h = true;
	} else {
		out->digi[i].ssid = left;
	}
	return true;
}
