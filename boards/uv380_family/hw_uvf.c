// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family hardware identity for About and the support info (board_t.hw_line, docs/spec/support-info.md 2): the
// family's chips, then the radio's own lines (uvf_variant_t.hw_line). Cached values only, no bus traffic
#include "uvf.h"
#include "ui/fmt.h"

bool uvf_hw_line(uint8_t i, uint8_t *role, char *buf, uint8_t len)
{
	switch (i) {
	case 0:
		*role = BOARD_HW_DMR;
		str_copy(buf, len, "HR_C6000");
		return true;
	case 1:
		*role = BOARD_HW_RF;
		str_copy(buf, len, "AT1846S");
		return true;
	default:
		return uvf_variant.hw_line != NULL && uvf_variant.hw_line((uint8_t)(i - 2u), role, buf, len);
	}
}
