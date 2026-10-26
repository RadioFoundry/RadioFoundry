// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The IP gateway's air side on the DMR data service (CONFIG_DMR_IP_GATEWAY): the default hostlink_net_air_t.
//
//   host link task   ip_to_air / raw_to_air copy the packet into the air queue (core/dmr/dmrAirQueue.h) and return
//                    (never block); false when the queue is full
//   radio task       the air queue's tick sends the queued packets in order through the data service: an IP packet
//                    with the compressed UDP/IPv4 header or the full one, confirmed or not (docs/spec/dmr-data.md 11,
//                    12), raw data as an unconfirmed SAP 10 packet; the service spaces them by its guard. The oldest
//                    packet refused by the service for HOSTLINK_NET_AIR_GIVE_UP_MS, or any packet queued longer than
//                    10 s, is dropped, counted and logged on the debug link
//   packets heard    the data service's packet hook (dmrDataEnv_t.packet) = hostlink_net_air_packet
//
// The radio core calls the radio task functions from its data service glue (radio_sm.c).
#ifndef RFOUNDRY_HOSTLINK_NET_AIR_H
#define RFOUNDRY_HOSTLINK_NET_AIR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#if CONFIG_DMR_IP_GATEWAY
#include "dmr/dmrData.h"

#define HOSTLINK_NET_AIR_MAX		DMR_DATA_MAX_PACKET	// layer 4 bytes of one packet to the air
#define HOSTLINK_NET_AIR_GIVE_UP_MS	2000U			// the oldest packet the service can't take by then is dropped

// Radio task: register the default air ops with hostlink_net (once, at the data service's start)
void hostlink_net_air_start(void);
// Radio task, with the data service's tick: own_id is the radio's DMR ID (the host's address follows it)
void hostlink_net_air_tick(uint32_t own_id, uint32_t now_ms);
// dmrDataEnv_t.packet
void hostlink_net_air_packet(const dmrDataPacket_t *packet, void *ctx);

typedef struct {
	uint32_t queued, sent, dropped, too_long;
	uint32_t refused_full;	// not taken: the air queue was full
	uint32_t failed;	// IP packets the service took but couldn't deliver (confirmed: no ACK after the tries)
} hostlink_net_air_stats_t;
void hostlink_net_air_stats(hostlink_net_air_stats_t *s);
#endif

#endif
