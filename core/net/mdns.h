// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A small Multicast DNS responder with DNS service discovery (RFC 6762, RFC 6763) for the USB network personality
// (docs/spec/mdns.md): the radio answers for <host>.local (its address on the link) and advertises one instance of
// each service in a table. Probes its names at link up, announces them, answers queries (multicast, the unicast
// response bit, legacy unicast), suppresses known answers, renames itself on a conflict, says goodbye when its
// identity changes. IPv4 only. CONFIG_MDNS.
//
// No RTOS and no gateway calls here: the user feeds datagrams to port 5353 in and calls mdnsTick every few ms; the
// replies go out through mdns_io_t (core/net/net_services.c is the firmware glue, tests/host/mdnsTest.c the fake).
#ifndef ODR_MDNS_H
#define ODR_MDNS_H

#include <stdint.h>
#include <stdbool.h>

#define MDNS_PORT		5353
#define MDNS_GROUP		0xE00000FBu	// 224.0.0.251
#define MDNS_SERVICES_MAX	8
#define MDNS_LABEL_MAX		63

// One advertised service: type is "_<service>._udp" or "_<service>._tcp", the port is on the radio's address
typedef struct {
	const char *type;
	uint16_t port;
} mdns_service_t;

// Who the radio is. ip 0: no address on the link, the responder stays quiet
typedef struct {
	uint32_t ip;
	uint32_t dmr_id;
	char callsign[17];		// "" = none
	const char *board;		// "DM-1701"
	const char *firmware;		// the git version
} mdns_identity_t;

typedef struct {
	void (*identity)(mdns_identity_t *out);		// read about once a second
	// Room for one message of at most max bytes, kept until send returns; send false = busy, tried again later
	uint8_t *(*buffer)(int *max);
	bool (*send)(uint32_t dstIp, uint16_t dstPort, const uint8_t *msg, int length);
	uint32_t (*random)(void);
} mdns_io_t;

typedef enum { MDNS_OFF, MDNS_PROBING, MDNS_ANNOUNCING, MDNS_RUNNING } mdns_state_t;

typedef struct {
	uint32_t queries;		// queries received
	uint32_t answered;		// queries that got an answer
	uint32_t sent;			// messages sent (probes, announcements, answers, goodbyes)
	uint32_t busy;			// sends refused (busy), tried again
	uint32_t conflicts;		// renames after a conflict
	uint32_t bad;			// malformed messages
	uint8_t state;			// mdns_state_t
} mdns_stats_t;

void mdnsInit(const mdns_io_t *io, const mdns_service_t *services, int count);
// A datagram to port 5353 (multicast or to the radio's address)
void mdnsReceive(uint32_t srcIp, uint16_t srcPort, const uint8_t *msg, int length, uint32_t now);
// Every few ms: timers, the identity, what waits to be sent. link_up: the network link is up
void mdnsTick(uint32_t now, bool link_up);
void mdnsStats(mdns_stats_t *s);
// The host name without ".local" ("" while off)
const char *mdnsHostName(void);

#endif
