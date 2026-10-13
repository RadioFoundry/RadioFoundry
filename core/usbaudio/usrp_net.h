// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * USRP voice over UDP on the radio's network address (CONFIG_NET_USRP, docs/spec/usb-audio.md 4.2 and 5): the
 * receiver's audio to a peer (AllStarLink's USRP channel driver, DVSwitch Analog_Bridge, a bridge or our reference
 * client tools/usrp_client.py) with the squelch as the keyup flag, and the peer's keyup frames transmitted. Runs in the
 * host link task (core/net/net.c calls it); the audio goes through core/usbaudio/audio_mux.c.
 *
 * Addressing: the radio listens on USRP_PORT and sends to USRP_PEER_PORT at the address that last sent it a valid
 * frame, or at the host's address on the USB link until one has. These are the port pair AllStar and Analog_Bridge
 * set-ups commonly use, seen from the radio's side (the peer sends to 34001 and listens on 32001).
 */
#ifndef ODR_USRP_NET_H
#define ODR_USRP_NET_H

#include <stdbool.h>
#include <stdint.h>
#include "debuglink/dl_fmt.h"

#define USRP_PORT		34001
#define USRP_PEER_PORT		32001
#define USRP_PEER_IDLE_MS	60000u	// a learned peer silent this long: back to the host's address

void usrp_net_start(void);
// Every host link pass in network mode; up: the link is up
void usrp_net_poll(uint32_t now_ms, bool up);
// usrp.* and the mux's counters, key=value lines (the debug link's net view)
void usrp_net_view(dl_text_t *t);

#endif
