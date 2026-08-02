// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The standard sub-audio lists (50 CTCSS tones, 104 DCS codes) and stepping through them for a tone picker.
// Picker order: none, CTCSS low to high, DCS normal by code, DCS inverted by code.
#ifndef RADIO_TONES_H
#define RADIO_TONES_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_radio.h"
#include "config.h"

#define TONES_CTCSS_COUNT	50
#define TONES_DCS_COUNT		104
#define TONES_PICKER_COUNT	(1 + TONES_CTCSS_COUNT + 2 * TONES_DCS_COUNT)

#if CONFIG_SUBTONE

// Hz x 10, ascending
extern const uint16_t tones_ctcss[TONES_CTCSS_COUNT];
// The 9-bit codes (written in octal), ascending
extern const uint16_t tones_dcs[TONES_DCS_COUNT];

// Whether a tone is one the radio can use: none, CTCSS 60.0-260.0 Hz (not only the standard ones), DCS 0-0777
bool tones_valid(const subtone_t *t);
// Whether it is on the standard lists (or none)
bool tones_standard(const subtone_t *t);
// The picker entry 0..TONES_PICKER_COUNT-1
subtone_t tones_picker(uint16_t index);
// The next picker entry from t in dir (+1 / -1), wrapping. A tone not on the lists (a non-standard CTCSS value from
// the codeplug) steps to the nearest standard entry in that direction
subtone_t tones_step(const subtone_t *t, int dir);

#else	// built without CTCSS / DCS: only "no tone" exists

static inline bool tones_valid(const subtone_t *t) { return t->type == TONE_NONE; }
static inline bool tones_standard(const subtone_t *t) { return t->type == TONE_NONE; }
static inline subtone_t tones_picker(uint16_t index) { (void)index; return (subtone_t){ TONE_NONE, 0 }; }
static inline subtone_t tones_step(const subtone_t *t, int dir) { (void)t; (void)dir; return (subtone_t){ TONE_NONE, 0 }; }

#endif

#endif
