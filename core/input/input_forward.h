// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef INPUT_FORWARD_H
#define INPUT_FORWARD_H

#include <stdbool.h>
#include "hal_input.h"

// Forward raw edges to the radio; blocking PTT also blocks burst DOWN, but never either UP.
void input_forward(const input_event_t *ev, bool ptt_blocked);

#endif
