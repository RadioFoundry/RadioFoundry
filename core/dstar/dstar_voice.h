// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR's voice frame seen from the DV layer (docs/spec/dstar-modem.md 4): 20 ms of 8 kHz speech is one 72-bit frame
// (3600 bit/s with its FEC, JARL D-STAR standard 4.1.2 b). The framing (dstar_frame.h) never looks inside a frame; the
// radio core codes them with the 2400 bit/s MBE codec (core/mbe/mbe2400.h, docs/spec/mbe-2400.md). Nine bytes, air
// order: the frame's bits as the standard sends them, byte 0 first, each byte least significant bit first.
#ifndef RFOUNDRY_DSTAR_VOICE_H
#define RFOUNDRY_DSTAR_VOICE_H

#include <stdint.h>
#include <stdbool.h>

#define DSTAR_VOICE_BITS	72u
#define DSTAR_VOICE_BYTES	9u
#define DSTAR_VOICE_SAMPLES	160u	// 20 ms at 8 kHz

// The voice frame the standard gives for a lost packet (6.6, the first nine bytes of 9E 8D 32 88 26 1A 3F 61 E8):
// what the transmitter sends with no codec (a data-only or test transmission) and what a gateway substitutes
extern const uint8_t dstar_voice_silence[DSTAR_VOICE_BYTES];

#endif
