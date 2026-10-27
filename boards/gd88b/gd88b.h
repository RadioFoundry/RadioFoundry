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
// The switch in A's loudspeaker path closed (on) or open: RX2_SPEAKER (paths_gd88b.c)
void gd88b_speaker(bool on);
bool gd88b_speaker_on(void);
// A transmits (pa_gd88b.c): the path pins for A's chip in its TX band (on), or back as the receivers have them
void gd88b_path_a_tx(bool on, bool uhf);
// The band's PA select and supply pins
void gd88b_path_pa(bool uhf, bool select, bool supply);
// A's PA (pa_gd88b.c): pa_init at start-up; pa_arm is RX2_TX (hz 0 disarms, and stops a transmission in progress)
void gd88b_pa_init(void);
bool gd88b_pa_arm(uint32_t hz, uint16_t pa, uint16_t on_us, uint16_t off_us);
bool gd88b_pa_diag_line(char *buf, uint8_t len);
// B's own transmitter, for the debug view: its path switched to it (and refused: A's transmission had it), its PA
// keyed (and skipped: not armed, or A's transmission had the PA)
void gd88b_own_tx_counts(uint32_t *paths, uint32_t *path_refused, uint32_t *keys, uint32_t *skips);
void gd88b_pa_own_counts(uint32_t *keys, uint32_t *skips);	// pa_gd88b.c's half
// The status LEDs (RX2_LED bits: bit 0 LED 1, bit 1 LED 2; RX2_LED_STUB blinks them until the next call)
void gd88b_leds(uint8_t leds);
void gd88b_leds_poll(uint32_t now_ms);	// the blink, from the link task's loop
bool gd88b_diag_line(uint8_t i, char *buf, uint8_t len);	// board.diag_line (app_gd88b.c)

#endif
