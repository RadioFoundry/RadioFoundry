// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// netSvcTest's cases: each TCP service against the scripted peer, with fakes for the APRS engine and the radio core
#include <stdlib.h>
#include <string.h>
#include "netSvcFakes.h"
#include "net/net.h"
#include "net/net_svc.h"
#include "net/tcp.h"
#include "config.h"
#include "radio/radio.h"
#include "ui/gfx.h"
#include "debuglink/debuglink.h"
#include "debuglink/dl_fmt.h"

// ---- fake APRS engine (aprs.h / aprs_host.h) ----
static bool kiss_on = true;
static uint8_t got[1024];
static size_t got_n;
static int got_frames;
static uint8_t ring[4096];
static size_t ring_n;

void aprs_host_kiss_frame(const uint8_t *f, size_t n, uint32_t t)
{
	(void)t;
	memcpy(got, f, n);
	got_n = n;
	got_frames++;
}

size_t aprs_host_output(uint8_t *out, size_t max)
{
	size_t n = ring_n < max ? ring_n : max;
	memcpy(out, ring, n);
	memmove(ring, ring + n, ring_n - n);
	ring_n -= n;
	return n;
}

bool aprs_host_enabled(void)
{
	return kiss_on;
}

static void ring_put(const void *d, size_t n)
{
	memcpy(ring + ring_n, d, n);
	ring_n += n;
}

// ---- KISS over TCP ----
static void test_kiss(void)
{
	static peer_t a, b;
	CHECK(peer_connect(&a, net_test_host_ip(), 50001, NET_KISS_PORT));
	CHECK(net_tnc_taken());

	// A data frame from the client, with an escaped FEND and FESC: the engine gets it decoded
	static const uint8_t frame[] = { 0xC0, 0x00, 'A', 0xDB, 0xDC, 'B', 0xDB, 0xDD, 'C', 0xC0 };
	peer_send(&a, frame, sizeof(frame));
	CHECK(got_frames == 1 && got_n == 6 && got[0] == 0 && got[1] == 'A' && got[2] == 0xC0 && got[4] == 0xDB);
	// Split across segments
	peer_send(&a, frame, 4);
	peer_send(&a, frame + 4, sizeof(frame) - 4);
	CHECK(got_frames == 2 && got_n == 6);

	// The engine's output goes to the client, frame for frame (the host link leaves it alone)
	static const uint8_t out1[] = { 0xC0, 0x00, 'x', 0xDB, 0xDC, 'y', 0xC0 };
	static const uint8_t out2[] = { 0xC0, 0x06, 'G', 'N', '0', 'C', 'A', 'L', 'L', 0xC0 };
	ring_put(out1, sizeof(out1));
	ring_put(out2, sizeof(out2));
	pump();
	CHECK(ring_n == 0 && a.rxn == (int)(sizeof(out1) + sizeof(out2)));
	CHECK(memcmp(a.rx, out1, sizeof(out1)) == 0 && memcmp(a.rx + sizeof(out1), out2, sizeof(out2)) == 0);

	// A lot of output: it goes as the client acknowledges, nothing lost
	peer_clear(&a);
	static uint8_t big[300];
	big[0] = 0xC0;
	big[1] = 0x00;
	for (int i = 2; i < 299; i++) {
		big[i] = (uint8_t)('a' + i % 26);
	}
	big[299] = 0xC0;
	for (int k = 0; k < 10; k++) {
		ring_put(big, sizeof(big));
	}
	pump();
	CHECK(ring_n == 0 && a.rxn == 10 * (int)sizeof(big));

	// A second client replaces the first
	CHECK(peer_connect(&b, net_test_host_ip(), 50002, NET_KISS_PORT));
	CHECK(a.reset && b.established);
	ring_put(out1, sizeof(out1));
	pump();
	CHECK(b.rxn == (int)sizeof(out1));

	// KISS turned off in the settings: the output stays with the host link
	kiss_on = false;
	CHECK(!net_tnc_taken());
	ring_put(out1, sizeof(out1));
	pump();
	CHECK(ring_n == sizeof(out1));
	kiss_on = true;
	ring_n = 0;

	// The client closes: the port is free, the host link has the output again
	peer_close(&b);
	CHECK(b.fin && !net_tnc_taken());
	run_for(3000);
}

// ---- fake radio core, frame buffer and debug link view ----
static radio_view_t fake_view;
void radio_get_view(radio_view_t *v) { *v = fake_view; }
static uint16_t fb[160 * 128];
static int holds;
static bool big_view;
const void *gfx_framebuffer(void) { return fb; }
void gfx_hold(uint32_t until) { (void)until; holds++; }
const char *debuglink_view_name(uint8_t s) { return s == 0 ? "core" : (s == 1 ? "net" : NULL); }
void debuglink_view_section(uint8_t s, const radio_view_t *v, dl_text_t *t)
{
	(void)v;
	dl_text(t, "section=%s\n", debuglink_view_name(s));
	if (s == 0) {
		dl_text(t, "rssi=-90\nhex=0c00\nzero=0\nlead=007\n");
		dl_text_kv_str(t, "name", "a\"b\\c");
	}
	for (int i = 0; big_view && i < 30; i++) {
		dl_text(t, "key_%02d=value_xxxxxxxxxxxxxx\n", i);
	}
}

static int http_get(peer_t *p, uint16_t port, const char *req)
{
	if (!peer_connect(p, net_test_host_ip(), port, NET_HTTP_PORT)) {
		return -1;
	}
	peer_send(p, req, (int)strlen(req));
	for (int i = 0; i < 100 && !p->fin && !p->reset; i++) {
		run_for(5);
	}
	p->rx[p->rxn] = 0;
	if (p->fin) {
		peer_close(p);
	}
	return p->rxn;
}

static const char *body_of(peer_t *p)
{
	char *b = strstr((char *)p->rx, "\r\n\r\n");
	return b ? b + 4 : "";
}

static void test_http(void)
{
	static peer_t a, b;

	// The page
	CHECK(http_get(&a, 51000, "GET / HTTP/1.1\r\nHost: 12.0.39.20\r\nAccept: */*\r\n\r\n") > 0);
	CHECK(strncmp((char *)a.rx, "HTTP/1.0 200 OK\r\n", 17) == 0 && strstr((char *)a.rx, "Content-Type: text/html"));
	CHECK(a.fin && strncmp(body_of(&a), "<!DOCTYPE html>", 15) == 0 && strstr(body_of(&a), "</html>"));
	const char *cl = strstr((char *)a.rx, "Content-Length: ");
	CHECK(cl && atoi(cl + 16) == (int)strlen(body_of(&a)));

	// HEAD: the header only
	CHECK(http_get(&a, 51001, "HEAD / HTTP/1.0\r\n\r\n") > 0);
	CHECK(strstr((char *)a.rx, "200 OK") && strlen(body_of(&a)) == 0 && a.fin);

	// The status as JSON, names escaped
	memset(&fake_view, 0, sizeof(fake_view));
	strcpy(fake_view.ch.name, "Ch \"1\"");
	fake_view.ch.rx_hz = 145500000;
	fake_view.ch.tx_hz = 145500000;
	fake_view.rssi_dbm = -97;
	fake_view.power = 1;
	CHECK(http_get(&a, 51002, "GET /status HTTP/1.0\r\n\r\n") > 0);
	const char *j = body_of(&a);
	CHECK(strstr((char *)a.rx, "application/json") && j[0] == '{');
	CHECK(strstr(j, "\"rx_hz\":145500000") && strstr(j, "\"channel\":\"Ch \\\"1\\\"\"") && strstr(j, "\"rssi_dbm\":-97"));
	CHECK(strstr(j, "\"power\":\"High\"") && strstr(j, "\"mode\":\"fm\"") && strstr(j, "\"radio\":\"fake\""));

	// A view section as JSON: numbers stay numbers, the rest strings
	CHECK(http_get(&a, 51003, "GET /api/view?section=core HTTP/1.0\r\n\r\n") > 0);
	j = body_of(&a);
	CHECK(strstr(j, "{\"section\":\"core\",\"rssi\":-90,\"hex\":\"0c00\",\"zero\":0,\"lead\":\"007\",") != NULL);
	CHECK(strstr(j, "\"name\":\"a\\\"b\\\\c\"}") != NULL);
	CHECK(http_get(&a, 51004, "GET /api/view?section=1 HTTP/1.0\r\n\r\n") > 0 && strstr(body_of(&a), "\"net\""));
	CHECK(http_get(&a, 51005, "GET /api/view HTTP/1.0\r\n\r\n") > 0);
	CHECK(strstr(body_of(&a), "{\"sections\":[\"core\",\"net\"]}") != NULL);
	CHECK(http_get(&a, 51006, "GET /api/view?section=nope HTTP/1.0\r\n\r\n") > 0 && strstr((char *)a.rx, " 404 "));

	// The screen as a BMP, straight from the frame buffer
	for (int i = 0; i < 160 * 128; i++) {
		fb[i] = (uint16_t)(i * 7);
	}
	holds = 0;
	CHECK(http_get(&a, 51007, "GET /api/screen.bmp HTTP/1.0\r\n\r\n") > 0);
	const uint8_t *bmp = (const uint8_t *)body_of(&a);
	int blen = a.rxn - (int)(bmp - a.rx);
	CHECK(blen == 66 + 160 * 128 * 2 && bmp[0] == 'B' && bmp[1] == 'M' && holds > 0);
	CHECK(bmp[10] == 66 && bmp[18] == 160 && bmp[28] == 16 && bmp[30] == 3);
	CHECK(bmp[22] == 0x80 && bmp[23] == 0xFF && bmp[24] == 0xFF && bmp[25] == 0xFF);	// -128: top-down
	CHECK(memcmp(bmp + 66, fb, sizeof(fb)) == 0);

	// Errors
	CHECK(http_get(&a, 51008, "POST / HTTP/1.0\r\n\r\n") > 0 && strstr((char *)a.rx, " 405 "));
	CHECK(http_get(&a, 51009, "GET /nothing HTTP/1.0\r\n\r\n") > 0 && strstr((char *)a.rx, " 404 "));
	CHECK(http_get(&a, 51010, "garbage\r\n\r\n") > 0 && strstr((char *)a.rx, " 400 "));
	char longreq[300] = "GET /";
	memset(longreq + 5, 'x', 200);
	strcpy(longreq + 205, " HTTP/1.0\r\n\r\n");
	CHECK(http_get(&a, 51011, longreq) > 0 && strstr((char *)a.rx, " 414 "));

	// A slow client, a byte at a time
	CHECK(peer_connect(&a, net_test_host_ip(), 51012, NET_HTTP_PORT));
	static const char slow[] = "GET /status HTTP/1.0\r\nX: y\r\n\r\n";
	for (size_t i = 0; i < sizeof(slow) - 1; i++) {
		peer_send(&a, slow + i, 1);
	}
	run_for(50);
	CHECK(a.fin && strstr((char *)a.rx, "200 OK") && strstr((char *)a.rx, "\"rx_hz\""));

	// Two JSON requests at once: the second waits for the shared buffer, both are answered
	CHECK(peer_connect(&a, net_test_host_ip(), 51013, NET_HTTP_PORT));
	CHECK(peer_connect(&b, net_test_host_ip(), 51014, NET_HTTP_PORT));
	a.no_ack = true;	// a holds the buffer: its response is longer than the TCP buffer and a doesn't acknowledge
	big_view = true;
	static const char vreq[] = "GET /api/view?section=core HTTP/1.0\r\n\r\n";
	peer_send(&a, vreq, sizeof(vreq) - 1);
	peer_send(&b, vreq, sizeof(vreq) - 1);
	run_for(50);
	CHECK(!b.fin);
	a.no_ack = false;
	run_for(3000);
	CHECK(a.fin && b.fin && strstr(body_of(&b), "\"key_29\":\"value_xxxxxxxxxxxxxx\"}"));
	big_view = false;
	peer_close(&a);
	peer_close(&b);

	// A client that never sends its request is dropped after 10 s
	CHECK(peer_connect(&a, net_test_host_ip(), 51015, NET_HTTP_PORT));
	run_for(11000);
	CHECK(a.reset);
	run_for(3000);
	CHECK(tcp_open_count() == 0);
}

void tests_run(void)
{
	test_kiss();
	test_http();
}
