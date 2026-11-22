// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Group key fill on the data service (keyfill/groupkey_api.h, docs/spec/group-key-fill.md 3): the UDP port, the air
// queue, the message held for the UI
#include "config.h"
#if CONFIG_GROUP_KEY_FILL	// the whole file
#include <string.h>
#include "keyfill/groupkey_api.h"
#include "dmr/dmrAirQueue.h"
#include "dmr/dmrDataService.h"

#define UDP_HEADER	8

// The one held message: written by the radio task (the data service's receive path), read by the UI task, both under
// the data service's lock
static struct {
	uint8_t len;		// 0: the slot is free
	uint32_t src;
	uint8_t msg[GK_MSG_LEN];
} held;
static volatile bool open_rx;
static volatile uint8_t send_state = GK_SEND_IDLE;
static gk_svc_stats_t stats;

// The radio task: our copy has ended
static void send_done(const airqItem_t *item, airqResult_t result)
{
	(void)item;
	if (result == AIRQ_SENT) {
		stats.sent++;
		send_state = GK_SEND_SENT;
	} else {
		stats.send_failed++;
		send_state = GK_SEND_FAILED;
	}
}

void group_key_svc_init(void)
{
	memset(&held, 0, sizeof held);
	memset(&stats, 0, sizeof stats);
	open_rx = false;
	send_state = GK_SEND_IDLE;
	airqSetDone(AIRQ_OWNER_GROUP_KEY, send_done);
}

void group_key_open(bool open)
{
	dmrDataServiceLock();
	open_rx = open;
	memset(&held, 0, sizeof held);	// a message of an earlier window never reaches a new one
	dmrDataServiceUnlock();
}

// The radio task. Not inlined: the frame stays off handlePacket's deepest path (docs/dev/stack-check.md)
__attribute__((noinline)) void dmrDataServiceGroupKeyRx(uint32_t src, const uint8_t *payload, int length)
{
	stats.rx++;
	if (!open_rx) {
		stats.rx_closed++;
		return;
	}
	if (length != GK_MSG_LEN) {
		stats.rx_bad++;
		return;
	}
	dmrDataServiceLock();
	bool busy = held.len != 0;
	if (!busy) {
		memcpy(held.msg, payload, (size_t)length);
		held.src = src;
		held.len = (uint8_t)length;
	}
	dmrDataServiceUnlock();
	if (busy)
		stats.rx_busy++;	// the sender's next copy brings it again
}

int group_key_take(uint8_t out[GK_MSG_LEN], uint32_t *src)
{
	dmrDataServiceLock();
	int n = held.len;
	if (n != 0) {
		memcpy(out, held.msg, (size_t)n);
		*src = held.src;
		memset(&held, 0, sizeof held);
	}
	dmrDataServiceUnlock();
	return n;
}

bool group_key_send(uint32_t tg, const uint8_t msg[GK_MSG_LEN])
{
	if (tg == 0 || tg > 0xFFFFFFu || send_state == GK_SEND_SENDING)
		return false;
	uint8_t udp[UDP_HEADER];
	int n = UDP_HEADER + GK_MSG_LEN;
	udp[0] = (uint8_t)(GK_PORT >> 8);
	udp[1] = (uint8_t)GK_PORT;
	udp[2] = (uint8_t)(GK_PORT >> 8);
	udp[3] = (uint8_t)GK_PORT;
	udp[4] = (uint8_t)(n >> 8);
	udp[5] = (uint8_t)n;
	udp[6] = 0;	// computed with the IPv4 header
	udp[7] = 0;
	airqItem_t it = { .kind = AIRQ_IP, .owner = AIRQ_OWNER_GROUP_KEY, .protocol = DMR_IP_PROTO_UDP,
			  .delivery = DMR_IPS_UNCONFIRMED, .group = true, .dst = tg, .waitMs = GK_SEND_WAIT_MS,
			  .data = udp, .length = UDP_HEADER, .tail = msg, .tailLength = GK_MSG_LEN };
	send_state = GK_SEND_SENDING;
	if (!airqPut(&it)) {
		send_state = GK_SEND_FAILED;
		return false;
	}
	return true;
}

gk_send_state_t group_key_send_state(void)
{
	return (gk_send_state_t)send_state;
}

void group_key_cancel(void)
{
	airqCancel(AIRQ_OWNER_GROUP_KEY, 0);
	send_state = GK_SEND_IDLE;
}

void group_key_get_stats(gk_svc_stats_t *out)
{
	*out = stats;
}

#endif	// CONFIG_GROUP_KEY_FILL
