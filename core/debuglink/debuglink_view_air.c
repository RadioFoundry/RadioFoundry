// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air section: the data service's TX queue (docs/spec/dmr-data.md 12), its post-TX guard, and zone sharing
// (docs/spec/zone-share.md). "airq.built=0" without the queue; "zs.built=0" without zone sharing
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_DMR_AIR_QUEUE
#include "dmr/dmrAirQueue.h"
#endif
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmrDataService.h"
#endif
#if CONFIG_ZONE_SHARE
#include "dmr/zone_share_api.h"
#endif

void debuglink_view_air(dl_text_t *t)
{
#if CONFIG_DMR_AIR_QUEUE
	airqStats_t q;

	airqGetStats(&q);
	// Depth in packets and in octets now, and the most of each since start: what sizes CONFIG_DMR_AIR_QUEUE_BYTES
	dl_text(t, "airq.built=1\nairq.depth=%u\nairq.bytes=%u\nairq.high=%u\nairq.high_bytes=%u\nairq.capacity=%u\n",
		q.depth, q.bytes, q.highWater, q.highWaterBytes, q.capacity);
	dl_text(t, "airq.queued=%u\nairq.full=%u\nairq.sent=%u\nairq.failed=%u\n", (unsigned)q.queued,
		(unsigned)q.refusedFull, (unsigned)q.sent, (unsigned)q.failed);
	dl_text(t, "airq.gave_up=%u\nairq.aged=%u\nairq.other=%u\nairq.waits=%u\n", (unsigned)q.gaveUp, (unsigned)q.aged,
		(unsigned)q.other, (unsigned)q.serviceBusy);
#else
	dl_text(t, "airq.built=0\n");
#endif
#if CONFIG_DMR_DATA_CORE
	dmrDataStats_t d;

	dmrDataServiceGetStats(&d);
	dl_text(t, "data.tx_guarded=%u\n", (unsigned)d.txGuarded);
#endif
#if CONFIG_ZONE_SHARE
	zone_share_stats_t z;
	zone_share_offer_t o;

	zone_share_get_stats(&z);
	bool pending = zone_share_offer(&o, NULL);
	dl_text(t, "zs.built=1\nzs.offers=%u\nzs.send_state=%u\nzs.offer_pending=%u\nzs.offer_src=%u\n",
		zone_share_offers(), (unsigned)zone_share_send_state(), pending, pending ? (unsigned)o.src : 0u);
	dl_text(t, "zs.rx_messages=%u\nzs.rx_bad=%u\nzs.rx_busy=%u\nzs.rx_off=%u\n", (unsigned)z.rxMessages,
		(unsigned)z.rxBad, (unsigned)z.rxBusy, (unsigned)z.rxOff);
	dl_text(t, "zs.offered=%u\nzs.rate_limited=%u\nzs.expired=%u\n", (unsigned)z.offers, (unsigned)z.rateLimited,
		(unsigned)z.expired);
	dl_text(t, "zs.sends=%u\nzs.sent=%u\nzs.send_failed=%u\nzs.requests=%u\nzs.refusals=%u\nzs.last_refusal=%u\n",
		(unsigned)z.sends, (unsigned)z.sent, (unsigned)z.sendFailed, (unsigned)z.requests, (unsigned)z.refusals,
		zone_share_last_refusal());
#else
	dl_text(t, "zs.built=0\n");
#endif
}
