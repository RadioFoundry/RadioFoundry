// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC B: what its own files share (docs/spec/hardware-gd88.md 10)
#ifndef GD88B_H
#define GD88B_H

#include <stdint.h>
#include <stdbool.h>

// B's audio to A held off (true) or as the radio sets it: A gives B the speaker with RX2_AUDIO (paths_gd88b.c)
void gd88b_path_mute(bool mute);
// A's own receiver on hz or off (RX2_PATH): its front end and path pins are B's (paths_gd88b.c)
void gd88b_path_main(bool on, uint32_t hz);
// The status LEDs (RX2_LED bits: bit 0 LED 1, bit 1 LED 2)
void gd88b_leds(uint8_t leds);
bool gd88b_diag_line(uint8_t i, char *buf, uint8_t len);	// board.diag_line (app_gd88b.c)

#endif
