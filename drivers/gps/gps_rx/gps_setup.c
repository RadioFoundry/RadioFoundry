// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS module set-up sentences (gps_setup.h)
#include "gps_setup.h"
#include <stddef.h>

const gps_setup_t *gps_setup_find(const gps_setup_t *list, uint8_t n, const char *s, uint32_t len)
{
	for (; n > 0; n--, list++) {
		const char *m = list->match;
		uint32_t i = 0;
		for (; m[i] != '\0' && i < len && (m[i] == '?' || m[i] == s[i]); i++) {
		}
		if (m[i] == '\0') {
			return list;
		}
	}
	return NULL;
}

static void hex(void (*put)(uint8_t c), uint8_t v)
{
	put((uint8_t)(v + (v < 10u ? (uint8_t)'0' : (uint8_t)('A' - 10))));
}

void gps_setup_send(const char *send, void (*put)(uint8_t c))
{
	while (*send != '\0') {
		uint8_t x = 0;
		put('$');
		for (; *send != '\n' && *send != '\0'; send++) {
			x ^= (uint8_t)*send;
			put((uint8_t)*send);
		}
		if (*send == '\n') {
			send++;
		}
		put('*');
		hex(put, x >> 4);
		hex(put, x & 0xFu);
		put('\r');
		put('\n');
	}
}
