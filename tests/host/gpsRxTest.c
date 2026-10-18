// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GPS receive ring and module detection for board GPS drivers (drivers/gps/gps_rx): sentences through the ring,
// found / present / absent after the listen time, ring overflow and UART errors in the counts, millisecond wrap
#include <stdio.h>
#include <string.h>
#include "gps/gps_rx/gps_rx.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int sentences;
static char last[100];
static uint32_t last_ms;

static void on_sentence(const char *s, uint8_t len, uint32_t now_ms)
{
	sentences++;
	CHECK(strlen(s) == len);
	snprintf(last, sizeof last, "%s", s);
	last_ms = now_ms;
}

static void put(gps_rx_t *g, const char *s)
{
	for (; *s; s++) {
		gps_rx_byte(g, (uint8_t)*s);
	}
}

static const char gga[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";

int main(void)
{
	static gps_rx_t g;
	uint32_t good, bad, n;

	// Listening, then absent once the listen time has passed with nothing valid
	gps_rx_init(&g, 1000, 5000);
	CHECK(gps_rx_poll(&g, 1000, on_sentence) == GPS_RX_LISTENING);
	put(&g, "noise $GPRMC,1*00\r\n");	// bad checksum: not a module
	CHECK(gps_rx_poll(&g, 5999, on_sentence) == GPS_RX_LISTENING);
	CHECK(gps_rx_poll(&g, 6000, on_sentence) == GPS_RX_ABSENT);
	CHECK(sentences == 0);
	gps_rx_counts(&g, &good, &bad, &n);
	CHECK(good == 0 && bad == 1 && n == 19);

	// The first valid sentence: found once, then present; every sentence to the callback with the poll's time
	gps_rx_init(&g, 0, 5000);
	put(&g, "\r\n");
	put(&g, gga);
	put(&g, "\r\n");
	CHECK(gps_rx_poll(&g, 20, on_sentence) == GPS_RX_FOUND);
	CHECK(sentences == 1 && strcmp(last, gga) == 0 && last_ms == 20);
	CHECK(gps_rx_poll(&g, 40, on_sentence) == GPS_RX_PRESENT);
	CHECK(gps_rx_poll(&g, 60000, on_sentence) == GPS_RX_PRESENT);	// never absent once heard
	put(&g, gga);
	put(&g, "\n");
	CHECK(gps_rx_poll(&g, 60020, NULL) == GPS_RX_PRESENT);	// no callback is fine
	gps_rx_counts(&g, &good, &bad, &n);
	CHECK(good == 2 && bad == 0);

	// A full ring drops bytes and counts them; UART errors count as bad
	gps_rx_init(&g, 0, 5000);
	for (uint32_t i = 0; i < GPS_RX_RING + 10u; i++) {
		gps_rx_byte(&g, 'x');
	}
	gps_rx_error(&g);
	gps_rx_counts(&g, &good, &bad, &n);
	CHECK(n == GPS_RX_RING + 10u && bad == 11u + 1u);	// the ring keeps RING - 1
	CHECK(gps_rx_poll(&g, 10, on_sentence) == GPS_RX_LISTENING);
	CHECK(g.tail == g.head);
	// Recovers: the ring wraps and the next sentence still comes through whole
	sentences = 0;
	put(&g, "\n");
	put(&g, gga);
	put(&g, "\n");
	CHECK(gps_rx_poll(&g, 20, on_sentence) == GPS_RX_FOUND && sentences == 1);

	// The millisecond counter wrapping during the listen time
	gps_rx_init(&g, 0xFFFFF000u, 5000);
	CHECK(gps_rx_poll(&g, 0x00000100u, NULL) == GPS_RX_LISTENING);
	CHECK(gps_rx_poll(&g, 0x00000388u, NULL) == GPS_RX_ABSENT);

	printf("gpsRxTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
