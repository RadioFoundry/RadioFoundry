// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power key: held to switch off (docs/spec/radio-features.md 5.5). On radios with a power key instead of a knob switch,
// the press that switched the radio on is usually still held when it starts, so the key counts only once it has been
// seen released; then held PWRKEY_OFF_POLLS polls in a row means off. Pure logic: the board feeds it the key's level
// from its switch_on(), which core/app/app.c polls every 100 ms.
#ifndef RADIO_POWER_KEY_H
#define RADIO_POWER_KEY_H

#include <stdint.h>
#include <stdbool.h>

#define PWRKEY_OFF_POLLS	20u	// 2 s at the 100 ms poll

typedef struct {
	bool released;		// seen up since start-up
	uint16_t held;		// polls held down since then
} pwrkey_t;

// At start-up: the key is taken as held until it reads up
void pwrkey_init(pwrkey_t *k);
// One poll with the key's level; true while the radio stays on
bool pwrkey_on(pwrkey_t *k, bool down);

#endif
