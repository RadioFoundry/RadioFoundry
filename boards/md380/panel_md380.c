// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 panel type: the st7735 driver's type from the controller's ID and the factory byte (hardware-md380.md 7).
// No hardware access, so the host tests run it (boards/md380/tests/md380PanelTest.c)
#include "panel_md380.h"

// The controller from its ID, the orientation from the factory byte where it was programmed. Bit 0 tells a GC9xxx's
// orientation in both stock layouts: 0 = flipped (type 2), 1 = normal (type 3)
md380_panel_t md380_panel_detect(int16_t factory, const uint8_t id[3])
{
	bool programmed = factory >= 0 && factory != 0xFF;

	if (id[0] == 0x00 && id[1] == 0x91) {
		return (md380_panel_t){ programmed && (factory & 1) == 0 ? 2 : 3, "gc9xxx", "id" };
	}
	if (id[0] == 0x7C) {
		return (md380_panel_t){ programmed && (factory & 3) == 1 ? 1 : 0, "st7735", "id" };
	}
	if (programmed) {
		return (md380_panel_t){ (uint8_t)(factory & 3), "unknown", "factory" };
	}
	return (md380_panel_t){ 0, "unknown", "default" };
}
