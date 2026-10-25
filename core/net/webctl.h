// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Web control (CONFIG_NET_WEBCTL; docs/spec/tcp-services.md 5.1): the status server's /ws WebSocket, for the front panel
// page the radio serves. One client. It gets the status as JSON when it changes and, when it asks, the receive audio
// as binary frames; it sends the radio's keys (down and up) and encoder steps, which go through the input task like
// the debug link's injected keys. Keys and PTT only from the computer the radio is plugged into; everything a client
// holds is released when it goes away.
#ifndef ODR_WEBCTL_H
#define ODR_WEBCTL_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "debuglink/dl_fmt.h"

#define WEBCTL_PING_MS		5000u	// a ping to the client this often (browsers answer by themselves)
#define WEBCTL_IDLE_MS		15000u	// nothing from the client for this long: it is gone
#define WEBCTL_STATUS_MS	250u	// the status is rendered this often and sent when it changed
#define WEBCTL_PTT_HOLD_MS	4000u	// a PTT down holds for this long; the page repeats it every 2 s while held
#define WEBCTL_PTT_MAX_MS	180000u	// a PTT from the page is released after this long, whatever the page does
#define WEBCTL_AUDIO_SAMPLES	160u	// one binary frame: 20 ms of 8 kHz PCM16, little-endian

#if CONFIG_NET_WEBCTL
// The HTTP server's side (http.c): /ws is taken while a client holds it; after its 101 response the connection is
// handed over and the server passes on its TCP events
int webctl_conn(void);		// the client's connection, -1 = none
void webctl_open(int c, uint32_t now_ms);
void webctl_recv(int c, const uint8_t *d, int n);
void webctl_sent(int c);
void webctl_peer_closed(int c);
void webctl_closed(int c);
void webctl_poll(uint32_t now_ms);
void webctl_view(dl_text_t *t);
// Receive audio, a 20 ms frame (USRP's reader, usrp_net.c); open: the squelch is open, only then is it sent
void webctl_audio(const int16_t *pcm, uint32_t n, bool open);
#endif

#endif
