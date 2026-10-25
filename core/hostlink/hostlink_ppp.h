// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The host link's PPP personality (CONFIG_NET_PPP, docs/spec/ppp.md): IPv4 over PPP (core/net/ppp.c) on the serial
// host port, into the same IP gateway and services as the USB network adapter.
//
// hostlink_ppp_netif is the board's netif (board_t.netif): IPv4 packets from PPP go to the gateway with an Ethernet
// header added in place (ipGatewayEthernetIn), the gateway's IPv4 frames go out with it taken off (ARP and the like
// have no meaning on PPP and are dropped). On a board that also has the USB network adapter, it wraps that one
// (hostlink_ppp_set_usb from board_init): PPP while a PPP link runs on the serial port, the adapter otherwise.
// The host link (hostlink.c) starts the link and feeds it the port's bytes while PPP has the port.
#ifndef RFOUNDRY_HOSTLINK_PPP_H
#define RFOUNDRY_HOSTLINK_PPP_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#if CONFIG_NET_PPP
#include "hal_netif.h"

extern const hal_netif_t hostlink_ppp_netif;
// The board's USB network adapter, wrapped (NULL: none)
void hostlink_ppp_set_usb(const hal_netif_t *usb);
// A PPP link starts on the host port: active from the debug link (our Configure-Request goes now), passive when the
// peer's first frame is arriving
void hostlink_ppp_begin(bool active, uint32_t now_ms);
#endif

#endif
