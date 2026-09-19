// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The TCP services' glue to the IP gateway and the host link task (see net.h)
#include "config.h"
#if CONFIG_NET_TCP	// the whole file
#include "net/net.h"
#include "net/tcp.h"
#include "dmr/ipGateway.h"
#include "board.h"
#include "hal_time.h"

#define IP_PROTO_TCP	6

static bool was_up;

static uint8_t *io_buffer(void)
{
	return ipGatewayLocalPayload();
}

// The USB IN endpoint holds one frame at a time: wait a little for it rather than lose the segment (an ACK lost here
// would cost the peer a retransmission timeout)
static bool io_send(uint32_t dst, int length)
{
	uint32_t t0 = hal_millis();

	while (!ipGatewaySendLocal(IP_PROTO_TCP, dst, length)) {
		if ((board.netif == NULL) || !board.netif->up() || ((hal_millis() - t0) >= NET_SEND_WAIT_MS)) {
			return false;
		}
		hal_delay_ms(1);
		board.netif->tick(hal_millis());
	}
	return true;
}

static const tcp_io_t io = { .buffer = io_buffer, .send = io_send, .local_ip = ipGatewayRadioIP };

static void tcp_in(uint32_t src, const uint8_t *seg, int length)
{
	tcp_input(src, seg, length, hal_millis());
}

void net_start(uint32_t salt)
{
	tcp_init(&io, salt);
	ipGatewaySetTCP(tcp_in);
}

void net_poll(uint32_t now_ms, bool link_up)
{
	if (!link_up) {
		if (was_up) {
			tcp_abort_all();	// the host is gone (unplugged, switched to serial): nobody to send a RST to
		}
		was_up = false;
		return;
	}
	was_up = true;
	tcp_poll(now_ms);
}

void net_view(dl_text_t *t)
{
	dl_text(t, "net.link_up=%u\n", was_up);
}

#endif	// CONFIG_NET_TCP
