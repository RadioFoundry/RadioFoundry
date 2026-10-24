// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's PPP personality on a pseudo-terminal, for interop tests with a real PPP implementation (pppd):
// core/net/ppp.c and core/hostlink/hostlink_ppp.c on the real IP gateway (core/dmr/ipGateway.c: ping), with the
// minimal TCP (core/net/tcp.c) serving a one-line HTTP answer on port 80, the SNMP agent's engine and MIB
// (core/snmp, a fixed snapshot as in snmpPeer) on UDP 161, and the debug link's wire format (core/debuglink) on the
// UDP serial port 3334 answering PING and VERSION, so tools/rfoundry_debug.py --port udp:<radio> works.
//
//   pppPeer [--once] [--active] [PTY_NAME_FILE]
//
// Prints the pseudo-terminal's name (and writes it to PTY_NAME_FILE) and serves until killed; --once: until the first
// PPP link ends (exit status 0 when IPCP was open). --active starts with our Configure-Request, as the debug link's PPP
// op does; otherwise passive, waiting for the peer's. tools/rfoundry_ppp_interop.sh drives it with pppd.
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include "board.h"
#include "net/ppp.h"
#include "net/tcp.h"
#include "hostlink/hostlink_ppp.h"
#include "dmr/ipGateway.h"
#include "debuglink/debuglink_proto.h"
#include "snmp/snmp.h"
#include "snmp/snmpMib.h"

#define RADIO_ID	10005u		// the host gets 12.0.39.21, the radio is 12.0.39.20

static int master = -1;
static uint32_t t0;

static uint32_t now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u) - t0;
}

// ---- the board: the pseudo-terminal is the host port ----

static size_t port_write(const uint8_t *b, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t w = write(master, b + done, n - done);

		if (w < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				usleep(1000);
				continue;
			}
			break;
		}
		done += (size_t)w;
	}
	return done;
}
static bool port_true(void) { return true; }
static size_t port_read(uint8_t *b, size_t n) { (void)b; (void)n; return 0; }
static bool port_wait(uint32_t t) { (void)t; return false; }
static const hal_stream_t port = { port_true, port_true, port_read, port_wait, port_write };
const board_t board = { .name = "pppPeer", .host_port = &port, .netif = &hostlink_ppp_netif };

static uint8_t frame_buf[IPGW_MAX_FRAME];
uint8_t *hostlink_net_frame(void) { return frame_buf; }

bool ppp_foreign_start(uint8_t b)
{
	return (b == DL_SOF1) || (b == 0xE0) || ((b >= 'A') && (b <= 'Z')) || ((b >= 'a') && (b <= 'z'));
}

// ---- the gateway's user ----

bool ipGatewaySendFrame(const uint8_t *f, int length) { return board.netif->send(f, length); }
uint32_t ipGatewayRadioId(void) { return RADIO_ID; }
bool ipGatewayIPToAir(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl, uint8_t protocol, const uint8_t *l4, int length)
{
	printf("pppPeer: to the air: %s %u, protocol %u, %d bytes\n", group ? "talkgroup" : "radio", (unsigned)dst,
	       protocol, length);
	return true;
}
bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length)
{
	(void)group; (void)dst; (void)data; (void)length;
	return true;
}

// The debug link on UDP 3334: PING and VERSION
static dl_parser_t dl;
static uint8_t dl_reply[DL_HDR + DL_REPLY_MAX + 2];
static unsigned dl_requests;

static size_t dl_handler(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	dl_requests++;
	out[0] = 0;
	if (cmd == DL_CMD_PING) {
		memcpy(out + 1, req, len);
		return 1u + len;
	}
	if (cmd == DL_CMD_VERSION) {
		static const char v[] = "fw=pppPeer\nboard=pppPeer\nproto=1\nfeatures=NET_PPP,DMR_IP_GATEWAY,NET_TCP,SNMP\n";
		size_t n = sizeof(v) - 1;

		memcpy(out + 1, v, (n < cap - 1) ? n : cap - 1);
		return 1u + n;
	}
	out[0] = 1;	// unknown command
	return 1;
}

static void dl_write(const uint8_t *b, size_t n)
{
	ipGatewaySerialOut(b, (int)n);
}

void ipGatewaySerialIn(const uint8_t *data, int length)
{
	size_t at = 0;

	while (at < (size_t)length) {
		size_t n = dl_parser_feed(&dl, data + at, (size_t)length - at, now_ms(), dl_handler, dl_write, dl_reply);

		at += (n > 0) ? n : 1;
	}
}

// ---- TCP and a one-line HTTP server ----

static unsigned http_requests;

static void http_recv(int c, const uint8_t *data, int len)
{
	static const char answer[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
				     "RadioFoundry over PPP\n";

	(void)data;
	(void)len;
	http_requests++;
	tcp_write(c, answer, (int)sizeof(answer) - 1);
	tcp_close(c);
}

static const tcp_app_t http_app = { .recv = http_recv };
static uint8_t *tcp_buffer(void) { return ipGatewayLocalPayload(); }
static bool tcp_send(uint32_t dst, int length) { return ipGatewaySendLocal(6, dst, length); }
static const tcp_io_t tcp_io = { .buffer = tcp_buffer, .send = tcp_send, .local_ip = ipGatewayRadioIP };

static void tcp_in(uint32_t src, const uint8_t *seg, int length)
{
	tcp_input(src, seg, length, now_ms());
}

// ---- SNMP on UDP 161 ----

static snmpData_t data;
static snmpConfig_t cfg;
static snmpStats_t stats;
static snmpMibCtx_t ctx;
static snmpMib_t mib;

static void snmp_udp(bool fromAir, uint32_t src, uint16_t srcPort, uint32_t dst, uint16_t dstPort, const uint8_t *in,
		     int length)
{
	uint8_t out[IPGW_UDP_PAYLOAD_MAX];
	snmpRequestCfg_t rc = { .mib = &mib, .readHash = cfg.readHash, .writeHash = cfg.writeHash, .writeAllowed = false,
				.maxRepetitions = 16, .stats = &stats };
	bool auth;

	(void)fromAir; (void)dst; (void)dstPort;
	data.uptimeMs = now_ms();
	int m = snmpProcess(&rc, in, length, out, (int)sizeof(out), &auth);

	if (m > 0) {
		ipGatewayUdpSend(src, 161, srcPort, out, m);
	}
}

static void snmp_setup(void)
{
	data.dmrId = RADIO_ID;
	strcpy(data.name, "N0CALL");
	data.board = "pppPeer";
	data.firmware = "ppp-interop";
	data.batteryMv = 7600;
	data.batteryPct = 90;
	strcpy(data.zoneName, "Zone A");
	strcpy(data.channelName, "TG 91");
	data.rxHz = 439412500;
	data.txHz = 430012500;
	data.powerName = "Low";
	cfg.readHash = snmpCommunityHash((const uint8_t *)"public", 6);
	ctx.data = &data;
	ctx.cfg = &cfg;
	ctx.stats = &stats;
	snmpMibInit(&mib, &ctx);
	ipGatewayUdpBind(161, snmp_udp);
}

// ---- main ----

static const char *state_name(uint8_t s)
{
	static const char *const names[] = { "initial", "?", "closed", "stopped", "closing", "stopping", "req-sent",
					     "ack-rcvd", "ack-sent", "opened" };

	return (s < 10) ? names[s] : "?";
}

int main(int argc, char **argv)
{
	bool once = false, active = false;
	const char *name_file = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--once") == 0) {
			once = true;
		} else if (strcmp(argv[i], "--active") == 0) {
			active = true;
		} else {
			name_file = argv[i];
		}
	}
	t0 = 0;
	t0 = now_ms() - 1000;
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if ((master < 0) || (grantpt(master) != 0) || (unlockpt(master) != 0)) {
		perror("pppPeer: pseudo-terminal");
		return 2;
	}
	const char *slave = ptsname(master);
	// The slave in raw mode from the start (pppd sets its own modes when it opens it)
	int s = open(slave, O_RDWR | O_NOCTTY);
	struct termios tio;

	if ((s >= 0) && (tcgetattr(s, &tio) == 0)) {
		cfmakeraw(&tio);
		tcsetattr(s, TCSANOW, &tio);
	}
	printf("pppPeer: %s\n", slave);
	fflush(stdout);
	if (name_file != NULL) {
		FILE *f = fopen(name_file, "w");

		if (f != NULL) {
			fprintf(f, "%s\n", slave);
			fclose(f);
		}
	}
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

	ipGatewayInit((const uint8_t *)"\x02\x4F\x44\x52\x50\x01", (const uint8_t *)"\x02\x4F\x44\x52\x50\x00");
	dl_parser_init(&dl);
	tcp_init(&tcp_io, 0x5EED);
	tcp_listen(80, &http_app);
	ipGatewaySetTCP(tcp_in);
	snmp_setup();
	hostlink_ppp_begin(active, now_ms());

	bool was_up = false, ever_up = false;
	uint8_t lcp = 0xFF, ipcp = 0xFF;

	for (;;) {
		struct pollfd p = { .fd = master, .events = POLLIN };
		uint8_t buf[512];

		poll(&p, 1, 5);
		ssize_t n = read(master, buf, sizeof(buf));

		if (n > 0) {
			size_t took = ppp_input(buf, (size_t)n, now_ms());

			if (took < (size_t)n) {
				printf("pppPeer: %zd bytes after the link ended (another protocol)\n", n - (ssize_t)took);
			}
		}
		board.netif->tick(now_ms());
		tcp_poll(now_ms());

		ppp_stats_t st;
		ppp_stats(&st);
		if ((st.lcp != lcp) || (st.ipcp != ipcp)) {
			lcp = st.lcp;
			ipcp = st.ipcp;
			printf("pppPeer: %6u ms LCP %s, IPCP %s\n", (unsigned)now_ms(), state_name(lcp), state_name(ipcp));
			fflush(stdout);
		}
		if (ppp_ip_up() && !was_up) {
			uint32_t r = ipGatewayRadioIP(), h = ipGatewayHostIP();

			printf("pppPeer: IP up: radio %u.%u.%u.%u, host %u.%u.%u.%u\n", r >> 24, (r >> 16) & 255,
			       (r >> 8) & 255, r & 255, h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
			fflush(stdout);
			ever_up = true;
		}
		was_up = ppp_ip_up();
		if (!ppp_running()) {
			printf("pppPeer: link ended: %u frames in, %u out, %u errors; debug link %u, HTTP %u, SNMP %u requests\n",
			       (unsigned)st.rx_frames, (unsigned)st.tx_frames, (unsigned)st.rx_errors, dl_requests,
			       http_requests, (unsigned)stats.inPkts);
			fflush(stdout);
			if (once) {
				return ever_up ? 0 : 1;
			}
			tcp_abort_all();
			hostlink_ppp_begin(false, now_ms());
		}
	}
}
