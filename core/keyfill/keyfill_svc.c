// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key fill on the data service: the UDP port, the air queue, the message held for the UI (keyfill/keyfill_api.h,
// docs/spec/key-fill.md 3)
#include "config.h"
#if CONFIG_KEY_FILL	// the whole file
#include <string.h>
#include "keyfill/keyfill_api.h"
#include "dmr/dmrAirQueue.h"
#include "dmr/dmrDataService.h"

#define UDP_HEADER	8

// The one held message: written by the radio task (the data service's receive path), read by the UI task, both under
// the data service's lock
static struct {
	uint8_t len;		// 0: the slot is free
	uint32_t src;
	uint8_t msg[KF_MSG_MAX];
} held;
static volatile bool open_rx;
static volatile uint8_t send_state = KF_SEND_IDLE;
static kf_svc_stats_t stats;

// The radio task: our message has ended
static void send_done(const airqItem_t *item, airqResult_t result)
{
	(void)item;
	if (result == AIRQ_SENT) {
		stats.sent++;
		send_state = KF_SEND_SENT;
	} else {
		stats.send_failed++;
		send_state = KF_SEND_FAILED;
	}
}

void key_fill_svc_init(void)
{
	memset(&held, 0, sizeof held);
	memset(&stats, 0, sizeof stats);
	open_rx = false;
	send_state = KF_SEND_IDLE;
	airqSetDone(AIRQ_OWNER_KEY_FILL, send_done);
}

void key_fill_open(bool open)
{
	dmrDataServiceLock();
	open_rx = open;
	memset(&held, 0, sizeof held);	// a message of an earlier session never reaches a new one
	dmrDataServiceUnlock();
	if (!open)
		airqCancel(AIRQ_OWNER_KEY_FILL, 0);
}

// The radio task. Not inlined: the frame stays off handlePacket's deepest path (docs/dev/stack-check.md)
__attribute__((noinline)) void dmrDataServiceKeyFillRx(uint32_t src, const uint8_t *payload, int length)
{
	stats.rx++;
	if (!open_rx) {
		stats.rx_closed++;
		return;
	}
	if (length < 1 || length > KF_MSG_MAX)
		return;		// the engine counts what it drops; this one can't be held whole
	dmrDataServiceLock();
	bool busy = held.len != 0;
	if (!busy) {
		memcpy(held.msg, payload, (size_t)length);
		held.src = src;
		held.len = (uint8_t)length;
	}
	dmrDataServiceUnlock();
	if (busy)
		stats.rx_busy++;
}

int key_fill_take(uint8_t out[KF_MSG_MAX], uint32_t *src)
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

bool key_fill_send(uint32_t dst, const uint8_t *msg, int len)
{
	if (dst == 0 || dst > 0xFFFFFFu || len < 1 || len > KF_MSG_MAX || send_state == KF_SEND_SENDING)
		return false;
	uint8_t udp[UDP_HEADER];
	int n = UDP_HEADER + len;
	udp[0] = (uint8_t)(KF_PORT >> 8);
	udp[1] = (uint8_t)KF_PORT;
	udp[2] = (uint8_t)(KF_PORT >> 8);
	udp[3] = (uint8_t)KF_PORT;
	udp[4] = (uint8_t)(n >> 8);
	udp[5] = (uint8_t)n;
	udp[6] = 0;	// computed with the IPv4 header
	udp[7] = 0;
	airqItem_t it = { .kind = AIRQ_IP, .owner = AIRQ_OWNER_KEY_FILL, .protocol = DMR_IP_PROTO_UDP,
			  .delivery = DMR_IPS_CONFIRMED, .dst = dst, .waitMs = KF_SEND_WAIT_MS, .data = udp,
			  .length = UDP_HEADER, .tail = msg, .tailLength = (uint16_t)len };
	send_state = KF_SEND_SENDING;
	if (!airqPut(&it)) {
		send_state = KF_SEND_FAILED;
		return false;
	}
	return true;
}

kf_send_state_t key_fill_send_state(void)
{
	return (kf_send_state_t)send_state;
}

void key_fill_get_stats(kf_svc_stats_t *out)
{
	*out = stats;
}

#endif	// CONFIG_KEY_FILL
