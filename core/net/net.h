// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio's own TCP services on the USB network link (CONFIG_NET_TCP, docs/spec/tcp-services.md): the TCP of
// core/net/tcp.c on the IP gateway's IPv4 layer (ipGatewaySetTCP / ipGatewaySendLocal), and the services on it.
// Everything runs in the host link task, from hostlink_net_init / hostlink_net_poll; never over RF.
#ifndef ODR_NET_H
#define ODR_NET_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define NET_SEND_WAIT_MS	20	// a segment waits this long for the USB IN endpoint (then the timers resend it)

// Once, when the network personality starts (salt: varies the TCP initial sequence numbers)
void net_start(uint32_t salt);
// Every pass of the host link task in network mode; link_up false drops every connection
void net_poll(uint32_t now_ms, bool link_up);

// The services' state and counters as key=value lines (the debug link's net view section)
#include "debuglink/dl_fmt.h"
void net_view(dl_text_t *t);

#endif
