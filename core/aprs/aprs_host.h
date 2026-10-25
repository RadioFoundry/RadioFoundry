// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// APRS on the host port (docs/spec/aprs.md 9): a KISS TNC (port 0 data frames both ways; SetHardware 'G' carries
// APRS-IS lines to and from the host helper tools/aprs_gate.py, 'N' NMEA sentences from the host) and plain NMEA
// lines ('$...') for the position. Registered with the host link as its TNC protocol (hostlink_set_tnc), so it works
// on the USB serial port and, in the network personality, on the UDP serial port. Only while the KISS setting is on.
#ifndef RFOUNDRY_APRS_HOST_H
#define RFOUNDRY_APRS_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// From hostlink_start
void aprs_host_start(void);

// The protocol's parts (exposed for the host tests)
bool aprs_host_claims(uint8_t b);
void aprs_host_feed(const uint8_t *buf, size_t len, uint32_t now_ms);
bool aprs_host_in_frame(void);
// One decoded KISS frame (type byte first) from any transport: the serial / UDP port, or KISS over TCP (core/net)
void aprs_host_kiss_frame(const uint8_t *f, size_t n, uint32_t now_ms);

#endif
