// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text editor's Back: drop the last character whole. A name field is ASCII / Latin-1 (one byte each)
// and 3-byte UTF-8 sequences for Chinese; there is no 2-byte form (the same rule as gfx_utf8_next()).
// Returns the new length; the buffer is unchanged up to it. Tested by tests/host/textEditTest.c
#ifndef CORE_UI_TEXT_EDIT_H
#define CORE_UI_TEXT_EDIT_H

#include <stdint.h>

// Walk from the start so the last character's boundary is the one the text layer drew. Stepping back over
// trailing continuation bytes instead would eat a Latin-1 letter in 0xA0-0xBF (a degree sign, 0xB0, is
// one character too), which must be deleted on its own.
static inline uint8_t text_edit_backspace(const char *buf, uint8_t len)
{
	const uint8_t *b = (const uint8_t *)buf;
	uint8_t start = 0;
	for (uint8_t i = 0; i < len; ) {
		start = i;
		uint8_t n = 1;
		if (i + 2 < len && (b[i] & 0xF0u) == 0xE0u &&
				(b[i + 1] & 0xC0u) == 0x80u && (b[i + 2] & 0xC0u) == 0x80u) {
			uint32_t cp = ((uint32_t)(b[i] & 0x0Fu) << 12) | ((uint32_t)(b[i + 1] & 0x3Fu) << 6) |
					(uint32_t)(b[i + 2] & 0x3Fu);
			if (cp >= 0x800u)		// not an overlong form (gfx_utf8_next)
				n = 3;
		}
		i = (uint8_t)(i + n);
	}
	return start;
}

#endif
