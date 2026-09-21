// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio's own services on the USB network link (CONFIG_MDNS, CONFIG_TFTP): their ports in the IP gateway's table
// (ipGatewayUdpBind), their replies through ipGatewayUdpSend, the mDNS service table and identity, the TFTP files.
// The host link's network personality (hostlink_net.c) calls these from its task.
#ifndef ODR_NET_SERVICES_H
#define ODR_NET_SERVICES_H

#include <stdint.h>
#include <stdbool.h>

// After ipGatewayInit: bind the ports
void net_services_init(void);
// Every pass of the host link task in network mode: timers and what waits to be sent. up: the link is up
void net_services_poll(uint32_t now_ms, bool up);

#endif
