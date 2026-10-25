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

#include "dmr/dmrAirQueue.h"
#include "debuglink/dlog.h"

_Static_assert(IPGW_AIR_SRC_OURS == DMR_DATA_IP_OURS, "the gateway's source goes to the service as is");

#define MAX_AGE_MS	10000U	// a packet still queued after this is dropped: what it was for has likely moved on

static volatile uint32_t own;
static hostlink_net_air_stats_t stats;

// The host link task: into the air queue, in order behind what is there (#154: a host's datagrams in a row all go)
static bool put(bool raw, bool group, uint32_t dst, uint32_t src_ip, uint32_t dst_ip, uint8_t ttl, uint8_t protocol,
		const uint8_t *data, int length)
{
	if ((length <= 0) || (length > HOSTLINK_NET_AIR_MAX)) {
		stats.too_long++;
		return false;
	}
	airqItem_t it = { .kind = raw ? AIRQ_RAW : AIRQ_IP, .owner = AIRQ_OWNER_GATEWAY, .protocol = protocol,
			  .delivery = DMR_IPS_SETTINGS, .group = group, .dst = dst, .srcIp = src_ip, .dstIp = dst_ip,
			  .ttl = ttl, .waitMs = HOSTLINK_NET_AIR_GIVE_UP_MS, .maxAgeMs = MAX_AGE_MS, .data = data, .length = (uint16_t)length };
	if (!airqPut(&it)) {
		stats.refused_full++;
		dlog("net: to air dropped, queue full, %d octets to %u", length, (unsigned)dst);
		return false;
	}
	stats.queued++;
	return true;
}

// The radio task: a packet of ours has ended. A drop is logged on the debug link (the host can watch the log)
static void done(const airqItem_t *it, airqResult_t result)
{
	static const char *const why[] = { "sent", "not delivered", "service busy", "too old", "no DMR ID", "too long",
					   "canceled" };
	switch (result) {
	case AIRQ_SENT:
		stats.sent++;
		return;
	case AIRQ_FAILED:
		stats.failed++;	// no clear channel in time, or confirmed and never acknowledged
		break;
	case AIRQ_TOO_LONG:
		stats.too_long++;	// more bursts than a list holds
		break;
	default:
		stats.dropped++;	// never taken by the service, too old, or no ID to send from
		break;
	}
	dlog("net: to air %s, %u octets to %s%u", why[result], (unsigned)it->length, it->group ? "TG " : "",
	     (unsigned)it->dst);
	(void)why;
	(void)it;
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
	airqSetDone(AIRQ_OWNER_GATEWAY, done);
	hostlink_net_set_air(&air);
}

void hostlink_net_air_tick(uint32_t own_id, uint32_t now_ms)
{
	(void)now_ms;
	own = own_id & 0xFFFFFFu;
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
