// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef SATELLITE_PATH_H
#define SATELLITE_PATH_H

#include <stdint.h>

#define SATELLITE_PASS_PATH_POINTS 16u

typedef struct {
	uint16_t points[SATELLITE_PASS_PATH_POINTS];
	uint16_t azimuth_deg;
	uint8_t count;
} satellite_path_view_t;

static inline uint16_t satellite_path_pack(uint16_t azimuth_deg, uint8_t elevation_deg)
{
	return (uint16_t)((azimuth_deg << 7) | elevation_deg);
}

static inline uint16_t satellite_path_azimuth(uint16_t point)
{
	return point >> 7;
}

static inline uint8_t satellite_path_elevation(uint16_t point)
{
	return (uint8_t)(point & 0x7fu);
}

#endif