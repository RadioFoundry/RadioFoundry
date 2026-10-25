// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The net view section: the radio's own services, TCP and its services (docs/spec/tcp-services.md), mDNS and TFTP
// (mdns.md, tftp.md); "net.built=0" without any of them
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_NET_LOCAL
#include "net/net.h"
#endif
#if CONFIG_NET_TCP
#include "net/tcp.h"
#endif

void debuglink_view_net(dl_text_t *t)
{
#if CONFIG_NET_TCP
	tcp_stats_t s;

	tcp_get_stats(&s);
	dl_text(t, "tcp.open=%u\ntcp.segs_in=%u\ntcp.segs_out=%u\n", (unsigned)tcp_open_count(), (unsigned)s.segs_in,
		(unsigned)s.segs_out);
	dl_text(t, "tcp.retransmits=%u\ntcp.rst_out=%u\ntcp.bad_checksum=%u\n", (unsigned)s.retransmits,
		(unsigned)s.rst_out, (unsigned)s.bad_checksum);
	dl_text(t, "tcp.accepted=%u\ntcp.refused=%u\ntcp.no_slot=%u\ntcp.timeouts=%u\n", (unsigned)s.accepted,
		(unsigned)s.refused, (unsigned)s.no_slot, (unsigned)s.timeouts);
#endif
#if CONFIG_NET_LOCAL
	net_view(t);
#else
	dl_text(t, "net.built=0\n");
#endif
}
