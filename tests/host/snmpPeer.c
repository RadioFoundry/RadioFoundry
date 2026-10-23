// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio's SNMP agent (core/snmp, the real MIB on a fixed snapshot) on a UDP socket at 127.0.0.1, for testing with
// independent managers (tools/odr_snmp.py, and the system's SNMP command line tools when installed):
//   snmpPeer PORT           answer requests until 1 s passes without one (read community public, write private)
//   snmpPeer --trap PORT    send one odrCallStart and one coldStart notification to 127.0.0.1:PORT
// snmp_peer_check.sh drives it.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "snmp/snmp.h"
#include "snmp/snmpMib.h"

static snmpData_t data;
static snmpConfig_t cfg;
static snmpStats_t stats;
static snmpMibCtx_t ctx;
static snmpMib_t mib;

int main(int argc, char **argv)
{
	bool trap = (argc == 3) && (strcmp(argv[1], "--trap") == 0);
	int port = (argc >= 2) ? atoi(argv[argc - 1]) : 0;

	if ((port <= 0) || ((argc == 3) && !trap)) {
		fprintf(stderr, "usage: snmpPeer [--trap] PORT\n");
		return 2;
	}
	data.uptimeMs = 987650;
	data.dmrId = 3100001;
	strcpy(data.name, "N0CALL");
	data.board = "board1";
	data.firmware = "peer-test";
	data.batteryMv = 7600;
	data.batteryPct = 90;
	data.mode = 1;
	data.rssiDbm = -101;
	strcpy(data.zoneName, "Zone A");
	strcpy(data.channelName, "TG 91");
	data.channelNumber = 3;
	data.rxHz = 439412500;
	data.txHz = 430012500;
	data.power = 1;
	data.powerName = "Low";
	data.cc = 1;
	data.ts = 1;
	data.talkTarget = 91;
	data.lastSrc = 3100002;
	data.lastDst = 91;
	data.lastGroup = true;
	data.lastMs = 900000;
	cfg.writeHash = snmpCommunityHash((const uint8_t *)"private", 7);
	ctx.data = &data;
	ctx.cfg = &cfg;
	ctx.stats = &stats;
	snmpMibInit(&mib, &ctx);

	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	uint8_t in[1500], out[1500];

	if (trap) {
		snmpEvent_t ev = { .kind = SNMP_EV_CALL_START, .src = 3100002, .dst = 91, .group = true, .slot = 1,
				   .atMs = 123400 };
		a.sin_port = htons((uint16_t)port);
		int n = snmpEventEncode(&ev, 1, out, sizeof(out));
		sendto(s, out, (size_t)n, 0, (struct sockaddr *)&a, sizeof(a));
		ev.kind = SNMP_EV_COLD_START;
		n = snmpEventEncode(&ev, 2, out, sizeof(out));
		sendto(s, out, (size_t)n, 0, (struct sockaddr *)&a, sizeof(a));
		close(s);
		return 0;
	}
	a.sin_port = htons((uint16_t)port);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
		perror("snmpPeer: bind");
		return 1;
	}
	// Up to 20 s for the first request (a loaded machine, e.g. make -j, can take seconds to start the Python
	// manager), then 1 s idle ends the run
	struct timeval tv = { .tv_sec = 20 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(s, in, sizeof(in), 0, (struct sockaddr *)&from, &fl);

		if (n <= 0) {
			break;	// idle: done
		}
		if (tv.tv_sec != 1) {
			tv.tv_sec = 1;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		}
		snmpRequestCfg_t rc = { .mib = &mib, .readHash = cfg.readHash, .writeHash = cfg.writeHash,
					.writeAllowed = true, .maxRepetitions = 32, .stats = &stats };
		bool auth;
		int m = snmpProcess(&rc, in, (int)n, out, 548, &auth);	// the USB answer limit (snmp_agent.c)

		if (m > 0) {
			sendto(s, out, (size_t)m, 0, (struct sockaddr *)&from, fl);
		}
		data.uptimeMs += 10;
	}
	close(s);
	printf("snmpPeer: %u requests, %u answers, %u parse errors\n", (unsigned)stats.inPkts, (unsigned)stats.outPkts,
	       (unsigned)stats.inASNParseErrs);
	return stats.inASNParseErrs ? 1 : 0;
}
