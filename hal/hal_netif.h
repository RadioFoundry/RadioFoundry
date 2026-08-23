// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A network interface to a host computer: Ethernet frames over the board's USB network personality (CDC-NCM on the
// STM32 radios, drivers/usb/usb_ncm.c). The host link's IP gateway (core/hostlink/hostlink_net.c) uses it. Every op
// is safe from any task and none blocks.
#ifndef HAL_NETIF_H
#define HAL_NETIF_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// Switch the USB personality: network (true) or serial (false). The host sees an unplug and enumerates the other
	// one (blocks about 100 ms). False if it can't
	bool (*select)(bool network);
	bool (*selected)(void);		// the network personality is the one in use
	bool (*up)(void);		// selected and configured by the host
	// One Ethernet frame to the host; false if the link is down or the previous frame is still going (retry)
	bool (*send)(const uint8_t *frame, int length);
	// The oldest frame from the host into buf: its length, 0 if none
	int (*receive)(uint8_t *buf, int max);
	void (*tick)(uint32_t now_ms);	// every few ms from the user's task
	// The host adapter's MAC address and the radio's side of the link
	void (*macs)(uint8_t host[6], uint8_t radio[6]);
	uint16_t frame_max;		// longest frame receive() returns
} hal_netif_t;

#endif
