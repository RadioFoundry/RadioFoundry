// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// netSvcTest's cases: each TCP service against the scripted peer, with fakes for the APRS engine and the radio core
#include <string.h>
#include "netSvcFakes.h"
#include "net/net.h"
#include "net/net_svc.h"
#include "net/tcp.h"
#include "config.h"

// ---- fake APRS engine (aprs.h / aprs_host.h) ----
static bool kiss_on = true;
static uint8_t got[1024];
static size_t got_n;
static int got_frames;
static uint8_t ring[4096];
static size_t ring_n;

void aprs_host_kiss_frame(const uint8_t *f, size_t n, uint32_t t)
{
	(void)t;
	memcpy(got, f, n);
	got_n = n;
	got_frames++;
}

size_t aprs_host_output(uint8_t *out, size_t max)
{
	size_t n = ring_n < max ? ring_n : max;
	memcpy(out, ring, n);
	memmove(ring, ring + n, ring_n - n);
	ring_n -= n;
	return n;
}

bool aprs_host_enabled(void)
{
	return kiss_on;
}

static void ring_put(const void *d, size_t n)
{
	memcpy(ring + ring_n, d, n);
	ring_n += n;
}

// ---- KISS over TCP ----
static void test_kiss(void)
{
	static peer_t a, b;
	CHECK(peer_connect(&a, net_test_host_ip(), 50001, NET_KISS_PORT));
	CHECK(net_tnc_taken());

	// A data frame from the client, with an escaped FEND and FESC: the engine gets it decoded
	static const uint8_t frame[] = { 0xC0, 0x00, 'A', 0xDB, 0xDC, 'B', 0xDB, 0xDD, 'C', 0xC0 };
	peer_send(&a, frame, sizeof(frame));
	CHECK(got_frames == 1 && got_n == 6 && got[0] == 0 && got[1] == 'A' && got[2] == 0xC0 && got[4] == 0xDB);
	// Split across segments
	peer_send(&a, frame, 4);
	peer_send(&a, frame + 4, sizeof(frame) - 4);
	CHECK(got_frames == 2 && got_n == 6);

	// The engine's output goes to the client, frame for frame (the host link leaves it alone)
	static const uint8_t out1[] = { 0xC0, 0x00, 'x', 0xDB, 0xDC, 'y', 0xC0 };
	static const uint8_t out2[] = { 0xC0, 0x06, 'G', 'N', '0', 'C', 'A', 'L', 'L', 0xC0 };
	ring_put(out1, sizeof(out1));
	ring_put(out2, sizeof(out2));
	pump();
	CHECK(ring_n == 0 && a.rxn == (int)(sizeof(out1) + sizeof(out2)));
	CHECK(memcmp(a.rx, out1, sizeof(out1)) == 0 && memcmp(a.rx + sizeof(out1), out2, sizeof(out2)) == 0);

	// A lot of output: it goes as the client acknowledges, nothing lost
	peer_clear(&a);
	static uint8_t big[300];
	big[0] = 0xC0;
	big[1] = 0x00;
	for (int i = 2; i < 299; i++) {
		big[i] = (uint8_t)('a' + i % 26);
	}
	big[299] = 0xC0;
	for (int k = 0; k < 10; k++) {
		ring_put(big, sizeof(big));
	}
	pump();
	CHECK(ring_n == 0 && a.rxn == 10 * (int)sizeof(big));

	// A second client replaces the first
	CHECK(peer_connect(&b, net_test_host_ip(), 50002, NET_KISS_PORT));
	CHECK(a.reset && b.established);
	ring_put(out1, sizeof(out1));
	pump();
	CHECK(b.rxn == (int)sizeof(out1));

	// KISS turned off in the settings: the output stays with the host link
	kiss_on = false;
	CHECK(!net_tnc_taken());
	ring_put(out1, sizeof(out1));
	pump();
	CHECK(ring_n == sizeof(out1));
	kiss_on = true;
	ring_n = 0;

	// The client closes: the port is free, the host link has the output again
	peer_close(&b);
	CHECK(b.fin && !net_tnc_taken());
	run_for(3000);
}

void tests_run(void)
{
	test_kiss();
}
