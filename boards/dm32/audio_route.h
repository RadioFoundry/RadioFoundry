// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The speaker source (PTB15), the PTB14 path pin and the FD6818's AF output for each radio state (radio_dm32.c
// speaker_source; during a transmission the TX path owns PTB14 and 0x47). Table-free, so the host test
// (boards/dm32/tests/audioRouteTest.c) checks both boards' levels. The two board options:
// - by_mode: PTB14 follows the mode (FM low, DMR high), as the stock firmware that sets it only at channel entry;
//   otherwise it is PTB15's complement
// - af_gated: in FM the FD6818's AF output is muted (0x47 bits 11:8 cleared) unless the speaker plays FM receive audio
//   or APRS listens, as stock mutes it before a tone and opens it with the squelch; otherwise it stays on
#ifndef DM32_AUDIO_ROUTE_H
#define DM32_AUDIO_ROUTE_H

#include <stdbool.h>

typedef struct {
	bool fm;		// FM mode (else DMR)
	bool transmitting;
	bool pcm;		// a tone or DMR voice plays through the codec
	bool rx_audio;		// the squelch is open
	bool aprs_listen;	// APRS takes the FD6818's audio through the HR_C7000
} dm32_audio_state_t;

typedef struct {
	bool fd6818_spk;	// the speaker plays the FD6818's audio (PTB15 at SPK_SOURCE_FD6818), else the codec
	bool fm_play;		// FM receive plays through the HR_C7000's FM playback (APRS listening, squelch open)
	bool path_pin;		// PTB14's level
	bool af_on;		// the FD6818's AF output on (only written in FM)
} dm32_audio_route_t;

static inline dm32_audio_route_t dm32_audio_route(dm32_audio_state_t s, bool by_mode, bool af_gated)
{
	// The FD6818's output is squelched noise while the squelch is closed: the speaker listens to it only while it is open
	bool fm_open = s.fm && !s.transmitting && !s.pcm && s.rx_audio;
	bool analog = fm_open && !s.aprs_listen;
	return (dm32_audio_route_t){
		.fd6818_spk = analog,
		.fm_play = fm_open && s.aprs_listen,
		.path_pin = by_mode ? !s.fm : !analog,
		.af_on = !af_gated || !s.fm || (!s.pcm && (s.rx_audio || s.aprs_listen)),
	};
}

#endif
