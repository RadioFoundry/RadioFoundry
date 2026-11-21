// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The board's temperature sensors and fan as the core reports them (hal_temp.h, docs/spec/temperature.md)
#ifndef CORE_TEMP_H
#define CORE_TEMP_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_temp.h"

typedef struct {
	uint8_t kind;		// temp_kind_t
	bool has_mv;
	uint16_t mv;
	bool has_dc;
	int16_t dc;		// 0.1 degree C
} temp_reading_t;

// The temperature a curve gives for mv (hal_temp.h: linear between points, the end point's outside); false for a curve
// of no points
bool temp_curve_dc(const temp_point_t *curve, uint8_t len, uint16_t mv, int16_t *dc);
// Sensor i of t (NULL: none), read now; false past the last sensor
bool temp_read(const hal_temp_t *t, uint8_t i, temp_reading_t *r);

#endif
