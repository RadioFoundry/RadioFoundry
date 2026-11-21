// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The audio route (boards/dm32/audio_route.h) in every radio state: the shared options give the levels the code had
// before the options existed; the stock-usage options give the tone sequence of hardware-dm36.md 9.1
#include <stdio.h>
#include "audio_route.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	for (unsigned bits = 0; bits < 32; bits++) {
		dm32_audio_state_t s = {
			.fm = bits & 1, .transmitting = bits & 2, .pcm = bits & 4, .rx_audio = bits & 8, .aprs_listen = bits & 16,
		};
		bool fm_open = s.fm && !s.transmitting && !s.pcm && s.rx_audio;
		bool analog = fm_open && !s.aprs_listen;

		// Shared: PTB14 PTB15's complement, the AF output never touched
		dm32_audio_route_t a = dm32_audio_route(s, false, false);
		CHECK(a.fd6818_spk == analog && a.path_pin == !analog && a.af_on && a.fm_play == (fm_open && s.aprs_listen));

		// Stock usage: the same speaker source; PTB14 by the mode alone
		dm32_audio_route_t b = dm32_audio_route(s, true, true);
		CHECK(b.fd6818_spk == a.fd6818_spk && b.fm_play == a.fm_play);
		CHECK(b.path_pin == !s.fm);
		// DMR never mutes; in FM a tone always does, and without one the squelch or APRS opens it
		if (!s.fm) {
			CHECK(b.af_on);
		} else if (s.pcm) {
			CHECK(!b.af_on);
		} else {
			CHECK(b.af_on == (s.rx_audio || s.aprs_listen));
		}
	}

	// The FM sequence: idle (codec, muted), squelch open (FD6818, on), a tone (codec, muted), after it with the
	// squelch closed (codec, still muted)
	dm32_audio_route_t r = dm32_audio_route((dm32_audio_state_t){ .fm = true }, true, true);
	CHECK(!r.fd6818_spk && !r.af_on && !r.path_pin);
	r = dm32_audio_route((dm32_audio_state_t){ .fm = true, .rx_audio = true }, true, true);
	CHECK(r.fd6818_spk && r.af_on);
	r = dm32_audio_route((dm32_audio_state_t){ .fm = true, .rx_audio = true, .pcm = true }, true, true);
	CHECK(!r.fd6818_spk && !r.af_on);
	r = dm32_audio_route((dm32_audio_state_t){ .fm = true }, true, true);
	CHECK(!r.fd6818_spk && !r.af_on);
	// DMR: codec, PTB14 high, AF left alone through a tone
	r = dm32_audio_route((dm32_audio_state_t){ .pcm = true }, true, true);
	CHECK(!r.fd6818_spk && r.path_pin && r.af_on);

	if (failures) {
		printf("audioRouteTest: %d failures\n", failures);
		return 1;
	}
	printf("audioRouteTest: all passed\n");
	return 0;
}
