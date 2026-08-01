// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Logical input: polls board.input, applies the board key map, produces actions with gestures
#ifndef CORE_INPUT_H
#define CORE_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_input.h"

#define INPUT_LONG_MS		600
#define INPUT_REPEAT_MS		120

void input_start(void);
// Wait up to wait_ms for the next event (0 = don't wait). PTT and MONITOR edges also go straight to the radio core
bool input_next(input_event_t *ev, uint32_t wait_ms);
// Milliseconds since the last key or encoder activity (backlight time-out)
uint32_t input_idle_ms(void);
// Raw state for the diagnostics page: physical key bits held now, encoder detents since start
uint32_t input_raw_keys(void);
int32_t input_encoder_total(void);

#endif
