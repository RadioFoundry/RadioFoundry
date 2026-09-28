// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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
void run_for(uint32_t ms);
uint32_t net_test_host_ip(void);
void tests_run(void);

#endif
