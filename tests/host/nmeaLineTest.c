// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// NMEA line framing for board GPS drivers (drivers/gps/nmea_line): checksums, any talker, CR / LF handling, noise and
// overlong lines (docs/spec/hardware-md380.md 13, gotchas 13.7)
#include <stdio.h>
#include <string.h>
#include "gps/nmea_line/nmea_line.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int feed(nmea_line_t *l, const char *s, char *last, size_t last_len)
{
	int n = 0;
	for (; *s; s++) {
		if (nmea_line_feed(l, (uint8_t)*s)) {
			n++;
			snprintf(last, last_len, "%s", l->buf);
		}
	}
	return n;
}

int main(void)
{
	nmea_line_t l;
	char last[100] = "";
	nmea_line_init(&l);
	// The classic example sentences (checksums computed by hand from the XOR rule)
	static const char rmc[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
	static const char gga[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
	CHECK(nmea_line_checksum_ok(rmc, (uint32_t)strlen(rmc)));
	CHECK(nmea_line_checksum_ok(gga, (uint32_t)strlen(gga)));
	CHECK(!nmea_line_checksum_ok("$GPRMC,1*00", 11));
	CHECK(!nmea_line_checksum_ok("GPRMC*00", 8));

	char in[300];
	snprintf(in, sizeof in, "garbage%s\r\n%s\n", rmc, gga);
	CHECK(feed(&l, in, last, sizeof last) == 2);
	CHECK(strcmp(last, gga) == 0);
	CHECK(l.good == 2 && l.bad == 0);

	// A GN talker (multi-constellation modules), lower case hex in the checksum
	nmea_line_init(&l);
	CHECK(feed(&l, "$GNGSA,A,3*2e\r\n", last, sizeof last) == 1);
	CHECK(strcmp(last, "$GNGSA,A,3*2e") == 0);

	// Bad checksum, missing checksum, a sentence cut by the next "$", noise, overlong
	nmea_line_init(&l);
	CHECK(feed(&l, "$GPRMC,1*00\r\n$GPRMC,1\r\n$GPRMC,12$GNGSA,A,3*2E\n", last, sizeof last) == 1);
	CHECK(l.bad == 3 && l.good == 1);
	nmea_line_init(&l);
	char noisy[] = "$GNGSA,A,\x01" "3*2E\n";
	CHECK(feed(&l, noisy, last, sizeof last) == 0 && l.bad == 1);
	nmea_line_init(&l);
	char longl[200];
	memset(longl, 'A', sizeof longl);
	longl[0] = '$';
	longl[sizeof longl - 2] = '\n';
	longl[sizeof longl - 1] = '\0';
	CHECK(feed(&l, longl, last, sizeof last) == 0 && l.bad == 1);
	// Recovers right after
	CHECK(feed(&l, "$GNGSA,A,3*2E\r\n", last, sizeof last) == 1);

	printf("nmeaLineTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
