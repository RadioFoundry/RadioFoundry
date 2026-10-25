// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The network personality's frames (CDC-NCM on TinyUSB, usb_device.c; docs/spec/hardware-dm1701.md 16): Ethernet
// frames to and from the host, for the IP gateway (core/dmr/ipGateway.c through core/hostlink/hostlink_net.c).
// Built with CONFIG_USB_NETWORK; every call is safe from any task (the USB stack's own work is deferred to its task).
#ifndef RFOUNDRY_USB_NCM_H
#define RFOUNDRY_USB_NCM_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_netif.h"

#define USB_NCM_FRAME_MAX	600	// largest frame kept from the host (IPGW_MAX_FRAME); longer ones are dropped
#define USB_NCM_RX_FRAMES	4	// frames from the host waiting for usbNcmReceive (more are dropped, counted)

// True in network mode once the host has configured the device (the data interface's alternate setting 1 follows)
bool usbNcmIsUp(void);

// One Ethernet frame to the host. False if the link is down or the previous frame is still being sent (try again)
bool usbNcmSendFrame(const uint8_t *frame, int length);

// The oldest frame from the host into buf (up to max bytes): its length, 0 if none
int usbNcmReceive(uint8_t *buf, int max);

// Housekeeping from the consumer's task every few ms: a pending frame to the host is retried, and the macOS cold boot
// quirk (no traffic from a configured host: re-enumerate once, hardware-dm1701.md 16)
void usbNcmTick(uint32_t now_ms);

// The two MAC addresses: the host's adapter (the descriptor's iMACAddress) and the radio's side of the link
void usbNcmMacs(uint8_t host[6], uint8_t radio[6]);

typedef hal_netif_stats_t usb_ncm_stats_t;
void usbNcmStats(usb_ncm_stats_t *s);

// The same as a hal_netif_t, for board_t.netif
extern const hal_netif_t usb_ncm_netif;

#endif
