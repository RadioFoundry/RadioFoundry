// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef PANEL_MD380_H
#define PANEL_MD380_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint8_t type;		// the st7735 driver's type
	const char *controller;	// "gc9xxx", "st7735" or "unknown"
	const char *source;	// "id" (the controller recognized), "factory" (the byte alone) or "default"
} md380_panel_t;

// factory: the byte from SPI flash security register 3 at 0x1D, -1 when it couldn't be read. id: the read display ID
// (0x04) after its dummy byte, all zero when it wasn't read
md380_panel_t md380_panel_detect(int16_t factory, const uint8_t id[3]);

#endif
