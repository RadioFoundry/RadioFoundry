// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 display: STUB, to be replaced (FSMC bus, panel init, backlight)
#include "dm1701.h"

static void init(void) {}
static void flush(const void *fb, uint16_t y0, uint16_t y1) { (void)fb; (void)y0; (void)y1; }
static void set_backlight(uint8_t percent) { (void)percent; }

const hal_display_t dm1701_display = { .width = 160, .height = 120, .bpp = 16, .init = init, .flush = flush, .set_backlight = set_backlight };
