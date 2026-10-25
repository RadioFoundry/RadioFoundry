// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The rules for passing packets between RF and APRS-IS, and the digipeater (docs/spec/aprs.md 9, 10). Pure functions
// on frames; aprs.c owns the state and the transport (KISS SetHardware 'G' frames to the host helper,
// tools/aprs_gate.py, which keeps the APRS-IS connection).
//
// RF -> IS (aprs-is.net "IGate Design" and "q Construct" pages): never gate a packet whose path holds TCPIP, TCPXX,
// NOGATE or RFONLY, or a general query ('?'); third-party packets ('}') are gated as the packet they carry, unless its
// path holds TCPIP / TCPXX (it came from the Internet); the line is the TNC2 form with ",qAR,<our call>" after the
// path; the same packet is gated once in 30 s; the information field ends at the first CR or LF.
//
// IS -> RF: only messages (and their acks), to a station heard directly on RF in the last 30 min, from a station not
// heard on RF lately, not from us, not seen in the last 30 s; sent as a third-party packet
// "}SRC>DST,TCPIP,OURCALL*:info" from our call on the APRS frequency.
#ifndef RFOUNDRY_APRS_IGATE_H
#define RFOUNDRY_APRS_IGATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "aprs/ax25.h"

#define APRS_DEDUP_SLOTS	16
#define APRS_DEDUP_MS		30000u

typedef struct {
	uint32_t hash[APRS_DEDUP_SLOTS];
	uint32_t ms[APRS_DEDUP_SLOTS];
	uint8_t next;
} aprs_dedup_t;

// Source, destination and information field (trailing CR / LF / spaces ignored): the same packet heard again over
// another digipeater has the same hash
uint32_t aprs_frame_hash(const ax25_frame_t *f);
// True when hash was seen in the last window_ms; else it is noted and false is returned
bool aprs_dedup_seen(aprs_dedup_t *d, uint32_t hash, uint32_t now_ms, uint32_t window_ms);

typedef enum {
	APRS_GATE_OK, APRS_GATE_NO_PATH, APRS_GATE_QUERY, APRS_GATE_DUP, APRS_GATE_FROM_IS, APRS_GATE_BAD,
} aprs_gate_t;

// RF -> IS: the line for APRS-IS (NUL terminated, no CR / LF) in out. Returns aprs_gate_t
uint8_t aprs_igate_rf_to_is(aprs_dedup_t *d, const ax25_frame_t *f, const ax25_addr_t *mycall, uint32_t now_ms,
			    char *out, size_t max);

// Lookups the IS -> RF rule needs (aprs.c's station list)
typedef struct {
	// Heard directly (no digipeater used) on RF within the last secs seconds
	bool (*heard_direct)(const char *call, uint8_t ssid, uint32_t secs, void *ctx);
	// Heard on RF at all within the last secs seconds
	bool (*heard_rf)(const char *call, uint8_t ssid, uint32_t secs, void *ctx);
	void *ctx;
} aprs_igate_env_t;

#define APRS_IGATE_LOCAL_S	1800u	// "local" = heard directly in the last 30 min
#define APRS_IGATE_SENDER_S	600u	// the sender heard on RF in the last 10 min: it can reach the station itself

// IS -> RF: an APRS-IS line; on true, the third-party information field to send ("}SRC>DST,TCPIP,MYCALL*:info") is in
// info (NUL terminated), its length in *len
bool aprs_igate_is_to_rf(aprs_dedup_t *d, const char *line, size_t n, const ax25_addr_t *mycall,
			 const aprs_igate_env_t *env, uint32_t now_ms, char *info, size_t max, size_t *len);

typedef enum { APRS_DIGI_OFF, APRS_DIGI_FILL_IN, APRS_DIGI_WIDE } aprs_digi_mode_t;

// The digipeater: in heard on RF; on true out is the frame to send (same info). FILL_IN repeats WIDE1-1 and our own
// call only; WIDE also WIDE2-N. Traced (our call inserted, marked used), at most AX25_MAX_DIGIS entries; the same
// packet once in 30 s; never our own packets
bool aprs_digi(aprs_dedup_t *d, const ax25_frame_t *in, ax25_frame_t *out, const ax25_addr_t *mycall, uint8_t mode,
	       uint32_t now_ms);

#endif
