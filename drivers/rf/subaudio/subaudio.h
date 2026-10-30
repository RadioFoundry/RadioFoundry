// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Sub-audio math shared by the RF chip drivers (AT1846S, FD6818)
#ifndef SUBAUDIO_H
#define SUBAUDIO_H

#include <stdint.h>
#include <stdbool.h>

// The 23-bit DCS word for a 9-bit code: Golay(23,12) parity bits 22:12, the marker 100 in 11:9, the code in 8:0
// (code 023 -> 0x763813, the example in both the AT1846S and the FD6818 datasheets)
uint32_t subaudio_dcs_word(uint16_t code);

// The BK4819 / FD6818 family's words, shared by their drivers. CTCSS (0x07) for a tone in Hz x 10: Hz x 2^27 / 6.5 MHz
// (about 20.6489). Tone generator (0x71, 0x72) for Hz: Hz x 2^21 / 203125 (about 10.3244). Rounded to nearest, or
// truncated where a working firmware's words are (its TX CTCSS word, its single tones)
uint16_t subaudio_ctcss_word(uint16_t hz_x10, bool round);
uint16_t subaudio_tone_word(uint16_t hz, bool round);

#endif
