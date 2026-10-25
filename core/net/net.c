// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's own services' entry point from the host link task (see net.h): the TCP services' glue to the IP gateway
// here, the UDP services' in net_services.c
#include "config.h"
#if CONFIG_NET_LOCAL	// the whole file
#include "net/net.h"
#include "dmr/ipGateway.h"
#include "board.h"
#include "hal_time.h"
#if CONFIG_MDNS || CONFIG_TFTP
#include "net/net_services.h"
#endif
#if CONFIG_NET_USRP
#include "usbaudio/usrp_net.h"
#endif

static bool was_up;

#if CONFIG_NET_TCP
#include "net/tcp.h"
#include "net/net_svc.h"

#define IP_PROTO_TCP	6

#if CONFIG_NET_HTTP || CONFIG_NET_RIGCTL
static radio_view_t view;

const radio_view_t *net_radio_view(void)
{
	radio_get_view(&view);
	return &view;
}
#endif

#if CONFIG_NET_KISS || CONFIG_NET_APRSIS
#include "aprs/aprs.h"
#include "aprs/kiss.h"
#if CONFIG_NET_APRSIS
#include "net/aprsis.h"
#else
#define aprsis_online()		false
#define aprsis_room(n)		false
#define aprsis_line(l, n)	do { } while (0)
#endif
#if !CONFIG_NET_KISS
#define kiss_tcp_active()	false
#define kiss_tcp_room(n)	false
#define kiss_tcp_frame(f, n)	do { } while (0)
#endif

#define KISS_WIRE_MAX	(2u * KISS_FRAME_MAX + 2u)	// the longest frame, every byte escaped
#define PUMP_CHUNK	32

static kiss_dec_t tnc_out;	// the APRS engine's KISS output, decoded again to route it frame by frame

bool net_tnc_taken(void)
{
	return kiss_tcp_active() || aprsis_online();
}

// Room for any frame that may complete in the next chunk
static bool tnc_room(void)
{
	return (!kiss_tcp_active() || kiss_tcp_room(KISS_WIRE_MAX + PUMP_CHUNK)) &&
	       (!aprsis_online() || aprsis_room(KISS_FRAME_MAX + 2u + PUMP_CHUNK));
}

// SetHardware 'G' lines to APRS-IS while it is logged in, everything else to the KISS client
static void tnc_route(const uint8_t *f, size_t n)
{
	if ((n >= 2) && (f[0] == KISS_CMD_SETHW) && (f[1] == KISS_TAG_IS_LINE) && aprsis_online()) {
		aprsis_line(f + 2, n - 2u);
	} else if (kiss_tcp_active()) {
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
#else
bool net_tnc_taken(void)
{
	return false;
}
#endif	// CONFIG_NET_TCP

void net_start(uint32_t salt)
{
#if CONFIG_MDNS || CONFIG_TFTP
	net_services_init();	// the UDP services bind their ports
#endif
#if CONFIG_NET_USRP
	usrp_net_start();
#endif
#if CONFIG_NET_TCP
	tcp_init(&io, salt);
	ipGatewaySetTCP(tcp_in);
#if CONFIG_NET_KISS || CONFIG_NET_APRSIS
	kiss_dec_init(&tnc_out);
#endif
#if CONFIG_NET_KISS
	kiss_tcp_start();
#endif
#if CONFIG_NET_APRSIS
	aprsis_start();
#endif
#if CONFIG_NET_HTTP
	http_start();
#endif
#if CONFIG_NET_RIGCTL
	rigctld_start();
#endif
#else
	(void)salt;
#endif
}

void net_poll(uint32_t now_ms, bool link_up)
{
#if CONFIG_MDNS || CONFIG_TFTP
	net_services_poll(now_ms, link_up);
#endif
#if CONFIG_NET_USRP
	usrp_net_poll(now_ms, link_up);	// unkeys and stops the audio when the link goes down
#endif
	if (!link_up) {
#if CONFIG_NET_TCP
		if (was_up) {
			tcp_abort_all();	// the host is gone (unplugged, switched to serial): nobody to send a RST to
		}
#endif
		was_up = false;
		return;
	}
	was_up = true;
#if CONFIG_NET_APRSIS
	aprsis_poll(now_ms);
#endif
#if CONFIG_NET_KISS || CONFIG_NET_APRSIS
	tnc_pump();
#endif
#if CONFIG_NET_HTTP
	http_poll(now_ms);
#endif
#if CONFIG_NET_RIGCTL
	rigctld_poll(now_ms);
#endif
#if CONFIG_NET_TCP
	tcp_poll(now_ms);
#endif
}

void net_view(dl_text_t *t)
{
	dl_text(t, "net.link_up=%u\n", was_up);
	if ((board.netif != NULL) && (board.netif->stats != NULL)) {
		hal_netif_stats_t s = { 0 };

		board.netif->stats(&s);
		dl_text(t, "netif.rx=%u\nnetif.rx_dropped=%u\nnetif.tx=%u\nnetif.tx_busy=%u\nnetif.reconnects=%u\n"
			"netif.switch_fallbacks=%u\nnetif.tx_stalls=%u\n", (unsigned)s.rx_frames, (unsigned)s.rx_dropped,
			(unsigned)s.tx_frames, (unsigned)s.tx_busy, (unsigned)s.reconnects, (unsigned)s.switch_fallbacks,
			(unsigned)s.tx_stalls);
	}
#if CONFIG_NET_KISS
	kiss_tcp_view(t);
#endif
#if CONFIG_NET_HTTP
	http_view(t);
#endif
#if CONFIG_NET_RIGCTL
	rigctld_view(t);
#endif
#if CONFIG_NET_APRSIS
	aprsis_view(t);
#endif
#if CONFIG_MDNS || CONFIG_TFTP
	net_services_view(t);
#endif
}

#endif	// CONFIG_NET_LOCAL
