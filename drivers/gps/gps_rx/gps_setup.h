// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Configuration sentences for a GPS module, sent once it has said what it is: the board lists the modules it may carry,
// each with a pattern for a sentence that module sends and the sentences that set it up (rate, output set, satellite
// systems). The driver passes every received sentence to gps_setup_find until one matches, sends that entry once and
// starts over when it powers the module off, since a module forgets an unsaved set-up with its supply. The sentences are
// stored as bodies, without "$" and "*hh", and framed when sent: shorter than the full sentences and no checksum to get
// wrong. Generic and pure: no UART, no RTOS; the board's list says which module protocol it speaks.
#ifndef GPS_SETUP_H
#define GPS_SETUP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	const char *match;	// a prefix of a sentence this module sends; '?' matches any character ("$??TXT")
	const char *send;	// sentence bodies, each ended by '\n' ("PCAS02,1000\n...")
} gps_setup_t;

// The first of n entries whose match begins the sentence s (n characters), or NULL
const gps_setup_t *gps_setup_find(const gps_setup_t *list, uint8_t n, const char *s, uint32_t len);
// Each body of send as "$" body "*hh\r\n", one character at a time to put
void gps_setup_send(const char *send, void (*put)(uint8_t c));

#endif
