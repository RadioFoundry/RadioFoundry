// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The TCP services' glue to the IP gateway and the host link task (see net.h)
#include "config.h"
#if CONFIG_NET_TCP	// the whole file
#include "net/net.h"
#include "net/tcp.h"
#include "net/net_svc.h"
#include "dmr/ipGateway.h"
#include "board.h"
#include "hal_time.h"

#define IP_PROTO_TCP	6

static bool was_up;

#if CONFIG_NET_KISS
#include "aprs/aprs.h"
#include "aprs/kiss.h"

#define KISS_WIRE_MAX	(2u * KISS_FRAME_MAX + 2u)	// the longest frame, every byte escaped
#define PUMP_CHUNK	32

static kiss_dec_t tnc_out;	// the APRS engine's KISS output, decoded again to route it frame by frame

bool net_tnc_taken(void)
{
	return kiss_tcp_active();
}

// Room for any frame that may complete in the next chunk
static bool tnc_room(void)
{
	return !kiss_tcp_active() || kiss_tcp_room(KISS_WIRE_MAX + PUMP_CHUNK);
}

static void tnc_route(const uint8_t *f, size_t n)
{
	if (kiss_tcp_active()) {
		kiss_tcp_frame(f, n);
	}
}

// What the APRS engine has for the host goes to the TCP services while one takes it, instead of the serial / UDP
// port (hostlink.c leaves the output alone then)
static void tnc_pump(void)
{
	uint8_t chunk[PUMP_CHUNK];

	if (!net_tnc_taken()) {
		return;
	}
	while (tnc_room()) {
		size_t n = aprs_host_output(chunk, sizeof(chunk));

		if (n == 0) {
			break;
		}
		for (size_t i = 0; i < n; i++) {
			size_t m = kiss_dec_byte(&tnc_out, chunk[i]);

			if (m != 0) {
				tnc_route(tnc_out.buf, m);
			}
		}
	}
}
#else
bool net_tnc_taken(void)
{
	return false;
}
#endif

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
#if CONFIG_NET_KISS
	kiss_dec_init(&tnc_out);
	kiss_tcp_start();
#endif
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
#if CONFIG_NET_KISS
	tnc_pump();
#endif
	tcp_poll(now_ms);
}

void net_view(dl_text_t *t)
{
	dl_text(t, "net.link_up=%u\n", was_up);
#if CONFIG_NET_KISS
	kiss_tcp_view(t);
#endif
}

#endif	// CONFIG_NET_TCP
