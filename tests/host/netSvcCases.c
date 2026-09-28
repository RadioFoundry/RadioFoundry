// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// netSvcTest's cases: each TCP service against the scripted peer, with fakes for the APRS engine and the radio core
#include <stdlib.h>
#include <string.h>
#include "netSvcFakes.h"
#include "fuzz.h"
#include "net/net.h"
#include "net/net_svc.h"
#include "net/tcp.h"
#include "config.h"
#include "radio/radio.h"
#include "ui/gfx.h"
#include "debuglink/debuglink.h"
#include "debuglink/dl_fmt.h"
#include "settings/settings.h"
#include "aprs/aprs.h"
#include "net/aprsis.h"
#include "net/http_assets.h"
#include "http_assets_gen.h"	// generated here by the Makefile (gzip, as the firmware's default)

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

static int http_get_from(peer_t *p, uint32_t ip, uint16_t port, const char *req)
{
	if (!peer_connect(p, ip, port, NET_HTTP_PORT)) {
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

static int http_get(peer_t *p, uint16_t port, const char *req)
{
	return http_get_from(p, net_test_host_ip(), port, req);
}

static const char *body_of(peer_t *p)
{
	char *b = strstr((char *)p->rx, "\r\n\r\n");
	return b ? b + 4 : "";
}

static void test_http(void)
{
	static peer_t a, b;

	// The page: the asset table's bytes as they are, gzip compressed with Content-Encoding (httpAssetsTest checks the
	// table against core/net/http_assets)
	CHECK(http_get(&a, 51000, "GET / HTTP/1.1\r\nHost: 12.0.39.20\r\nAccept: */*\r\n\r\n") > 0);
	CHECK(strncmp((char *)a.rx, "HTTP/1.0 200 OK\r\n", 17) == 0 && strstr((char *)a.rx, "Content-Type: text/html"));
	CHECK(strcmp(http_assets[0].path, "/") == 0 && http_assets[0].gzip &&
	      strstr((char *)a.rx, "\r\nContent-Encoding: gzip\r\n"));
	int body_at = (int)(body_of(&a) - (const char *)a.rx);
	CHECK(a.fin && (a.rxn - body_at) == (int)http_assets[0].length &&
	      memcmp(a.rx + body_at, http_assets[0].data, http_assets[0].length) == 0 && a.rx[body_at] == 0x1F);
	const char *cl = strstr((char *)a.rx, "Content-Length: ");
	CHECK(cl && atoi(cl + 16) == (int)http_assets[0].length);
	// Not a file: 404
	CHECK(http_get(&a, 51020, "GET /index.htm HTTP/1.0\r\n\r\n") > 0 && strstr((char *)a.rx, " 404 "));

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

// ---- fake radio core requests ----
static int req_vfo_on, req_vfo_set, req_bw = -1, req_mode = -1;
static uint32_t req_hz;
static bool req_ptt;
static int ptt_downs;
void radio_req_vfo(bool on) { req_vfo_on += on; }
void radio_req_vfo_set(uint32_t hz) { req_vfo_set++; req_hz = hz; }
void radio_req_ptt(bool down) { ptt_downs += down && !req_ptt; req_ptt = down; }
void radio_req_bandwidth(radio_bw_t bw) { req_bw = bw; }
void radio_req_mode(uint8_t mode) { req_mode = mode; }

// One command line, the answer (waits for it)
static const char *rig(peer_t *p, const char *line)
{
	peer_clear(p);
	peer_send(p, line, (int)strlen(line));
	run_for(10);
	p->rx[p->rxn] = 0;
	return (const char *)p->rx;
}

static void test_rigctl(void)
{
	static peer_t a, far;
	memset(&fake_view, 0, sizeof(fake_view));
	fake_view.ch.rx_hz = 145500000;
	fake_view.ch.tx_hz = 145500000;
	fake_view.rssi_dbm = -73;
	CHECK(peer_connect(&a, net_test_host_ip(), 52000, NET_RIGCTL_PORT));

	CHECK(strcmp(rig(&a, "f\n"), "145500000\n") == 0);
	CHECK(strcmp(rig(&a, "\\get_freq\n"), "145500000\n") == 0);
	CHECK(strcmp(rig(&a, "f VFOA\n"), "145500000\n") == 0);
	CHECK(strcmp(rig(&a, "+f\n"), "get_freq:\nFrequency: 145500000\nRPRT 0\n") == 0);

	// A frequency moves a channel to the VFO; read back at once it is the new one, before the radio shows it
	CHECK(strcmp(rig(&a, "F 146520000.000000\n"), "RPRT 0\n") == 0);
	CHECK(req_vfo_on == 1 && req_vfo_set == 1 && req_hz == 146520000u);
	CHECK(strcmp(rig(&a, "f\n"), "146520000\n") == 0);
	// Two at once: the second waits out the request gap, then goes
	fake_view.vfo = true;
	peer_clear(&a);
	peer_send(&a, "F 146530000\nF 146540000\n", 24);
	a.rx[a.rxn] = 0;
	CHECK(strcmp((char *)a.rx, "RPRT 0\nRPRT 0\n") == 0);
	run_for(50);
	CHECK(req_hz == 146540000u && req_vfo_on == 1);
	CHECK(strcmp(rig(&a, "F 50000000\n"), "RPRT -1\n") == 0);	// no band there
	CHECK(strcmp(rig(&a, "F abc\n"), "RPRT -1\n") == 0);
	run_for(1100);
	CHECK(strcmp(rig(&a, "f\n"), "145500000\n") == 0);	// the hold is over: what the radio shows

	// Mode
	CHECK(strcmp(rig(&a, "m\n"), "FM\n12500\n") == 0);
	CHECK(strcmp(rig(&a, "M FM 25000\n"), "RPRT 0\n") == 0 && req_bw == RADIO_BW_25K);
	CHECK(strcmp(rig(&a, "M FMN 0\n"), "RPRT 0\n") == 0 && req_bw == RADIO_BW_12K5);
	CHECK(strcmp(rig(&a, "M USB 2400\n"), "RPRT -1\n") == 0);
	CHECK(strcmp(rig(&a, "+m\n"), "get_mode:\nMode: FM\nPassband: 12500\nRPRT 0\n") == 0);

	// PTT: keyed, released, rate limited, released when the client goes
	CHECK(strcmp(rig(&a, "t\n"), "0\n") == 0);
	CHECK(strcmp(rig(&a, "T 1\n"), "RPRT 0\n") == 0 && req_ptt);
	fake_view.state = RS_TX;
	CHECK(strcmp(rig(&a, "t\n"), "1\n") == 0);
	CHECK(strcmp(rig(&a, "T 0\n"), "RPRT 0\n") == 0 && !req_ptt);
	fake_view.state = RS_IDLE;
	CHECK(strcmp(rig(&a, "T 1\n"), "RPRT -9\n") == 0 && !req_ptt);
	run_for(1000);
	CHECK(strcmp(rig(&a, "T 1\n"), "RPRT 0\n") == 0 && req_ptt);
	// The safety limit
	run_for(181000);
	CHECK(!req_ptt);
	run_for(1000);
	CHECK(strcmp(rig(&a, "\\set_ptt 1\n"), "RPRT 0\n") == 0 && req_ptt);

	// The S meter, other levels, VFO, split, info, chk_vfo, dump_state, unknown commands
	CHECK(strcmp(rig(&a, "l STRENGTH\n"), "20\n") == 0);
	CHECK(strcmp(rig(&a, "l RFPOWER\n"), "0.00\n") == 0);
	CHECK(strcmp(rig(&a, "l AF\n"), "RPRT -11\n") == 0);
	CHECK(strcmp(rig(&a, "v\n"), "VFOA\n") == 0);
	CHECK(strcmp(rig(&a, "V VFOA\n"), "RPRT 0\n") == 0);
	CHECK(strcmp(rig(&a, "V VFOB\n"), "RPRT -1\n") == 0);
	CHECK(strcmp(rig(&a, "s\n"), "0\nVFOA\n") == 0);
	CHECK(strcmp(rig(&a, "\\get_info\n"), "OpenDigitalRadio fake\n") == 0);
	CHECK(strcmp(rig(&a, "\\chk_vfo\n"), "0\n") == 0);
	const char *ds = rig(&a, "\\dump_state\n");
	CHECK(strncmp(ds, "0\n2\n2\n136000000.000000 174000000.000000 0x20 -1 -1", 49) == 0);
	CHECK(strstr(ds, "0x40000000\n0x0\n0x0\n0x0\n") != NULL);
	CHECK(strcmp(rig(&a, "xyz\n"), "RPRT -4\n") == 0);

	// A line longer than the line buffer is refused whole, never run cut short: 4.4652 GHz cut by a digit would be
	// 446.52 MHz, in band
	char longline[96];
	memset(longline, ' ', sizeof(longline));
	strcpy(longline + 52, "F 4465200000\n");	// 64 characters before the newline (63 fit)
	uint32_t sets = (uint32_t)req_vfo_set;
	run_for(1100);
	CHECK(strcmp(rig(&a, longline), "RPRT -1\n") == 0);
	run_for(50);
	CHECK((uint32_t)req_vfo_set == sets);
	// exactly 63 characters is still a line; the next line after a long one is taken as usual
	strcpy(longline + 52, "F 446520000\n");
	CHECK(strlen(longline) == 64 && strcmp(rig(&a, longline), "RPRT 0\n") == 0);
	run_for(50);
	CHECK(req_hz == 446520000u);
	memset(longline, 'x', 90);
	strcpy(longline + 90, "\n");
	peer_clear(&a);
	peer_send(&a, longline, 91);
	peer_send(&a, "f\n", 2);
	run_for(10);
	a.rx[a.rxn] = 0;
	CHECK(strncmp((char *)a.rx, "RPRT -1\n", 8) == 0 && strchr((char *)a.rx + 8, 'R') == NULL);
	run_for(1100);

	// The client goes while keyed: released
	peer_close(&a);
	CHECK(!req_ptt);

	// PTT only from the computer the radio is plugged into
	CHECK(peer_connect(&far, 0xC0000201u, 52001, NET_RIGCTL_PORT));
	CHECK(strcmp(rig(&far, "f\n"), "145500000\n") == 0);
	CHECK(strcmp(rig(&far, "T 1\n"), "RPRT -9\n") == 0 && !req_ptt);
	CHECK(strcmp(rig(&far, "q\n"), "") == 0 && far.fin);
	peer_close(&far);
	run_for(3000);
}

// ---- fake settings store and APRS view for the APRS-IS client ----
static settings_t settings;
static int settings_writes;
void settings_read(settings_t *out) { *out = settings; }
settings_t *settings_begin(void) { return &settings; }
void settings_end(uint32_t t) { (void)t; settings_writes++; }
static char my_call[12] = "N0CALL-7";
void aprs_get_view(aprs_view_t *v) { memset(v, 0, sizeof(*v)); strcpy(v->call, my_call); }
static char is_line[256];
static int is_lines;
void aprs_host_is_line(const char *line, size_t n, uint32_t t)
{
	(void)t;
	memcpy(is_line, line, n);
	is_line[n] = 0;
	is_lines++;
}

#define SERVER_IP	0xC000020Au	// 192.0.2.10, beyond the host

static void test_aprsis(void)
{
	static peer_t srv, h, far;

	CHECK(aprsis_passcode("N0CALL") == 13023 && aprsis_passcode("n0call-7") == 13023 && aprsis_passcode("W1AW") == 25988);

	// Off: no server set
	peer_listen(&srv, SERVER_IP, 14580);
	run_for(2000);
	CHECK(srv.syns == 0);

	// Set from beyond the host: refused; from the host: stored, the client connects and logs in
	CHECK(http_get_from(&far, 0xC0000201u, 53000, "GET /api/aprsis?server=192.0.2.10 HTTP/1.0\r\n\r\n") > 0);
	CHECK(strstr((char *)far.rx, " 403 ") && settings.aprsis_ip == 0);
	CHECK(http_get(&h, 53001, "GET /api/aprsis?server=999.0.2.10 HTTP/1.0\r\n\r\n") > 0 && strstr((char *)h.rx, " 400 "));
	CHECK(http_get(&h, 53002, "GET /api/aprsis?server=192.0.2.10%3A14580&range=50 HTTP/1.0\r\n\r\n") > 0);
	CHECK(strstr(body_of(&h), "\"server\":\"192.0.2.10:14580\"") && strstr(body_of(&h), "\"range_km\":50"));
	CHECK(settings.aprsis_ip == SERVER_IP && settings.aprsis_port == 14580 && settings.aprsis_range_km == 50);
	run_for(2000);
	CHECK(srv.syns == 1 && srv.established);
	srv.rx[srv.rxn] = 0;
	CHECK(strcmp((char *)srv.rx, "user N0CALL-7 pass 13023 vers OpenDigitalRadio unknown filter m/50\r\n") == 0);
	CHECK(!net_tnc_taken());
	static const char hello[] = "# aprsc 2.1.14\r\n# logresp N0CALL-7 verified, server T2TEST\r\n";
	peer_send(&srv, hello, sizeof(hello) - 1);
	CHECK(aprsis_online() && net_tnc_taken());

	// The engine's lines for APRS-IS go to the server; other frames don't
	peer_clear(&srv);
	static const uint8_t g[] = { 0xC0, 0x06, 'G', 'N', '0', 'C', 'A', 'L', 'L', '-', '7', '>', 'A', 'P', 'Z', 'O', 'D',
				     'R', ',', 'T', 'C', 'P', 'I', 'P', '*', ':', '>', 'h', 'i', 0xC0 };
	static const uint8_t data[] = { 0xC0, 0x00, 'x', 0xC0 };
	ring_put(data, sizeof(data));
	ring_put(g, sizeof(g));
	pump();
	srv.rx[srv.rxn] = 0;
	CHECK(ring_n == 0 && strcmp((char *)srv.rx, "N0CALL-7>APZODR,TCPIP*:>hi\r\n") == 0);

	// The server's lines go to the engine (its iGate rules decide); comments don't
	is_lines = 0;
	static const char msg[] = "# filter active\r\nW1AW>APRS,TCPIP*,qAC,T2TEST::N0CALL-7 :hello{12\r\n";
	peer_send(&srv, msg, sizeof(msg) - 1);
	CHECK(is_lines == 1 && strcmp(is_line, "W1AW>APRS,TCPIP*,qAC,T2TEST::N0CALL-7 :hello{12") == 0);

	// A silent server: dropped after 2 minutes, tried again 30 s later
	run_for(APRSIS_SILENCE_MS + 1000);
	CHECK(srv.reset && !aprsis_online());
	peer_listen(&srv, SERVER_IP, 14580);
	run_for(APRSIS_RETRY_MS + 1000);
	CHECK(srv.syns == 1 && srv.established);

	// Unverified (a wrong passcode): dropped, the next try much later
	static const char unv[] = "# logresp N0CALL-7 unverified, server T2TEST\r\n";
	peer_send(&srv, unv, sizeof(unv) - 1);
	CHECK(srv.reset && !aprsis_online());
	peer_listen(&srv, SERVER_IP, 14580);
	run_for(APRSIS_RETRY_MS * 2);
	CHECK(srv.syns == 0);
	run_for(APRSIS_RETRY_MAX_MS - APRSIS_RETRY_MS * 2 + 1000);
	CHECK(srv.syns == 1);
	peer_send(&srv, hello, sizeof(hello) - 1);
	CHECK(aprsis_online());

	// The callsign changes: a new login with the new one
	strcpy(my_call, "W1AW");
	peer_listen(&srv, SERVER_IP, 14580);
	run_for(2000);
	CHECK(srv.syns == 1);
	srv.rx[srv.rxn] = 0;
	CHECK(strncmp((char *)srv.rx, "user W1AW pass 25988 ", 21) == 0);

	// Turned off from the host: the connection goes
	CHECK(http_get(&h, 53003, "GET /api/aprsis?server=off HTTP/1.0\r\n\r\n") > 0 && strstr(body_of(&h), "\"state\""));
	run_for(2000);
	CHECK(srv.reset && settings.aprsis_ip == 0 && !aprsis_online());
	run_for(5000);
}

// ---- mutated input to every service (fuzz.h; ODR_FUZZ_ITERS) ----

// KISS: frames with escapes at the ends, cut, doubled FENDs, over the decoder's buffer; HTTP: mutated requests, each
// on its own connection, always answered with a status line and closed; rigctld: mutated command lines on one
// connection, each answered with something and the connection kept unless it was a quit; APRS-IS: mutated server
// lines. All through the real TCP; afterwards the services still work and nothing is left open.
static void test_fuzz(void)
{
	static peer_t a;
	fuzz_t f;
	uint8_t buf[700];
	long iters = fuzz_iters(1500), bad = 0;
	net_capture = false;
	fuzz_init(&f, 0x4E455453u);

	// KISS
	CHECK(peer_connect(&a, net_test_host_ip(), 50100, NET_KISS_PORT));
	static const uint8_t kseeds[3][12] = {
		{ 0xC0, 0x00, 'A', 0xDB, 0xDC, 'B', 0xDB, 0xDD, 'C', 0xC0 },
		{ 0xC0, 0x00, 0xDB, 0xC0, 0xDB, 0xDB, 0xDC, 0xC0, 0xC0 },	// an escape right before a FEND
		{ 0xC0, 0x06, 'G', 'N', '0', 'C', 'A', 'L', 'L', 0xDB },	// ends inside an escape
	};
	static const int klen[3] = { 10, 9, 10 };
	int frames0 = got_frames;
	for (long i = 0; i < iters; i++) {
		int s = (int)fuzz_below(&f, 4);
		int n;
		if (s == 3) {	// longer than the decoder's buffer
			n = 2 + (int)fuzz_below(&f, 600);
			memset(buf, 'z', (size_t)n);
			buf[0] = 0xC0;
			buf[1] = 0x00;
		} else {
			memcpy(buf, kseeds[s], (size_t)klen[s]);
			n = fuzz_mutate(&f, buf, klen[s], (int)sizeof(buf));
		}
		got_n = 0;
		peer_send(&a, buf, n);
		if (got_n > sizeof(got) || !a.established || a.reset) {
			bad++;
		}
	}
	CHECK(bad == 0 && got_frames > frames0);
	static const uint8_t frame[] = { 0xC0, 0xC0, 0x00, 'O', 'K', 0xC0 };	// after all that: decoded as usual
	got_n = 0;
	peer_send(&a, frame, sizeof(frame));
	CHECK(got_n == 3 && got[1] == 'O');
	peer_close(&a);
	run_for(3000);

	// HTTP
	static const char *const hseeds[] = {
		"GET / HTTP/1.1\r\nHost: 12.0.39.20\r\nAccept: */*\r\n\r\n",
		"GET /status HTTP/1.0\r\n\r\n",
		"GET /api/view?section=core&x=1 HTTP/1.0\r\n\r\n",
		"HEAD /api/view?section=1 HTTP/1.0\r\nX: y\r\n\r\n",
		"GET /api/screen.bmp HTTP/1.0\r\n\r\n",
		"GET /api/aprsis?server=off HTTP/1.0\r\n\r\n",
	};
	bad = 0;
	long answered = 0;
	for (long i = 0; i < iters / 3; i++) {
		int s = (int)fuzz_below(&f, 6);
		int n = (int)strlen(hseeds[s]);
		memcpy(buf, hseeds[s], (size_t)n);
		n = fuzz_mutate(&f, buf, n, (int)sizeof(buf) - 4);
		if (fuzz_below(&f, 2) == 0) {
			memcpy(buf + n, "\r\n\r\n", 4);	// mostly complete requests: the mutation is in the request
			n += 4;
		}
		uint16_t port = (uint16_t)(40000 + (i % 20000));
		if (!peer_connect(&a, net_test_host_ip(), port, NET_HTTP_PORT)) {
			bad++;
			continue;
		}
		peer_send(&a, buf, n);
		for (int k = 0; k < 40 && !a.fin && !a.reset; k++) {
			run_for(5);
		}
		if (a.fin) {
			answered++;
			if (strncmp((char *)a.rx, "HTTP/1.0 ", 9) != 0) {
				bad++;
			}
		}
		peer_close(&a);
		if (!a.fin) {
			run_for(10100);	// an incomplete request: the server drops it after 10 s
			if (!a.reset && !a.fin) {
				bad++;
			}
		}
	}
	run_for(3000);
	CHECK(bad == 0 && answered > iters / 12);
	CHECK(http_get(&a, 39999, "GET /status HTTP/1.0\r\n\r\n") > 0 && strstr((char *)a.rx, "200 OK"));
	run_for(3000);

	// rigctld (from beyond the host: PTT refused, so the fuzz never keys the radio)
	static const char *const rseeds[] = { "f\n", "F 146520000\n", "+M FM 12500\n", "\\set_ptt 1\n", "l STRENGTH\n",
					      "V VFOA\n", "\\dump_state\n", ";\\get_info\n", "f VFOA\n" };
	CHECK(peer_connect(&a, 0xC0000201u, 52100, NET_RIGCTL_PORT));
	bad = 0;
	for (long i = 0; i < iters; i++) {
		int s = (int)fuzz_below(&f, 9);
		int n = (int)strlen(rseeds[s]);
		memcpy(buf, rseeds[s], (size_t)n);
		n = fuzz_mutate(&f, buf, n, 200);
		if ((n == 0) || (buf[n - 1] != '\n')) {
			buf[n++] = '\n';
		}
		// q / Q / \quit would close: replace a line that starts so
		for (int k = 0; k < n; k++) {
			if ((k == 0 || buf[k - 1] == '\n') && (buf[k] == 'q' || buf[k] == 'Q' || buf[k] == '\\')) {
				buf[k] = 'x';
			}
		}
		peer_clear(&a);
		peer_send(&a, buf, n);
		run_for(25);
		if (!a.established || a.reset || a.fin || req_ptt) {
			bad++;
			if (a.reset || a.fin) {
				break;
			}
		}
	}
	CHECK(bad == 0);
	CHECK(strcmp(rig(&a, "\nf\n"), "") != 0);	// still answering
	peer_close(&a);
	run_for(3000);
	CHECK(tcp_open_count() == 0);
	net_capture = true;
	printf("# services fuzz: %ld KISS chunks (%d frames), %ld HTTP requests (%ld answered), %ld rigctld lines\n", iters,
	       got_frames - frames0, iters / 3, answered, iters);
}

void tests_run(void)
{
	test_kiss();
	test_http();
	test_rigctl();
	test_aprsis();
	test_fuzz();
}
