// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS module set-up sentences (drivers/gps/gps_rx/gps_setup): which module a sentence names (any talker, prefixes,
// short sentences), and the framing of the bodies into "$...*hh\r\n" that the module and our own line framing accept
#include <stdio.h>
#include <string.h>
#include "gps/gps_rx/gps_setup.h"
#include "gps/nmea_line/nmea_line.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char out[256];
static size_t out_n;

static void put(uint8_t c)
{
	if (out_n < sizeof out - 1) {
		out[out_n++] = (char)c;
		out[out_n] = '\0';
	}
}

static const gps_setup_t *find(const gps_setup_t *list, uint8_t n, const char *s)
{
	return gps_setup_find(list, n, s, (uint32_t)strlen(s));
}

int main(void)
{
	static const gps_setup_t list[] = {
		{ "$PMTK010,001", "PMTK314,0,1,0,1,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0\n" },
		{ "$??TXT", "PCAS02,1000\nPCAS03,1,0,0,0,1,0,0,0\nPCAS04,1\n" },
	};

	// Any talker before TXT; the first matching entry; a prefix, not the whole sentence
	CHECK(find(list, 2, "$GPTXT,01,01,02,MA=CASIC*27") == &list[1]);
	CHECK(find(list, 2, "$BDTXT,01,01,02,ANTENNA OK*2B") == &list[1]);
	CHECK(find(list, 2, "$GNTXT,01,01,01,ANTENNA OPEN*3B") == &list[1]);
	CHECK(find(list, 2, "$PMTK010,001*2E") == &list[0]);
	// Not a match: other sentences, another PMTK010 message, too short, an empty list
	CHECK(find(list, 2, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A") == NULL);
	CHECK(find(list, 2, "$PMTK010,002*2D") == NULL);
	CHECK(find(list, 2, "$GPTX") == NULL);
	CHECK(gps_setup_find(list, 2, "$GPTXT", 5) == NULL);	// only the first len characters count
	CHECK(find(list, 0, "$GPTXT,01,01,02,MA=CASIC*27") == NULL);
	CHECK(find(list, 2, "") == NULL);

	// Framing: "$", the body, "*" and the XOR of the body in upper-case hex, CR LF; one sentence a body
	out_n = 0;
	gps_setup_send(list[1].send, put);
	CHECK(strcmp(out, "$PCAS02,1000*2E\r\n$PCAS03,1,0,0,0,1,0,0,0*02\r\n$PCAS04,1*18\r\n") == 0);

	// A last body without its '\n' is sent as well; an empty list sends nothing
	out_n = 0;
	out[0] = '\0';
	gps_setup_send("PMTK010,001", put);
	CHECK(strcmp(out, "$PMTK010,001*2E\r\n") == 0);
	out_n = 0;
	out[0] = '\0';
	gps_setup_send("", put);
	CHECK(out_n == 0);

	// Every framed sentence passes our own line framing (checksum letters a module accepts)
	out_n = 0;
	gps_setup_send(list[0].send, put);
	gps_setup_send(list[1].send, put);
	static nmea_line_t l;
	nmea_line_init(&l);
	int good = 0;
	for (size_t i = 0; i < out_n; i++) {
		good += nmea_line_feed(&l, (uint8_t)out[i]);
	}
	CHECK(good == 4 && l.bad == 0);

	if (failures == 0) {
		printf("gpsSetupTest: all passed\n");
	}
	return failures != 0;
}
