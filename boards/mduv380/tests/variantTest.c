// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-UV380 Plus 10 W marker (variant_mduv380.h): the image and the marker disagree only when the byte was read, and the
// two images treat the marker alike. A 5 W radio that carries the marker (#773) gets a warning, nothing more: the
// board has no way left to refuse TX on the marker alone
#include <stdio.h>
#include "variant_mduv380.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	// 5 W image: the marker warns (the #773 radio), anything else is quiet
	CHECK(mduv380_variant_mismatch(true, MDUV380_MARK_10W, false));
	CHECK(!mduv380_variant_mismatch(true, 0xFFu, false));
	CHECK(!mduv380_variant_mismatch(true, 0x00u, false));
	CHECK(!mduv380_variant_mismatch(true, 0xF5u, false));
	// 10 W image: the opposite, the marker's absence warns
	CHECK(!mduv380_variant_mismatch(true, MDUV380_MARK_10W, true));
	CHECK(mduv380_variant_mismatch(true, 0xFFu, true));
	CHECK(mduv380_variant_mismatch(true, 0x00u, true));
	CHECK(mduv380_variant_mismatch(true, 0xF3u, true));
	// An unread byte says nothing, whatever the buffer holds, for both images
	for (unsigned m = 0; m <= 0xFFu; m++) {
		CHECK(!mduv380_variant_mismatch(false, (uint8_t)m, false));
		CHECK(!mduv380_variant_mismatch(false, (uint8_t)m, true));
	}
	// Exhaustive: for a read byte exactly one of the two images warns
	for (unsigned m = 0; m <= 0xFFu; m++)
		CHECK(mduv380_variant_mismatch(true, (uint8_t)m, false) != mduv380_variant_mismatch(true, (uint8_t)m, true));
	printf("mduv380 variant: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
