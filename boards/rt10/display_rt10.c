// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The RT10 has no display. The core still draws its screens into a 128x64 1 bpp frame buffer (board.mk), which the
// debug link's screen capture reads; nothing is shown
#include "rt10.h"

static void init(void)
{
}

static void flush(const void *fb, uint16_t y0, uint16_t y1)
{
	(void)fb;
	(void)y0;
	(void)y1;
}

static void set_backlight(uint8_t percent)
{
	(void)percent;
}

const hal_display_t rt10_display = {
	.width = 128,
	.height = 64,
	.bpp = 1,
	.init = init,
	.flush = flush,
	.set_backlight = set_backlight,
};
