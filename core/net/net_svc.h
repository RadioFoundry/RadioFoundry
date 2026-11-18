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
// The radio core's view, fetched now into the host link task's one copy (hostlink_view.h; the services all run in
// that task)
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
// The frame buffer can be sent as it is: /api/screen.bmp and the web control page's screen
bool http_screen_ok(void);
// A string as a JSON string, quotes included
void http_json_str(dl_text_t *t, const char *s);
#endif

#if CONFIG_NET_RADIO_API
// Zone and channel control on the status server (http_radio.c): the reply's HTTP status for path, its JSON in t when
// 200 (0: not one of its paths). from_ip: the client's, control only from the host. head: answer, change nothing
int http_radio(const char *path, const char *query, uint32_t from_ip, bool head, dl_text_t *t);
#if CONFIG_NET_RADIO_DUMP
#define HTTP_RADIO_STREAM	1	// http_radio: 200, t has the start, http_radio_more the rest
// The streamed reply's next part into t: 1 more, 0 done (t empty), -1 the codeplug changed (cut the connection)
int http_radio_more(dl_text_t *t);
#endif
void http_radio_poll(uint32_t now_ms);	// passes on requests held back by the rate limit
void http_radio_view(dl_text_t *t);
#endif

#endif
