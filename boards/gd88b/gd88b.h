// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC B: what its own files share (docs/spec/hardware-gd88.md 10)
#ifndef GD88B_H
#define GD88B_H

#include <stdint.h>
#include <stdbool.h>

// B's audio to A held off (true) or as the radio sets it: A gives B the speaker with RX2_AUDIO (paths_gd88b.c)
void gd88b_path_mute(bool mute);
// The status LEDs (RX2_LED bits: bit 0 LED 1, bit 1 LED 2)
void gd88b_leds(uint8_t leds);

#endif
