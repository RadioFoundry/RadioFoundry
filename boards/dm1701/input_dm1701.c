// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 keypad, side keys, PTT and encoder: STUB, to be replaced
#include "dm1701.h"

static void init(void) {}
static uint32_t scan(void) { return 0; }
static int8_t encoder(void) { return 0; }

const hal_input_t dm1701_input = { .init = init, .scan = scan, .encoder = encoder };
