// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The IP gateway's air side on the DMR data service (CONFIG_DMR_IP_GATEWAY): the default hostlink_net_air_t.
//
//   host link task   ip_to_air / raw_to_air copy the packet into a one-packet mailbox and return (never block)
//   radio task       hostlink_net_air_tick() builds the bursts (dmrDataBuildIP: SAP 4, or an unconfirmed SAP 10
//                    packet for raw data, rate 1/2) and hands them to dmrDataServiceSendBursts, which does the listen
//                    before talk and the TX; a busy service is tried again until HOSTLINK_NET_AIR_GIVE_UP_MS
//   packets heard    the data service's packet hook (dmrDataEnv_t.packet) = hostlink_net_air_packet
//
// The radio core calls the three radio task functions from its data service glue (radio_sm.c).
#ifndef ODR_HOSTLINK_NET_AIR_H
#define ODR_HOSTLINK_NET_AIR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#if CONFIG_DMR_IP_GATEWAY
#include "dmr/dmrData.h"

#define HOSTLINK_NET_AIR_MAX		DMR_DATA_MAX_PACKET	// layer 4 bytes of one packet to the air
#define HOSTLINK_NET_AIR_GIVE_UP_MS	2000U			// a packet the service can't take by then is dropped

// Radio task: register the default air ops with hostlink_net (once, at the data service's start)
void hostlink_net_air_start(void);
// Radio task, with the data service's tick: own_id is the radio's DMR ID (the host's address follows it)
void hostlink_net_air_tick(uint32_t own_id, uint32_t now_ms);
// dmrDataEnv_t.packet
void hostlink_net_air_packet(const dmrDataPacket_t *packet, void *ctx);

typedef struct {
	uint32_t queued, sent, refused_busy, dropped, too_long;
} hostlink_net_air_stats_t;
void hostlink_net_air_stats(hostlink_net_air_stats_t *s);
#endif

#endif
