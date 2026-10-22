// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro audio path pins PTB14 / PTB15 (hardware-dm1801pro.md 3.1). A table-free helper so the host test
// (tests/dm1801proPinsTest.c) can check the levels every radio state gets.
//
// PTB15 (PIN_SPK_SOURCE) high puts the FD6818's audio on the speaker and takes it off the HR_C7000's input (FM receive
// with the squelch open); low leaves the speaker on the HR_C7000's codec and the FD6818's audio on its input. PTB14
// (PIN_MIC_PATH) is always its complement, as in the radio's own firmware, which writes the two only as a pair. That
// includes DMR transmit: the DM-32UV's DMR route (PTB14 low) took the microphone off the codec ADC on this radio, and
// the vocoder encoded near silence (hardware-dm1801pro.md 12)
#ifndef DM1801PRO_AUDIO_ROUTE_H
#define DM1801PRO_AUDIO_ROUTE_H

#include <stdbool.h>

typedef struct {
	bool spk_source;	// PTB15
	bool mic_path;		// PTB14
} dm1801pro_audio_route_t;

// analog_rx: FM receive plays the FD6818's audio straight to the speaker (never while transmitting, never for APRS)
static inline dm1801pro_audio_route_t dm1801pro_audio_route(bool analog_rx)
{
	return (dm1801pro_audio_route_t){ .spk_source = analog_rx, .mic_path = !analog_rx };
}

#endif
