// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS module detection with a rate hunt (drivers/gps/gps_rx/gps_baud.c over gps_rx and nmea_line), driven the way
// platform/stm32f4/gps_uart/gps_uart.c drives it: canned NMEA streams with garbage, cut-off and corrupted sentences, a
// silent line (no module), a module at another rate, and one that only ever sends rubbish
#include <stdio.h>
#include <string.h>
#include "gps/gps_rx/gps_rx.h"
#include "gps/gps_rx/gps_baud.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const uint32_t rates[] = { 9600, 4800, 38400, 115200 };
#define N_RATES		(sizeof rates / sizeof rates[0])
#define WINDOW_MS	3000u

static int sentences;
static char last[100];

static void on_sentence(const char *s, uint8_t len, uint32_t now_ms)
{
	(void)now_ms;
	sentences++;
	CHECK(strlen(s) == len);
	snprintf(last, sizeof last, "%s", s);
}

// The radio's side: a ring fed by the "interrupt", the hunt stepped after every poll
typedef struct {
	gps_rx_t rx;
	gps_baud_t hunt;
	uint32_t now;
	uint32_t switches;
	gps_baud_step_t last;
} rig_t;

static void rig_init(rig_t *r, uint32_t now)
{
	memset(r, 0, sizeof *r);
	r->now = now;
	gps_rx_init(&r->rx, now, 0xFFFFFFFFu);
	gps_baud_init(&r->hunt, rates, N_RATES, WINDOW_MS, now, 0);
	r->last = GPS_BAUD_LISTEN;
	sentences = 0;
}

static void feed(rig_t *r, const char *s)
{
	for (; *s; s++) {
		gps_rx_byte(&r->rx, (uint8_t)*s);
	}
}

// Characters the UART flagged (a module at the wrong rate makes framing errors)
static void flagged(rig_t *r, uint32_t n)
{
	while (n--) {
		gps_rx_error(&r->rx);
	}
}

static gps_baud_step_t poll(rig_t *r, uint32_t advance_ms)
{
	r->now += advance_ms;
	gps_rx_state_t st = gps_rx_poll(&r->rx, r->now, on_sentence);
	r->last = gps_baud_step(&r->hunt, st, r->now, r->rx.bytes + r->rx.uart_errors);
	if (r->last == GPS_BAUD_SWITCH) {
		r->switches++;
	}
	return r->last;
}

// Poll every 20 ms for ms, or until the hunt says anything but "listen"
static gps_baud_step_t run(rig_t *r, uint32_t ms)
{
	gps_baud_step_t s = GPS_BAUD_LISTEN;
	for (uint32_t t = 0; t < ms; t += 20u) {
		s = poll(r, 20);
		if (s != GPS_BAUD_LISTEN) {
			break;
		}
	}
	return s;
}

static const char rmc[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
static const char gga[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
static const char gn_rmc[] = "$GNRMC,000000,V,,,,,,,010180,,,N*45";	// no fix yet: still a valid sentence

int main(void)
{
	static rig_t r;
	uint32_t good, bad, n;

	// The checksums above are right (the line framer is the judge)
	CHECK(nmea_line_checksum_ok(rmc, sizeof rmc - 1));
	CHECK(nmea_line_checksum_ok(gga, sizeof gga - 1));
	CHECK(nmea_line_checksum_ok(gn_rmc, sizeof gn_rmc - 1));

	// No module: the line stays silent for the first window and the hunt ends there, without trying other rates
	rig_init(&r, 1000);
	CHECK(gps_baud_rate(&r.hunt) == 9600);
	CHECK(run(&r, 2980) == GPS_BAUD_LISTEN);
	CHECK(run(&r, 100) == GPS_BAUD_ABSENT);
	CHECK(r.switches == 0 && sentences == 0);
	CHECK(r.now - 1000u >= WINDOW_MS && r.now - 1000u < WINDOW_MS + 40u);
	CHECK(poll(&r, 20) == GPS_BAUD_ABSENT);		// and stays so

	// A module at the first rate: garbage and a sentence cut off by the power-up, then valid ones
	rig_init(&r, 0);
	feed(&r, "\xff\x01 junk\r\n$GPGGA,1235");	// cut off ...
	feed(&r, rmc);					// ... by the next '$'
	feed(&r, "\r\n");
	CHECK(poll(&r, 20) == GPS_BAUD_FOUND);
	CHECK(sentences == 1 && strcmp(last, rmc) == 0);
	CHECK(gps_baud_rate(&r.hunt) == 9600 && r.switches == 0);
	gps_rx_counts(&r.rx, &good, &bad, &n);
	CHECK(good == 1 && bad == 1);			// the cut-off GGA
	// Present for good: a later silence or rubbish doesn't end it, and valid sentences keep coming through
	CHECK(run(&r, 20000) == GPS_BAUD_FOUND && r.now == 40u);
	feed(&r, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48\r\n");	// checksum off by one
	feed(&r, gga);
	feed(&r, "\n");
	feed(&r, "$GPRMC,12351");	// partial: waits for its end
	CHECK(poll(&r, 20) == GPS_BAUD_FOUND);
	CHECK(sentences == 2 && strcmp(last, gga) == 0);
	feed(&r, "9,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n");
	CHECK(poll(&r, 20) == GPS_BAUD_FOUND);
	CHECK(sentences == 3 && strcmp(last, rmc) == 0);

	// A module at 38400: rubbish (characters and flagged ones) at 9600 and 4800, valid sentences once switched
	rig_init(&r, 500);
	flagged(&r, 30);
	feed(&r, "\x80\xfe\x13~~");
	CHECK(run(&r, WINDOW_MS + 20u) == GPS_BAUD_SWITCH && r.switches == 1);
	CHECK(gps_baud_rate(&r.hunt) == 4800);
	flagged(&r, 50);
	CHECK(run(&r, WINDOW_MS + 20u) == GPS_BAUD_SWITCH && r.switches == 2);
	CHECK(gps_baud_rate(&r.hunt) == 38400 && r.switches == 2);
	CHECK(sentences == 0);
	feed(&r, gn_rmc);
	feed(&r, "\r\n");
	CHECK(poll(&r, 20) == GPS_BAUD_FOUND);
	CHECK(sentences == 1 && gps_baud_rate(&r.hunt) == 38400);

	// Something that only ever sends rubbish: every rate tried, then absent
	rig_init(&r, 0);
	for (unsigned i = 0; i < N_RATES * (WINDOW_MS / 20u) + 10u; i++) {
		feed(&r, "$GPXXX,garbage*ZZ\r\n");
		if (poll(&r, 20) == GPS_BAUD_ABSENT) {
			break;
		}
	}
	CHECK(r.last == GPS_BAUD_ABSENT);
	CHECK(r.switches == N_RATES - 1u && sentences == 0);

	// A module that goes quiet after the first rate's rubbish: absent at the next window (silence ends the hunt)
	rig_init(&r, 0);
	flagged(&r, 5);
	CHECK(run(&r, WINDOW_MS + 20u) == GPS_BAUD_SWITCH && r.switches == 1);
	CHECK(run(&r, WINDOW_MS + 40u) == GPS_BAUD_ABSENT && r.switches == 1);

	// The millisecond counter wrapping inside a window
	rig_init(&r, 0xFFFFF000u);
	CHECK(run(&r, 2000) == GPS_BAUD_LISTEN);
	CHECK(run(&r, 1100) == GPS_BAUD_ABSENT);

	// An empty list: nothing to listen at
	gps_baud_t empty;
	gps_baud_init(&empty, rates, 0, WINDOW_MS, 0, 0);
	CHECK(gps_baud_step(&empty, GPS_RX_LISTENING, 0, 0) == GPS_BAUD_ABSENT && gps_baud_rate(&empty) == 0);

	printf("gpsBaudTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
