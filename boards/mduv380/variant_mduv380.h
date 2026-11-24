// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TYT MD-UV380: does the Plus 10 W marker disagree with the image (hardware-mduv380.md 4)? Pure, so the host tests check
// it. Security register 3 byte 0x1C = 0xF4 was reported as the Plus 10 W marker, but at least one 5 W radio carries the
// same value (#773), so a disagreement is only a warning (board_t.variant_mismatch): either image transmits on either
// radio. Only a 5 W image warns, when the marker is there: the marker is unconfirmed, so its absence says nothing about
// a 10 W radio. An unread byte says nothing
#ifndef VARIANT_MDUV380_H
#define VARIANT_MDUV380_H

#include <stdbool.h>
#include <stdint.h>

#define MDUV380_MARK_10W	0xF4u

static inline bool mduv380_variant_mismatch(bool read_ok, uint8_t mark, bool image_10w)
{
	return read_ok && !image_10w && mark == MDUV380_MARK_10W;
}

#endif
