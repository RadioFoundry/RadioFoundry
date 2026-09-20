// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio's own APRS-IS client (see aprsis.h), after aprs-is.net "Connecting to APRS-IS" (the login line, the
// passcode, server comments and the logresp line) and "IGate Design". The iGate rules themselves stay in the APRS
// engine (core/aprs/aprs_igate.c): this file only carries lines, as the host helper tools/aprs_gate.py does over USB.
#include "config.h"
#if CONFIG_NET_APRSIS	// the whole file
#include <string.h>
#include "net/aprsis.h"
#include "net/tcp.h"
#include "aprs/aprs.h"
#include "settings/settings.h"
#include "dmr/ipGateway.h"
#include "hal_time.h"

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

#if CONFIG_APRS_IGATE
#define RX_MAX		256	// a server line for the engine (messages to local stations); longer ones are skipped
#else
#define RX_MAX		96	// only the server's logresp line matters
#endif
#define CHECK_MS	1000u	// the settings are looked at this often

static int conn = -1;
static uint8_t state = APRSIS_OFF;
static uint32_t next_try, retry_ms = APRSIS_RETRY_MS, since, last_rx, last_check;
static uint32_t cur_ip;
static uint16_t cur_port, cur_range;
static char cur_call[AX25_CALL_LEN + 4];
static char rx[RX_MAX];
static uint16_t rxn;
static bool rx_skip;		// the current line is too long: dropped up to its end
static struct {
	uint32_t connects, logins, unverified, lines_out, lines_in, drops;
} st;

uint16_t aprsis_passcode(const char *call)
{
	uint16_t h = 0x73E2u;
	char base[AX25_CALL_LEN + 4];
	size_t n = 0;

	for (; (call[n] != '\0') && (call[n] != '-') && (n < sizeof(base) - 1); n++) {
		char ch = call[n];

		base[n] = ((ch >= 'a') && (ch <= 'z')) ? (char)(ch - 'a' + 'A') : ch;
	}
	base[n] = '\0';
	for (size_t i = 0; i < n; i += 2) {
		h ^= (uint16_t)((uint8_t)base[i] << 8);
		if (i + 1 < n) {
			h ^= (uint8_t)base[i + 1];
		}
	}
	return h & 0x7FFFu;
}

// What the client should connect to now: false when it is off (no server, no callsign)
static bool wanted(uint32_t *ip, uint16_t *port, uint16_t *range, char call[AX25_CALL_LEN + 4])
{
	settings_t s;
	aprs_view_t v;

	settings_read(&s);
	aprs_get_view(&v);
	*ip = s.aprsis_ip;
	*port = (s.aprsis_port != 0) ? s.aprsis_port : APRSIS_PORT_DEFAULT;
	*range = s.aprsis_range_km;
	memcpy(call, v.call, AX25_CALL_LEN + 4);
	call[AX25_CALL_LEN + 3] = '\0';
	return (*ip != 0) && (call[0] != '\0');
}

static void backoff(uint32_t now)
{
	next_try = now + retry_ms;
	retry_ms = ((retry_ms * 2u) > APRSIS_RETRY_MAX_MS) ? APRSIS_RETRY_MAX_MS : (retry_ms * 2u);
	state = APRSIS_WAIT;
}

static void drop_conn(uint32_t now)
{
	if (conn >= 0) {
		tcp_abort(conn);
		conn = -1;
	}
	backoff(now);
}

static void a_opened(int c)
{
	char line[128];
	size_t n = dl_fmt(line, sizeof(line), "user %s pass %u vers OpenDigitalRadio " ODR_GIT, cur_call,
			  aprsis_passcode(cur_call));

	if (cur_range != 0) {
		n += dl_fmt(line + n, sizeof(line) - n, " filter m/%u", cur_range);
	}
	n += dl_fmt(line + n, sizeof(line) - n, "\r\n");
	tcp_write(c, line, (int)n);
	state = APRSIS_LOGIN;
	since = last_rx = hal_millis();
	rxn = 0;
	rx_skip = false;
}

// "# logresp CALL verified, server NAME" (or "unverified")
static void server_line(const char *line, size_t n, uint32_t now)
{
	if (line[0] == '#') {
		if ((state == APRSIS_LOGIN) && (n > 10) && (strncmp(line, "# logresp ", 10) == 0)) {
			if ((strstr(line, " verified") != NULL)) {
				state = APRSIS_ONLINE;
				retry_ms = APRSIS_RETRY_MS;
				st.logins++;
			} else {
				st.unverified++;	// a wrong passcode doesn't get better by trying again soon
				retry_ms = APRSIS_RETRY_MAX_MS;
				drop_conn(now);
			}
		}
		return;
	}
	if (state == APRSIS_ONLINE) {
		st.lines_in++;
		aprs_host_is_line(line, n, now);	// the engine's IS -> RF rules decide (iGate builds)
	}
}

static void a_recv(int c, const uint8_t *d, int n)
{
	uint32_t now = hal_millis();

	(void)c;
	last_rx = now;
	for (int i = 0; (i < n) && (conn >= 0); i++) {
		char ch = (char)d[i];

		if ((ch == '\r') || (ch == '\n')) {
			if (!rx_skip && (rxn > 0)) {
				rx[rxn] = '\0';
				server_line(rx, rxn, now);
			}
			rxn = 0;
			rx_skip = false;
		} else if (rxn < RX_MAX - 1) {
			rx[rxn++] = ch;
		} else {
			rx_skip = true;
		}
	}
}

static void a_peer_closed(int c)
{
	tcp_close(c);
	conn = -1;
	backoff(hal_millis());
}

static void a_closed(int c, bool error)
{
	(void)c;
	(void)error;
	conn = -1;
	backoff(hal_millis());
}

static const tcp_app_t app = {
	.opened = a_opened, .recv = a_recv, .peer_closed = a_peer_closed, .closed = a_closed,
};

void aprsis_start(void)
{
	conn = -1;
	state = APRSIS_OFF;
	next_try = 0;
	retry_ms = APRSIS_RETRY_MS;
}

void aprsis_poll(uint32_t now)
{
	if (((now - last_check) >= CHECK_MS) || (last_check == 0)) {
		uint32_t ip;
		uint16_t port, range;
		char call[AX25_CALL_LEN + 4];
		bool on = wanted(&ip, &port, &range, call);

		last_check = (now != 0) ? now : 1;
		if ((conn >= 0) && (!on || (ip != cur_ip) || (port != cur_port) || (range != cur_range) ||
				    (strcmp(call, cur_call) != 0))) {
			tcp_abort(conn);	// the settings changed: start again with the new ones
			conn = -1;
			state = APRSIS_WAIT;
			next_try = now;
			retry_ms = APRSIS_RETRY_MS;
		}
		if (!on) {
			state = APRSIS_OFF;
			return;
		}
		if (state == APRSIS_OFF) {
			state = APRSIS_WAIT;
			next_try = now;
		}
		cur_ip = ip;
		cur_port = port;
		cur_range = range;
		memcpy(cur_call, call, sizeof(cur_call));
	}
	switch (state) {
	case APRSIS_WAIT:
		if ((int32_t)(now - next_try) >= 0) {
			conn = tcp_connect(cur_ip, cur_port, &app, now);
			if (conn < 0) {
				next_try = now + 1000u;	// no free connection: soon again
			} else {
				st.connects++;
				state = APRSIS_CONNECTING;	// tcp gives up on its own (SYN retries): closed() then
			}
		}
		break;
	case APRSIS_LOGIN:
		if ((now - since) >= APRSIS_LOGIN_MS) {
			drop_conn(now);
		}
		break;
	case APRSIS_ONLINE:
		if ((now - last_rx) >= APRSIS_SILENCE_MS) {
			drop_conn(now);
		}
		break;
	default:
		break;
	}
}

bool aprsis_online(void)
{
	return (state == APRSIS_ONLINE) && (conn >= 0);
}

bool aprsis_room(size_t n)
{
	return aprsis_online() && ((size_t)tcp_space(conn) >= n);
}

void aprsis_line(const uint8_t *line, size_t n)
{
	if (!aprsis_room(n + 2u)) {
		st.drops++;
		return;
	}
	tcp_write(conn, line, (int)n);
	tcp_write(conn, "\r\n", 2);
	st.lines_out++;
}

static const char *state_name(void)
{
	static const char *const names[] = { "off", "wait", "connecting", "login", "online" };

	return (state < 5) ? names[state] : "?";
}

void aprsis_view(dl_text_t *t)
{
	dl_text(t, "aprsis.state=%s\naprsis.server=%u.%u.%u.%u:%u\naprsis.connects=%u\naprsis.logins=%u\n", state_name(),
		(unsigned)(cur_ip >> 24), (unsigned)((cur_ip >> 16) & 0xFF), (unsigned)((cur_ip >> 8) & 0xFF),
		(unsigned)(cur_ip & 0xFF), cur_port, (unsigned)st.connects, (unsigned)st.logins);
	dl_text(t, "aprsis.unverified=%u\naprsis.lines_out=%u\naprsis.lines_in=%u\naprsis.drops=%u\n",
		(unsigned)st.unverified, (unsigned)st.lines_out, (unsigned)st.lines_in, (unsigned)st.drops);
}

// ---- GET /api/aprsis ----

static int hexval(char c)
{
	return ((c >= '0') && (c <= '9')) ? (c - '0') : (((c | 0x20) >= 'a') && ((c | 0x20) <= 'f')) ? ((c | 0x20) - 'a' + 10) : -1;
}

// The value of key in a query string, percent-decoded into out; false when absent
static bool query_get(const char *q, const char *key, char *out, size_t max)
{
	size_t kn = strlen(key);

	while ((q != NULL) && (*q != '\0')) {
		if ((strncmp(q, key, kn) == 0) && (q[kn] == '=')) {
			size_t n = 0;

			for (q += kn + 1; (*q != '\0') && (*q != '&') && (n < max - 1); q++) {
				if ((*q == '%') && (hexval(q[1]) >= 0) && (hexval(q[2]) >= 0)) {
					out[n++] = (char)(hexval(q[1]) * 16 + hexval(q[2]));
					q += 2;
				} else {
					out[n++] = (*q == '+') ? ' ' : *q;
				}
			}
			out[n] = '\0';
			return true;
		}
		q = strchr(q, '&');
		if (q != NULL) {
			q++;
		}
	}
	return false;
}

// a.b.c.d[:port]
static bool parse_server(const char *s, uint32_t *ip, uint16_t *port)
{
	uint32_t v = 0, part = 0;
	int dots = 0, digits = 0;

	for (; (*s != '\0') && (*s != ':'); s++) {
		if (*s == '.') {
			if ((digits == 0) || (++dots > 3)) {
				return false;
			}
			v = (v << 8) | part;
			part = 0;
			digits = 0;
		} else if ((*s >= '0') && (*s <= '9') && (digits < 3)) {
			part = part * 10u + (uint32_t)(*s - '0');
			digits++;
			if (part > 255) {
				return false;
			}
		} else {
			return false;
		}
	}
	if ((dots != 3) || (digits == 0)) {
		return false;
	}
	*ip = (v << 8) | part;
	*port = 0;
	if (*s == ':') {
		uint32_t p = 0;

		for (s++; (*s >= '0') && (*s <= '9'); s++) {
			p = p * 10u + (uint32_t)(*s - '0');
			if (p > 65535u) {
				return false;
			}
		}
		if ((*s != '\0') || (p == 0)) {
			return false;
		}
		*port = (uint16_t)p;
	}
	return (*ip != 0) && ((*ip >> 24) != 127) && ((*ip >> 28) < 0xE);
}

int aprsis_http(dl_text_t *t, const char *query, uint32_t from_ip)
{
	char val[24], rv[8];
	uint32_t ip = 0, km = 0;
	uint16_t port = 0;
	bool set_server = query_get(query, "server", val, sizeof(val));
	bool server_ok = set_server && ((strcmp(val, "off") == 0) || (strcmp(val, "0") == 0) || parse_server(val, &ip, &port));
	bool set_range = query_get(query, "range", rv, sizeof(rv));

	if (set_server || set_range) {
		if (from_ip != ipGatewayHostIP()) {
			return 403;	// settings only from the computer the radio is plugged into
		}
		if (set_server && !server_ok) {
			return 400;
		}
		for (const char *p = rv; set_range && (*p != '\0'); p++) {
			if ((*p < '0') || (*p > '9') || ((km = km * 10u + (uint32_t)(*p - '0')) > 20000u)) {
				return 400;
			}
		}
		settings_t *w = settings_begin();

		if (set_server) {
			w->aprsis_ip = ip;
			w->aprsis_port = port;
		}
		if (set_range) {
			w->aprsis_range_km = (uint16_t)km;
		}
		settings_end(hal_millis());
		last_check = 0;	// looked at again on the next pass
	}
	settings_t s;

	settings_read(&s);
	dl_text(t, "{\"state\":\"%s\",\"server\":\"%u.%u.%u.%u:%u\",\"range_km\":%u,\"call\":\"%s\",\"connects\":%u,"
		"\"logins\":%u,\"unverified\":%u,\"lines_out\":%u,\"lines_in\":%u}\n", state_name(),
		(unsigned)(s.aprsis_ip >> 24), (unsigned)((s.aprsis_ip >> 16) & 0xFF), (unsigned)((s.aprsis_ip >> 8) & 0xFF),
		(unsigned)(s.aprsis_ip & 0xFF), (s.aprsis_port != 0) ? s.aprsis_port : APRSIS_PORT_DEFAULT,
		s.aprsis_range_km, cur_call, (unsigned)st.connects, (unsigned)st.logins, (unsigned)st.unverified,
		(unsigned)st.lines_out, (unsigned)st.lines_in);
	return 200;
}

#endif	// CONFIG_NET_APRSIS
