// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The IP gateway's air side on the DMR data service (see hostlink_net_air.h)
#include "config.h"
#if CONFIG_DMR_IP_GATEWAY	// the whole file
#include <string.h>
#include "hostlink/hostlink_net_air.h"
#include "hostlink/hostlink_net.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"
#include "dmr/ipGateway.h"

_Static_assert(IPGW_AIR_SRC_OURS == DMR_DATA_IP_OURS, "the gateway's source goes to the service as is");

#define PREAMBLES	2	// preamble CSBKs before the packet, as for an SMS (receivers get time to sync)

// FULL: waiting for the radio task; SERVICE: an IP packet the data service has (compressed / confirmed there), read
// from mb.data until dmrDataServiceIpBusy() is false
typedef enum { MB_EMPTY, MB_FULL, MB_SERVICE } mb_state_t;

// One packet from the host link task to the radio task (single producer, single consumer)
static struct {
	bool raw;		// SAP 10 user data, else an IP packet from layer 4 on
	bool group;
	uint32_t dst;
	uint32_t src_ip;	// the IPv4 addresses and TTL for the air (ipGatewayIPToAir)
	uint32_t dst_ip;
	uint8_t ttl;
	uint8_t protocol;
	int length;
	uint32_t since_ms;	// first tried (0: not yet)
	uint8_t data[HOSTLINK_NET_AIR_MAX];
} mb;
static volatile uint8_t mb_state = MB_EMPTY;
static volatile uint32_t own;
static DMR_DATA_BUFFER dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
static hostlink_net_air_stats_t stats;

static bool put(bool raw, bool group, uint32_t dst, uint32_t src_ip, uint32_t dst_ip, uint8_t ttl, uint8_t protocol,
		const uint8_t *data, int length)
{
	if ((length <= 0) || (length > HOSTLINK_NET_AIR_MAX)) {
		stats.too_long++;
		return false;
	}
	if (mb_state != MB_EMPTY) {
		return false;	// one at a time: the host's stack retries (TCP is not carried anyway, ping repeats)
	}
	mb.raw = raw;
	mb.group = group;
	mb.dst = dst;
	mb.src_ip = src_ip;
	mb.dst_ip = dst_ip;
	mb.ttl = ttl;
	mb.protocol = protocol;
	mb.length = length;
	mb.since_ms = 0;
	memcpy(mb.data, data, (size_t)length);
	__sync_synchronize();	// the packet is complete before the flag says so
	mb_state = MB_FULL;
	stats.queued++;
	return true;
}

static uint32_t air_radio_id(void)
{
	return own;
}

static bool air_ip(bool group, uint32_t dst, uint32_t src_ip, uint32_t dst_ip, uint8_t ttl, uint8_t protocol,
		   const uint8_t *l4, int length)
{
	return put(false, group, dst, src_ip, dst_ip, ttl, protocol, l4, length);
}

static bool air_raw(bool group, uint32_t dst, const uint8_t *data, int length)
{
	return put(true, group, dst, 0, 0, 0, 0, data, length);
}

static const hostlink_net_air_t air = { .radio_id = air_radio_id, .ip_to_air = air_ip, .raw_to_air = air_raw };

void hostlink_net_air_start(void)
{
	hostlink_net_set_air(&air);
}

void hostlink_net_air_tick(uint32_t own_id, uint32_t now_ms)
{
	own = own_id & 0xFFFFFFu;
	if (mb_state == MB_SERVICE) {
		if (!dmrDataServiceIpBusy()) {
			if (dmrDataServiceIpResult() == DMR_REQ_OK) {
				stats.sent++;
			} else {
				stats.failed++;	// no clear channel in time, or confirmed and never acknowledged
			}
			mb_state = MB_EMPTY;
		}
		return;
	}
	if (mb_state != MB_FULL) {
		return;
	}
	__sync_synchronize();
	if (mb.since_ms == 0) {
		mb.since_ms = (now_ms != 0) ? now_ms : 1;
	}
	if (own == 0) {
		stats.dropped++;	// no ID to send from
		mb_state = MB_EMPTY;
		return;
	}
	if (!mb.raw) {
		// The service builds it (compressed header, confirmed delivery: its settings) and keeps reading mb.data
		if (dmrDataServiceSendIP(mb.group, mb.dst, mb.src_ip, mb.dst_ip, mb.ttl, mb.protocol, mb.data, mb.length)) {
			mb_state = MB_SERVICE;
			return;
		}
		stats.refused_busy++;
		if ((now_ms - mb.since_ms) >= HOSTLINK_NET_AIR_GIVE_UP_MS) {
			stats.dropped++;
			mb_state = MB_EMPTY;
		}
		return;
	}
	int n = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_SHORT_DATA, mb.group, mb.dst, own, mb.data, mb.length,
				   DT_RATE_12_DATA, PREAMBLES, bursts, DMR_DATA_MAX_BURSTS);

	if (n <= 0) {
		stats.too_long++;	// more bursts than a list holds
		mb_state = MB_EMPTY;
		return;
	}
	if (dmrDataServiceSendBursts(bursts, n)) {
		stats.sent++;
		mb_state = MB_EMPTY;
		return;
	}
	stats.refused_busy++;
	if ((now_ms - mb.since_ms) >= HOSTLINK_NET_AIR_GIVE_UP_MS) {
		stats.dropped++;	// the channel never came free, or not a DMR channel / receive only
		mb_state = MB_EMPTY;
	}
}

void hostlink_net_air_packet(const dmrDataPacket_t *packet, void *ctx)
{
	(void)ctx;
	hostlink_net_packet(packet);
}

void hostlink_net_air_stats(hostlink_net_air_stats_t *s)
{
	*s = stats;
}

#endif	// CONFIG_DMR_IP_GATEWAY
