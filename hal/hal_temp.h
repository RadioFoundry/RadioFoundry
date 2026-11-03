// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Temperature sensors and the fan (docs/spec/temperature.md). A board lists what it has: a PA or board thermistor, the
// MCU's own sensor, a battery's. Each sensor gives its output in mV and, where the board knows how, the temperature:
// through a curve of mV to degrees, or read directly (a digital sensor, an MCU sensor with its factory constants). The
// core reads them on request (core/app/temp.c) for the debug link and SNMP; what a board does with a sensor itself
// (a fan, a TX power cut back) stays in the board.
#ifndef HAL_TEMP_H
#define HAL_TEMP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	TEMP_KIND_PA = 1,	// the transmitter's power amplifier or its heatsink
	TEMP_KIND_BOARD,	// the main board, away from the PA
	TEMP_KIND_MCU,		// the MCU's own sensor
	TEMP_KIND_BATTERY,	// in the battery pack
} temp_kind_t;

// One point of a sensor's curve: its output and the temperature it means. A curve lists points in mV order (rising or
// falling with temperature, the same way throughout); between points the temperature is linear, outside them the end
// point's
typedef struct {
	uint16_t mv;
	int16_t dc;		// 0.1 degree C
} temp_point_t;

typedef struct {
	uint8_t kind;		// temp_kind_t
	// The sensor's output in mV; NULL for a sensor that only gives degrees (read_dc)
	uint16_t (*read_mv)(void);
	// The temperature, 0.1 degree C, read directly; NULL to use the curve. False: no reading now
	bool (*read_dc)(int16_t *dc);
	const temp_point_t *curve;	// NULL with read_dc, or while the board doesn't know the sensor's curve
	uint8_t curve_len;
} hal_temp_sensor_t;

typedef struct {
	const hal_temp_sensor_t *sensors;
	uint8_t count;
	// Optional (NULL: no fan the firmware drives): whether the fan runs now
	bool (*fan_on)(void);
} hal_temp_t;

#endif
