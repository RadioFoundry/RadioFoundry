// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The services on the radio's TCP (docs/spec/tcp-services.md), each behind its own switch. Internal to core/net: net.c
// starts them, polls them and routes the APRS engine's KISS output (the TNC pump) between them.
#ifndef RFOUNDRY_NET_SVC_H
#define RFOUNDRY_NET_SVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "debuglink/dl_fmt.h"

#define NET_KISS_PORT		8001	// the de facto KISS-over-TCP TNC port
#define NET_HTTP_PORT		80
#define NET_RIGCTL_PORT		4532	// Hamlib's rigctld

#if CONFIG_NET_HTTP || CONFIG_NET_RIGCTL
// The radio core's view, fetched now into the one copy the services share (static: too large for the host link task's
// stack; the services all run in that task)
#include "radio/radio.h"
const radio_view_t *net_radio_view(void);
#endif

#if CONFIG_NET_KISS
// KISS over TCP (kiss_tcp.c): one client, bridged to the APRS engine's KISS TNC (aprs_host.c)
void kiss_tcp_start(void);
bool kiss_tcp_active(void);		// a client is connected (and the APRS KISS setting is on)
bool kiss_tcp_room(size_t n);		// the client's transmit buffer takes n more bytes
void kiss_tcp_frame(const uint8_t *f, size_t n);	// one frame (type byte first) to the client, KISS encoded
void kiss_tcp_view(dl_text_t *t);
#endif

#if CONFIG_NET_RIGCTL
// Hamlib rigctld (rigctld.c)
void rigctld_start(void);
void rigctld_poll(uint32_t now_ms);
void rigctld_view(dl_text_t *t);
#endif

#if CONFIG_NET_HTTP
// The status server (http.c)
void http_start(void);
void http_poll(uint32_t now_ms);
void http_view(dl_text_t *t);
// The /status JSON (one object and a newline), also what webctl.c pushes
void http_status_json(dl_text_t *t);
#endif

#endif
