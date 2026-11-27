// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The services on the radio's TCP (docs/spec/tcp-services.md), each behind its own switch. Internal to core/net: net.c
// starts them, polls them and routes the APRS engine's KISS output (the TNC pump) between them.
#ifndef RFOUNDRY_NET_SVC_H
#define RFOUNDRY_NET_SVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "debuglink/dl_fmt.h"

#define NET_KISS_PORT		8001	// the de facto KISS-over-TCP TNC port
#define NET_HTTP_PORT		80
#define NET_RIGCTL_PORT		4532	// Hamlib's rigctld

// d starts as an HTTP request does (a method, then a space): what a browser sends when a web page posts to a port
// that isn't HTTP. The line-based and KISS services drop such a connection before any of it is taken as commands
static inline bool net_http_request(const uint8_t *d, size_t n)
{
	static const char *const methods[] = { "GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH ",
					       "CONNECT " };

	for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		size_t m = strlen(methods[i]);

		if ((n >= m) && (memcmp(d, methods[i], m) == 0)) {
			return true;
		}
	}
	return false;
}

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
#endif

#if CONFIG_NET_WEBCTL
// Zone and channel control for /ws (radio_ctl.c): the requests, and the pending steps passed on by radio_ctl_poll
int radio_ctl_request(const char *r, uint32_t now, const radio_view_t *v);
void radio_ctl_poll(uint32_t now);
#endif

#endif
