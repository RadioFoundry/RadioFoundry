// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An HTTP/1.0 status server on port 80 (RFC 1945; docs/spec/tcp-services.md 5): static files (http_assets.h), the
// radio's state as JSON, the debug link's view sections as JSON, the screen as a BMP. GET and HEAD, one request per
// connection (Connection: close), no keep-alive, no chunking. With CONFIG_NET_WEBCTL, GET /ws upgrades to a WebSocket
// (RFC 6455 4.2) and the connection is handed to webctl.c. Responses are streamed into TCP as its buffer frees up:
// the files from flash, the screen straight from the frame buffer, JSON from one shared buffer (a second JSON request
// waits for it).
#include "config.h"
#if CONFIG_NET_HTTP	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "net/tcp.h"
#include "board.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "ui/gfx.h"
#include "debuglink/dl_fmt.h"
#if CONFIG_DEBUGLINK
#include "debuglink/debuglink.h"
#endif
#if CONFIG_NET_APRSIS
#include "net/aprsis.h"
#endif
#include "ui/ui.h"
#if CONFIG_NET_WEBCTL
#include "net/ws.h"
#include "net/webctl.h"
#endif
#include "net/http_assets.h"
#include "http_assets_gen.h"	// the build's gen directory: core/net/http_assets/* (tools/gen_http_assets.py)

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "unknown"
#endif

#define LINE_MAX	96	// request line ("GET /api/view?section=dmr_data_stats HTTP/1.1" and room)
#define HDR_MAX		192	// the longest header: an asset's, with Content-Encoding
#define BUF_MAX		1536	// JSON bodies; a view section's text is rendered into its upper part first
#define VIEW_AT		512	// where the section's text goes (the JSON written from 0 never catches up with it)
#define REQ_TIMEOUT_MS	10000u
#define BMP_HEADER_MAX	118	// file header 14, BITMAPINFOHEADER 40, then 16 palette entries (bpp 4) at most
#define SHOT_HOLD_MS	1000u	// UI redraws are held while the screen goes out, so it is one frame

enum { H_FREE, H_REQ, H_WAIT, H_SEND, H_WS };	// H_WS: webctl.c has the connection
enum { SRC_NONE, SRC_MEM, SRC_BUF, SRC_BMP };

typedef struct {
	uint8_t state;
	uint8_t src;
	bool head;		// HEAD: the header only
	bool line_done;		// the request line is in
	bool at_bol;		// at the start of a header line (an empty one ends the request)
	uint8_t ln;
#if CONFIG_NET_WEBCTL
	bool ws;		// the 101 is going out: the connection is webctl's once it's sent
	uint8_t hn;		// the header line under way, in hdr (free until the response)
	char key[WS_KEY_LEN + 1];	// Sec-WebSocket-Key, "" = none
#endif
	char line[LINE_MAX];
	uint16_t hlen, hoff;
	char hdr[HDR_MAX];
	const uint8_t *body;
	uint32_t blen, boff;
	uint32_t since;
} hconn_t;

static hconn_t hc[TCP_CONNS];
static char buf[BUF_MAX];
static int buf_owner = -1;
static struct {
	uint32_t requests, ok, not_found, bad, busy_waits;
} st;

static void put16le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32le(uint8_t *p, uint32_t v)
{
	put16le(p, v);
	put16le(p + 2, v >> 16);
}

// ---- JSON ----

static void json_str(dl_text_t *t, const char *s)
{
	char tmp[8];

	dl_text(t, "\"");
	for (; *s != '\0'; s++) {
		unsigned char ch = (unsigned char)*s;

		if ((ch == '"') || (ch == '\\')) {
			dl_text(t, "\\%c", ch);
		} else if (ch < 0x20) {
			dl_fmt(tmp, sizeof(tmp), "\\u%04x", ch);
			dl_text(t, "%s", tmp);
		} else {
			dl_text(t, "%c", ch);
		}
	}
	dl_text(t, "\"");
}

static const char *bool_s(bool b)
{
	return b ? "true" : "false";
}

void http_status_json(dl_text_t *t)
{
	static const char *const states[] = { "idle", "rx", "tx", "tx_denied" };
	const radio_view_t *v = net_radio_view();
	const channel_t *ch = &v->ch;
	const char *pw = ((v->power < board.power_count) && (board.power_names != NULL)) ? board.power_names[v->power] : "?";
	dl_text(t, "{\"radio\":");
	json_str(t, board.name);
	dl_text(t, ",\"version\":\"%s\",\"uptime_s\":%u,\"state\":\"%s\",\"vfo\":%s,\"zone\":%u,\"channel_number\":%u,"
		"\"channel\":", RFOUNDRY_GIT, (unsigned)(hal_millis() / 1000u), (v->state < 4) ? states[v->state] : "?",
		bool_s(v->vfo), v->zone_index, v->channel_number);
	json_str(t, ch->name);
	dl_text(t, ",\"rx_hz\":%u,\"tx_hz\":%u,\"mode\":\"%s\",\"bw\":\"%s\",\"power\":", (unsigned)ch->rx_hz,
		(unsigned)ch->tx_hz, (ch->mode == RADIO_MODE_DMR) ? "dmr" : "fm", (ch->bw == RADIO_BW_25K) ? "25" : "12.5");
	json_str(t, pw);
	dl_text(t, ",\"rssi_dbm\":%d,\"squelch\":%u", v->rssi_dbm, v->squelch);
	if (board_has_battery()) {	// a radio without a battery leaves both fields out
		dl_text(t, ",\"battery_pct\":%u,\"battery_mv\":%u", v->battery_pct, v->battery_mv);
	} else if (board_volts() == BOARD_VOLTS_SUPPLY) {
		dl_text(t, ",\"supply_mv\":%u", v->supply_mv);
	}
	dl_text(t, ",\"hotspot\":%s", bool_s(v->hotspot));
#if CONFIG_DMR
	if (ch->mode == RADIO_MODE_DMR) {
		dl_text(t, ",\"dmr\":{\"own_id\":%u,\"cc\":%u,\"ts\":%u,\"target\":%u,\"private\":%s,\"heard\":%u}",
			(unsigned)v->dmr.own_id, ch->color_code, ch->timeslot, (unsigned)v->dmr.target_id,
			bool_s(v->dmr.target_private), (unsigned)v->dmr.call.src);
	}
#endif
	ui_app_status_t ui;

	ui_get_status(&ui);
	dl_text(t, ",\"led_green\":%s,\"led_red\":%s,\"backlight\":%u,\"draws\":%u", bool_s((v->leds & RADIO_LED_GREEN) != 0),
		bool_s((v->leds & RADIO_LED_RED) != 0), ui_backlight(), (unsigned)ui.draws);
	dl_text(t, ",\"tcp_open\":%u}\n", (unsigned)tcp_open_count());
}

#if CONFIG_DEBUGLINK
// A number as JSON accepts it: -?(0|[1-9][0-9]*)
static bool is_number(const char *s, size_t n)
{
	size_t i = (n > 0 && s[0] == '-') ? 1u : 0u;

	if ((i >= n) || (n - i > 10) || ((s[i] == '0') && (n - i > 1))) {
		return false;
	}
	for (; i < n; i++) {
		if ((s[i] < '0') || (s[i] > '9')) {
			return false;
		}
	}
	return true;
}

// A view section's key=value lines as one flat JSON object; numbers stay numbers, the rest are strings
static bool view_json(dl_text_t *t, const char *query)
{
	int section = -1;
	const char *q = (query != NULL) ? strstr(query, "section=") : NULL;

	if (q == NULL) {
		dl_text(t, "{\"sections\":[");
		for (uint8_t i = 0; debuglink_view_name(i) != NULL; i++) {
			dl_text(t, "%s\"%s\"", (i > 0) ? "," : "", debuglink_view_name(i));
		}
		dl_text(t, "]}\n");
		return true;
	}
	q += 8;
	size_t qn = strcspn(q, "&");

	for (uint8_t i = 0; debuglink_view_name(i) != NULL; i++) {
		const char *nm = debuglink_view_name(i);

		if ((strlen(nm) == qn) && (memcmp(nm, q, qn) == 0)) {
			section = i;
		}
	}
	if ((section < 0) && (qn > 0) && (qn <= 2) && (q[0] >= '0') && (q[0] <= '9')) {
		section = (qn == 2) ? ((q[0] - '0') * 10 + (q[1] - '0')) : (q[0] - '0');
		if (debuglink_view_name((uint8_t)section) == NULL) {
			section = -1;
		}
	}
	if (section < 0) {
		return false;
	}
	dl_text_t v;

	dl_text_init(&v, buf + VIEW_AT, BUF_MAX - VIEW_AT);
	debuglink_view_section((uint8_t)section, net_radio_view(), &v);
	const char *p = buf + VIEW_AT, *end = p + v.len;
	bool first = true;

	dl_text(t, "{");
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *eq;

		if (nl == NULL) {
			nl = end;
		}
		eq = memchr(p, '=', (size_t)(nl - p));
		if (eq != NULL) {
			char key[48], val[96];
			size_t kn = (size_t)(eq - p), vn = (size_t)(nl - eq - 1);

			kn = (kn < sizeof(key) - 1) ? kn : sizeof(key) - 1;
			vn = (vn < sizeof(val) - 1) ? vn : sizeof(val) - 1;
			memcpy(key, p, kn);
			key[kn] = '\0';
			memcpy(val, eq + 1, vn);
			val[vn] = '\0';
			dl_text(t, "%s", first ? "" : ",");
			json_str(t, key);
			dl_text(t, ":");
			if (is_number(val, vn)) {
				dl_text(t, "%s", val);
			} else {
				json_str(t, val);
			}
			first = false;
		}
		p = nl + 1;
	}
	dl_text(t, "}\n");
	return true;
}
#endif

// ---- responses ----

static const char *reason(int code)
{
	switch (code) {
	case 101: return "Switching Protocols";
	case 200: return "OK";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 414: return "URI Too Long";
	case 503: return "Service Unavailable";
	default: return "Error";
	}
}

static void respond_enc(int c, int code, const char *type, uint8_t src, const void *body, uint32_t len, bool gz)
{
	hconn_t *h = &hc[c];

	h->hlen = (uint16_t)dl_fmt(h->hdr, sizeof(h->hdr),
				   "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n%sContent-Length: %u\r\nConnection: close\r\n"
				   "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
				   code, reason(code), type, gz ? "Content-Encoding: gzip\r\n" : "", (unsigned)len);
	h->hoff = 0;
	h->src = src;
	h->body = body;
	h->blen = h->head ? 0 : len;
	h->boff = 0;
	h->state = H_SEND;
	if (code == 200) {
		st.ok++;
	} else if (code == 404) {
		st.not_found++;
	} else {
		st.bad++;
	}
}

static void respond(int c, int code, const char *type, uint8_t src, const void *body, uint32_t len)
{
	respond_enc(c, code, type, src, body, len, false);
}

static void fail(int c, int code)
{
	respond(c, code, "text/plain", SRC_MEM, NULL, 0);	// the status line says it all
}

static bool screen_ok(void)
{
	const hal_display_t *d = board.display;

	return (d != NULL) && (gfx_framebuffer() != NULL) && ((d->bpp == 1) || (GFX_INDEXED && d->bpp == 4) ||
	       ((d->bpp == 16) && ((uint32_t)d->width * d->height * 2u <= GFX_FB_BYTES)));
}

// The screen goes out at the panel's own depth, so a 1 or 4 bpp frame is 16 or 4 times smaller than as RGB565
// (over PPP at 115200 baud a 240x320 screen takes about 4 s instead of 15): file header, BITMAPINFOHEADER, then the
// palette (bpp 1, 4) or the three BI_BITFIELDS masks (bpp 16)
static uint32_t bmp_header(void)
{
	uint8_t bpp = board.display->bpp;

	return 54u + ((bpp == 16) ? 12u : (4u << bpp));
}

static uint32_t bmp_row(void)
{
	return (((uint32_t)board.display->width * board.display->bpp + 31u) / 32u) * 4u;
}

static uint32_t bmp_size(void)
{
	return bmp_header() + bmp_row() * board.display->height;
}

static void put_bgr(uint8_t *p, color_t c)
{
	p[0] = (uint8_t)((c & 0x1Fu) << 3);
	p[1] = (uint8_t)(((c >> 5) & 0x3Fu) << 2);
	p[2] = (uint8_t)((c >> 11) << 3);
	p[3] = 0;
}

// Bytes of the BMP at off: a top-down (negative height) bitmap of the frame buffer. bpp 1 and 4: its rows as they
// are (both pack the leftmost pixel in the high bits, as BMP does), with the palette (bpp 1: the paper and ink of the
// UI gallery's screenshots). bpp 16: BI_BITFIELDS RGB565
static void bmp_bytes(uint32_t off, uint8_t *out, size_t n)
{
	const hal_display_t *d = board.display;
	uint32_t head = bmp_header(), row = bmp_row(), raw = ((uint32_t)d->width * d->bpp + 7u) / 8u;
	const uint8_t *fb = gfx_framebuffer();
	uint8_t h[BMP_HEADER_MAX];

	if (off < head) {
		memset(h, 0, sizeof(h));
		h[0] = 'B';
		h[1] = 'M';
		put32le(h + 2, bmp_size());
		put32le(h + 10, head);
		put32le(h + 14, 40);
		put32le(h + 18, d->width);
		put32le(h + 22, (uint32_t)-(int32_t)d->height);
		put16le(h + 26, 1);
		put16le(h + 28, d->bpp);
		put32le(h + 34, row * d->height);
		if (d->bpp == 16) {
			put32le(h + 30, 3);	// BI_BITFIELDS
			put32le(h + 54, 0xF800u);
			put32le(h + 58, 0x07E0u);
			put32le(h + 62, 0x001Fu);
		} else if (d->bpp == 1) {
			put32le(h + 46, 2);	// colors used
			put_bgr(h + 54, RGB565(0xC8, 0xD0, 0xB8));
			put_bgr(h + 58, RGB565(0x14, 0x18, 0x10));
		} else {
			put32le(h + 46, 16);
			for (uint8_t i = 0; i < 16u; i++) {
				put_bgr(h + 54 + 4u * i, gfx_palette(i));
			}
		}
	}
	for (size_t i = 0; i < n; i++, off++) {
		if (off < head) {
			out[i] = h[off];
		} else {
			uint32_t p = off - head, y = p / row, x = p % row;

			if (x >= raw) {
				out[i] = 0;
			} else if (d->bpp == 16) {
				gfx_read_rgb565(y * raw + x, &out[i], 1);
			} else {
				out[i] = fb[y * raw + x];
			}
		}
	}
	// Held while it goes out; the last byte lets the UI draw again at once (a page polling the screen would
	// otherwise keep it frozen)
	gfx_hold((off >= bmp_size()) ? 0 : hal_millis() + SHOT_HOLD_MS);
}

static bool claim_buf(int c)
{
	if ((buf_owner >= 0) && (buf_owner != c)) {
		return false;
	}
	buf_owner = c;
	return true;
}

static void release(int c)
{
	if (buf_owner == c) {
		buf_owner = -1;
	}
	hc[c].state = H_FREE;
}

// The request line is in h->line: pick the response
static void dispatch(int c)
{
	hconn_t *h = &hc[c];
	char *sp1 = strchr(h->line, ' ');
	char *path = (sp1 != NULL) ? sp1 + 1 : NULL;
	char *sp2 = (path != NULL) ? strchr(path, ' ') : NULL;

	st.requests++;
	if ((sp1 == NULL) || (sp2 == NULL)) {
		fail(c, 400);
		return;
	}
	*sp1 = '\0';
	*sp2 = '\0';
	h->head = strcmp(h->line, "HEAD") == 0;
	if (!h->head && (strcmp(h->line, "GET") != 0)) {
		fail(c, 405);
		return;
	}
	char *query = strchr(path, '?');

	if (query != NULL) {
		*query++ = '\0';
	}
	// The static files, gzip compressed with CONFIG_NET_HTTP_GZIP: sent so whatever the request's Accept-Encoding
	// (without one any coding is acceptable, RFC 9110 12.5.3; every browser takes gzip; curl needs --compressed)
	for (int i = 0; i < HTTP_ASSET_COUNT; i++) {
		if (strcmp(path, http_assets[i].path) == 0) {
			respond_enc(c, 200, http_assets[i].type, SRC_MEM, http_assets[i].data, http_assets[i].length,
				    http_assets[i].gzip != 0);
			return;
		}
	}
#if CONFIG_NET_WEBCTL
	if (strcmp(path, "/ws") == 0) {
		char acc[WS_ACCEPT_LEN + 1];

		if (h->head || !ws_accept(h->key, strlen(h->key), acc)) {
			fail(c, 400);	// not a WebSocket handshake
			return;
		}
		if (webctl_conn() >= 0) {
			fail(c, 503);	// one client at a time
			return;
		}
		h->hlen = (uint16_t)dl_fmt(h->hdr, sizeof(h->hdr), "HTTP/1.1 101 %s\r\nUpgrade: websocket\r\n"
					   "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", reason(101), acc);
		h->hoff = 0;
		h->src = SRC_NONE;
		h->blen = 0;
		h->boff = 0;
		h->ws = true;
		h->state = H_SEND;
		st.ok++;
		return;
	}
#endif
	if (strcmp(path, "/api/screen.bmp") == 0) {
		if (!screen_ok()) {
			fail(c, 404);
			return;
		}
		respond(c, 200, "image/bmp", SRC_BMP, NULL, bmp_size());
		return;
	}
	bool status = strcmp(path, "/status") == 0;
	bool viewp = strcmp(path, "/api/view") == 0;
#if CONFIG_NET_APRSIS
	bool setp = strcmp(path, "/api/aprsis") == 0;
#else
	bool setp = false;
#endif

	if (!status && !viewp && !setp) {
		fail(c, 404);
		return;
	}
	if (!claim_buf(c)) {
		*sp1 = ' ';	// the request line as it came, for the second dispatch
		*sp2 = ' ';
		if (query != NULL) {
			query[-1] = '?';
		}
		h->state = H_WAIT;	// another JSON response is going out: dispatched again when it's done
		st.busy_waits++;
		return;
	}
	dl_text_t t;
	bool ok = true;

	dl_text_init(&t, buf, BUF_MAX);
	if (status) {
		http_status_json(&t);
	} else if (viewp) {
#if CONFIG_DEBUGLINK
		ok = view_json(&t, query);
#else
		ok = false;
#endif
	} else {
#if CONFIG_NET_APRSIS
		// The APRS-IS client's settings, changed from the host only (not from beyond it)
		int code = aprsis_http(&t, query, tcp_remote_ip(c));

		if (code != 200) {
			buf_owner = -1;
			fail(c, code);
			return;
		}
#endif
	}
	if (!ok || t.full) {
		buf_owner = -1;
		fail(c, ok ? 500 : 404);
		return;
	}
	respond(c, 200, "application/json", SRC_BUF, buf, (uint32_t)t.len);
}

static void send_more(int c)
{
	hconn_t *h = &hc[c];
	uint8_t chunk[64];

	while (h->state == H_SEND) {
		int room = tcp_space(c);

		if (room <= 0) {
			return;
		}
		if (h->hoff < h->hlen) {
			int n = tcp_write(c, h->hdr + h->hoff, h->hlen - h->hoff);

			h->hoff = (uint16_t)(h->hoff + n);
			continue;
		}
		if (h->boff >= h->blen) {
#if CONFIG_NET_WEBCTL
			if (h->ws) {
				h->state = H_WS;
				webctl_open(c, hal_millis());
				return;
			}
#endif
			tcp_close(c);
			release(c);
			return;
		}
		uint32_t n = h->blen - h->boff;

		if (n > (uint32_t)room) {
			n = (uint32_t)room;
		}
		if (h->src == SRC_BMP) {
			n = (n > sizeof(chunk)) ? sizeof(chunk) : n;
			bmp_bytes(h->boff, chunk, n);
			n = (uint32_t)tcp_write(c, chunk, (int)n);
		} else {
			n = (uint32_t)tcp_write(c, h->body + h->boff, (int)n);
		}
		h->boff += n;
	}
}

static void h_opened(int c)
{
	memset(&hc[c], 0, sizeof(hc[c]));
	hc[c].state = H_REQ;
	hc[c].since = hal_millis();
}

#if CONFIG_NET_WEBCTL
// A header line is in: keep Sec-WebSocket-Key (field names are case-insensitive, RFC 9110 5.1)
static void header_line(hconn_t *h)
{
	static const char name[] = "sec-websocket-key:";
	size_t n = sizeof(name) - 1;

	h->hdr[h->hn] = '\0';
	h->hn = 0;
	for (size_t i = 0; i < n; i++) {
		char ch = h->hdr[i];

		if ((ch >= 'A') && (ch <= 'Z')) {
			ch = (char)(ch - 'A' + 'a');
		}
		if (ch != name[i]) {
			return;
		}
	}
	const char *v = h->hdr + n;

	while (*v == ' ' || *v == '\t') {
		v++;
	}
	size_t vn = strcspn(v, " \t");

	if (vn <= WS_KEY_LEN) {
		memcpy(h->key, v, vn);
		h->key[vn] = '\0';
	}
}
#endif

static void h_recv(int c, const uint8_t *d, int n)
{
	hconn_t *h = &hc[c];

#if CONFIG_NET_WEBCTL
	if (h->state == H_WS) {
		webctl_recv(c, d, n);
		return;
	}
#endif
	for (int i = 0; (i < n) && (h->state == H_REQ); i++) {
		char ch = (char)d[i];

		if (ch == '\r') {
			continue;
		}
		if (ch != '\n') {
			if (!h->line_done) {
				if (h->ln < LINE_MAX - 1) {
					h->line[h->ln] = ch;
				}
				h->ln = (uint8_t)((h->ln < LINE_MAX) ? (h->ln + 1u) : LINE_MAX);
			}
#if CONFIG_NET_WEBCTL
			else if (h->hn < HDR_MAX - 1) {
				h->hdr[h->hn++] = ch;
			}
#endif
			h->at_bol = false;	// header lines are skipped
			continue;
		}
		if (!h->line_done) {
			h->line_done = true;
			h->at_bol = true;
			if (h->ln >= LINE_MAX - 1) {
				fail(c, 414);
				send_more(c);
				return;
			}
			h->line[h->ln] = '\0';
		} else if (h->at_bol) {
			dispatch(c);	// the empty line: the request is complete
			send_more(c);
			return;
		} else {
			h->at_bol = true;
#if CONFIG_NET_WEBCTL
			header_line(h);
#endif
		}
	}
}

static void h_sent(int c)
{
#if CONFIG_NET_WEBCTL
	if (hc[c].state == H_WS) {
		webctl_sent(c);
		return;
	}
#endif
	send_more(c);
}

static void h_peer_closed(int c)
{
#if CONFIG_NET_WEBCTL
	if (hc[c].state == H_WS) {
		webctl_peer_closed(c);	// closes it
		hc[c].state = H_FREE;
		return;
	}
#endif
	if (hc[c].state != H_SEND) {
		tcp_close(c);
		release(c);
	}
}

static void h_closed(int c, bool error_)
{
	(void)error_;
#if CONFIG_NET_WEBCTL
	if (hc[c].state == H_WS) {
		webctl_closed(c);
	}
#endif
	release(c);
}

static const tcp_app_t app = {
	.opened = h_opened, .recv = h_recv, .sent = h_sent, .peer_closed = h_peer_closed, .closed = h_closed,
};

void http_start(void)
{
	memset(hc, 0, sizeof(hc));
	buf_owner = -1;
	tcp_listen(NET_HTTP_PORT, &app);
}

void http_poll(uint32_t now_ms)
{
#if CONFIG_NET_WEBCTL
	webctl_poll(now_ms);
#endif
	for (int c = 0; c < TCP_CONNS; c++) {
		hconn_t *h = &hc[c];

#if CONFIG_NET_WEBCTL
		if ((h->state == H_WS) && (webctl_conn() != c)) {
			h->state = H_FREE;	// webctl.c closed it
		}
#endif
		if ((h->state == H_REQ) && ((now_ms - h->since) > REQ_TIMEOUT_MS)) {
			tcp_abort(c);
			release(c);
		} else if ((h->state == H_WAIT) && (buf_owner < 0)) {
			dispatch(c);
			send_more(c);
		} else if (h->state == H_SEND) {
			send_more(c);
		}
	}
}

void http_view(dl_text_t *t)
{
	dl_text(t, "http.requests=%u\nhttp.ok=%u\nhttp.not_found=%u\nhttp.bad=%u\nhttp.busy_waits=%u\n",
		(unsigned)st.requests, (unsigned)st.ok, (unsigned)st.not_found, (unsigned)st.bad, (unsigned)st.busy_waits);
#if CONFIG_NET_WEBCTL
	webctl_view(t);
#endif
}

#endif	// CONFIG_NET_HTTP
