// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's own APRS-IS client (CONFIG_NET_APRSIS, docs/spec/tcp-services.md 7): a TCP connection out through the
// host to an APRS-IS server, logged in with the APRS callsign and its passcode, carrying the APRS engine's lines for
// APRS-IS (our beacons and messages, RF packets the iGate rules pass) and the server's lines back to the engine. Off
// unless a server address is set and the APRS callsign is.
#ifndef RFOUNDRY_APRSIS_H
#define RFOUNDRY_APRSIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "debuglink/dl_fmt.h"

#define APRSIS_PORT_DEFAULT	14580	// the servers' filtered port
#define APRSIS_RETRY_MS		30000u	// the first reconnect; doubled up to APRSIS_RETRY_MAX_MS
#define APRSIS_RETRY_MAX_MS	300000u
#define APRSIS_SILENCE_MS	120000u	// servers send a comment every 20 s: this long without a line, the link is dead
#define APRSIS_LOGIN_MS		30000u	// no "# logresp" this long after connecting: start again

typedef enum { APRSIS_OFF, APRSIS_WAIT, APRSIS_CONNECTING, APRSIS_LOGIN, APRSIS_ONLINE } aprsis_state_t;

// The APRS-IS passcode of a callsign: the published 15-bit hash of the base call (upper case, no SSID)
uint16_t aprsis_passcode(const char *call);

void aprsis_start(void);
void aprsis_poll(uint32_t now_ms);
bool aprsis_online(void);		// logged in and verified: the TNC pump sends it the engine's 'G' lines
bool aprsis_room(size_t n);
void aprsis_line(const uint8_t *line, size_t n);	// one line for APRS-IS (CR LF added)
void aprsis_view(dl_text_t *t);
// GET /api/aprsis (http.c): the state as JSON in t, and with a query (server=a.b.c.d[:port] or off, range=<km>) the
// settings changed, from the host only. Returns the HTTP status
int aprsis_http(dl_text_t *t, const char *query, uint32_t from_ip);

#endif
