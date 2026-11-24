// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// netSvcTest's scripted TCP peer (netSvcTest.c) for the service cases (netSvcCases.c)
#ifndef NET_SVC_FAKES_H
#define NET_SVC_FAKES_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

extern int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

extern uint32_t now_ms;
extern bool net_capture;	// frames go to netSvcTest.pcap (off for the fuzz loop: tshark sees good traffic only)

typedef struct {
	uint32_t ip;		// the peer's address (the host, or a server beyond it)
	uint16_t port;		// the peer's port
	uint16_t rport;		// the radio's port
	uint32_t snd_nxt, rcv_nxt;
	uint16_t wnd;
	bool established, fin, reset, listening, refuse, no_ack;
	int syns;
	uint8_t rx[65536];
	int rxn;
} peer_t;

bool peer_connect(peer_t *p, uint32_t ip, uint16_t port, uint16_t rport);	// active open from the peer
void peer_listen(peer_t *p, uint32_t ip, uint16_t port);			// a server for the radio's active open
void peer_send(peer_t *p, const void *data, int len);
void peer_close(peer_t *p);
void peer_clear(peer_t *p);
void pump(void);
// UDP the radio sends to the host's port udp_catch.port (0: none caught): how many, the last one and its source port
typedef struct {
	uint16_t port;
	int count;
	uint16_t from_port;
	char last[600];
	int len;
} udp_catch_t;
extern udp_catch_t udp_catch;
void udp_to_radio(uint32_t ip, uint16_t sport, uint16_t dport, const char *text);
void run_for(uint32_t ms);
extern bool fake_gps_fitted;		// the board's GPS module answers present
uint32_t net_test_host_ip(void);
extern uint32_t net_test_radio_ip;	// the address the peers speak to: the radio's own, or IPGW_ANY_RADIO_IP
void tests_run(void);

#endif
